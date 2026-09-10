#pragma once

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QVariant>

/// 一条告警规则：某设备某点位的值超出 [lowLimit, highLimit] 即触发告警。
struct AlarmRule
{
    QString deviceId;
    QString tagId;
    double lowLimit = 0.0;
    double highLimit = 100.0;
    bool enabled = true;
};

/// 一条告警记录。
struct AlarmRecord
{
    QDateTime time;
    QString deviceId;
    QString tagId;
    double value = 0.0;
    QString message;
};

/// 告警引擎：吃进数据点，吐出告警。
///
/// 输入来自采集调度器的 tagUpdated 信号，输出是 alarmRaised / alarmCleared 信号。
/// 状态机：正常 →（越限）→ 活动告警 →（回到范围）→ 正常。
/// 同一点位在告警未消除前不会重复触发，避免刷屏。
class AlarmEngine : public QObject
{
    Q_OBJECT

public:
    explicit AlarmEngine(QObject *parent = nullptr);

    void addRule(const AlarmRule &rule);
    void removeRule(const QString &deviceId, const QString &tagId);
    void clearRules();
    QList<AlarmRule> rules() const;

    QList<AlarmRecord> activeAlarms() const;
    void acknowledge(const QString &deviceId, const QString &tagId);
    void acknowledgeAll();

public slots:
    /// 由采集调度器的 tagUpdated 信号驱动。
    void checkValue(const QString &deviceId, const QString &tagId, const QVariant &value);

signals:
    void alarmRaised(const AlarmRecord &record);
    void alarmCleared(const QString &deviceId, const QString &tagId);
    void activeAlarmsChanged();

private:
    static QString makeKey(const QString &deviceId, const QString &tagId);

    QList<AlarmRule> m_rules;
    QHash<QString, AlarmRecord> m_active; ///< key = deviceId + '/' + tagId
};

Q_DECLARE_METATYPE(AlarmRule)
Q_DECLARE_METATYPE(AlarmRecord)
