#pragma once

#include "comm/deviceconnection.h"
#include "core/devicemanager.h" // TagPoint

#include <QByteArray>
#include <QList>
#include <QString>

class QTcpSocket;
class QTimer;

/// Modbus TCP 连接（现场最常用的工业协议）。
///
/// 面向"读保持寄存器 / 输入寄存器 / 线圈"这三类常见点位：
///   - open() 后按 pollIntervalMs 轮询点位表，读到值即通过 tagValueChanged 抛出，
///     与 MockConnection 的行为完全一致，因此上层调度器无需改动；
///   - writeTag() 用 06（写单寄存器）/ 05（写单线圈）下发。
///
/// 实现说明：基于 QTcpSocket 手写 MBAP + PDU 帧，用阻塞式收发（waitForXxx）完成
/// 请求-响应。这样代码短、能直接对照 Modbus 协议原文阅读。数据量很小、超时给得
/// 又短，影响可忽略；若要接入大量设备，应把本类移到独立线程或改成异步状态机。
class ModbusTcpConnection : public DeviceConnection
{
    Q_OBJECT

public:
    explicit ModbusTcpConnection(QObject *parent = nullptr);
    ~ModbusTcpConnection() override;

    void configure(const DeviceInfo &device) override;

    bool open(const QString &host, quint16 port) override;
    void close() override;
    bool isOpen() const override;

    bool readTag(const QString &tagId, QVariant &value) override;
    bool writeTag(const QString &tagId, const QVariant &value) override;

    /// 收发超时（毫秒），默认 800。
    void setTimeoutMs(int ms) { m_timeoutMs = qMax(50, ms); }

private:
    /// 发一帧请求并读回响应 PDU（已剥离 MBAP 头、已判异常码）。
    bool transact(int function, quint16 address, quint16 payload, QByteArray &pduOut);

    /// 读一个点位（按 registerType 选功能码），成功时把工程值写入 value。
    bool readPoint(const TagPoint &point, double &value);

    /// 轮询定时器回调：逐点位读取并上报。
    void poll();

    QTcpSocket *m_socket = nullptr;
    QTimer *m_pollTimer = nullptr;

    QList<TagPoint> m_points;
    QString m_host;
    quint16 m_port = 502;
    int m_slaveId = 1;
    int m_pollIntervalMs = 1000;
    int m_timeoutMs = 800;
    quint16 m_transactionId = 0;
    bool m_open = false;
};
