#include "core/devicemanager.h"

#include "comm/protocolregistry.h"
#include "utils/logger.h"

#include <QUuid>

DeviceManager::DeviceManager(QObject *parent)
    : QObject(parent)
{
    // 默认分组必须是"天生存在"的：首次运行、空库、导入配置，都靠它兜底
    ensureGroup(defaultGroupName());
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

    // 归组要落在台账真正认得的名字上，否则树里会出现一个"影子分组"
    item.group = normalizeGroup(item.group);
    ensureGroup(item.group);

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

    DeviceInfo item = device;
    item.group = normalizeGroup(item.group);
    ensureGroup(item.group); // 编辑对话框里手敲的新分组名要就此建档

    m_devices[idx] = item;
    emit deviceUpdated(item);
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

void DeviceManager::setFault(const QString &id, bool fault)
{
    const int idx = indexOf(id);
    if (idx < 0)
        return;

    if (m_devices.at(idx).fault == fault)
        return;

    m_devices[idx].fault = fault;
    emit faultChanged(id, fault);
}

void DeviceManager::clear()
{
    const QList<DeviceInfo> old = m_devices;
    for (const DeviceInfo &item : old)
        emit deviceRemoved(item.id);

    m_devices.clear();
}

// ============================ 分组 ============================

QString DeviceManager::normalizeGroup(const QString &name)
{
    const QString trimmed = name.trimmed();
    return trimmed.isEmpty() ? defaultGroupName() : trimmed;
}

void DeviceManager::ensureGroup(const QString &name)
{
    const QString normalized = normalizeGroup(name);
    if (m_groups.contains(normalized))
        return;

    // 默认分组永远排第一：它是"收容所"，位置固定才好在树里一眼找到
    if (normalized == defaultGroupName())
        m_groups.prepend(normalized);
    else
        m_groups.append(normalized);
}

QStringList DeviceManager::groups() const
{
    QStringList result = m_groups;
    if (!result.contains(defaultGroupName()))
        result.prepend(defaultGroupName());
    return result;
}

void DeviceManager::setGroups(const QStringList &groups)
{
    m_groups.clear();
    ensureGroup(defaultGroupName());
    for (const QString &name : groups)
        ensureGroup(name);

    // 设备原本归的组如果没被保留下来，就回落到默认分组 ——
    // 宁可换个组，也不能让设备在树里无家可归。
    for (int i = 0; i < m_devices.size(); ++i) {
        if (m_groups.contains(m_devices.at(i).groupName()))
            continue;

        m_devices[i].group = defaultGroupName();
        emit deviceUpdated(m_devices.at(i));
    }

    emit groupsChanged();
}

bool DeviceManager::addGroup(const QString &name)
{
    const QString normalized = normalizeGroup(name);
    if (name.trimmed().isEmpty() || m_groups.contains(normalized))
        return false;

    ensureGroup(normalized);
    Log::info(QStringLiteral("新建设备分组 %1").arg(normalized));
    emit groupsChanged();
    return true;
}

bool DeviceManager::renameGroup(const QString &oldName, const QString &newName)
{
    const QString from = normalizeGroup(oldName);
    const QString to = normalizeGroup(newName);

    if (from == to)
        return false;
    if (from == defaultGroupName())
        return false; // 默认分组是兜底容器，改名会让"回落到默认分组"这句话失去意义
    if (newName.trimmed().isEmpty() || m_groups.contains(to))
        return false;

    const int idx = m_groups.indexOf(from);
    if (idx < 0)
        return false;

    m_groups[idx] = to;

    // 组内设备跟着改名，否则它们会变成"组不存在"的孤儿
    for (int i = 0; i < m_devices.size(); ++i) {
        if (m_devices.at(i).groupName() != from)
            continue;

        m_devices[i].group = to;
        emit deviceUpdated(m_devices.at(i));
    }

    Log::info(QStringLiteral("设备分组 %1 已重命名为 %2").arg(from, to));
    emit groupsChanged();
    return true;
}

bool DeviceManager::removeGroup(const QString &name)
{
    const QString target = normalizeGroup(name);
    if (target == defaultGroupName())
        return false;

    const int idx = m_groups.indexOf(target);
    if (idx < 0)
        return false;

    m_groups.removeAt(idx);

    // 关键：只解散分组，不动设备 —— 组里的设备回落到默认分组
    int moved = 0;
    for (int i = 0; i < m_devices.size(); ++i) {
        if (m_devices.at(i).groupName() != target)
            continue;

        m_devices[i].group = defaultGroupName();
        ++moved;
        emit deviceUpdated(m_devices.at(i));
    }

    Log::info(QStringLiteral("已删除设备分组 %1，%2 台设备回落至 %3")
                  .arg(target)
                  .arg(moved)
                  .arg(defaultGroupName()));
    emit groupsChanged();
    return true;
}

bool DeviceManager::setDeviceGroup(const QString &deviceId, const QString &group)
{
    const int idx = indexOf(deviceId);
    if (idx < 0)
        return false;

    const QString target = normalizeGroup(group);
    if (m_devices.at(idx).groupName() == target)
        return false;

    ensureGroup(target);
    m_devices[idx].group = target;
    emit deviceUpdated(m_devices.at(idx));
    return true;
}

QString defaultGroupName()
{
    return QStringLiteral("默认分组");
}

QString deviceStateName(DeviceState state)
{
    switch (state) {
    case DeviceState::Online:
        return QStringLiteral("在线");
    case DeviceState::Fault:
        return QStringLiteral("故障");
    case DeviceState::Offline:
        break;
    }
    return QStringLiteral("离线");
}

QString protocolName(const QString &protocolId)
{
    // 统一走协议注册表：内置协议与外部插件在这里没有区别，
    // 界面也不需要知道"哪种协议长什么样"。
    return ProtocolRegistry::instance().displayName(protocolId);
}

quint16 defaultPortForProtocol(const QString &protocolId)
{
    return ProtocolRegistry::instance().defaultPort(protocolId);
}

QList<TagPoint> defaultTagPoints()
{
    // 与 MockConnection 的内置数据源、以及默认告警规则保持一致
    // （温度上限 60 / 压力上限 80 / 转速上限 90 / 振动上限 70）。
    return {
        TagPoint{QStringLiteral("temperature"), QStringLiteral("温度"), QStringLiteral("℃"), 0, 3, 1.0, false},
        TagPoint{QStringLiteral("pressure"), QStringLiteral("压力"), QStringLiteral("kPa"), 1, 3, 1.0, false},
        TagPoint{QStringLiteral("speed"), QStringLiteral("转速"), QStringLiteral("r/min"), 2, 3, 1.0, false},
        TagPoint{QStringLiteral("running"), QStringLiteral("运行状态"), QString(), 3, 1, 1.0, true},
        TagPoint{QStringLiteral("vibration"), QStringLiteral("振动"), QStringLiteral("mm/s"), 4, 3, 1.0, false},
    };
}

const TagPoint *findTagPoint(const QList<TagPoint> &points, const QString &id)
{
    for (const TagPoint &point : points) {
        if (point.id == id)
            return &point;
    }
    return nullptr;
}
