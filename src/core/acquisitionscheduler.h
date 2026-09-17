#pragma once

#include "core/devicemanager.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <QVariant>

class DeviceConnection;
class QThread;
class QTimer;

/// 采集调度器：为每台设备建立一条连接，负责取数与下发。
///
/// 设计约束：调度器**不接触任何界面对象**，取到的数据一律通过信号抛出，
/// 由上层（界面）自行决定怎么显示。这样采集逻辑可以独立测试，
/// 也方便以后把采集放到独立线程里。
///
/// **线程模型**：调度器自己住在 GUI 线程，每台设备的 DeviceConnection
/// 独占一条 QThread（对象 moveToThread 过去，socket / 定时器都在那条线程里创建）。
/// 因此本类的所有成员只允许在 GUI 线程读写；工作线程只做"收发 + 抛信号"，
/// 两者之间一律靠队列连接（queued connection）通信 —— 对上层完全透明。
///
/// 断线后**自动重连**（指数退避：1s → 2s → 4s → 8s → 16s → 30s 封顶），
/// 只有显式 stop() 才会停止重连。重连复用同一条线程，不反复创建 / 销毁线程。
class AcquisitionScheduler : public QObject
{
    Q_OBJECT

public:
    explicit AcquisitionScheduler(DeviceManager *manager, QObject *parent = nullptr);
    ~AcquisitionScheduler() override;

    /// 开始采集指定设备；设备不存在时返回 false。
    /// 首次连接失败不返回错误，而是转入后台自动重连。
    /// 注意：建连在设备线程里异步进行，"连上"以 connectionStateChanged(true) 为准。
    bool start(const QString &deviceId);

    void stop(const QString &deviceId);
    void stopAll();

    /// 是否正处于"连上并在采集"的状态（重连等待中不算）。
    bool isRunning(const QString &deviceId) const;

    /// 向指定设备下发指令（写入一个点位）。
    /// 线程安全：请求会被投递到设备线程执行，本函数在结果回来后才返回。
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
    /// 每台设备的运行时状态。只在 GUI 线程访问。
    struct Runtime
    {
        DeviceConnection *connection = nullptr; ///< 住在 thread 里
        QThread *thread = nullptr;              ///< 该设备专属的采集线程
        QTimer *reconnectTimer = nullptr;       ///< 住在 GUI 线程（调度器自己的线程）
        int attempt = 0;                        ///< 已重连次数（用于退避）
        bool manualStop = false;                ///< 用户主动停止 → 不再重连
        bool connected = false;                 ///< 连接状态快照（见 isRunning 的说明）
    };

    /// 按设备配置创建连接（协议分支 + 统一 configure）。
    /// 返回的对象**没有父对象** —— moveToThread 要求如此。
    DeviceConnection *createConnection(const DeviceInfo &device);

    /// 起一条线程并挂上第一个连接（start 路径）。
    bool startWorker(const QString &deviceId);

    /// 造一个连接挂到已有线程上并投递 open()（首连 / 重连共用）。
    void attachConnection(const QString &deviceId, Runtime &runtime);

    /// 把连接从线程上摘下来：让它在自己的线程里 close + 析构。
    void detachConnection(Runtime &runtime);

    /// 投递一次 open() 到设备线程；结果异步回到 onOpenFinished()。
    void requestOpen(const QString &deviceId);

    /// open() 的结果回到 GUI 线程后的处理：置状态、发信号、安排重连。
    void onOpenFinished(const QString &deviceId, bool ok);

    /// 停线程：quit() + wait()，线程没退干净时兜底终止，绝不泄漏。
    void teardownWorker(Runtime &runtime);

    /// 连接断开回调：置离线、发信号、按需安排重连。
    void onConnectionClosed(const QString &deviceId);

    /// 安排一次重连（指数退避）。
    void scheduleReconnect(const QString &deviceId);

    /// 执行重连尝试。
    void attemptReconnect(const QString &deviceId);

    DeviceManager *m_manager = nullptr;
    QHash<QString, Runtime> m_runtimes;
};
