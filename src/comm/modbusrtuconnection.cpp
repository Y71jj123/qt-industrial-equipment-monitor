#include "comm/modbusrtuconnection.h"
#include "comm/modbuscommon.h"

#include "utils/logger.h"

// 见头文件的说明：没装 Qt6SerialPort 时整个文件不进入编译。
#if defined(HAVE_QT_SERIALPORT)

#include <QElapsedTimer>
#include <QSerialPort>

namespace {

QSerialPort::Parity toParity(int p)
{
    switch (p) {
    case 1: return QSerialPort::OddParity;
    case 2: return QSerialPort::EvenParity;
    default: return QSerialPort::NoParity;
    }
}

QSerialPort::StopBits toStopBits(int s)
{
    return (s == 2) ? QSerialPort::TwoStop : QSerialPort::OneStop;
}

QSerialPort::DataBits toDataBits(int d)
{
    switch (d) {
    case 5: return QSerialPort::Data5;
    case 6: return QSerialPort::Data6;
    case 7: return QSerialPort::Data7;
    default: return QSerialPort::Data8;
    }
}

QString parityChar(int p)
{
    return (p == 1) ? QStringLiteral("O") : (p == 2) ? QStringLiteral("E") : QStringLiteral("N");
}

} // namespace

ModbusRtuConnection::ModbusRtuConnection(QObject *parent)
    : DeviceConnection(parent)
{
}

ModbusRtuConnection::~ModbusRtuConnection()
{
    close();
}

void ModbusRtuConnection::configure(const DeviceInfo &device)
{
    m_points = device.points.isEmpty() ? defaultTagPoints() : device.points;
    m_slaveId = qMax(1, device.slaveId);
    m_pollIntervalMs = qBound(100, device.pollIntervalMs, 60000);
    // 串口参数直接来自设备配置；缺省值保证"什么都不配也能用 9600/8/N/1"连上大部分仪表。
    m_baudRate = device.baudRate > 0 ? device.baudRate : 9600;
    m_dataBits = device.dataBits;
    m_parity = device.parity;
    m_stopBits = device.stopBits;
}

bool ModbusRtuConnection::open(const QString &host, quint16 /*port*/)
{
    if (m_open)
        close();

    m_portName = host;

    if (!m_port) {
        m_port = new QSerialPort(this);
        // 串口热插拔 / 权限变化都会触发这个信号；这里只在"开着的时候"才视为掉线，
        // 避免 close() 过程中自己触发的错误被当成意外断开。
        connect(m_port, &QSerialPort::errorOccurred, this,
                [this](QSerialPort::SerialPortError err) {
                    if (err != QSerialPort::NoError && m_open) {
                        m_open = false;
                        if (m_pollTimer)
                            m_pollTimer->stop();
                        emit errorOccurred(
                            QStringLiteral("Modbus RTU 串口错误: %1").arg(m_port->errorString()));
                        emit closed();
                    }
                });
    }

    m_port->setPortName(host);
    m_port->setBaudRate(m_baudRate);
    m_port->setDataBits(toDataBits(m_dataBits));
    m_port->setParity(toParity(m_parity));
    m_port->setStopBits(toStopBits(m_stopBits));
    m_port->setFlowControl(QSerialPort::NoFlowControl);

    if (!m_port->open(QIODevice::ReadWrite)) {
        emit errorOccurred(QStringLiteral("Modbus RTU 打开串口失败 %1（%2）")
                               .arg(host, m_port->errorString()));
        return false;
    }

    m_open = true;
    Log::info(QStringLiteral("Modbus RTU 已连接 %1（从站 %2，%3 个点位，%4 %5%6%7）")
                  .arg(host)
                  .arg(m_slaveId)
                  .arg(m_points.size())
                  .arg(m_baudRate)
                  .arg(m_dataBits)
                  .arg(parityChar(m_parity))
                  .arg(m_stopBits));
    emit opened();

    if (!m_pollTimer) {
        m_pollTimer = new QTimer(this);
        connect(m_pollTimer, &QTimer::timeout, this, &ModbusRtuConnection::poll);
    }
    m_pollTimer->start(m_pollIntervalMs);
    return true;
}

void ModbusRtuConnection::close()
{
    if (m_pollTimer)
        m_pollTimer->stop();

    const bool wasOpen = m_open;
    m_open = false; // 先置位，避免 errorOccurred 回调重复上报

    if (m_port && m_port->isOpen())
        m_port->close();

    if (wasOpen) {
        Log::info(QStringLiteral("Modbus RTU 已断开 %1").arg(m_portName));
        emit closed();
    }
}

bool ModbusRtuConnection::isOpen() const
{
    return m_open;
}

bool ModbusRtuConnection::transactPdu(quint8 function, const QByteArray &requestPdu, QByteArray &responsePduOut)
{
    if (!m_port || !m_port->isOpen())
        return false;

    m_port->clear(QSerialPort::Input); // 丢弃残留，严格配对

    // 帧 = [从站地址][PDU][CRC16(小端)]
    QByteArray frame;
    frame.append(static_cast<char>(m_slaveId));
    frame.append(requestPdu);
    frame = modbus::appendCrc(frame);

    m_port->write(frame);
    if (!m_port->waitForBytesWritten(m_timeoutMs))
        return false;

    // RTU 没有长度字段，但**响应自身带了长度**：先收前 3 字节
    // （从站 + 功能码 + 字节数/异常码），再据此把整帧收完。
    //
    // 不能按"请求"反推长度：读线圈的应答是 ceil(count/8) 字节而不是 2*count，
    // 异常应答更是只有 5 字节 —— 按错的长度一直等，表现就是明明应答到了却超时。
    QByteArray buf;
    QElapsedTimer timer;
    timer.start();

    const auto readExact = [&](int n) {
        while (buf.size() < n) {
            if (!m_port->waitForReadyRead(m_timeoutMs))
                return false;
            buf.append(m_port->readAll());
            // 防止收到垃圾数据后一直凑不够长度却永不超时
            if (timer.elapsed() > m_timeoutMs * 2)
                return false;
        }
        return true;
    };

    if (!readExact(3))
        return false;

    const quint8 respFn = static_cast<quint8>(buf.at(1));
    int frameLen = 8; // 写单线圈/单寄存器：8 字节（从站+fn+地址2+值2+CRC2）
    if (respFn & 0x80) {
        frameLen = 5; // 异常应答：从站 + 功能码|0x80 + 异常码 + CRC2
    } else if (respFn == modbus::ReadCoils || respFn == modbus::ReadHoldingRegisters
               || respFn == modbus::ReadInputRegisters) {
        frameLen = 3 + static_cast<quint8>(buf.at(2)) + 2;
    }
    if (frameLen < 5 || frameLen > 260)
        return false;
    if (!readExact(frameLen))
        return false;
    if (buf.size() > frameLen)
        buf.truncate(frameLen); // 多收的（下一帧/噪声）丢掉，别把 CRC 算错

    // 校验 CRC：收到的最后一字节是高 8 位、倒数第二是低 8 位
    if (buf.size() < 4)
        return false;
    const quint16 recvCrc = static_cast<quint16>(
        (static_cast<quint8>(buf.at(buf.size() - 1)) << 8)
        | static_cast<quint8>(buf.at(buf.size() - 2)));
    const quint16 calcCrc = modbus::crc16(buf.left(buf.size() - 2));
    if (recvCrc != calcCrc) {
        emit errorOccurred(QStringLiteral("Modbus RTU CRC 校验失败"));
        return false;
    }

    // 剥掉从站地址与 CRC，剩下 PDU 交给共享的解析函数
    responsePduOut = buf.mid(1, buf.size() - 3);

    const quint8 rfn = responsePduOut.size() ? static_cast<quint8>(responsePduOut.at(0)) : 0;
    if (rfn & 0x80) {
        const quint8 code = responsePduOut.size() > 1 ? static_cast<quint8>(responsePduOut.at(1)) : 0;
        emit errorOccurred(QStringLiteral("Modbus RTU 异常响应（功能码 %1，异常码 %2）")
                               .arg(function)
                               .arg(code));
        return false;
    }
    return rfn == function;
}

void ModbusRtuConnection::poll()
{
    if (!m_open)
        return;

    modbus::runPoll(
        m_points,
        [this](quint8 fn, const QByteArray &req, QByteArray &resp) {
            return transactPdu(fn, req, resp);
        },
        [this](const QString &id, double v) {
            emit tagValueChanged(id, v);
        });
}

bool ModbusRtuConnection::readTag(const QString &tagId, QVariant &value)
{
    if (!m_open) {
        emit errorOccurred(QStringLiteral("Modbus RTU 读取失败：连接未建立"));
        return false;
    }

    const TagPoint *point = findTagPoint(m_points, tagId);
    if (!point) {
        emit errorOccurred(QStringLiteral("Modbus RTU 读取失败：点位未配置 %1").arg(tagId));
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

bool ModbusRtuConnection::writeTag(const QString &tagId, const QVariant &value)
{
    if (!m_open) {
        emit errorOccurred(QStringLiteral("Modbus RTU 写入失败：连接未建立"));
        return false;
    }

    const TagPoint *point = findTagPoint(m_points, tagId);
    if (!point) {
        emit errorOccurred(QStringLiteral("Modbus RTU 写入失败：点位未配置 %1").arg(tagId));
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

    Log::info(QStringLiteral("Modbus RTU 下发 %1 = %2").arg(tagId, value.toString()));
    emit tagValueChanged(tagId, value);
    return true;
}

#endif // QT_CONFIG(serialport)
