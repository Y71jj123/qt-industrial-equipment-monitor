#pragma once

#include "core/alarmengine.h"
#include "core/devicemanager.h"

#include <QDateTime>
#include <QList>
#include <QObject>
#include <QSqlDatabase>
#include <QString>

class QTimer;

/// 历史数据存储层（SQLite）。
///
/// 只负责"把数据存下来、按条件查回来"，不含业务判断。
/// 四张表：
///   - `samples`    采样值
///   - `alarms`     告警历史（含活动 / 确认状态）
///   - `operations` 操作留痕（远程下发、配置变更）
///   - `devices`    设备台账（配置持久化）
/// 服务端部署时把 QSQLITE 换成 QMYSQL 即可，接口不变。
///
/// **写放大控制**：采样点先进内存队列，攒够 kSampleBatchSize 条或每
/// kSampleFlushIntervalMs 由定时器触发一次，用**一个事务**批量 INSERT。
/// 高频采集下把 N 次磁盘同步压成 1 次，这是量级上的差别。
///
/// **线程约束**：SQLite 连接不能跨线程共用 —— 队列提交始终由本对象所在线程
/// （GUI 线程）的定时器驱动，工作线程只负责把数据通过信号抛过来。
class DataStorage : public QObject
{
    Q_OBJECT

public:
    struct Sample
    {
        QDateTime time;
        QString deviceId;
        QString tagId;
        double value = 0.0;
    };

    /// 操作留痕条目（远程下发、配置变更等）。
    struct OperationEntry
    {
        QDateTime time;
        QString user;
        QString deviceId;
        QString tagId;
        QString action;  ///< 动作名，如"写入点位"
        QString detail;  ///< 细节，如"value: 42 → 50"
        bool success = true;
    };

    /// 单台设备的统计行。
    struct DeviceStats
    {
        QString deviceId;
        qint64 sampleCount = 0; ///< 采样点总数
        int alarmCount = 0;     ///< 告警次数
        QDateTime firstSample;  ///< 首次采样时间
        QDateTime lastSample;   ///< 最后采样时间
        int handledCount = 0;   ///< 已录入处理结论的告警条数
        qint64 avgHandleMs = -1; ///< 该设备的平均处理时长（毫秒），无样本为 -1
    };

    explicit DataStorage(QObject *parent = nullptr, const QString &databasePath = QString());
    ~DataStorage() override;

    bool open();
    void close();
    bool isOpen() const;

    // ---------------- 采样 ----------------

    /// 攒够这么多条就立刻提交，不等定时器（高频采集时才走这条路）。
    static constexpr int kSampleBatchSize = 200;

    /// 提交间隔（毫秒）：低频采集时靠它保证数据不会在内存里久留。
    static constexpr int kSampleFlushIntervalMs = 200;

    /// 保留策略检查间隔（毫秒）：每日一次。
    static constexpr qint64 kRetentionCheckIntervalMs = 24LL * 60 * 60 * 1000;

    /// 写入一条采样记录。**只入内存队列，不立即落库** ——
    /// 返回值表示"已入队"，真正的落库结果是异步的。
    ///
    /// 注意：**数据库没打开也返回 true**。数据先入队，由 flush() 决定落库还是
    /// 溢出到磁盘队列 —— 采样是持续流，任何一条都不该因为库暂时不可用而被丢掉。
    bool insertSample(const QString &deviceId,
                      const QString &tagId,
                      double value,
                      const QDateTime &time = QDateTime());

    /// 立即把队列里的数据事务提交落库（队列为空时什么都不做）。
    /// 退出前必须在 aboutToQuit 里调用，否则最后一批数据会丢。
    void flush();

    /// 磁盘溢出队列里残留的采样条数（数据库不可用期间攒下的）。
    /// 0 表示没有积压；界面可据此提示"有数据待补传"。
    qint64 spillBacklogCount() const;

    /// 按设备 + 点位 + 时间区间查询历史数据（按时间升序）。
    QList<Sample> querySamples(const QString &deviceId,
                               const QString &tagId,
                               const QDateTime &from,
                               const QDateTime &to,
                               int limit = 10000) const;

    /// 把查询结果导出为 CSV（UTF-8 BOM，Excel 打开中文不乱码）。
    bool exportSamplesCsv(const QString &deviceId,
                          const QString &tagId,
                          const QDateTime &from,
                          const QDateTime &to,
                          const QString &filePath) const;

    // ---------------- 告警历史 ----------------

    bool insertAlarm(const AlarmRecord &record);

    /// 把某点位最近一条告警标记为已恢复（active = 0）。
    bool markAlarmInactive(const QString &deviceId, const QString &tagId);

    /// 把某点位最近一条告警标记为已确认（acknowledged = 1）。
    bool markAlarmAcknowledged(const QString &deviceId, const QString &tagId);

    /// 把某点位最近一条告警**补上处理结论**（工单闭环）。
    ///
    /// 与 markAlarmAcknowledged 分开是刻意的：确认只改一个布尔，
    /// 处理要写处理人 / 结论 / 分类三项，混在一个方法里以后必然要加参数、改到调用方。
    bool markAlarmHandled(const QString &deviceId,
                          const QString &tagId,
                          AlarmDisposition disposition,
                          const QString &handledBy,
                          const QString &note);

    QList<AlarmRecord> queryAlarms(const QDateTime &from,
                                   const QDateTime &to,
                                   int limit = 1000) const;

    bool clearAlarms();

    // ---------------- 操作留痕 ----------------

    bool insertOperation(const OperationEntry &entry);
    QList<OperationEntry> queryOperations(int limit = 500) const;

    // ---------------- 设备台账 ----------------

    bool saveDevice(const DeviceInfo &device);
    bool removeDevice(const QString &id);
    QList<DeviceInfo> loadDevices() const;

    /// 清空设备台账（配置导入时整体替换用）。历史采样 / 告警记录不受影响。
    bool clearDevices();

    // ---------------- 统计 ----------------

    QList<DeviceStats> queryDeviceStats(const QDateTime &from, const QDateTime &to) const;

    /// 时间区间内的采样点总数。
    ///
    /// 单独开一个 COUNT 而不是复用 queryDeviceStats()：仪表盘每几秒就要刷一次，
    /// 而后者会把全表按 `device` 分组算一遍（还多跑一条告警统计），开销不成比例。
    /// 区间传 QDateTime() （无效值）表示不限时间。
    qint64 countSamples(const QDateTime &from = QDateTime(), const QDateTime &to = QDateTime()) const;

    /// 时间区间内的告警条数（含已恢复的）。
    int countAlarms(const QDateTime &from = QDateTime(), const QDateTime &to = QDateTime()) const;

    // ---------------- 数据保留策略 ----------------

    /// 配置数据保留策略。
    /// @param retentionDays     原始采样只保留最近这么多天（更早的会被聚合后清理）
    /// @param downsampleEnabled 是否把过期数据按时间桶聚合成趋势行（默认开）
    /// @param bucketHours       降采样桶粒度（小时），默认 1 小时一个桶
    void setRetentionPolicy(int retentionDays, bool downsampleEnabled, int bucketHours);

    /// 立即执行一次保留策略：把超过保留期的原始采样按桶聚合进降采样表，再删除原始旧行。
    /// 返回是否成功，结果会写日志（清理 / 聚合了多少）。建议启动时调一次 + 每日定时调。
    bool applyRetentionPolicy();

    /// 降采样表里当前的趋势行数（测试 / 界面展示用）。
    qint64 downsampledRowCount() const;

    QString lastError() const;

private:
    bool createTables();

    /// 创建（若不存在）并启动数据保留策略的每日定时器。
    void startRetentionTimer();

    /// 给老库补列（CREATE TABLE IF NOT EXISTS 不会修改已存在的表）。
    /// 列已存在时静默返回 true —— 每次启动都会走一遍，不能刷错误日志。
    bool ensureColumn(const QString &table, const QString &column, const QString &definition);

    /// 待落库的采样点。
    struct PendingSample
    {
        QDateTime time;
        QString deviceId;
        QString tagId;
        double value = 0.0;
    };

    /// 把一批采样**溢出写入磁盘队列**（数据库不可用时的兜底）。
    ///
    /// 「断线」在这里指**落库链路不可用**（数据库没打开 / 事务提交失败），
    /// 不是设备链路 —— 设备断开时根本采不到值，谈不上缓存。
    /// 返回是否成功写盘。
    bool spillBatch(const QList<PendingSample> &batch);

    /// 重放磁盘队列（启动时调用）。按时间升序补写进库，成功后清空队列文件。
    ///
    /// **不重复**：只有写库失败的批次才会进队列，成功的批次绝不重放；
    /// 重放成功即整文件删除，所以也不会重复写第二次。
    void replaySpillFile();

    /// 队列 + 定时器都只在 GUI 线程碰，不需要加锁。
    QList<PendingSample> m_pendingSamples;
    QTimer *m_flushTimer = nullptr;

    /// 数据保留策略定时器：每日触发一次 applyRetentionPolicy()。
    QTimer *m_retentionTimer = nullptr;

    /// 保留策略参数（由 setRetentionPolicy 设置，默认保留 30 天、按 1 小时桶降采样）。
    int m_retentionDays = 30;
    bool m_downsampleEnabled = true;
    int m_bucketHours = 1;

    QString m_databasePath;
    /// 采样溢出队列文件：与数据库同目录，文件名派生自数据库名。
    QString m_spillPath;
    QSqlDatabase m_db;
    mutable QString m_lastError;
};
