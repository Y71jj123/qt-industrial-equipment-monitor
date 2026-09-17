#include "core/acquisitionscheduler.h"

#include "comm/mockconnection.h"
#include "comm/modbusconnection.h"
#include "comm/mqttconnection.h"
#include "core/devicemanager.h"
#include "utils/logger.h"

#include <QMetaObject>
#include <QThread>
#include <QTimer>

namespace {

/// 重连退避上限（毫秒）。
constexpr int kMaxReconnectDelayMs = 30000;

/// 停线程时最多等多久（毫秒）。
///
/// 线程平时都空闲在事件循环里等定时器，quit() 后立刻就能退出，等的时间约等于 0；
/// 只有恰好卡在阻塞式收发里才需要等，而单个采集周期的阻塞上限是几秒
/// （Modbus 每点位 800ms、MQTT 建连两次 3s），10s 足够覆盖，留出余量是为了
/// 让下面的 terminate() 兜底永远走不到。
constexpr int kThreadStopTimeoutMs = 10000;

} // namespace

AcquisitionScheduler::AcquisitionScheduler(DeviceManager *manager, QObject *parent)
    : QObject(parent)
    , m_manager(manager)
{
}

AcquisitionScheduler::~AcquisitionScheduler()
{
    stopAll();
}

DeviceConnection *AcquisitionScheduler::createConnection(const DeviceInfo &device)
{
    DeviceConnection *connection = nullptr;

    switch (device.protocol) {
    case DeviceProtocol::ModbusTcp:
        connection = new ModbusTcpConnection();
        break;
    case DeviceProtocol::Mqtt:
        connection = new MqttConnection();
        break;
    case DeviceProtocol::Mock:
    default:
        // 无硬件时也能跑通全流程
        connection = new MockConnection();
        break;
    }

    // 唯一入口：把设备配置（点位表 / 从站号 / 采集周期）喂给协议实现。
    // MockConnection 会忽略协议参数，只用点位表和周期。
    //
    // configure() 只写普通成员（不建 socket、不起定时器），所以在 moveToThread
    // 之前调用是安全的；放在这里是为了让工作线程一开工就能直接 open()。
    connection->configure(device);
    return connection;
}

bool AcquisitionScheduler::start(const QString &deviceId)
{
    if (!m_manager || !m_manager->contains(deviceId)) {
        Log::warn(QStringLiteral("启动采集失败：设备不存在 %1").arg(deviceId));
        return false;
    }

    if (m_runtimes.contains(deviceId))
        return false;

    Runtime runtime;
    m_runtimes.insert(deviceId, runtime);

    if (!startWorker(deviceId)) {
        m_runtimes.remove(deviceId);
        return false;
    }
    return true;
}

bool AcquisitionScheduler::startWorker(const QString &deviceId)
{
    const auto it = m_runtimes.find(deviceId);
    if (it == m_runtimes.end())
        return false;

    // 每台设备一条线程：一台设备卡在 socket 收发上，不会拖住别的设备和界面
    auto *thread = new QThread(this);
    thread->setObjectName(QStringLiteral("acq-%1").arg(deviceId.left(8)));
    it->thread = thread;

    attachConnection(deviceId, *it);

    thread->start();

    // 线程跑起来之后再投递 open()：事件循环没起来的话排队事件无处可投
    requestOpen(deviceId);
    return true;
}

void AcquisitionScheduler::attachConnection(const QString &deviceId, Runtime &runtime)
{
    const DeviceInfo info = m_manager->device(deviceId);

    DeviceConnection *connection = createConnection(info);

    // 对象连同将来的 socket / 定时器一起搬到设备线程。
    // 搬之前不能有父对象，否则 moveToThread 会失败。
    connection->moveToThread(runtime.thread);

    // 下面三个信号的发送者住在设备线程，接收者（this）住在 GUI 线程，
    // 自动连接会退化成队列连接 —— 信号到达时已经在 GUI 线程，可以放心改本类成员。
    connect(connection, &DeviceConnection::tagValueChanged, this,
            [this, deviceId](const QString &tagId, const QVariant &value) {
                emit tagUpdated(deviceId, tagId, value);
            });
    connect(connection, &DeviceConnection::errorOccurred, this,
            [this, deviceId](const QString &message) {
                emit errorOccurred(deviceId, message);
            });
    connect(connection, &DeviceConnection::closed, this, [this, deviceId]() {
        onConnectionClosed(deviceId);
    });

    // 线程结束时让连接**在自己的线程里**析构（deleteLater 由线程收尾时投递处理），
    // 否则跨线程 delete 一个还挂着 socket 的对象是未定义行为。
    connect(runtime.thread, &QThread::finished, connection, &QObject::deleteLater);

    runtime.connection = connection;
    runtime.connected = false;
}

void AcquisitionScheduler::detachConnection(Runtime &runtime)
{
    DeviceConnection *connection = runtime.connection;
    runtime.connection = nullptr;
    runtime.connected = false;

    if (!connection)
        return;

    // close() 必须回到它自己的线程里做（socket 的线程归属不能错），
    // 线程已经退出时 invokeMethod 会返回 false —— 那时对象也已由 finished 信号销毁了。
    QMetaObject::invokeMethod(connection, [connection]() {
        connection->close();
        connection->deleteLater();
    }, Qt::QueuedConnection);
}

void AcquisitionScheduler::requestOpen(const QString &deviceId)
{
    const auto it = m_runtimes.find(deviceId);
    if (it == m_runtimes.end() || !it->connection)
        return;

    DeviceConnection *connection = it->connection;
    const DeviceInfo info = m_manager->device(deviceId);
    const QString host = info.host;
    const quint16 port = info.port;

    // ⚠️ 外层 lambda 在**设备线程**里执行：只允许碰 connection 和入参副本，
    //    绝不能读写调度器的成员；结果一律再排队回 GUI 线程处理。
    const bool posted = QMetaObject::invokeMethod(
        connection,
        [this, connection, deviceId, host, port]() {
            const bool ok = connection->open(host, port);
            QMetaObject::invokeMethod(this, [this, deviceId, ok]() { onOpenFinished(deviceId, ok); },
                                      Qt::QueuedConnection);
        },
        Qt::QueuedConnection);

    if (!posted) {
        Log::error(QStringLiteral("设备 %1 的采集线程不可用，无法建立连接").arg(deviceId));
        onOpenFinished(deviceId, false);
    }
}

void AcquisitionScheduler::onOpenFinished(const QString &deviceId, bool ok)
{
    const auto it = m_runtimes.find(deviceId);
    if (it == m_runtimes.end())
        return; // 已经 stop()，结果直接丢弃

    if (!ok) {
        const DeviceInfo info = m_manager->device(deviceId);
        Log::error(QStringLiteral("连接设备失败: %1 (%2:%3)")
                       .arg(info.name, info.host)
                       .arg(info.port));
        if (m_manager)
            m_manager->setOnline(deviceId, false);

        // 首次连接失败不致命：转入后台自动重连
        scheduleReconnect(deviceId);
        return;
    }

    // 退避计数非零说明这是重连回来（首次连接时恒为 0）
    const bool reconnected = (it->attempt > 0);
    it->attempt = 0;
    it->connected = true;

    if (m_manager) {
        m_manager->setOnline(deviceId, true);
        m_manager->setFault(deviceId, false);
    }

    if (reconnected)
        Log::info(QStringLiteral("设备 %1 已重新连接").arg(deviceId));

    emit started(deviceId);
    emit connectionStateChanged(deviceId, true);
}

void AcquisitionScheduler::teardownWorker(Runtime &runtime)
{
    QThread *thread = runtime.thread;
    if (!thread)
        return;

    runtime.connection = nullptr; // 由 finished → deleteLater 负责销毁

    // 只有 quit() + wait() 才能保证线程真的结束：不等就析构 QThread 会直接崩。
    thread->quit();
    if (!thread->wait(kThreadStopTimeoutMs)) {
        // 兜底：线程还卡在阻塞式收发里（正常不会走到这里）
        Log::warn(QStringLiteral("采集线程 %1 未在 %2 ms 内退出，强制终止")
                      .arg(thread->objectName())
                      .arg(kThreadStopTimeoutMs));
        thread->terminate();
        thread->wait();
    }

    delete thread; // 已确认结束，可以放心销毁（它同时还是本对象的子对象）
    runtime.thread = nullptr;
}

void AcquisitionScheduler::stop(const QString &deviceId)
{
    const auto it = m_runtimes.find(deviceId);
    if (it == m_runtimes.end())
        return;

    Runtime runtime = *it;
    m_runtimes.erase(it);
    runtime.manualStop = true;

    if (runtime.reconnectTimer) {
        runtime.reconnectTimer->stop();
        runtime.reconnectTimer->deleteLater();
        runtime.reconnectTimer = nullptr;
    }

    teardownWorker(runtime);

    if (m_manager)
        m_manager->setOnline(deviceId, false);

    emit stopped(deviceId);
    emit connectionStateChanged(deviceId, false);
}

void AcquisitionScheduler::stopAll()
{
    const QList<QString> ids = m_runtimes.keys();
    for (const QString &id : ids)
        stop(id);
}

bool AcquisitionScheduler::isRunning(const QString &deviceId) const
{
    const auto it = m_runtimes.constFind(deviceId);
    if (it == m_runtimes.constEnd())
        return false;

    // 读快照而不是 connection->isOpen()：后者住在别的线程，跨线程读它的成员是数据竞争
    return it->connected;
}

void AcquisitionScheduler::onConnectionClosed(const QString &deviceId)
{
    const auto it = m_runtimes.find(deviceId);
    if (it == m_runtimes.end())
        return; // 已 stop()，忽略

    it->connected = false;

    if (m_manager)
        m_manager->setOnline(deviceId, false);

    emit stopped(deviceId);
    emit connectionStateChanged(deviceId, false);

    if (it->manualStop)
        return;

    // 非用户主动停止 → 视为意外掉线，触发离线告警并安排重连
    emit deviceOffline(deviceId);
    scheduleReconnect(deviceId);
}

void AcquisitionScheduler::scheduleReconnect(const QString &deviceId)
{
    const auto it = m_runtimes.find(deviceId);
    if (it == m_runtimes.end() || it->manualStop)
        return;

    int delay = 1000 * (1 << qMin(it->attempt, 5)); // 1s,2s,4s,8s,16s,32s
    delay = qMin(delay, kMaxReconnectDelayMs);
    ++it->attempt;

    if (!it->reconnectTimer) {
        // 重连定时器住在 GUI 线程：它只负责"到点叫醒调度器"，不碰设备
        it->reconnectTimer = new QTimer(this);
        it->reconnectTimer->setSingleShot(true);
        connect(it->reconnectTimer, &QTimer::timeout, this, [this, deviceId]() {
            attemptReconnect(deviceId);
        });
    }
    it->reconnectTimer->start(delay);

    Log::warn(QStringLiteral("设备 %1 连接中断，%2 ms 后自动重连（第 %3 次）")
                  .arg(deviceId)
                  .arg(delay)
                  .arg(it->attempt));
}

void AcquisitionScheduler::attemptReconnect(const QString &deviceId)
{
    const auto it = m_runtimes.find(deviceId);
    if (it == m_runtimes.end() || it->manualStop)
        return;

    // 重连复用同一条线程（线程寿命跟"设备是否在采集"对齐），只换连接对象
    detachConnection(*it);
    attachConnection(deviceId, *it);
    requestOpen(deviceId);
}

bool AcquisitionScheduler::writeTag(const QString &deviceId, const QString &tagId, const QVariant &value)
{
    const auto it = m_runtimes.constFind(deviceId);
    if (it == m_runtimes.constEnd() || !it->connection) {
        Log::warn(QStringLiteral("下发失败：设备未启动采集 %1").arg(deviceId));
        return false;
    }

    DeviceConnection *connection = it->connection;

    // 同线程时直接调，免得自己等自己造成死锁（调度器常驻 GUI 线程，理论上不会发生）
    if (connection->thread() == QThread::currentThread())
        return connection->writeTag(tagId, value);

    bool ok = false;
    // 阻塞式队列调用：写入发生在设备线程里（socket 的线程归属正确），
    // 而远程控制面板要靠返回值给出"成功 / 失败"提示，所以这里等结果。
    // 下发是用户点一次才发生一次的低频操作，阻塞时间可忽略。
    // 设备线程已退出时 invokeMethod 直接返回 false，不会挂住界面。
    const bool invoked = QMetaObject::invokeMethod(
        connection,
        [connection, tagId, value, &ok]() { ok = connection->writeTag(tagId, value); },
        Qt::BlockingQueuedConnection);

    if (!invoked) {
        Log::warn(QStringLiteral("下发失败：设备 %1 的采集线程已退出").arg(deviceId));
        return false;
    }
    return ok;
}
