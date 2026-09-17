#pragma once

#include "core/alarmengine.h"
#include "core/devicemanager.h"

#include <QDateTime>
#include <QList>
#include <QObject>
#include <QSqlDatabase>
#include <QString>

/// 历史数据存储层（SQLite）。
///
/// 只负责"把数据存下来、按条件查回来"，不含业务判断。
/// 四张表：
///   - `samples`    采样值
///   - `alarms`     告警历史（含活动 / 确认状态）
///   - `operations` 操作留痕（远程下发、配置变更）
///   - `devices`    设备台账（配置持久化）
/// 服务端部署时把 QSQLITE 换成 QMYSQL 即可，接口不变。
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

    /// 写入一条采样记录；失败时返回 false，错误信息见 lastError()。
    bool insertSample(const QString &deviceId,
                      const QString &tagId,
                      double value,
                      const QDateTime &time = QDateTime());

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

    // ---------------- 统计 ----------------

    QList<DeviceStats> queryDeviceStats(const QDateTime &from, const QDateTime &to) const;

    QString lastError() const;

private:
    bool createTables();

    QString m_databasePath;
    QSqlDatabase m_db;
    mutable QString m_lastError;
};
