#include "core/acquisitionscheduler.h"

#include "comm/mockconnection.h"
#include "core/devicemanager.h"
#include "utils/logger.h"

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
    // TODO: 按 device 的协议类型分支，例如：
    //   case Protocol::ModbusTcp: return new ModbusConnection(parent);
    //   case Protocol::Mqtt:      return new MqttConnection(parent);
    // 现在统一返回模拟连接，保证无硬件时也能跑通全流程。
    Q_UNUSED(device);
    return new MockConnection(parent);
}

bool AcquisitionScheduler::start(const QString &deviceId)
{
    if (!m_manager || !m_manager->contains(deviceId)) {
        Log::warn(QStringLiteral("启动采集失败：设备不存在 %1").arg(deviceId));
        return false;
    }

    if (m_connections.contains(deviceId))
        return false;

    const DeviceInfo info = m_manager->device(deviceId);

    DeviceConnection *conn = createConnection(info, this);
    connect(conn, &DeviceConnection::tagValueChanged, this,
            [this, deviceId](const QString &tagId, const QVariant &value) {
                emit tagUpdated(deviceId, tagId, value);
            });
    connect(conn, &DeviceConnection::errorOccurred, this,
            [this, deviceId](const QString &message) {
                emit errorOccurred(deviceId, message);
            });
    connect(conn, &DeviceConnection::closed, this, [this, deviceId]() {
        m_manager->setOnline(deviceId, false);
        emit stopped(deviceId);
    });

    if (!conn->open(info.host, info.port)) {
        Log::error(QStringLiteral("连接设备失败: %1").arg(info.name));
        conn->deleteLater();
        return false;
    }

    m_connections.insert(deviceId, conn);
    m_manager->setOnline(deviceId, true);
    emit started(deviceId);
    return true;
}

void AcquisitionScheduler::stop(const QString &deviceId)
{
    DeviceConnection *conn = m_connections.take(deviceId);
    if (!conn)
        return;

    conn->close();
    conn->deleteLater();

    if (m_manager)
        m_manager->setOnline(deviceId, false);

    emit stopped(deviceId);
}

void AcquisitionScheduler::stopAll()
{
    const QList<QString> ids = m_connections.keys();
    for (const QString &id : ids)
        stop(id);
}

bool AcquisitionScheduler::isRunning(const QString &deviceId) const
{
    return m_connections.contains(deviceId);
}

bool AcquisitionScheduler::writeTag(const QString &deviceId, const QString &tagId, const QVariant &value)
{
    DeviceConnection *conn = m_connections.value(deviceId, nullptr);
    if (!conn) {
        Log::warn(QStringLiteral("下发失败：设备未启动采集 %1").arg(deviceId));
        return false;
    }
    return conn->writeTag(tagId, value);
}
