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

/// 处理结论分类（工单闭环用）。
///
/// 分这个类不是为了好看 —— 现场真正要回答的是"这批告警里有多少是真故障"、
/// "误报占几成"。有了这个字段，报表才能给出可用结论，而不只是告警条数。
enum class AlarmDisposition
{
    Resolved = 0,   ///< 已处理恢复
    Maintained = 1, ///< 停机检修
    FalseAlarm = 2, ///< 误报
    Ignored = 3     ///< 观察中 / 暂不处理
};

/// 处理结论的中文名。
QString alarmDispositionName(AlarmDisposition disposition);

/// 把时长（毫秒）格式化成现场读得懂的文案："45.0 秒" / "12.4 分" / "1.2 时"。
///
/// 传入负值（表示"没有样本"）时返回 "—"。放在这里是因为告警面板与总览页都要用，
/// 各写一份迟早会出现两处显示不一致。
QString formatHandleDuration(qint64 milliseconds);

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

    // ---- 工单闭环：处理结论 ----
    // 设计意图：把"知道有告警"和"处理完了"分成两件事。
    // acknowledged 只表示"看见了"，handledAt 才表示"有人写了结论"。
    AlarmDisposition disposition = AlarmDisposition::Resolved; ///< 处理结论分类
    QString handledBy;      ///< 处理人
    QDateTime handledAt;    ///< 处理时间（无效 = 尚未录入结论）
    QString handlingNote;   ///< 处理结论 / 备注

    /// 是否已录入处理结论。
    bool handled() const { return handledAt.isValid(); }
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
    ///
    /// 注意与 handleAlarm() 的区别：**确认只是"看见了"，处理才代表"有人写了结论"**。
    /// 现场真正要考核的是后者。
    void acknowledge(const QString &deviceId, const QString &tagId);

    /// 确认全部活动告警。
    void acknowledgeAll();

    /// 录入处理结论（工单闭环）。
    ///
    /// 只有**活动告警**能被处理 —— 已恢复的记录再补结论属于事后补录，不在本接口范围内。
    /// 处理会顺带把该条标记为已确认（现场的"知道了"和"处理了"本来就是一次动作的两半）。
    ///
    /// @return 该点位没有活动告警时返回 false。
    bool handleAlarm(const QString &deviceId,
                     const QString &tagId,
                     AlarmDisposition disposition,
                     const QString &handledBy,
                     const QString &note);

    /// 已录入处理结论的告警条数（统计范围：全部历史）。
    int handledCount() const;

    /// 平均处理时长 MTTR（毫秒），统计范围为全部历史里已录入结论的告警。
    ///
    /// **没有任何样本时返回 -1** —— 界面据此显示"—"，而不是一个会误导人的 0。
    qint64 averageHandleDurationMs() const;

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

    /// 某条告警录入了处理结论。上层据此写操作留痕（谁、什么时候、怎么处理的）。
    void alarmHandled(const QString &deviceId, const QString &tagId, AlarmDisposition disposition);

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
Q_DECLARE_METATYPE(AlarmDisposition)
