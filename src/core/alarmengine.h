#pragma once

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QMetaType>
#include <QObject>
#include <QPair>
#include <QString>
#include <QVariant>

/// 告警级别：按"越限幅度 / 量程"自动分级。
enum class AlarmLevel
{
    Info = 0,    ///< 提示：轻微越限
    Warning = 1, ///< 警告：明显越限
    Critical = 2 ///< 严重：大幅越限
};

/// 告警级别的中文名（界面展示用）。
QString alarmLevelName(AlarmLevel level);

/// 告警类型。
enum class AlarmKind
{
    Range = 0,      ///< 阈值越限
    RateChange = 1, ///< 变化率超限
    Offline = 2     ///< 设备离线
};

/// 告警类型的中文名。
QString alarmKindName(AlarmKind kind);

/// 一条告警规则。
///
/// `kind == Range` 时用 [lowLimit, highLimit] 判阈值；
/// `kind == RateChange` 时用 rateLimit（单位 / 秒）判变化率。
struct AlarmRule
{
    QString deviceId;
    QString tagId;
    AlarmKind kind = AlarmKind::Range;
    double lowLimit = 0.0;
    double highLimit = 100.0;
    double rateLimit = 0.0; ///< 变化率上限（单位 / 秒），kind == RateChange 时生效
    bool enabled = true;
};

/// 一条告警记录。
struct AlarmRecord
{
    QDateTime time;                         ///< 触发时间
    QString deviceId;                       ///< 设备
    QString tagId;                          ///< 点位
    AlarmKind kind = AlarmKind::Range;      ///< 告警类型
    double value = 0.0;                     ///< 触发时的值
    double lowLimit = 0.0;                  ///< 规则下限
    double highLimit = 0.0;                 ///< 规则上限
    AlarmLevel level = AlarmLevel::Warning; ///< 告警级别
    QString message;                        ///< 可读描述
    bool active = true;                     ///< 是否仍在活动（尚未恢复 / 未消警）
    bool acknowledged = false;              ///< 是否已被人工确认
};

/// 告警引擎：吃进数据点，吐出告警。
///
/// 输入来自采集调度器的 tagUpdated 信号，输出是 alarmRaised / alarmCleared 信号。
/// 状态机：正常 →（越限 / 变化率超限）→ 活动告警 →（恢复 / 消警）→ 正常。
/// 同一点位在告警未消除前不会重复触发，避免刷屏。
///
/// 除"当前活动告警"外，本类还维护一份**告警历史**（含已恢复的），
/// 供界面做告警列表 / 历史查询 / 人工确认 / 消警。
class AlarmEngine : public QObject
{
    Q_OBJECT

public:
    explicit AlarmEngine(QObject *parent = nullptr);

    void addRule(const AlarmRule &rule);
    void removeRule(const QString &deviceId, const QString &tagId);
    void clearRules();
    QList<AlarmRule> rules() const;

    /// 当前活动（未恢复 / 未消警）的告警。
    QList<AlarmRecord> activeAlarms() const;

    /// 全部告警历史，最新在前（含已恢复的）。
    QList<AlarmRecord> history() const;

    /// 人工确认某条活动告警（标记已确认，不改变其活动状态）。
    void acknowledge(const QString &deviceId, const QString &tagId);

    /// 确认全部活动告警。
    void acknowledgeAll();

    /// 人工消警：把某点位的活动告警标记为已消除（active = false）。
    void clearAlarm(const QString &deviceId, const QString &tagId);

    /// 消警全部已确认的活动告警。
    void clearAcknowledged();

    /// 清空告警历史（不影响当前活动告警）。
    void clearHistory();

    /// 设备离线告警（由上层在设备掉线时调用）。
    void raiseOfflineAlarm(const QString &deviceId, const QString &deviceName);

    /// 设备恢复在线时清除其离线告警。
    void clearOfflineAlarm(const QString &deviceId);

public slots:
    /// 由采集调度器的 tagUpdated 信号驱动。
    void checkValue(const QString &deviceId, const QString &tagId, const QVariant &value);

signals:
    void alarmRaised(const AlarmRecord &record);
    void alarmCleared(const QString &deviceId, const QString &tagId);
    void activeAlarmsChanged();
    /// 历史或确认状态发生变化，界面据此刷新告警面板。
    void historyChanged();

private:
    static QString makeKey(const QString &deviceId, const QString &tagId);
    static AlarmLevel judgeLevel(double value, double low, double high);
    static QString offlineTag();

    /// 把一条新告警写入活动集合 + 历史，并发出信号。
    void raiseRecord(const QString &key, AlarmRecord record, bool logAsWarning);

    /// 把某 key 的活动告警置为已消除（恢复或消警共用）。
    void closeRecord(const QString &key, const QString &deviceId, const QString &tagId);

    QList<AlarmRule> m_rules;
    QHash<QString, AlarmRecord> m_active;              ///< key = deviceId + '/' + tagId
    QList<AlarmRecord> m_history;                      ///< 告警历史（最新在前）
    QHash<QString, QPair<double, QDateTime>> m_lastSamples; ///< 变化率计算用的上次数值
};

Q_DECLARE_METATYPE(AlarmRule)
Q_DECLARE_METATYPE(AlarmRecord)
Q_DECLARE_METATYPE(AlarmLevel)
Q_DECLARE_METATYPE(AlarmKind)
