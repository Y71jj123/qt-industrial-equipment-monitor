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

    /// 写入一条采样记录。**只入内存队列，不立即落库** ——
    /// 返回值表示"已入队"，真正的落库结果是异步的（失败会记日志）。
    /// 数据库未打开时返回 false。
    bool insertSample(const QString &deviceId,
                      const QString &tagId,
                      double value,
                      const QDateTime &time = QDateTime());

    /// 立即把队列里的数据事务提交落库（队列为空时什么都不做）。
    /// 退出前必须在 aboutToQuit 里调用，否则最后一批数据会丢。
    void flush();

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

    QString lastError() const;

private:
    bool createTables();

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

    /// 队列 + 定时器都只在 GUI 线程碰，不需要加锁。
    QList<PendingSample> m_pendingSamples;
    QTimer *m_flushTimer = nullptr;

    QString m_databasePath;
    QSqlDatabase m_db;
    mutable QString m_lastError;
};
