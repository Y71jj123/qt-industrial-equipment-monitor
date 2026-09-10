#include "core/devicemanager.h"

#include "utils/logger.h"

#include <QUuid>

DeviceManager::DeviceManager(QObject *parent)
    : QObject(parent)
{
}

int DeviceManager::indexOf(const QString &id) const
{
    for (int i = 0; i < m_devices.size(); ++i) {
        if (m_devices.at(i).id == id)
            return i;
    }
    return -1;
}

QString DeviceManager::addDevice(const DeviceInfo &device)
{
    DeviceInfo item = device;
    if (item.id.isEmpty())
        item.id = QUuid::createUuid().toString(QUuid::WithoutBraces);

    if (indexOf(item.id) >= 0) {
        Log::warn(QStringLiteral("设备已存在，忽略新增: %1").arg(item.id));
        return item.id;
    }

    if (item.name.isEmpty())
        item.name = item.id;

    m_devices.append(item);
    Log::info(QStringLiteral("新增设备 %1 (%2:%3)").arg(item.name).arg(item.host).arg(item.port));
    emit deviceAdded(item);
    return item.id;
}

bool DeviceManager::removeDevice(const QString &id)
{
    const int idx = indexOf(id);
    if (idx < 0)
        return false;

    const DeviceInfo item = m_devices.at(idx);
    m_devices.removeAt(idx);
    Log::info(QStringLiteral("移除设备 %1").arg(item.name));
    emit deviceRemoved(id);
    return true;
}

bool DeviceManager::updateDevice(const DeviceInfo &device)
{
    const int idx = indexOf(device.id);
    if (idx < 0)
        return false;

    m_devices[idx] = device;
    emit deviceUpdated(device);
    return true;
}

QList<DeviceInfo> DeviceManager::devices() const
{
    return m_devices;
}

bool DeviceManager::contains(const QString &id) const
{
    return indexOf(id) >= 0;
}

DeviceInfo DeviceManager::device(const QString &id) const
{
    const int idx = indexOf(id);
    return idx >= 0 ? m_devices.at(idx) : DeviceInfo{};
}

void DeviceManager::setOnline(const QString &id, bool online)
{
    const int idx = indexOf(id);
    if (idx < 0)
        return;

    if (m_devices.at(idx).online == online)
        return;

    m_devices[idx].online = online;
    emit onlineChanged(id, online);
}

void DeviceManager::clear()
{
    const QList<DeviceInfo> old = m_devices;
    for (const DeviceInfo &item : old)
        emit deviceRemoved(item.id);

    m_devices.clear();
}
