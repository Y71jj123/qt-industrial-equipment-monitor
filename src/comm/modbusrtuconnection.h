#pragma once

#include "comm/deviceconnection.h"
#include "core/devicemanager.h"

// 串口类是 Qt6SerialPort 模块提供的；该模块不一定装在所有构建环境里
// （例如本机 Windows 开发环境就没装）。用 HAVE_QT_SERIALPORT 宏把整段代码隔离，
// 没装时这里就是空的 —— 主程序照常编译，只是不会出现 Modbus RTU 这个选项。
// CMake 一侧只在 Qt6SerialPort_FOUND 时才把本类的 .cpp 编进工程。
#if defined(HAVE_QT_SERIALPORT)

#include <QByteArray>
#include <QList>
#include <QString>
#include <QTimer>

class QSerialPort;

/// Modbus RTU 连接（串口传输层）。
///
/// 与 ModbusTcpConnection 共享同一套 PDU / 块读 / 解析逻辑（modbus::runPoll），
/// 区别只在"怎么把 PDU 发到线上"：RTU 走串口，帧 = [从站地址][PDU][CRC16]，
/// 用 3.5 个字符时间的静默间隔来分隔报文；而 TCP 用的是 MBAP 头。
///
/// 串口参数（波特率 / 数据位 / 校验 / 停止位）由 configure() 从 DeviceInfo 取，
/// 所以设备对话框里配的串口参数能直接生效，不用改代码 —— 这也是工业现场最常见的
/// "9600 / 8 / N / 1" 之外的其他接线方式（比如 19200 / 奇校验）需要支持的原因。
class ModbusRtuConnection : public DeviceConnection
{
    Q_OBJECT

public:
    explicit ModbusRtuConnection(QObject *parent = nullptr);
    ~ModbusRtuConnection() override;

    void configure(const DeviceInfo &device) override;

    /// host 在这里是"串口设备名"（如 Linux 的 /dev/ttyS0、Windows 的 COM3）。
    /// port 参数对串口无意义，忽略。
    bool open(const QString &host, quint16 /*port*/) override;
    void close() override;
    bool isOpen() const override;

    bool readTag(const QString &tagId, QVariant &value) override;
    bool writeTag(const QString &tagId, const QVariant &value) override;

    /// 收发超时（毫秒），默认 800。
    void setTimeoutMs(int ms) { m_timeoutMs = qMax(50, ms); }

private:
    /// 发一个 PDU（不含从站地址与 CRC）并读回响应 PDU（已剥离从站地址与 CRC、已判异常码）。
    bool transactPdu(quint8 function, const QByteArray &requestPdu, QByteArray &responsePduOut);

    /// 轮询定时器回调：复用 modbus::runPoll 的块读逻辑。
    void poll();

    QSerialPort *m_port = nullptr;
    QTimer *m_pollTimer = nullptr;

    QList<TagPoint> m_points;
    QString m_portName;
    int m_slaveId = 1;
    int m_pollIntervalMs = 1000;
    int m_timeoutMs = 800;

    // 串口参数（来自 DeviceInfo，默认 9600 / 8 / 无校验 / 1 停止位）
    int m_baudRate = 9600;
    int m_dataBits = 8;
    int m_parity = 0;   ///< 0=无 1=奇 2=偶
    int m_stopBits = 1; ///< 1 或 2

    bool m_open = false;
};

#endif // HAVE_QT_SERIALPORT
