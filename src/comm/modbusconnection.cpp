#include "comm/modbusconnection.h"
#include "comm/modbuscommon.h"

#include "utils/logger.h"

#include <QAbstractSocket>
#include <QDataStream>
#include <QTcpSocket>
#include <QTimer>

namespace {

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

bool ModbusTcpConnection::transactPdu(quint8 function, const QByteArray &requestPdu, QByteArray &responsePduOut)
{
    if (!m_socket || m_socket->state() != QAbstractSocket::ConnectedState)
        return false;

    m_socket->readAll(); // 丢弃上一次的残留，保证请求-响应严格配对

    const quint16 transactionId = ++m_transactionId;

    // ---- 请求帧：MBAP(7) + PDU(变长) ----
    QByteArray request;
    QDataStream out(&request, QIODevice::WriteOnly);
    out.setByteOrder(QDataStream::BigEndian);
    out << transactionId                                // 事务号
        << quint16(0)                                  // 协议号（Modbus 固定 0）
        << quint16(1 + requestPdu.size())              // 后续字节数 = 单元号(1) + PDU
        << quint8(m_slaveId);                          // 单元号（从站地址）
    request.append(requestPdu);                        // PDU 原样追加（不要长度前缀）

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
    responsePduOut = m_socket->read(pduLength);

    const quint8 fn = quint8(responsePduOut.at(0));
    if (fn & 0x80) { // 最高位置 1 = 异常响应
        const quint8 code = responsePduOut.size() > 1 ? quint8(responsePduOut.at(1)) : 0;
        emit errorOccurred(QStringLiteral("Modbus 异常响应（功能码 %1，异常码 %2）")
                               .arg(function)
                               .arg(code));
        return false;
    }
    return fn == function;
}

void ModbusTcpConnection::poll()
{
    if (!m_open)
        return;

    // 块读：按寄存器类型分组、合并连续地址成块、每块一次读。TCP 与 RTU 共用同一份逻辑。
    modbus::runPoll(
        m_points,
        [this](quint8 fn, const QByteArray &req, QByteArray &resp) {
            return transactPdu(fn, req, resp);
        },
        [this](const QString &id, double v) {
            emit tagValueChanged(id, v);
        });
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

    const int rt = point->registerType;
    const quint8 function = (rt == 4) ? modbus::ReadInputRegisters
                                       : (rt == 1) ? modbus::ReadCoils
                                                   : modbus::ReadHoldingRegisters;
    const QByteArray req = modbus::buildReadPdu(function, quint16(point->address), 1);
    QByteArray resp;
    if (!transactPdu(function, req, resp))
        return false;

    double v = 0.0;
    if (rt == 1) {
        const QVector<bool> bits = modbus::parseReadCoils(resp, 1);
        if (bits.isEmpty())
            return false;
        v = bits.at(0) ? 1.0 : 0.0;
    } else {
        const QVector<quint16> regs = modbus::parseReadRegisters(resp);
        if (regs.isEmpty())
            return false;
        v = point->boolean ? double(regs.at(0)) : double(regs.at(0)) * point->scale;
    }

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

    const quint8 function = (point->registerType == 1) ? modbus::WriteSingleCoil : modbus::WriteSingleRegister;
    const quint16 address = quint16(point->address);
    const quint16 payload = (point->registerType == 1) ? (raw ? 0xFF00 : 0x0000) : raw;
    const QByteArray req = modbus::buildWriteSinglePdu(function, address, payload);
    QByteArray resp;
    if (!transactPdu(function, req, resp))
        return false;

    Log::info(QStringLiteral("Modbus 下发 %1 = %2").arg(tagId, value.toString()));
    emit tagValueChanged(tagId, value);
    return true;
}
