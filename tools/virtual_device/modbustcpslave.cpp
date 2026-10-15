#include "modbustcpslave.h"

#include <QDataStream>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QVector>

#include <utility>

namespace {

/// MBAP 头固定长度：事务号(2) + 协议号(2) + 后续字节数(2) + 单元号(1)。
constexpr int kMbapHeaderSize = 7;

/// 虚拟设备的地址空间上限。范围内的未映射地址返回 0（方便块读有空洞的情况），
/// 超出范围一律回异常码 0x02 —— 这样"点位地址配错"在监控端能被明确暴露出来。
constexpr quint16 kMaxAddress = 99;

/// Modbus 各功能码对"一次读多少个"的上限（协议规定）。
constexpr int kMaxReadCoils = 2000;
constexpr int kMaxReadRegisters = 125;

/// 异常码。
enum Exception : quint8 {
    IllegalFunction = 0x01,
    IllegalDataAddress = 0x02,
    IllegalDataValue = 0x03,
};

quint8 byteAt(const QByteArray &data, int index)
{
    return static_cast<quint8>(data.at(index));
}

quint16 wordAt(const QByteArray &data, int index)
{
    return static_cast<quint16>((byteAt(data, index) << 8) | byteAt(data, index + 1));
}

} // namespace

ModbusTcpSlave::ModbusTcpSlave(QObject *parent)
    : QObject(parent)
{
}

ModbusTcpSlave::~ModbusTcpSlave()
{
    stop();
}

bool ModbusTcpSlave::start(quint16 port)
{
    stop();

    if (!m_server) {
        m_server = new QTcpServer(this);
        connect(m_server, &QTcpServer::newConnection, this, &ModbusTcpSlave::onNewConnection);
    }

    if (!m_server->listen(QHostAddress::Any, port)) {
        emit logMessage(QStringLiteral("监听失败：端口 %1 不可用（%2）")
                            .arg(port)
                            .arg(m_server->errorString()));
        return false;
    }

    m_port = port;
    emit logMessage(QStringLiteral("已开始监听 0.0.0.0:%1（从站地址 1，功能码 01/03/04/05/06）").arg(port));
    emit clientCountChanged(m_clients.size());
    return true;
}

void ModbusTcpSlave::stop()
{
    if (m_server && m_server->isListening()) {
        m_server->close();
        emit logMessage(QStringLiteral("已停止监听（端口 %1）").arg(m_port));
    }

    dropAllClients();
    m_port = 0;
}

bool ModbusTcpSlave::isListening() const
{
    return m_server && m_server->isListening();
}

void ModbusTcpSlave::dropAllClients()
{
    const QList<QTcpSocket *> clients = m_clients;
    m_clients.clear();
    m_buffers.clear();

    for (QTcpSocket *socket : clients) {
        socket->disconnect(this);
        socket->abort();
        socket->deleteLater();
    }

    if (!clients.isEmpty()) {
        emit logMessage(QStringLiteral("已掐断 %1 个连接").arg(clients.size()));
        emit clientCountChanged(0);
    }
}

void ModbusTcpSlave::onNewConnection()
{
    while (m_server && m_server->hasPendingConnections()) {
        QTcpSocket *socket = m_server->nextPendingConnection();
        m_clients.append(socket);
        m_buffers.insert(socket, QByteArray());

        connect(socket, &QTcpSocket::readyRead, this, [this, socket]() { onReadyRead(socket); });
        connect(socket, &QTcpSocket::disconnected, this, [this, socket]() { onDisconnected(socket); });

        emit logMessage(QStringLiteral("监控端已连接：%1:%2")
                            .arg(socket->peerAddress().toString())
                            .arg(socket->peerPort()));
        emit clientCountChanged(m_clients.size());
    }
}

void ModbusTcpSlave::onDisconnected(QTcpSocket *socket)
{
    if (m_clients.removeOne(socket)) {
        emit logMessage(QStringLiteral("监控端已断开：%1:%2")
                            .arg(socket->peerAddress().toString())
                            .arg(socket->peerPort()));
        emit clientCountChanged(m_clients.size());
    }
    m_buffers.remove(socket);
    socket->deleteLater();
}

void ModbusTcpSlave::onReadyRead(QTcpSocket *socket)
{
    QByteArray &buffer = m_buffers[socket];
    buffer.append(socket->readAll());

    // TCP 是字节流：可能一次读到半帧，也可能一次读到两帧，按 MBAP 的长度字段切。
    while (buffer.size() >= kMbapHeaderSize) {
        const int payload = (byteAt(buffer, 4) << 8) | byteAt(buffer, 5); // 后续字节数（含单元号）
        if (payload < 2 || payload > 260) {
            emit logMessage(QStringLiteral("收到非法 MBAP 长度字段（%1），断开该连接").arg(payload));
            socket->abort();
            return;
        }

        const int total = 6 + payload; // 前 6 字节 + 长度字段所覆盖的字节
        if (buffer.size() < total)
            break; // 帧还没收全，等下一批

        const QByteArray frame = buffer.left(total);
        buffer.remove(0, total);

        const QByteArray response = handleFrame(frame);
        if (!response.isEmpty())
            socket->write(response);
    }
}

QByteArray ModbusTcpSlave::handleFrame(const QByteArray &frame)
{
    if (frame.size() < kMbapHeaderSize + 1)
        return QByteArray();

    const quint16 transactionId = wordAt(frame, 0);
    const quint8 unitId = byteAt(frame, 6);
    const QByteArray pdu = frame.mid(kMbapHeaderSize);

    ++m_requestCount;

    bool ok = false;
    const QByteArray responsePdu = handlePdu(byteAt(pdu, 0), pdu, ok);
    return makeFrame(transactionId, unitId, responsePdu);
}

QByteArray ModbusTcpSlave::handlePdu(quint8 function, const QByteArray &pdu, bool &ok)
{
    ok = false;

    switch (function) {
    case 0x03: // 读保持寄存器
    case 0x04: { // 读输入寄存器
        if (pdu.size() < 5)
            return exceptionPdu(function, IllegalDataValue);

        const quint16 start = wordAt(pdu, 1);
        const quint16 count = wordAt(pdu, 3);
        if (count == 0 || count > kMaxReadRegisters)
            return exceptionPdu(function, IllegalDataValue);
        if (static_cast<int>(start) + count - 1 > kMaxAddress)
            return exceptionPdu(function, IllegalDataAddress);

        // 虚拟设备的输入寄存器与保持寄存器共用同一份数据（演示够用）
        const int registerType = (function == 0x04) ? 4 : 3;

        QByteArray body;
        for (quint16 i = 0; i < count; ++i) {
            const quint16 address = static_cast<quint16>(start + i);
            const int index = m_gen.indexOf(address, registerType);
            const quint16 value = (index >= 0) ? m_gen.rawValueAt(index) : 0;
            body.append(static_cast<char>((value >> 8) & 0xFF));
            body.append(static_cast<char>(value & 0xFF));
        }

        QByteArray resp;
        resp.append(static_cast<char>(function));
        resp.append(static_cast<char>(body.size()));
        resp.append(body);
        ok = true;
        return resp;
    }

    case 0x01: { // 读线圈
        if (pdu.size() < 5)
            return exceptionPdu(function, IllegalDataValue);

        const quint16 start = wordAt(pdu, 1);
        const quint16 count = wordAt(pdu, 3);
        if (count == 0 || count > kMaxReadCoils)
            return exceptionPdu(function, IllegalDataValue);
        if (static_cast<int>(start) + count - 1 > kMaxAddress)
            return exceptionPdu(function, IllegalDataAddress);

        const int byteCount = (count + 7) / 8;
        QVector<quint8> bits(byteCount, 0);
        for (quint16 i = 0; i < count; ++i) {
            const quint16 address = static_cast<quint16>(start + i);
            const int index = m_gen.indexOf(address, 1);
            const bool on = (index >= 0) && (m_gen.rawValueAt(index) != 0);
            if (on)
                bits[i / 8] = static_cast<quint8>(bits.at(i / 8) | (1 << (i % 8)));
        }

        QByteArray resp;
        resp.append(static_cast<char>(function));
        resp.append(static_cast<char>(byteCount));
        for (quint8 b : std::as_const(bits))
            resp.append(static_cast<char>(b));
        ok = true;
        return resp;
    }

    case 0x06: { // 写单寄存器
        if (pdu.size() < 5)
            return exceptionPdu(function, IllegalDataValue);

        const quint16 address = wordAt(pdu, 1);
        const quint16 value = wordAt(pdu, 3);
        if (address > kMaxAddress)
            return exceptionPdu(function, IllegalDataAddress);

        // 下位机被写入后保持该值（否则下一周期就被自动波动覆盖，演示看不到效果）
        m_gen.setOverride(address, value);
        emit logMessage(QStringLiteral("写入保持寄存器 %1 = %2（转为手动保持）").arg(address).arg(value));

        ok = true;
        return pdu; // 写操作的正常响应就是请求原样回显
    }

    case 0x05: { // 写单线圈
        if (pdu.size() < 5)
            return exceptionPdu(function, IllegalDataValue);

        const quint16 address = wordAt(pdu, 1);
        const quint16 value = wordAt(pdu, 3);
        if (address > kMaxAddress)
            return exceptionPdu(function, IllegalDataAddress);
        if (value != 0xFF00 && value != 0x0000)
            return exceptionPdu(function, IllegalDataValue);

        m_gen.setOverride(address, value == 0xFF00 ? 1 : 0);
        emit logMessage(QStringLiteral("写入线圈 %1 = %2（转为手动保持）")
                            .arg(address)
                            .arg(value == 0xFF00 ? 1 : 0));

        ok = true;
        return pdu;
    }

    default:
        return exceptionPdu(function, IllegalFunction);
    }
}

QByteArray ModbusTcpSlave::exceptionPdu(quint8 function, quint8 code)
{
    QByteArray pdu;
    pdu.append(static_cast<char>(function | 0x80));
    pdu.append(static_cast<char>(code));
    return pdu;
}

QByteArray ModbusTcpSlave::makeFrame(quint16 transactionId, quint8 unitId, const QByteArray &pdu)
{
    QByteArray frame;
    QDataStream out(&frame, QIODevice::WriteOnly);
    out.setByteOrder(QDataStream::BigEndian);
    out << transactionId                 // 事务号（原样回填）
        << quint16(0)                    // 协议号
        << quint16(1 + pdu.size())       // 后续字节数 = 单元号(1) + PDU
        << quint8(unitId);               // 单元号（原样回填）
    frame.append(pdu);
    return frame;
}
