#pragma once

#include <QObject>
#include <QString>
#include <QVariant>

/// 设备通信的抽象接口。
///
/// 所有具体协议实现（Modbus TCP/RTU、MQTT、自定义 TCP）都继承本类，
/// 上层（采集调度器、界面）只依赖这个抽象。**新增一种协议不需要改动上层代码** ——
/// 这是本项目通信层最重要的设计约束。
class DeviceConnection : public QObject
{
    Q_OBJECT

public:
    explicit DeviceConnection(QObject *parent = nullptr)
        : QObject(parent)
    {
    }
    ~DeviceConnection() override = default;

    /// 建立连接。host 形如 "192.168.1.10"，port 为协议端口（Modbus TCP 默认 502）。
    virtual bool open(const QString &host, quint16 port) = 0;

    /// 断开连接并释放资源。
    virtual void close() = 0;

    virtual bool isOpen() const = 0;

    /// 读取一个数据点（Tag）。成功返回 true，并通过 value 写出结果。
    virtual bool readTag(const QString &tagId, QVariant &value) = 0;

    /// 写入一个数据点（Tag）。
    virtual bool writeTag(const QString &tagId, const QVariant &value) = 0;

signals:
    void opened();
    void closed();
    void errorOccurred(const QString &message);

    /// 主动上报的数据变化（订阅型协议如 MQTT 使用；轮询型协议由采集器主动取值）
    void tagValueChanged(const QString &tagId, const QVariant &value);
};
