#include "comm/mockconnection.h"

#include "utils/logger.h"

#include <QRandomGenerator>
#include <QTimer>

namespace {

const QStringList kDefaultTags = {
    QStringLiteral("temperature"),  // 温度 °C
    QStringLiteral("pressure"),     // 压力 kPa
    QStringLiteral("speed"),        // 转速 rpm
    QStringLiteral("running"),      // 运行状态 0/1
};

} // namespace

MockConnection::MockConnection(QObject *parent)
    : DeviceConnection(parent)
    , m_tags(kDefaultTags)
{
    for (const QString &tag : m_tags)
        m_base.insert(tag, QRandomGenerator::global()->bounded(100.0));

    // 定时器属于创建它的线程；本类对象若被 moveToThread，定时器需随对象重建。
    m_timer = new QTimer(this);
    m_timer->setInterval(m_intervalMs);
    connect(m_timer, &QTimer::timeout, this, &MockConnection::tick);
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
    for (const QString &tag : m_tags) {
        m_base.insert(tag, QRandomGenerator::global()->bounded(100.0));
        m_values.insert(tag, 0.0);
    }
}

QStringList MockConnection::tags() const
{
    return m_tags;
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

    // 以基础值做缓慢漂移 + 随机噪声，看起来像真实工况
    for (const QString &tag : qAsConst(m_tags)) {
        double &base = m_base[tag];
        base += QRandomGenerator::global()->bounded(2.0) - 1.0;
        base = qBound(0.0, base, 100.0);

        const double noise = (QRandomGenerator::global()->bounded(200.0) - 100.0) / 100.0;
        const double value = base + noise;

        m_values.insert(tag, value);
        emit tagValueChanged(tag, value);
    }
}
