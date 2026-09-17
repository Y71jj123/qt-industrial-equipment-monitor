#pragma once

#include "comm/deviceconnection.h"
#include "comm/protocolplugin.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <QVariant>

class QNetworkAccessManager;

/// HTTP + JSON 数据源连接。
///
/// 很多现场网关不给 Modbus / OPC UA，而是直接暴露一个 REST 接口：
///
///     GET http://<host>:<port><路径>   →   {"temperature":42.5,"pressure":53,"running":true}
///
/// 本插件就按这种最常见的形态做轮询。
///
/// **一轮采集只发一次请求**：采集调度器是"一个点位调一次 readTag"，
/// 如果每个 readTag 都发一次 HTTP，20 个点位就是每轮 20 次往返 ——
/// 现场会被网管当成异常流量。所以这里加了一层按采集周期过期的缓存，
/// 一次 GET 填满整张点位表，其余点位直接吃缓存。
class HttpJsonConnection : public DeviceConnection
{
    Q_OBJECT

public:
    explicit HttpJsonConnection(QObject *parent = nullptr);
    ~HttpJsonConnection() override;

    /// 端点字段在本插件里解释为**请求路径**（默认 /api/points）。
    void configure(const DeviceInfo &device) override;

    bool open(const QString &host, quint16 port) override;
    void close() override;
    bool isOpen() const override;

    bool readTag(const QString &tagId, QVariant &value) override;
    bool writeTag(const QString &tagId, const QVariant &value) override;

private:
    /// 发一次 GET 并解析 JSON，填满缓存。失败返回 false。
    bool fetch();

    /// 上报一次取数失败。**只在"由好变坏"的那一刻报一次** ——
    /// 每轮都报的话日志会被刷爆，真正的其它错误反而被埋掉。
    bool reportFetchFailure(const QString &message);

    QString endpointUrl() const;

    QString m_host;
    quint16 m_port = 8080;
    QString m_path;         ///< 请求路径
    int m_refreshMs = 1000; ///< 缓存有效期 = 采集周期

    QHash<QString, QVariant> m_cache;
    qint64 m_lastFetchMs = 0;
    bool m_open = false;
    bool m_fetchFailing = false;

    /// 在 open() 里创建：QNetworkAccessManager 的内部实现会起自己的线程 / 定时器，
    /// 必须在对象已经 moveToThread 到采集线程**之后**再建。
    QNetworkAccessManager *m_net = nullptr;
};

/// 插件本体：元数据 + 工厂。
///
/// 这就是"新增协议"要写的全部东西 —— 一个动态库，核心代码一行都不用动。
class HttpJsonProtocolPlugin : public QObject, public IProtocolPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID MonitorProtocolPlugin_iid)
    Q_INTERFACES(IProtocolPlugin)

public:
    QString id() const override { return QStringLiteral("http_json"); }
    QString displayName() const override { return QStringLiteral("HTTP / JSON 数据源"); }
    quint16 defaultPort() const override { return 8080; }
    QString description() const override
    {
        return QStringLiteral("轮询一个返回 JSON 对象的 HTTP 接口（GET 请求路径 → "
                              "{\"字段\": 数值}），字段名与点位表的 ID 对应。"
                              "许多现场网关的 REST 接口就是这个形态。");
    }
    ProtocolTraits traits() const override
    {
        ProtocolTraits traits;
        traits.usesEndpoint = true;
        traits.endpointLabel = QStringLiteral("请求路径"); // 同一个配置字段，换个叫法
        return traits;
    }
    DeviceConnection *create() const override { return new HttpJsonConnection(); }
};
