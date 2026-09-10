#include "core/alarmengine.h"

#include "utils/logger.h"

#include <utility>

AlarmEngine::AlarmEngine(QObject *parent)
    : QObject(parent)
{
}

QString AlarmEngine::makeKey(const QString &deviceId, const QString &tagId)
{
    return deviceId + QLatin1Char('/') + tagId;
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
    Log::info(QStringLiteral("新增告警规则 %1/%2 [%3, %4]")
                  .arg(rule.deviceId, rule.tagId)
                  .arg(rule.lowLimit, 0, 'f', 1)
                  .arg(rule.highLimit, 0, 'f', 1));
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

void AlarmEngine::acknowledge(const QString &deviceId, const QString &tagId)
{
    const QString key = makeKey(deviceId, tagId);
    if (m_active.remove(key) > 0)
        emit activeAlarmsChanged();
}

void AlarmEngine::acknowledgeAll()
{
    if (m_active.isEmpty())
        return;

    m_active.clear();
    emit activeAlarmsChanged();
}

void AlarmEngine::checkValue(const QString &deviceId, const QString &tagId, const QVariant &value)
{
    bool ok = false;
    const double v = value.toDouble(&ok);
    if (!ok)
        return;

    const QString key = makeKey(deviceId, tagId);

    for (const AlarmRule &rule : std::as_const(m_rules)) {
        if (!rule.enabled || rule.deviceId != deviceId || rule.tagId != tagId)
            continue;

        const bool outOfRange = (v < rule.lowLimit) || (v > rule.highLimit);
        const bool alreadyActive = m_active.contains(key);

        if (outOfRange && !alreadyActive) {
            AlarmRecord record;
            record.time = QDateTime::currentDateTime();
            record.deviceId = deviceId;
            record.tagId = tagId;
            record.value = v;
            record.message = QStringLiteral("%1 越限：当前 %2，允许范围 [%3, %4]")
                                 .arg(tagId)
                                 .arg(v, 0, 'f', 2)
                                 .arg(rule.lowLimit, 0, 'f', 2)
                                 .arg(rule.highLimit, 0, 'f', 2);

            m_active.insert(key, record);
            Log::warn(record.message);
            emit alarmRaised(record);
            emit activeAlarmsChanged();
        } else if (!outOfRange && alreadyActive) {
            m_active.remove(key);
            Log::info(QStringLiteral("%1 已恢复正常（%2）").arg(tagId).arg(v, 0, 'f', 2));
            emit alarmCleared(deviceId, tagId);
            emit activeAlarmsChanged();
        }

        break; // 每个点位只匹配第一条规则
    }
}
