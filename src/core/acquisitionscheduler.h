#pragma once

#include "core/devicemanager.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <QVariant>

class DeviceConnection;
class QTimer;

/// 采集调度器：为每台设备建立一条连接，负责取数与下发。
///
/// 设计约束：调度器**不接触任何界面对象**，取到的数据一律通过信号抛出，
/// 由上层（界面）自行决定怎么显示。这样采集逻辑可以独立测试，
/// 也方便以后把采集放到独立线程里。
///
/// 断线后**自动重连**（指数退避：1s → 2s → 4s → 8s → 16s → 30s 封顶），
/// 只有显式 stop() 才会停止重连。
class AcquisitionScheduler : public QObject
{
    Q_OBJECT

public:
    explicit AcquisitionScheduler(DeviceManager *manager, QObject *parent = nullptr);
    ~AcquisitionScheduler() override;

    /// 开始采集指定设备；设备不存在时返回 false。
    /// 首次连接失败不返回错误，而是转入后台自动重连。
    bool start(const QString &deviceId);

    void stop(const QString &deviceId);
    void stopAll();

    /// 是否正处于"连上并在采集"的状态（重连等待中不算）。
    bool isRunning(const QString &deviceId) const;

    /// 向指定设备下发指令（写入一个点位）。
    bool writeTag(const QString &deviceId, const QString &tagId, const QVariant &value);

signals:
    void started(const QString &deviceId);
    void stopped(const QString &deviceId);
    void tagUpdated(const QString &deviceId, const QString &tagId, const QVariant &value);
    void errorOccurred(const QString &deviceId, const QString &message);

    /// 连接状态变化：true = 已连上（含重连成功），false = 断开 / 停止。
    void connectionStateChanged(const QString &deviceId, bool connected);

    /// 设备**意外掉线**（非用户主动 stop），用于触发离线告警。
    void deviceOffline(const QString &deviceId);

private:
    /// 每台设备的运行时状态。
    struct Runtime
    {
        DeviceConnection *connection = nullptr;
        QTimer *reconnectTimer = nullptr;
        int attempt = 0;       ///< 已重连次数（用于退避）
        bool manualStop = false; ///< 用户主动停止 → 不再重连
    };

    /// 按设备配置创建连接（协议分支 + 统一 configure）。
    DeviceConnection *createConnection(const DeviceInfo &device, QObject *parent);

    /// 建立连接并接线；成功返回 true。
    bool openConnection(const QString &deviceId, Runtime &runtime);

    /// 连接断开回调：置离线、发信号、按需安排重连。
    void onConnectionClosed(const QString &deviceId);

    /// 安排一次重连（指数退避）。
    void scheduleReconnect(const QString &deviceId);

    /// 执行重连尝试。
    void attemptReconnect(const QString &deviceId);

    DeviceManager *m_manager = nullptr;
    QHash<QString, Runtime> m_runtimes;
};
