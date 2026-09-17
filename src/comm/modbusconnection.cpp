#include "comm/modbusconnection.h"

#include "utils/logger.h"

#include <QAbstractSocket>
#include <QDataStream>
#include <QTcpSocket>
#include <QTimer>

#include <utility>

namespace {

/// 功能码：读线圈 / 读保持寄存器 / 读输入寄存器 / 写单线圈 / 写单寄存器
enum ModbusFunction {
    ReadCoils = 0x01,
    ReadHoldingRegisters = 0x03,
    ReadInputRegisters = 0x04,
    WriteSingleCoil = 0x05,
    WriteSingleRegister = 0x06,
};

/// MBAP 头长度：事务号(2) + 协议号(2) + 长度(2) + 单元号(1)
constexpr int kMbapHeaderSize = 7;

} // namespace

ModbusTcpConnection::ModbusTcpConnection(QObject *parent)
    : DeviceConnection(parent)
{
}

ModbusTcpConnection::~ModbusTcpConnection()
{
    close();
}

void ModbusTcpConnection::configure(const DeviceInfo &device)
{
    m_points = device.points.isEmpty() ? defaultTagPoints() : device.points;
    m_slaveId = qMax(1, device.slaveId);
    m_pollIntervalMs = qBound(100, device.pollIntervalMs, 60000);
}

bool ModbusTcpConnection::open(const QString &host, quint16 port)
{
    if (m_open)
        close();

    m_host = host;
    m_port = port;

    if (!m_socket) {
        m_socket = new QTcpSocket(this);
        connect(m_socket, &QTcpSocket::disconnected, this, [this]() {
            if (m_open) {
                m_open = false;
                if (m_pollTimer)
                    m_pollTimer->stop();
                emit errorOccurred(QStringLiteral("Modbus 连接已断开: %1:%2")
                                       .arg(m_host)
                                       .arg(m_port));
                emit closed();
            }
        });
    }

    m_socket->connectToHost(host, port);
    if (!m_socket->waitForConnected(m_timeoutMs)) {
        emit errorOccurred(QStringLiteral("Modbus 连接失败 %1:%2（%3）")
                               .arg(host)
                               .arg(port)
                               .arg(m_socket->errorString()));
        return false;
    }

    m_open = true;
    Log::info(QStringLiteral("Modbus TCP 已连接 %1:%2（从站 %3，%4 个点位）")
                  .arg(host)
                  .arg(port)
                  .arg(m_slaveId)
                  .arg(m_points.size()));
    emit opened();

    if (!m_pollTimer) {
        m_pollTimer = new QTimer(this);
        connect(m_pollTimer, &QTimer::timeout, this, &ModbusTcpConnection::poll);
    }
    m_pollTimer->start(m_pollIntervalMs);
    return true;
}

void ModbusTcpConnection::close()
{
    if (m_pollTimer)
        m_pollTimer->stop();

    const bool wasOpen = m_open;
    m_open = false; // 先置位，避免 disconnected 回调重复上报

    if (m_socket && m_socket->state() != QAbstractSocket::UnconnectedState) {
        m_socket->disconnectFromHost();
        if (m_socket->state() != QAbstractSocket::UnconnectedState)
            m_socket->waitForDisconnected(200);
    }

    if (wasOpen) {
        Log::info(QStringLiteral("Modbus TCP 已断开 %1:%2").arg(m_host).arg(m_port));
        emit closed();
    }
}

bool ModbusTcpConnection::isOpen() const
{
    return m_open;
}

bool ModbusTcpConnection::transact(int function, quint16 address, quint16 payload, QByteArray &pduOut)
{
    if (!m_socket || m_socket->state() != QAbstractSocket::ConnectedState)
        return false;

    m_socket->readAll(); // 丢弃上一次的残留，保证请求-响应严格配对

    const quint16 transactionId = ++m_transactionId;

    // ---- 请求帧：MBAP(7) + PDU(5) ----
    QByteArray request;
    QDataStream out(&request, QIODevice::WriteOnly);
    out.setByteOrder(QDataStream::BigEndian);
    out << transactionId          // 事务号
        << quint16(0)             // 协议号（Modbus 固定 0）
        << quint16(1 + 5)         // 后续字节数 = 单元号(1) + PDU(5)
        << quint8(m_slaveId)      // 单元号（从站地址）
        << quint8(function)       // 功能码
        << address                // 起始地址
        << payload;               // 寄存器数量 / 写入值

    if (m_socket->write(request) != request.size())
        return false;
    if (!m_socket->waitForBytesWritten(m_timeoutMs))
        return false;

    // ---- 读 MBAP 头 ----
    while (m_socket->bytesAvailable() < kMbapHeaderSize) {
        if (!m_socket->waitForReadyRead(m_timeoutMs))
            return false;
    }
    const QByteArray header = m_socket->read(kMbapHeaderSize);
    const quint8 *hp = reinterpret_cast<const quint8 *>(header.constData());
    const int length = (int(hp[4]) << 8) | int(hp[5]);
    const int pduLength = length - 1; // 减掉单元号
    if (pduLength <= 0)
        return false;

    // ---- 读 PDU ----
    while (m_socket->bytesAvailable() < pduLength) {
        if (!m_socket->waitForReadyRead(m_timeoutMs))
            return false;
    }
    pduOut = m_socket->read(pduLength);

    const quint8 fn = quint8(pduOut.at(0));
    if (fn & 0x80) { // 最高位置 1 = 异常响应
        const quint8 code = pduOut.size() > 1 ? quint8(pduOut.at(1)) : 0;
        emit errorOccurred(QStringLiteral("Modbus 异常响应（功能码 %1，异常码 %2）")
                               .arg(function)
                               .arg(code));
        return false;
    }
    return fn == quint8(function);
}

bool ModbusTcpConnection::readPoint(const TagPoint &point, double &value)
{
    QByteArray pdu;
    quint16 raw = 0;

    if (point.registerType == 1) {
        // 线圈：功能码 01，读 1 位
        if (!transact(ReadCoils, quint16(point.address), 1, pdu))
            return false;
        if (pdu.size() < 3)
            return false;
        raw = (quint8(pdu.at(2)) & 0x01) ? 1 : 0;
    } else {
        const int function = (point.registerType == 4) ? ReadInputRegisters : ReadHoldingRegisters;
        if (!transact(function, quint16(point.address), 1, pdu))
            return false;
        if (pdu.size() < 4)
            return false;
        raw = quint16((quint8(pdu.at(2)) << 8) | quint8(pdu.at(3)));
    }

    // 开关量（线圈 / 离散输入）本身就是 0 / 1，不做工程量换算；
    // 只有模拟量才乘 scale（例如寄存器存"工程值×100"时 scale = 0.01）。
    value = point.boolean ? double(raw) : double(raw) * point.scale;
    return true;
}

void ModbusTcpConnection::poll()
{
    if (!m_open)
        return;

    for (const TagPoint &point : std::as_const(m_points)) {
        double value = 0.0;
        if (readPoint(point, value))
            emit tagValueChanged(point.id, value);
    }
}

bool ModbusTcpConnection::readTag(const QString &tagId, QVariant &value)
{
    if (!m_open) {
        emit errorOccurred(QStringLiteral("Modbus 读取失败：连接未建立"));
        return false;
    }

    const TagPoint *point = findTagPoint(m_points, tagId);
    if (!point) {
        emit errorOccurred(QStringLiteral("Modbus 读取失败：点位未配置 %1").arg(tagId));
        return false;
    }

    double v = 0.0;
    if (!readPoint(*point, v))
        return false;

    value = v;
    return true;
}

bool ModbusTcpConnection::writeTag(const QString &tagId, const QVariant &value)
{
    if (!m_open) {
        emit errorOccurred(QStringLiteral("Modbus 写入失败：连接未建立"));
        return false;
    }

    const TagPoint *point = findTagPoint(m_points, tagId);
    if (!point) {
        emit errorOccurred(QStringLiteral("Modbus 写入失败：点位未配置 %1").arg(tagId));
        return false;
    }

    const double scale = (point->scale == 0.0) ? 1.0 : point->scale;
    const quint16 raw = quint16(qRound(value.toDouble() / scale));

    QByteArray pdu;
    if (point->registerType == 1) {
        if (!transact(WriteSingleCoil, quint16(point->address), raw ? 0xFF00 : 0x0000, pdu))
            return false;
    } else {
        if (!transact(WriteSingleRegister, quint16(point->address), raw, pdu))
            return false;
    }

    Log::info(QStringLiteral("Modbus 下发 %1 = %2").arg(tagId, value.toString()));
    emit tagValueChanged(tagId, value);
    return true;
}
