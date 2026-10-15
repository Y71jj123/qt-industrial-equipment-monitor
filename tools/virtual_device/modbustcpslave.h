#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>

#include "signalgenerator.h"

class QTcpServer;
class QTcpSocket;
class QTimer;

/// Modbus TCP 从站（服务端）。
///
/// 只实现监控端会用到的功能码，够用来做端到端验证：
///   0x01 读线圈          0x03 读保持寄存器      0x04 读输入寄存器
///   0x05 写单线圈        0x06 写单寄存器
///
/// 帧格式与主程序 `ModbusTcpConnection::transactPdu()` 对齐：
///   MBAP 头 = 事务号(2) + 协议号(2, 固定 0) + 后续字节数(2) + 单元号(1)
///   后续字节数 = 1（单元号）+ PDU 长度
/// 因此响应的长度字段 = 1 + PDU.size()，事务号原样回填，单元号原样回填。
///
/// 异常处理：不支持的地址回异常码 0x02（非法数据地址），
/// 不支持的功能码回 0x01（非法功能），数量越界回 0x03（非法数据值）。
/// 单元号不做过滤（任何 unitId 都响应），避免演示时因为从站号填错而"连上了却读不到值"。
class ModbusTcpSlave : public QObject
{
    Q_OBJECT

public:
    explicit ModbusTcpSlave(QObject *parent = nullptr);
    ~ModbusTcpSlave() override;

    /// 开始监听。失败返回 false（端口被占用等），原因写进日志。
    bool start(quint16 port);
    void stop();
    bool isListening() const;

    quint16 port() const { return m_port; }
    int clientCount() const { return m_clients.size(); }
    quint64 requestCount() const { return m_requestCount; }

    /// 数据发生器（界面定时推进它，从站读值时用它的当前值）。
    SignalGenerator &generator() { return m_gen; }

    /// 掐断所有已建立的连接（用于演示监控端的"断线自动重连"）。
    void dropAllClients();

signals:
    void logMessage(const QString &text);
    void clientCountChanged(int count);

private:
    void onNewConnection();
    void onReadyRead(QTcpSocket *socket);
    void onDisconnected(QTcpSocket *socket);

    /// 处理一个完整请求帧，返回响应帧（已含 MBAP 头）。
    QByteArray handleFrame(const QByteArray &frame);

    /// 处理 PDU，成功时 ok = true 且返回正常响应 PDU；失败返回异常响应 PDU。
    QByteArray handlePdu(quint8 function, const QByteArray &pdu, bool &ok);

    /// 构造异常响应 PDU：功能码 | 0x80，后跟异常码。
    static QByteArray exceptionPdu(quint8 function, quint8 code);

    /// 组一个完整响应帧。
    static QByteArray makeFrame(quint16 transactionId, quint8 unitId, const QByteArray &pdu);

    QTcpServer *m_server = nullptr;
    QList<QTcpSocket *> m_clients;
    QHash<QTcpSocket *, QByteArray> m_buffers; ///< 每连接的收包缓冲（TCP 会粘包/分片）

    SignalGenerator m_gen;
    quint16 m_port = 0;
    quint64 m_requestCount = 0;
};
