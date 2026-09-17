#include "httpjsonplugin.h"

#include "core/devicemanager.h"
#include "utils/logger.h"

#include <QDateTime>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

namespace {

/// 单次 HTTP 请求的超时（毫秒）。
///
/// 每台设备独占一条采集线程，卡住只影响这一台；但也不能无限等 ——
/// 超时上限必须小于采集周期带来的容错窗口，否则"重连退避"永远轮不到执行。
constexpr int kRequestTimeoutMs = 3000;

constexpr auto kDefaultPath = "/api/points";

/// JSON 值 → 点位值。
///
/// 数值直接用；布尔转 0/1（与开关量点位的约定一致）；字符串尝试当数字解析。
/// 解析不了的返回无效 QVariant —— **由调用方跳过，而不是塞一个 0 进去**：
/// 塞 0 会让"这个字段没采到"和"这个字段真的是 0"永远分不开，
/// 而这两者在现场是完全不同的两件事（后者可能是告警）。
QVariant jsonToPointValue(const QJsonValue &value)
{
    switch (value.type()) {
    case QJsonValue::Bool:
        return value.toBool() ? 1.0 : 0.0;
    case QJsonValue::Double:
        return value.toDouble();
    case QJsonValue::String: {
        bool ok = false;
        const double number = value.toString().toDouble(&ok);
        return ok ? QVariant(number) : QVariant();
    }
    default:
        return QVariant();
    }
}

} // namespace

HttpJsonConnection::HttpJsonConnection(QObject *parent)
    : DeviceConnection(parent)
{
    // 这里**不建 QNetworkAccessManager**：本对象会被 moveToThread 到采集线程，
    // 而它内部有线程 / 定时器，必须在最终所属线程里创建 —— 统一留到 open()。
}

HttpJsonConnection::~HttpJsonConnection() = default;

void HttpJsonConnection::configure(const DeviceInfo &device)
{
    // 端点字段（DeviceInfo::mqttTopic）在本插件里被解释为"请求路径"。
    // 这正是 ProtocolTraits::endpointLabel 存在的意义：同一个配置字段，
    // 由协议自己决定怎么解释、界面叫什么。
    m_path = device.mqttTopic.trimmed();
    if (m_path.isEmpty())
        m_path = QString::fromLatin1(kDefaultPath);
    if (!m_path.startsWith(QLatin1Char('/')))
        m_path.prepend(QLatin1Char('/'));

    m_refreshMs = qMax(100, device.pollIntervalMs);
}

QString HttpJsonConnection::endpointUrl() const
{
    return QStringLiteral("http://%1:%2%3").arg(m_host).arg(m_port).arg(m_path);
}

bool HttpJsonConnection::open(const QString &host, quint16 port)
{
    if (m_open)
        close();

    m_host = host;
    m_port = port;

    // open() 由采集线程调用，网络管理器在这里诞生就一直属于那条线程
    if (!m_net)
        m_net = new QNetworkAccessManager(this);

    m_cache.clear();
    m_lastFetchMs = 0;
    m_fetchFailing = false;

    // 先试取一次：连不上就直接如实返回失败，交给采集调度器的退避重连。
    // 在这里再套一层自己的重试只会和调度器的退避互相打乱（两套节奏叠在一起，
    // 现场看到的"重连间隔"就完全不可预测了）。
    if (!fetch())
        return false;

    m_open = true;
    Log::info(QStringLiteral("HTTP/JSON 数据源已连接：%1（%2 个字段）")
                  .arg(endpointUrl())
                  .arg(m_cache.size()));
    emit opened();
    return true;
}

void HttpJsonConnection::close()
{
    if (!m_open)
        return;

    m_open = false;
    m_cache.clear();
    Log::info(QStringLiteral("HTTP/JSON 数据源已断开：%1").arg(endpointUrl()));
    emit closed();
}

bool HttpJsonConnection::isOpen() const
{
    return m_open;
}

bool HttpJsonConnection::fetch()
{
    if (!m_net)
        return reportFetchFailure(QStringLiteral("网络未初始化"));

    QNetworkRequest request{QUrl(endpointUrl())};
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

    QNetworkReply *reply = m_net->get(request);

    // 采集线程里没有别的活要干，用局部事件循环等结果最直接；等待有硬上限。
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(kRequestTimeoutMs);
    loop.exec();

    const bool finishedInTime = reply->isFinished();
    if (!finishedInTime)
        reply->abort(); // 让 socket 立刻收摊，别把半截连接挂在半路上

    const QByteArray body = reply->readAll();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError netError = reply->error();
    const QString netErrorText = reply->errorString();
    reply->deleteLater();

    if (!finishedInTime) {
        return reportFetchFailure(
            QStringLiteral("请求超时（%1 ms）：%2").arg(kRequestTimeoutMs).arg(endpointUrl()));
    }
    if (netError != QNetworkReply::NoError) {
        return reportFetchFailure(QStringLiteral("请求失败：%1（%2）")
                                      .arg(netErrorText, endpointUrl()));
    }
    if (status != 200) {
        return reportFetchFailure(
            QStringLiteral("HTTP 状态码 %1：%2").arg(status).arg(endpointUrl()));
    }

    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return reportFetchFailure(QStringLiteral("返回内容不是 JSON 对象：%1")
                                      .arg(parseError.errorString()));
    }

    QHash<QString, QVariant> cache;
    const QJsonObject object = document.object();
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        const QVariant value = jsonToPointValue(it.value());
        if (value.isValid())
            cache.insert(it.key(), value);
    }

    if (cache.isEmpty()) {
        return reportFetchFailure(
            QStringLiteral("JSON 里没有可用的数值字段（点位 ID 要和返回的字段名对上）"));
    }

    m_cache = cache;
    m_lastFetchMs = QDateTime::currentMSecsSinceEpoch();
    m_fetchFailing = false;
    return true;
}

bool HttpJsonConnection::reportFetchFailure(const QString &message)
{
    if (!m_fetchFailing) {
        m_fetchFailing = true;
        Log::warn(QStringLiteral("HTTP/JSON 取数失败：%1").arg(message));
        emit errorOccurred(message);
    }
    return false;
}

bool HttpJsonConnection::readTag(const QString &tagId, QVariant &value)
{
    if (!m_open)
        return false;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_lastFetchMs >= m_refreshMs) {
        // 缓存过期才真正发请求 —— 一轮采集只请求一次，见类注释。
        if (!fetch())
            return false;
    }

    const auto it = m_cache.constFind(tagId);
    if (it == m_cache.constEnd())
        return false; // 现场没上报这个字段：算读失败，而不是编一个 0 出来

    value = it.value();
    return true;
}

bool HttpJsonConnection::writeTag(const QString &tagId, const QVariant &value)
{
    if (!m_open || !m_net)
        return false;

    // 约定：写回是 POST 同一个路径，体是 {"tag": "...", "value": ...}。
    // 现场 REST 接口的形态五花八门，本项目只定义一种最小可用的形态，
    // 具体现场按需改这个插件即可（这正是插件化的意义）。
    QJsonObject payload;
    payload.insert(QStringLiteral("tag"), tagId);
    payload.insert(QStringLiteral("value"), QJsonValue::fromVariant(value));

    QNetworkRequest request{QUrl(endpointUrl())};
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

    QNetworkReply *reply = m_net->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));

    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(kRequestTimeoutMs);
    loop.exec();

    const bool finishedInTime = reply->isFinished();
    if (!finishedInTime)
        reply->abort();

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError netError = reply->error();
    const QString netErrorText = reply->errorString();
    reply->deleteLater();

    if (!finishedInTime) {
        emit errorOccurred(QStringLiteral("下发超时（%1 ms）").arg(kRequestTimeoutMs));
        return false;
    }
    if (netError != QNetworkReply::NoError) {
        emit errorOccurred(QStringLiteral("下发失败：%1").arg(netErrorText));
        return false;
    }
    if (status < 200 || status >= 300) {
        emit errorOccurred(QStringLiteral("设备拒绝了这次下发（HTTP %1）").arg(status));
        return false;
    }

    Log::info(QStringLiteral("下发指令 %1 = %2（HTTP/JSON）").arg(tagId, value.toString()));

    // 写成功之后把缓存里这个值同步过来，界面立刻能看到新值，
    // 不用干等下一次轮询（不然操作员会怀疑"到底写进去没有"）。
    m_cache.insert(tagId, value);
    emit tagValueChanged(tagId, value);
    return true;
}
