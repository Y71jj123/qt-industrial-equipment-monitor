#pragma once

#include <QList>
#include <QObject>
#include <QString>

/// 单台设备的配置信息。
struct DeviceInfo
{
    QString id;                    ///< 唯一标识（自动生成）
    QString name;                  ///< 显示名称
    QString host;                  ///< IP 或主机名
    quint16 port = 502;            ///< 协议端口（Modbus TCP 默认 502）
    int slaveId = 1;               ///< 从站地址
    int pollIntervalMs = 1000;     ///< 采集周期
    bool online = false;           ///< 当前是否在线
};

/// 设备管理器：维护设备台账，是全局唯一的设备信息源。
///
/// 只管“有哪些设备、配置是什么、在线与否”，不做通信、不做界面。
class DeviceManager : public QObject
{
    Q_OBJECT

public:
    explicit DeviceManager(QObject *parent = nullptr);

    /// 新增设备；device.id 为空时自动生成，返回最终使用的 id。
    QString addDevice(const DeviceInfo &device);

    bool removeDevice(const QString &id);
    bool updateDevice(const DeviceInfo &device);

    QList<DeviceInfo> devices() const;
    bool contains(const QString &id) const;

    /// 取设备信息；id 不存在时返回默认构造的 DeviceInfo（用 contains 先判断）。
    DeviceInfo device(const QString &id) const;

    /// 更新在线状态，状态真正变化时才发信号。
    void setOnline(const QString &id, bool online);

    void clear();

signals:
    void deviceAdded(const DeviceInfo &device);
    void deviceRemoved(const QString &id);
    void deviceUpdated(const DeviceInfo &device);
    void onlineChanged(const QString &id, bool online);

private:
    int indexOf(const QString &id) const;

    QList<DeviceInfo> m_devices;
};
