#include "comm/protocolregistry.h"

#include "comm/mockconnection.h"
#include "comm/modbusconnection.h"
#include "comm/mqttconnection.h"

namespace {

/// 模拟设备：内置数据发生器，不需要任何网络配置。
class MockProtocolPlugin : public IProtocolPlugin
{
public:
    QString id() const override { return QStringLiteral("mock"); }
    QString displayName() const override { return QStringLiteral("模拟设备"); }
    quint16 defaultPort() const override { return 0; } // 不走网络，没有端口
    QString description() const override
    {
        return QStringLiteral("内置数据发生器：无需硬件与网络即可跑通采集 / 告警 / 落库全流程，"
                              "适合演示与自测。");
    }
    ProtocolTraits traits() const override
    {
        ProtocolTraits traits;
        traits.usesNetwork = false; // 连地址端口都不需要
        return traits;
    }
    DeviceConnection *create() const override { return new MockConnection(); }
};

/// Modbus TCP：PLC / 仪表最常用的工业现场协议。
class ModbusTcpProtocolPlugin : public IProtocolPlugin
{
public:
    QString id() const override { return QStringLiteral("modbus_tcp"); }
    QString displayName() const override { return QStringLiteral("Modbus TCP"); }
    quint16 defaultPort() const override { return 502; }
    QString description() const override
    {
        return QStringLiteral("Modbus TCP（功能码 01/03/04/05/06）：按点位表逐点轮询，"
                              "寄存器地址与类型在下方点位表里配。");
    }
    ProtocolTraits traits() const override
    {
        ProtocolTraits traits;
        traits.usesSlaveId = true;
        return traits;
    }
    DeviceConnection *create() const override { return new ModbusTcpConnection(); }
};

/// MQTT：物联网网关常用的订阅 / 上报协议。
class MqttProtocolPlugin : public IProtocolPlugin
{
public:
    QString id() const override { return QStringLiteral("mqtt"); }
    QString displayName() const override { return QStringLiteral("MQTT"); }
    quint16 defaultPort() const override { return 1883; }
    QString description() const override
    {
        return QStringLiteral("MQTT 3.1.1 客户端：连接后订阅主题，按上报格式解析为点位值。"
                              "支持匿名接入或填写接入账号。");
    }
    ProtocolTraits traits() const override
    {
        ProtocolTraits traits;
        traits.usesEndpoint = true;
        traits.endpointLabel = QStringLiteral("订阅主题");
        traits.usesCredentials = true;
        return traits;
    }
    DeviceConnection *create() const override { return new MqttConnection(); }
};

} // namespace

void registerBuiltinProtocols()
{
    ProtocolRegistry &registry = ProtocolRegistry::instance();
    registry.registerPlugin(new MockProtocolPlugin);
    registry.registerPlugin(new ModbusTcpProtocolPlugin);
    registry.registerPlugin(new MqttProtocolPlugin);
}
