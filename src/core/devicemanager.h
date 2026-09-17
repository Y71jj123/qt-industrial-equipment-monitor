#pragma once

#include <QList>
#include <QMetaType>
#include <QObject>
#include <QString>

/// 设备通信协议类型。
enum class DeviceProtocol
{
    Mock,      ///< 模拟设备：内置数据发生器，无硬件也能跑通全流程
    ModbusTcp, ///< Modbus TCP：PLC / 仪表最常用的工业现场协议
    Mqtt       ///< MQTT：物联网网关常用的订阅 / 上报协议
};

/// 设备状态（三态）。
enum class DeviceState
{
    Offline = 0, ///< 离线：未连接或连接失败
    Online = 1,  ///< 在线：连接正常、数据在流动
    Fault = 2    ///< 故障：能连上但通信持续异常
};

/// 设备状态的中文名。
QString deviceStateName(DeviceState state);

/// 单个点位的定义（与具体协议无关的抽象描述）。
///
/// - Modbus：address 是寄存器地址，registerType 决定功能码，scale 做工程量换算；
/// - MQTT：id 即数据键（订阅主题的末级，或 JSON 报文字段名）；
/// - Mock：id 决定内置数据源的特性（temperature / pressure / speed / running / vibration）。
struct TagPoint
{
    QString id;              ///< 点位标识（唯一，如 temperature）
    QString name;            ///< 显示名（为空时回退到 id）
    QString unit;            ///< 工程单位（如 ℃ / kPa / r/min）
    int address = 0;         ///< Modbus 寄存器地址
    int registerType = 3;    ///< Modbus 寄存器类型：3=保持寄存器 4=输入寄存器 1=线圈
    double scale = 1.0;      ///< 原始值 × scale = 工程值
    bool boolean = false;    ///< 是否开关量（只取 0 / 1）

    QString displayName() const { return name.isEmpty() ? id : name; }
};

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
    bool fault = false;            ///< 在线但通信异常

    DeviceProtocol protocol = DeviceProtocol::Mock; ///< 通信协议类型
    QString mqttTopic;             ///< MQTT 订阅主题（protocol == Mqtt 时使用）
    QList<TagPoint> points;        ///< 点位表（为空时按 defaultTagPoints() 处理）

    /// 三态视图：离线 / 在线 / 故障。
    DeviceState state() const
    {
        if (!online)
            return DeviceState::Offline;
        return fault ? DeviceState::Fault : DeviceState::Online;
    }
};

/// 协议的中文名，用于界面展示。
QString protocolName(DeviceProtocol protocol);

/// 协议对应的默认端口。
quint16 defaultPortForProtocol(DeviceProtocol protocol);

/// 默认点位表：既是模拟设备的数据源，也作为新建真实设备时的点位模板。
QList<TagPoint> defaultTagPoints();

/// 在点位表中按 id 查找；找不到返回 nullptr。
const TagPoint *findTagPoint(const QList<TagPoint> &points, const QString &id);

/// 设备管理器：维护设备台账，是全局唯一的设备信息源。
///
/// 只管"有哪些设备、配置是什么、在线与否"，不做通信、不做界面。
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

    /// 更新故障标记（在线但通信异常）。
    void setFault(const QString &id, bool fault);

    void clear();

signals:
    void deviceAdded(const DeviceInfo &device);
    void deviceRemoved(const QString &id);
    void deviceUpdated(const DeviceInfo &device);
    void onlineChanged(const QString &id, bool online);
    void faultChanged(const QString &id, bool fault);

private:
    int indexOf(const QString &id) const;

    QList<DeviceInfo> m_devices;
};

Q_DECLARE_METATYPE(TagPoint)
Q_DECLARE_METATYPE(DeviceInfo)
