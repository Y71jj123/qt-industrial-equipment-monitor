#pragma once

#include <QHash>
#include <QString>
#include <QVector>

/// 单个虚拟点位的定义。
///
/// 地址与寄存器类型**刻意与主程序 `defaultTagPoints()` 保持一致** ——
/// 这样监控端"新建 Modbus TCP 设备 → 用默认点位表"就能直接读到值，
/// 不必手工照着文档把 5 个点位再敲一遍。
struct VirtualTag
{
    QString id;              ///< 点位标识（temperature / pressure / ...）
    QString name;            ///< 显示名
    QString unit;            ///< 工程单位
    int address = 0;         ///< Modbus 寄存器地址
    int registerType = 3;    ///< 3=保持寄存器 4=输入寄存器 1=线圈
    double baseline = 0.0;   ///< 基准值（均值回归的目标）
    double lo = 0.0;         ///< 波动下限
    double hi = 100.0;       ///< 波动上限
    bool boolean = false;    ///< 开关量（只输出 0/1）
};

/// 虚拟设备的数据发生器。
///
/// 沿用主程序 `MockConnection` 的模型：**均值回归 + 小幅扰动 + 输出噪声**。
/// 相比纯随机数，它有两个好处：数据围绕基准值波动、像真实工况；
/// 且不会长时间贴在量程边界上（那会造成"越限→恢复→越限"的告警抖动）。
///
/// 支持故障注入，用于演示监控端的告警与断线处理：
///   - `None`      正常波动
///   - `Freeze`    冻结（数值不再变化，模拟设备死机）
///   - `OverLimit` 强制越限（把模拟量顶过参考上限，用于触发告警）
///
/// 另外支持**手动置值**（下位机被写入后保持该值），这样监控端的
/// "远程控制 / 指令下发"演示才有可见效果 —— 否则刚写下去就被下一周期覆盖了。
class SignalGenerator
{
public:
    enum class Fault
    {
        None = 0,
        Freeze,    ///< 冻结
        OverLimit, ///< 强制越限
    };

    SignalGenerator();

    /// 内置点位表（与主程序默认点位表对齐）。
    const QVector<VirtualTag> &tags() const { return m_tags; }

    /// 推进一个采样周期（冻结状态下不改变数值）。
    void step();

    /// 当前工程值（浮点，供界面显示）。
    double valueAt(int index) const;

    /// 第 index 个点位当前对应的**寄存器原始值**（线圈为 0/1，模拟量取整）。
    quint16 rawValueAt(int index) const;

    /// 按地址 + 寄存器类型查找点位下标；找不到返回 -1。
    int indexOf(int address, int registerType) const;

    // ---------------- 故障注入 ----------------

    Fault fault() const { return m_fault; }
    void setFault(Fault fault);

    // ---------------- 手动置值 ----------------

    /// 下位机被写入后的保持值（key = 保持寄存器地址）。
    /// 被手动置值的点位**不再参与自动波动**，直到调用 clearOverrides()。
    void setOverride(int address, quint16 raw);
    void clearOverrides();
    bool isOverridden(int address) const { return m_overrides.contains(address); }
    int overrideCount() const { return m_overrides.size(); }

    /// 冻结/越限状态的中文名（供界面显示）。
    static QString faultName(Fault fault);

private:
    QVector<VirtualTag> m_tags;
    QVector<double> m_values; ///< 与 m_tags 一一对应：当前工程值
    QVector<double> m_bases;  ///< 与 m_tags 一一对应：均值回归的中间状态

    Fault m_fault = Fault::None;
    QHash<int, quint16> m_overrides; ///< 保持寄存器地址 → 手动值
};
