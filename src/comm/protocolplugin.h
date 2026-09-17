#pragma once

#include <QString>
#include <QtPlugin>

class DeviceConnection;

/// 协议需要哪些配置字段。
///
/// 界面据此决定表单显示什么 —— 这样新增协议**不用改界面代码**：
/// 插件声明自己需要「从站地址 / 订阅主题 / 接入账号」，设备对话框照着显示即可，
/// 而不是在界面里 `if (protocol == Mqtt)` 地硬判断协议名。
struct ProtocolTraits
{
    bool usesSlaveId = false;     ///< 需要从站地址（Modbus 这类主从协议）
    bool usesCredentials = false; ///< 需要接入账号与密码
    bool usesNetwork = true;      ///< 需要地址与端口（纯本地数据源不需要）

    /// 需要一个**字符串端点**字段。
    ///
    /// MQTT 用它当订阅主题，HTTP 插件用它当请求路径 —— 落在同一个配置字段
    /// （`DeviceInfo::mqttTopic`）里。刻意共用而不是每个协议加一列：
    /// 这类"协议自己才解释得清的字符串"再多几种，也不会让 devices 表长出一堆空列。
    bool usesEndpoint = false;

    /// 该字段在界面上的叫法。为空时用「订阅主题」。
    QString endpointLabel;
};

/// 协议插件接口。
///
/// 一个插件 = 一段元数据 + 一个连接工厂。**内置协议与外部插件实现的是同一个接口**：
/// 前者由 `registerBuiltinProtocols()` 注册进主程序，后者编成动态库放在
/// `plugins/protocols/` 下由 `QPluginLoader` 加载 —— 于是"新增一种协议"只需要
/// 丢一个动态库进去，核心代码零改动、零重编译。
///
/// 刻意写成纯虚基类（不继承 QObject）：插件实现里不需要信号槽，派生 QObject
/// 反而会把元对象系统牵进跨动态库的边界，徒增风险。真正的连接对象由
/// `create()` 返回，那个才是 QObject。
class IProtocolPlugin
{
public:
    virtual ~IProtocolPlugin() = default;

    /// 协议唯一标识，全小写下划线风格（如 `modbus_tcp`）。
    /// **存库、写配置文件用的就是它**，所以一旦发布就不要改。
    virtual QString id() const = 0;

    /// 界面上显示的名字（如 "Modbus TCP"）。
    virtual QString displayName() const = 0;

    /// 该协议的默认端口，界面切换协议时自动填。
    virtual quint16 defaultPort() const = 0;

    /// 一句话说明，给设备对话框做提示用。
    virtual QString description() const { return QString(); }

    /// 需要哪些配置字段。
    virtual ProtocolTraits traits() const { return ProtocolTraits(); }

    /// 创建连接实例。
    ///
    /// **不要给它设父对象** —— 采集调度器要把它 `moveToThread`，
    /// 有父对象的 QObject 搬不了线程。生命周期由调度器负责。
    virtual DeviceConnection *create() const = 0;
};

#define MonitorProtocolPlugin_iid "com.y71jj.monitor.ProtocolPlugin/1.0"
Q_DECLARE_INTERFACE(IProtocolPlugin, MonitorProtocolPlugin_iid)
