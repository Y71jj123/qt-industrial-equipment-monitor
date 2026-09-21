#pragma once

#include <QByteArray>
#include <QList>
#include <QVector>
#include <QString>
#include <functional>
#include <algorithm>

#include "core/devicemanager.h" // TagPoint

// =============================================================================
// Modbus 协议公共逻辑（TCP 与 RTU 共用）。
//
// 把"组帧之外"的东西都放这里：PDU 构造、CRC16（RTU 用）、响应解析、
// 以及最关键的 **块读合并** —— 把分散在同一种寄存器类型里的多个点位，
// 按地址排序后合并成尽量少的连续块（每块最多 125 个寄存器）一次性读回，
// 而不是每个点位发一次请求。
//
// 项目里"演示版 vs 能上线"最明显的一道坎就是这里：20 个点逐点读 = 20 次往返，
// 合并后通常 ≤3 块。这块逻辑对两种传输层完全一致，所以抽成共享代码，
// TCP 与 RTU 各自只负责"怎么把 PDU 发到线上、怎么收回来"。
// =============================================================================

namespace modbus {

enum Function : quint8 {
    ReadCoils = 0x01,
    ReadHoldingRegisters = 0x03,
    ReadInputRegisters = 0x04,
    WriteSingleCoil = 0x05,
    WriteSingleRegister = 0x06,
};

/// Modbus CRC16：多项式 0x8005，初值 0xFFFF，无反射、无最终异或。
/// 这是 Modbus RTU 帧的校验方式；TCP 走 MBAP 头不做 CRC。
inline quint16 crc16(const QByteArray &data)
{
    quint16 crc = 0xFFFF;
    for (char ch : data) {
        crc ^= static_cast<quint8>(ch);
        for (int i = 0; i < 8; ++i) {
            if (crc & 0x0001)
                crc = static_cast<quint16>((crc >> 1) ^ 0xA001);
            else
                crc = static_cast<quint16>(crc >> 1);
        }
    }
    return crc;
}

/// 给一帧（从站地址 + PDU）追加 2 字节 CRC（小端）。
inline QByteArray appendCrc(const QByteArray &frame)
{
    const quint16 crc = crc16(frame);
    QByteArray out = frame;
    out.append(static_cast<char>(crc & 0xFF));
    out.append(static_cast<char>((crc >> 8) & 0xFF));
    return out;
}

/// 构造读请求 PDU（不含传输层头）：[功能码][起始地址 Hi/Lo][数量 Hi/Lo]。
inline QByteArray buildReadPdu(quint8 function, quint16 start, quint16 count)
{
    QByteArray pdu;
    pdu.append(static_cast<char>(function));
    pdu.append(static_cast<char>((start >> 8) & 0xFF));
    pdu.append(static_cast<char>(start & 0xFF));
    pdu.append(static_cast<char>((count >> 8) & 0xFF));
    pdu.append(static_cast<char>(count & 0xFF));
    return pdu;
}

/// 构造写单寄存器 / 单线圈请求 PDU：[功能码][地址 Hi/Lo][值 Hi/Lo]。
inline QByteArray buildWriteSinglePdu(quint8 function, quint16 address, quint16 value)
{
    QByteArray pdu;
    pdu.append(static_cast<char>(function));
    pdu.append(static_cast<char>((address >> 8) & 0xFF));
    pdu.append(static_cast<char>(address & 0xFF));
    pdu.append(static_cast<char>((value >> 8) & 0xFF));
    pdu.append(static_cast<char>(value & 0xFF));
    return pdu;
}

/// 解析"读寄存器"响应 PDU [功能码][字节数][……寄存器……] 成寄存器值列表。
/// 返回空表示帧异常或长度不对。
inline QVector<quint16> parseReadRegisters(const QByteArray &pdu)
{
    QVector<quint16> out;
    if (pdu.size() < 2)
        return out;
    if (static_cast<quint8>(pdu.at(0)) & 0x80)
        return out; // 异常响应
    const int byteCount = static_cast<quint8>(pdu.at(1));
    if (byteCount % 2 != 0 || pdu.size() < 2 + byteCount)
        return out;
    const int regCount = byteCount / 2;
    out.reserve(regCount);
    for (int i = 0; i < regCount; ++i) {
        const int off = 2 + i * 2;
        const quint16 v = static_cast<quint16>(
            (static_cast<quint8>(pdu.at(off)) << 8) | static_cast<quint8>(pdu.at(off + 1)));
        out.append(v);
    }
    return out;
}

/// 解析"读线圈"响应 PDU [功能码][字节数][……位……] 成 count 个布尔值。
inline QVector<bool> parseReadCoils(const QByteArray &pdu, int count)
{
    QVector<bool> out;
    if (pdu.size() < 2)
        return out;
    if (static_cast<quint8>(pdu.at(0)) & 0x80)
        return out;
    const int byteCount = static_cast<quint8>(pdu.at(1));
    if (pdu.size() < 2 + byteCount)
        return out;
    out.reserve(count);
    for (int i = 0; i < count; ++i)
        out.append((static_cast<quint8>(pdu.at(2 + i / 8)) >> (i % 8)) & 0x01);
    return out;
}

/// 一次连续块读：起始地址 + 寄存器数量 + 原始点位表里的下标。
struct ReadBlock {
    quint16 startAddress = 0;
    quint16 count = 0;
    QVector<int> pointIndices; // 指向传入的 points 列表
};

/// 把同一种寄存器类型下的点位按下标集合，按地址排序后合并成连续块。
/// 相邻地址（next == prev + prevCount）且不超过 maxRegs 才并入同一块 ——
/// 这正是把"20 个点"收敛成"≤3 块"的关键。
inline QVector<ReadBlock> buildBlocks(const QList<TagPoint> &points,
                                      const QVector<int> &indices,
                                      int maxRegs = 125)
{
    QVector<int> sorted = indices;
    std::sort(sorted.begin(), sorted.end(),
              [&](int a, int b) { return points.at(a).address < points.at(b).address; });

    QVector<ReadBlock> blocks;
    ReadBlock current;
    for (int i = 0; i < sorted.size(); ++i) {
        const int idx = sorted.at(i);
        const quint16 addr = static_cast<quint16>(points.at(idx).address);
        if (current.count == 0) {
            current.startAddress = addr;
            current.count = 1;
            current.pointIndices.append(idx);
        } else {
            // 期望的下一个地址 = 当前块首地址 + 已含寄存器数
            const quint16 expectedNext =
                static_cast<quint16>(current.startAddress + current.count);
            if (addr == expectedNext && current.count < maxRegs) {
                ++current.count;
                current.pointIndices.append(idx);
            } else {
                blocks.append(current);
                current = ReadBlock{};
                current.startAddress = addr;
                current.count = 1;
                current.pointIndices.append(idx);
            }
        }
    }
    if (current.count > 0)
        blocks.append(current);
    return blocks;
}

/// 传输层抽象：发出一个 PDU（仅功能码 + 参数，不含传输头），把响应 PDU 填回。
using ModbusTransport = std::function<bool(quint8 function,
                                           const QByteArray &requestPdu,
                                           QByteArray &responsePdu)>;

/// 读到一点位值时的回调。
using EmitValue = std::function<void(const QString &tagId, double value)>;

/// 高级轮询：按寄存器类型分组（保持寄存器 / 输入寄存器 / 线圈）→ 各自合并成块 →
/// 每块一次读 → 解析 → 上报。TCP 与 RTU 调用同一个函数，保证块读行为一致。
///
/// transport 由调用方提供（TCP 用 MBAP 组帧、RTU 用 CRC 组帧），emitValue 由调用方接到
/// 自己的 tagValueChanged 信号上。
inline void runPoll(const QList<TagPoint> &points,
                    ModbusTransport transport,
                    EmitValue emitValue)
{
    QVector<int> holding, input, coils;
    for (int i = 0; i < points.size(); ++i) {
        const int rt = points.at(i).registerType;
        if (rt == 4)
            input.append(i);
        else if (rt == 1)
            coils.append(i);
        else
            holding.append(i); // 3（保持寄存器）及未知默认都按保持寄存器处理
    }

    const auto readGroup = [&](const QVector<int> &indices,
                               quint8 function,
                               bool isCoil) {
        if (indices.isEmpty())
            return;

        const QVector<ReadBlock> blocks = buildBlocks(points, indices, 125);
        for (const ReadBlock &block : blocks) {
            const QByteArray req = buildReadPdu(function, block.startAddress, block.count);
            QByteArray resp;
            if (!transport(function, req, resp))
                continue; // 本块失败跳过，不影响其它块

            if (isCoil) {
                const QVector<bool> bits = parseReadCoils(resp, block.count);
                for (int k = 0; k < block.pointIndices.size(); ++k) {
                    const int idx = block.pointIndices.at(k);
                    const TagPoint &p = points.at(idx);
                    const int offset = p.address - block.startAddress;
                    if (offset >= 0 && offset < bits.size())
                        emitValue(p.id, bits.at(offset) ? 1.0 : 0.0);
                }
            } else {
                const QVector<quint16> regs = parseReadRegisters(resp);
                for (int k = 0; k < block.pointIndices.size(); ++k) {
                    const int idx = block.pointIndices.at(k);
                    const TagPoint &p = points.at(idx);
                    const int offset = p.address - block.startAddress;
                    if (offset >= 0 && offset < regs.size()) {
                        const quint16 raw = regs.at(offset);
                        // 开关量（线圈/离散输入）本身 0/1 不缩放；模拟量乘 scale
                        const double value = p.boolean ? double(raw) : double(raw) * p.scale;
                        emitValue(p.id, value);
                    }
                }
            }
        }
    };

    readGroup(holding, ReadHoldingRegisters, false);
    readGroup(input, ReadInputRegisters, false);
    readGroup(coils, ReadCoils, true);
}

} // namespace modbus
