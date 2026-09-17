#include "comm/mockconnection.h"

#include "core/devicemanager.h"
#include "utils/logger.h"

#include <QRandomGenerator>
#include <QTimer>

#include <utility>

namespace {

/// 单个模拟点位的描述。
struct TagSpec
{
    QString id;
    double baseline;  ///< 基准值：工况常态（均值回归的目标）
    double lo;        ///< 波动下界
    double hi;        ///< 波动上界
    bool boolean;     ///< 开关量，只输出 0 / 1
};

/// 默认点位表。
///
/// 设计要点：hi 取得比对应的告警上限略低一点，让数值**偶尔**越限
/// （这样能演示告警功能），但因为有均值回归，它不会长时间贴在边界上
/// 反复穿过阈值 —— 那会造成“越限 → 恢复 → 越限”的告警抖动。
///   温度 上限 60 / 压力 上限 80 / 转速 上限 90
const QList<TagSpec> &defaultSpecs()
{
    static const QList<TagSpec> specs = {
        {QStringLiteral("temperature"), 42.0, 25.0, 68.0, false},
        {QStringLiteral("pressure"), 55.0, 30.0, 85.0, false},
        {QStringLiteral("speed"), 58.0, 30.0, 88.0, false},
        {QStringLiteral("running"), 1.0, 0.0, 1.0, true},
        {QStringLiteral("vibration"), 30.0, 10.0, 60.0, false},
    };
    return specs;
}

const TagSpec *findSpec(const QString &tag)
{
    const QList<TagSpec> &specs = defaultSpecs();
    for (const TagSpec &spec : specs) {
        if (spec.id == tag)
            return &spec;
    }
    return nullptr;
}

/// [0, 1) 之间的随机数
double randomUnit()
{
    return QRandomGenerator::global()->bounded(1000) / 1000.0;
}

} // namespace

MockConnection::MockConnection(QObject *parent)
    : DeviceConnection(parent)
{
    for (const TagSpec &spec : defaultSpecs()) {
        m_tags << spec.id;
        m_base.insert(spec.id, spec.baseline);
        m_values.insert(spec.id, spec.baseline);
    }
    // 这里**不建定时器**：本对象会被 moveToThread 到采集线程，
    // 而 QTimer 必须在它最终所属的线程里创建 —— 统一留到 open() 里做。
}

MockConnection::~MockConnection()
{
    close();
}

bool MockConnection::open(const QString &host, quint16 port)
{
    if (m_open)
        close();

    m_host = host;
    m_port = port;
    m_open = true;

    // open() 由采集线程调用，定时器在这里诞生就一直属于那条线程
    if (!m_timer) {
        m_timer = new QTimer(this);
        m_timer->setInterval(m_intervalMs);
        connect(m_timer, &QTimer::timeout, this, &MockConnection::tick);
    }
    m_timer->start();
    Log::info(QStringLiteral("模拟连接已建立: %1:%2").arg(host).arg(port));
    emit opened();
    return true;
}

void MockConnection::close()
{
    if (m_timer)
        m_timer->stop();

    if (!m_open)
        return;

    m_open = false;
    Log::info(QStringLiteral("模拟连接已断开: %1:%2").arg(m_host).arg(m_port));
    emit closed();
}

bool MockConnection::isOpen() const
{
    return m_open;
}

bool MockConnection::readTag(const QString &tagId, QVariant &value)
{
    if (!m_open) {
        emit errorOccurred(QStringLiteral("读取失败：连接未建立"));
        return false;
    }

    if (!m_values.contains(tagId)) {
        emit errorOccurred(QStringLiteral("读取失败：点位不存在 %1").arg(tagId));
        return false;
    }

    value = m_values.value(tagId);
    return true;
}

bool MockConnection::writeTag(const QString &tagId, const QVariant &value)
{
    if (!m_open) {
        emit errorOccurred(QStringLiteral("写入失败：连接未建立"));
        return false;
    }

    if (!m_values.contains(tagId)) {
        emit errorOccurred(QStringLiteral("写入失败：点位不存在 %1").arg(tagId));
        return false;
    }

    m_values.insert(tagId, value);
    Log::info(QStringLiteral("下发指令 %1 = %2").arg(tagId, value.toString()));
    emit tagValueChanged(tagId, value);
    return true;
}

void MockConnection::setTags(const QStringList &tags)
{
    if (tags.isEmpty())
        return;

    m_tags = tags;
    m_values.clear();
    m_base.clear();
    for (const QString &tag : tags) {
        const TagSpec *spec = findSpec(tag);
        const double baseline = spec ? spec->baseline : 50.0;
        m_base.insert(tag, baseline);
        m_values.insert(tag, baseline);
    }
}

QStringList MockConnection::tags() const
{
    return m_tags;
}

void MockConnection::configure(const DeviceInfo &device)
{
    // 模拟源没有协议参数，只需把"点位表"和"采集周期"套进来。
    if (!device.points.isEmpty()) {
        QStringList ids;
        ids.reserve(device.points.size());
        for (const TagPoint &point : device.points)
            ids << point.id;
        setTags(ids);
    }
    setIntervalMs(device.pollIntervalMs);
}

void MockConnection::setIntervalMs(int intervalMs)
{
    m_intervalMs = qMax(50, intervalMs);
    if (m_timer)
        m_timer->setInterval(m_intervalMs);
}

int MockConnection::intervalMs() const
{
    return m_intervalMs;
}

void MockConnection::tick()
{
    if (!m_open)
        return;

    for (const QString &tag : std::as_const(m_tags)) {
        const TagSpec *spec = findSpec(tag);

        // 开关量：只输出 0 / 1，大部分时间处于“运行”状态
        if (spec && spec->boolean) {
            const double v = (QRandomGenerator::global()->bounded(100) < 85) ? 1.0 : 0.0;
            m_values.insert(tag, v);
            emit tagValueChanged(tag, v);
            continue;
        }

        const double lo = spec ? spec->lo : 0.0;
        const double hi = spec ? spec->hi : 100.0;
        const double baseline = spec ? spec->baseline : (lo + hi) / 2.0;

        // 均值回归 + 小幅扰动：始终往基准值靠，不会被随机游走带到边界上卡住
        double &base = m_base[tag];
        base += (baseline - base) * 0.10;
        base += (randomUnit() - 0.5) * 4.0;
        base = qBound(lo, base, hi);

        // 输出时再叠加一点测量噪声
        const double value = qBound(lo, base + (randomUnit() - 0.5) * 2.0, hi);

        m_values.insert(tag, value);
        emit tagValueChanged(tag, value);
    }
}
