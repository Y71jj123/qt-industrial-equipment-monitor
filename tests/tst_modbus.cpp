#include <QTest>

#include "comm/modbuscommon.h"

#include <QList>
#include <QMap>

namespace {

/// 造一个"按请求回数据"的传输层：请求里 start/count，响应里寄存器值 = 起始地址 + 偏移。
/// 这样跑完 runPoll 后，每个点位应能收到自己地址对应的数值，正好用来断言块读的正确性。
modbus::ModbusTransport makeEchoTransport(int &callCount, QVector<quint8> &functions)
{
    return [&](quint8 fn, const QByteArray &req, QByteArray &resp) -> bool {
        ++callCount;
        functions.append(fn);

        const int start = (static_cast<quint8>(req.at(1)) << 8) | static_cast<quint8>(req.at(2));
        const int count = (static_cast<quint8>(req.at(3)) << 8) | static_cast<quint8>(req.at(4));

        QByteArray out;
        out.append(static_cast<char>(fn));
        if (fn == modbus::ReadCoils) {
            const int bytes = (count + 7) / 8;
            out.append(static_cast<char>(bytes));
            for (int i = 0; i < bytes; ++i)
                out.append(static_cast<char>(0)); // 全 0：线圈都断开
        } else {
            out.append(static_cast<char>(count * 2)); // 字节数
            for (int i = 0; i < count; ++i) {
                const quint16 v = static_cast<quint16>(start + i);
                out.append(static_cast<char>((v >> 8) & 0xFF));
                out.append(static_cast<char>(v & 0xFF));
            }
        }
        resp = out;
        return true;
    };
}

TagPoint makeHolding(int addr, const QString &id)
{
    TagPoint p;
    p.id = id;
    p.address = addr;
    p.registerType = 3; // 保持寄存器
    p.scale = 1.0;
    return p;
}

} // namespace

class tst_Modbus : public QObject
{
    Q_OBJECT

private slots:
    void crc16_knownVector();
    void crc16_appendRoundTrip();
    void buildBlocks_mergesConsecutive();
    void buildBlocks_respectsMaxRegs();
    void buildBlocks_breaksOnGap();
    void parseReadRegisters_basic();
    void parseReadRegisters_exception();
    void parseReadCoils_basic();
    void runPoll_oneBlockForConsecutive();
    void runPoll_groupsByType();
};

void tst_Modbus::crc16_knownVector()
{
    // 标准 Modbus CRC 校验向量：ASCII "123456789" → 0x4B37（小端字节序列 0x37 0x4B）。
    QByteArray data = QByteArrayLiteral("123456789");
    QCOMPARE(modbus::crc16(data), quint16(0x4B37));
}

void tst_Modbus::crc16_appendRoundTrip()
{
    QByteArray frame = QByteArrayLiteral("\x01\x03\x00\x6B\x00\x03");
    const QByteArray withCrc = modbus::appendCrc(frame);
    QCOMPARE(withCrc.size(), frame.size() + 2);
    // 对"帧 + CRC"再算 CRC（不带尾部 2 字节）应等于写进去的 CRC。
    const quint16 recv = static_cast<quint16>(
        (static_cast<quint8>(withCrc.at(withCrc.size() - 1)) << 8)
        | static_cast<quint8>(withCrc.at(withCrc.size() - 2)));
    QCOMPARE(modbus::crc16(withCrc.left(withCrc.size() - 2)), recv);
}

void tst_Modbus::buildBlocks_mergesConsecutive()
{
    QList<TagPoint> points;
    QVector<int> indices;
    for (int i = 0; i < 20; ++i) {
        points.append(makeHolding(100 + i, QStringLiteral("h%1").arg(i)));
        indices.append(i);
    }
    const QVector<modbus::ReadBlock> blocks = modbus::buildBlocks(points, indices, 125);
    QCOMPARE(blocks.size(), 1);          // 20 个连续点 → 1 块
    QCOMPARE(int(blocks.at(0).startAddress), 100);
    QCOMPARE(int(blocks.at(0).count), 20);
    QCOMPARE(blocks.at(0).pointIndices.size(), 20);
}

void tst_Modbus::buildBlocks_respectsMaxRegs()
{
    QList<TagPoint> points;
    QVector<int> indices;
    for (int i = 0; i < 130; ++i) {
        points.append(makeHolding(100 + i, QStringLiteral("h%1").arg(i)));
        indices.append(i);
    }
    // 每块最多 125 个寄存器 → 130 个点拆成 125 + 5 两块。
    const QVector<modbus::ReadBlock> blocks = modbus::buildBlocks(points, indices, 125);
    QCOMPARE(blocks.size(), 2);
    QCOMPARE(int(blocks.at(0).count), 125);
    QCOMPARE(int(blocks.at(1).count), 5);
}

void tst_Modbus::buildBlocks_breaksOnGap()
{
    // 地址 100、102、104：相邻差 2，不连续 → 三块。
    QList<TagPoint> points;
    QVector<int> indices;
    for (int i = 0; i < 3; ++i) {
        points.append(makeHolding(100 + i * 2, QStringLiteral("h%1").arg(i)));
        indices.append(i);
    }
    const QVector<modbus::ReadBlock> blocks = modbus::buildBlocks(points, indices, 125);
    QCOMPARE(blocks.size(), 3);
    for (const modbus::ReadBlock &b : blocks)
        QCOMPARE(int(b.count), 1);
}

void tst_Modbus::parseReadRegisters_basic()
{
    // 响应：功能码 0x03，字节数 4，寄存器 0x000A、0x00FF
    QByteArray pdu;
    pdu.append(static_cast<char>(0x03));
    pdu.append(static_cast<char>(4));
    pdu.append(static_cast<char>(0x00));
    pdu.append(static_cast<char>(0x0A));
    pdu.append(static_cast<char>(0x00));
    pdu.append(static_cast<char>(0xFF));
    const QVector<quint16> regs = modbus::parseReadRegisters(pdu);
    QCOMPARE(regs.size(), 2);
    QCOMPARE(int(regs.at(0)), 10);
    QCOMPARE(int(regs.at(1)), 255);
}

void tst_Modbus::parseReadRegisters_exception()
{
    // 异常响应：最高位置 1 → 返回空。
    QByteArray pdu;
    pdu.append(static_cast<char>(0x83));
    pdu.append(static_cast<char>(0x02));
    QVERIFY(modbus::parseReadRegisters(pdu).isEmpty());
}

void tst_Modbus::parseReadCoils_basic()
{
    // 响应：功能码 0x01，字节数 1，bit0=1 bit1=0（0x01）
    QByteArray pdu;
    pdu.append(static_cast<char>(0x01));
    pdu.append(static_cast<char>(1));
    pdu.append(static_cast<char>(0x01));
    const QVector<bool> bits = modbus::parseReadCoils(pdu, 3);
    QCOMPARE(bits.size(), 3);
    QCOMPARE(bits.at(0), true);
    QCOMPARE(bits.at(1), false);
    QCOMPARE(bits.at(2), false);
}

void tst_Modbus::runPoll_oneBlockForConsecutive()
{
    QList<TagPoint> points;
    for (int i = 0; i < 20; ++i)
        points.append(makeHolding(100 + i, QStringLiteral("h%1").arg(i)));

    int callCount = 0;
    QVector<quint8> functions;
    QMap<QString, double> values;
    modbus::runPoll(
        points,
        makeEchoTransport(callCount, functions),
        [&](const QString &id, double v) { values.insert(id, v); });

    QCOMPARE(callCount, 1);                       // 20 个连续点只发了 1 个请求
    QCOMPARE(int(functions.at(0)), int(modbus::ReadHoldingRegisters));
    QCOMPARE(values.size(), 20);
    QCOMPARE(values.value(QStringLiteral("h0")), 100.0);
    QCOMPARE(values.value(QStringLiteral("h19")), 119.0);
}

void tst_Modbus::runPoll_groupsByType()
{
    QList<TagPoint> points;
    for (int i = 0; i < 20; ++i)
        points.append(makeHolding(100 + i, QStringLiteral("h%1").arg(i)));
    for (int i = 0; i < 5; ++i) {
        TagPoint p;
        p.id = QStringLiteral("in%1").arg(i);
        p.address = 200 + i;
        p.registerType = 4; // 输入寄存器
        p.scale = 1.0;
        points.append(p);
    }

    int callCount = 0;
    QVector<quint8> functions;
    QMap<QString, double> values;
    modbus::runPoll(
        points,
        makeEchoTransport(callCount, functions),
        [&](const QString &id, double v) { values.insert(id, v); });

    // 保持寄存器一段（1 块）+ 输入寄存器一段（1 块）= 2 次请求；线圈段没有点位。
    QCOMPARE(callCount, 2);
    QCOMPARE(int(functions.at(0)), int(modbus::ReadHoldingRegisters));
    QCOMPARE(int(functions.at(1)), int(modbus::ReadInputRegisters));
    QCOMPARE(values.size(), 25);
    QCOMPARE(values.value(QStringLiteral("in4")), 204.0);
}

QTEST_MAIN(tst_Modbus)

#include "tst_modbus.moc"
