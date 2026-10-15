#include "signalgenerator.h"

#include <QRandomGenerator>
#include <QtGlobal>

#include <utility>

namespace {

/// [0, 1) 之间的随机数。
double randomUnit()
{
    return QRandomGenerator::global()->bounded(1000) / 1000.0;
}

} // namespace

SignalGenerator::SignalGenerator()
{
    // 点位表与主程序 defaultTagPoints() 对齐：
    //   温度 ℃ @ 保持寄存器 0   压力 kPa @ 保持寄存器 1   转速 r/min @ 保持寄存器 2
    //   运行状态 @ 线圈 3       振动 mm/s @ 保持寄存器 4
    //
    // 量程与基准值取自主程序 MockConnection 的 defaultSpecs()：
    //   temperature 42 [25, 68] / pressure 55 [30, 85] / speed 58 [30, 88] / vibration 30 [10, 60]
    // 参考告警上限（温度 60 / 压力 80 / 转速 90 / 振动 70）由 hi 略高于上限的方式保证
    // "偶尔越限"，正好能把监控端的告警链路演示出来。
    m_tags = {
        {QStringLiteral("temperature"), QStringLiteral("温度"), QStringLiteral("℃"), 0, 3, 42.0, 25.0, 68.0, false},
        {QStringLiteral("pressure"), QStringLiteral("压力"), QStringLiteral("kPa"), 1, 3, 55.0, 30.0, 85.0, false},
        {QStringLiteral("speed"), QStringLiteral("转速"), QStringLiteral("r/min"), 2, 3, 58.0, 30.0, 88.0, false},
        {QStringLiteral("running"), QStringLiteral("运行状态"), QString(), 3, 1, 1.0, 0.0, 1.0, true},
        {QStringLiteral("vibration"), QStringLiteral("振动"), QStringLiteral("mm/s"), 4, 3, 30.0, 10.0, 60.0, false},
    };

    m_values.reserve(m_tags.size());
    m_bases.reserve(m_tags.size());
    for (const VirtualTag &tag : std::as_const(m_tags)) {
        m_values.append(tag.baseline);
        m_bases.append(tag.baseline);
    }
}

void SignalGenerator::step()
{
    if (m_fault == Fault::Freeze)
        return; // 冻结：数值原地不动

    for (int i = 0; i < m_tags.size(); ++i) {
        const VirtualTag &tag = m_tags.at(i);

        // 被手动置值的点位不再自动波动（下位机被写入后保持该值）。
        // 线圈与模拟量一视同仁 —— 否则"远程控制运行状态"刚下发就被覆盖，演示不出效果。
        if (m_overrides.contains(tag.address))
            continue;

        // 开关量：只输出 0 / 1，大部分时间处于"运行"状态
        if (tag.boolean) {
            const double v = (QRandomGenerator::global()->bounded(100) < 85) ? 1.0 : 0.0;
            m_values[i] = (m_fault == Fault::OverLimit) ? 0.0 : v;
            continue;
        }

        // 越限故障：直接把值顶到上限之上（越过参考告警上限）
        if (m_fault == Fault::OverLimit) {
            m_values[i] = tag.hi * 1.15;
            continue;
        }

        // 均值回归 + 小幅扰动：始终往基准值靠，不会被随机游走带到边界上卡住
        double &base = m_bases[i];
        base += (tag.baseline - base) * 0.10;
        base += (randomUnit() - 0.5) * 4.0;
        base = qBound(tag.lo, base, tag.hi);

        // 输出时再叠加一点测量噪声
        m_values[i] = qBound(tag.lo, base + (randomUnit() - 0.5) * 2.0, tag.hi);
    }
}

double SignalGenerator::valueAt(int index) const
{
    if (index < 0 || index >= m_values.size())
        return 0.0;
    return m_values.at(index);
}

quint16 SignalGenerator::rawValueAt(int index) const
{
    if (index < 0 || index >= m_tags.size())
        return 0;

    const VirtualTag &tag = m_tags.at(index);

    // 手动置值优先（线圈与保持寄存器同理）
    if (m_overrides.contains(tag.address))
        return m_overrides.value(tag.address);

    if (tag.boolean)
        return (m_values.at(index) >= 0.5) ? 1 : 0;

    const double v = qBound(0.0, m_values.at(index), 65535.0);
    return static_cast<quint16>(qRound(v));
}

int SignalGenerator::indexOf(int address, int registerType) const
{
    for (int i = 0; i < m_tags.size(); ++i) {
        if (m_tags.at(i).address == address && m_tags.at(i).registerType == registerType)
            return i;
    }
    return -1;
}

void SignalGenerator::setFault(Fault fault)
{
    m_fault = fault;
}

void SignalGenerator::setOverride(int address, quint16 raw)
{
    m_overrides.insert(address, raw);

    // 同步刷新当前值，让界面立刻反映写入结果
    for (int i = 0; i < m_tags.size(); ++i) {
        if (m_tags.at(i).address != address)
            continue;
        m_values[i] = m_tags.at(i).boolean ? (raw ? 1.0 : 0.0) : double(raw);
        break;
    }
}

void SignalGenerator::clearOverrides()
{
    m_overrides.clear();
}

QString SignalGenerator::faultName(Fault fault)
{
    switch (fault) {
    case Fault::None:
        return QStringLiteral("正常");
    case Fault::Freeze:
        return QStringLiteral("冻结");
    case Fault::OverLimit:
        return QStringLiteral("强制越限");
    }
    return QStringLiteral("正常");
}
