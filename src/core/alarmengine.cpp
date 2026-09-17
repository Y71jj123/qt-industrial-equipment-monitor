#include "core/alarmengine.h"

#include "utils/logger.h"

#include <QtGlobal>

#include <utility>

namespace {

/// 历史记录上限，避免长时间运行后无限增长。
constexpr int kMaxHistory = 500;

/// 变化率告警的严重程度：超过阈值 2 倍算严重。
constexpr double kRateCriticalFactor = 2.0;

} // namespace

QString alarmLevelName(AlarmLevel level)
{
    switch (level) {
    case AlarmLevel::Critical:
        return QStringLiteral("严重");
    case AlarmLevel::Warning:
        return QStringLiteral("警告");
    case AlarmLevel::Info:
        break;
    }
    return QStringLiteral("提示");
}

QString alarmKindName(AlarmKind kind)
{
    switch (kind) {
    case AlarmKind::RateChange:
        return QStringLiteral("变化率");
    case AlarmKind::Offline:
        return QStringLiteral("离线");
    case AlarmKind::Range:
        break;
    }
    return QStringLiteral("阈值");
}

AlarmEngine::AlarmEngine(QObject *parent)
    : QObject(parent)
{
}

QString AlarmEngine::makeKey(const QString &deviceId, const QString &tagId)
{
    return deviceId + QLatin1Char('/') + tagId;
}

QString AlarmEngine::offlineTag()
{
    return QStringLiteral("__offline__");
}

AlarmLevel AlarmEngine::judgeLevel(double value, double low, double high)
{
    // 按"越限幅度 / 量程"分级：越得越多越严重。
    const double span = qMax(1.0, high - low);
    double over = 0.0;
    if (value > high)
        over = value - high;
    else if (value < low)
        over = low - value;

    const double ratio = over / span;
    if (ratio >= 0.10)
        return AlarmLevel::Critical;
    if (ratio >= 0.02)
        return AlarmLevel::Warning;
    return AlarmLevel::Info;
}

void AlarmEngine::addRule(const AlarmRule &rule)
{
    for (int i = 0; i < m_rules.size(); ++i) {
        if (m_rules.at(i).deviceId == rule.deviceId && m_rules.at(i).tagId == rule.tagId) {
            m_rules[i] = rule; // 同点位重复配置视为更新
            return;
        }
    }
    m_rules.append(rule);

    if (rule.kind == AlarmKind::RateChange) {
        Log::info(QStringLiteral("新增告警规则 %1/%2 变化率 ≤ %3/s")
                      .arg(rule.deviceId, rule.tagId)
                      .arg(rule.rateLimit, 0, 'f', 2));
    } else {
        Log::info(QStringLiteral("新增告警规则 %1/%2 [%3, %4]")
                      .arg(rule.deviceId, rule.tagId)
                      .arg(rule.lowLimit, 0, 'f', 1)
                      .arg(rule.highLimit, 0, 'f', 1));
    }
}

void AlarmEngine::removeRule(const QString &deviceId, const QString &tagId)
{
    for (int i = 0; i < m_rules.size(); ++i) {
        if (m_rules.at(i).deviceId == deviceId && m_rules.at(i).tagId == tagId) {
            m_rules.removeAt(i);
            return;
        }
    }
}

void AlarmEngine::clearRules()
{
    m_rules.clear();
}

QList<AlarmRule> AlarmEngine::rules() const
{
    return m_rules;
}

QList<AlarmRecord> AlarmEngine::activeAlarms() const
{
    return m_active.values();
}

QList<AlarmRecord> AlarmEngine::history() const
{
    return m_history;
}

void AlarmEngine::raiseRecord(const QString &key, AlarmRecord record, bool logAsWarning)
{
    m_active.insert(key, record);
    m_history.prepend(record);
    while (m_history.size() > kMaxHistory)
        m_history.removeLast();

    const QString text = QStringLiteral("[%1] %2").arg(alarmLevelName(record.level), record.message);
    if (logAsWarning)
        Log::warn(text);
    else
        Log::info(text);

    emit alarmRaised(record);
    emit activeAlarmsChanged();
    emit historyChanged();
}

void AlarmEngine::closeRecord(const QString &key, const QString &deviceId, const QString &tagId)
{
    m_active.remove(key);

    // 把历史里该点位仍未恢复的那条标记为已恢复。
    for (AlarmRecord &record : m_history) {
        if (makeKey(record.deviceId, record.tagId) == key && record.active) {
            record.active = false;
            break;
        }
    }

    emit alarmCleared(deviceId, tagId);
    emit activeAlarmsChanged();
    emit historyChanged();
}

void AlarmEngine::acknowledge(const QString &deviceId, const QString &tagId)
{
    const QString key = makeKey(deviceId, tagId);
    bool changed = false;

    if (m_active.contains(key) && !m_active[key].acknowledged) {
        m_active[key].acknowledged = true;
        changed = true;
    }

    // 同步历史里该点位最近一条活动记录，保证面板显示一致。
    for (AlarmRecord &record : m_history) {
        if (makeKey(record.deviceId, record.tagId) == key && record.active) {
            if (!record.acknowledged) {
                record.acknowledged = true;
                changed = true;
            }
            break;
        }
    }

    if (changed) {
        emit activeAlarmsChanged();
        emit historyChanged();
    }
}

void AlarmEngine::acknowledgeAll()
{
    bool changed = false;

    for (AlarmRecord &record : m_active) {
        if (!record.acknowledged) {
            record.acknowledged = true;
            changed = true;
        }
    }
    for (AlarmRecord &record : m_history) {
        if (record.active && !record.acknowledged) {
            record.acknowledged = true;
            changed = true;
        }
    }

    if (changed) {
        emit activeAlarmsChanged();
        emit historyChanged();
    }
}

void AlarmEngine::clearAlarm(const QString &deviceId, const QString &tagId)
{
    const QString key = makeKey(deviceId, tagId);
    if (!m_active.contains(key))
        return;

    m_active.remove(key);
    for (AlarmRecord &record : m_history) {
        if (makeKey(record.deviceId, record.tagId) == key && record.active) {
            record.active = false;
            break;
        }
    }

    Log::info(QStringLiteral("%1/%2 告警已人工消除").arg(deviceId, tagId));
    emit activeAlarmsChanged();
    emit historyChanged();
}

void AlarmEngine::clearAcknowledged()
{
    const QList<QString> keys = m_active.keys();
    int cleared = 0;

    for (const QString &key : keys) {
        const AlarmRecord record = m_active.value(key);
        if (!record.acknowledged)
            continue;

        m_active.remove(key);
        for (AlarmRecord &item : m_history) {
            if (makeKey(item.deviceId, item.tagId) == key && item.active) {
                item.active = false;
                break;
            }
        }
        ++cleared;
    }

    if (cleared > 0) {
        Log::info(QStringLiteral("已消警 %1 条").arg(cleared));
        emit activeAlarmsChanged();
        emit historyChanged();
    }
}

void AlarmEngine::clearHistory()
{
    if (m_history.isEmpty())
        return;

    m_history.clear();
    emit historyChanged();
}

void AlarmEngine::raiseOfflineAlarm(const QString &deviceId, const QString &deviceName)
{
    const QString key = makeKey(deviceId, offlineTag());
    if (m_active.contains(key))
        return;

    AlarmRecord record;
    record.time = QDateTime::currentDateTime();
    record.deviceId = deviceId;
    record.tagId = offlineTag();
    record.kind = AlarmKind::Offline;
    record.level = AlarmLevel::Critical;
    record.active = true;
    record.acknowledged = false;
    record.message = QStringLiteral("设备「%1」通信中断，已离线")
                         .arg(deviceName.isEmpty() ? deviceId : deviceName);

    raiseRecord(key, record, true);
}

void AlarmEngine::clearOfflineAlarm(const QString &deviceId)
{
    const QString key = makeKey(deviceId, offlineTag());
    if (!m_active.contains(key))
        return;

    Log::info(QStringLiteral("设备 %1 已恢复在线").arg(deviceId));
    closeRecord(key, deviceId, offlineTag());
}

void AlarmEngine::checkValue(const QString &deviceId, const QString &tagId, const QVariant &value)
{
    bool ok = false;
    const double v = value.toDouble(&ok);
    if (!ok)
        return;

    const QString key = makeKey(deviceId, tagId);
    const QDateTime now = QDateTime::currentDateTime();

    // 变化率要用"上一次的值"，所以先算再更新缓存。
    const auto lastIt = m_lastSamples.constFind(key);
    const bool hasLast = (lastIt != m_lastSamples.constEnd());
    const double lastValue = hasLast ? lastIt->first : v;
    const QDateTime lastTime = hasLast ? lastIt->second : now;
    m_lastSamples.insert(key, qMakePair(v, now));

    const double seconds = qMax(0.001, double(lastTime.msecsTo(now)) / 1000.0);
    const double rate = hasLast ? qAbs(v - lastValue) / seconds : 0.0;

    for (const AlarmRule &rule : std::as_const(m_rules)) {
        if (!rule.enabled || rule.deviceId != deviceId || rule.tagId != tagId)
            continue;

        bool outOfRange = false;
        if (rule.kind == AlarmKind::RateChange) {
            if (rule.rateLimit <= 0.0)
                break;
            outOfRange = (rate > rule.rateLimit);
        } else {
            outOfRange = (v < rule.lowLimit) || (v > rule.highLimit);
        }

        const bool alreadyActive = m_active.contains(key);

        if (outOfRange && !alreadyActive) {
            AlarmRecord record;
            record.time = now;
            record.deviceId = deviceId;
            record.tagId = tagId;
            record.value = v;
            record.lowLimit = rule.lowLimit;
            record.highLimit = rule.highLimit;
            record.kind = rule.kind;
            record.active = true;
            record.acknowledged = false;

            if (rule.kind == AlarmKind::RateChange) {
                record.level = (rate > rule.rateLimit * kRateCriticalFactor) ? AlarmLevel::Critical
                                                                            : AlarmLevel::Warning;
                record.message = QStringLiteral("%1 变化率超限：%2/s（上限 %3/s）")
                                     .arg(tagId)
                                     .arg(rate, 0, 'f', 2)
                                     .arg(rule.rateLimit, 0, 'f', 2);
            } else {
                record.level = judgeLevel(v, rule.lowLimit, rule.highLimit);
                const QString direction = (v > rule.highLimit) ? QStringLiteral("高于上限")
                                                               : QStringLiteral("低于下限");
                record.message = QStringLiteral("%1 %2：当前 %3，允许范围 [%4, %5]")
                                     .arg(tagId, direction)
                                     .arg(v, 0, 'f', 2)
                                     .arg(rule.lowLimit, 0, 'f', 2)
                                     .arg(rule.highLimit, 0, 'f', 2);
            }

            raiseRecord(key, record, true);
        } else if (!outOfRange && alreadyActive) {
            Log::info(QStringLiteral("%1 已恢复正常（%2）").arg(tagId).arg(v, 0, 'f', 2));
            closeRecord(key, deviceId, tagId);
        }

        break; // 每个点位只匹配第一条规则
    }
}
