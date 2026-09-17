#include "core/acquisitionscheduler.h"

#include "comm/mockconnection.h"
#include "comm/modbusconnection.h"
#include "comm/mqttconnection.h"
#include "core/devicemanager.h"
#include "utils/logger.h"

#include <QTimer>

namespace {

/// 重连退避上限（毫秒）。
constexpr int kMaxReconnectDelayMs = 30000;

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

DeviceConnection *AcquisitionScheduler::createConnection(const DeviceInfo &device, QObject *parent)
{
    DeviceConnection *connection = nullptr;

    switch (device.protocol) {
    case DeviceProtocol::ModbusTcp:
        connection = new ModbusTcpConnection(parent);
        break;
    case DeviceProtocol::Mqtt:
        connection = new MqttConnection(parent);
        break;
    case DeviceProtocol::Mock:
    default:
        // 无硬件时也能跑通全流程
        connection = new MockConnection(parent);
        break;
    }

    // 唯一入口：把设备配置（点位表 / 从站号 / 采集周期）喂给协议实现。
    // MockConnection 会忽略协议参数，只用点位表和周期。
    connection->configure(device);
    return connection;
}

bool AcquisitionScheduler::openConnection(const QString &deviceId, Runtime &runtime)
{
    const DeviceInfo info = m_manager->device(deviceId);

    DeviceConnection *connection = createConnection(info, this);
    runtime.connection = connection;

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

    if (!connection->open(info.host, info.port)) {
        Log::error(QStringLiteral("连接设备失败: %1 (%2:%3)")
                       .arg(info.name, info.host)
                       .arg(info.port));
        disconnect(connection, nullptr, this, nullptr);
        connection->deleteLater();
        runtime.connection = nullptr;

        if (m_manager)
            m_manager->setOnline(deviceId, false);
        return false;
    }

    runtime.attempt = 0;
    m_manager->setOnline(deviceId, true);
    m_manager->setFault(deviceId, false);

    emit started(deviceId);
    emit connectionStateChanged(deviceId, true);
    return true;
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

    if (!openConnection(deviceId, m_runtimes[deviceId])) {
        // 首次连接失败不致命：转入后台自动重连
        scheduleReconnect(deviceId);
        return false;
    }
    return true;
}

void AcquisitionScheduler::stop(const QString &deviceId)
{
    if (!m_runtimes.contains(deviceId))
        return;

    Runtime runtime = m_runtimes.take(deviceId);
    runtime.manualStop = true;

    if (runtime.reconnectTimer) {
        runtime.reconnectTimer->stop();
        runtime.reconnectTimer->deleteLater();
    }
    if (runtime.connection) {
        runtime.connection->close();
        runtime.connection->deleteLater();
    }

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
    return it->connection && it->connection->isOpen();
}

void AcquisitionScheduler::onConnectionClosed(const QString &deviceId)
{
    const auto it = m_runtimes.find(deviceId);
    if (it == m_runtimes.end())
        return; // 已 stop()，忽略

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

    if (it->connection) {
        it->connection->deleteLater();
        it->connection = nullptr;
    }

    if (openConnection(deviceId, *it)) {
        Log::info(QStringLiteral("设备 %1 已重新连接").arg(deviceId));
    } else {
        scheduleReconnect(deviceId);
    }
}

bool AcquisitionScheduler::writeTag(const QString &deviceId, const QString &tagId, const QVariant &value)
{
    const auto it = m_runtimes.constFind(deviceId);
    if (it == m_runtimes.constEnd() || !it->connection) {
        Log::warn(QStringLiteral("下发失败：设备未启动采集 %1").arg(deviceId));
        return false;
    }
    return it->connection->writeTag(tagId, value);
}
