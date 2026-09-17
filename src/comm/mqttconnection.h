#pragma once

#include "comm/deviceconnection.h"
#include "core/devicemanager.h"

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QVariant>

class QTcpSocket;
class QTimer;

/// MQTT 连接（协议版本 3.1.1，手写最小可用实现）。
///
/// Qt 官方 MQTT 模块未随本机 Qt 安装，故这里直接基于 QTcpSocket 实现
/// CONNECT / CONNACK / SUBSCRIBE / PUBLISH / PINGREQ 这一最小子集。
///
/// 工作模型是"订阅 → 接收上报"：连接后订阅 topic，网关/设备把数据以 PUBLISH 发来，
/// 本类解析后通过 tagValueChanged 抛出。支持两种常见上报格式：
///   - **单值**：订阅主题的末级作为点位 id，载荷是数字
///       主题 factory/line1/temperature，载荷 42.5  →  temperature = 42.5
///   - **JSON**：载荷是对象，逐字段上报
///       载荷 {"temperature":42.5,"pressure":53}   →  temperature / pressure 各来一次
/// 因此订阅时建议用通配主题，例如 `factory/line1/#`。
///
/// 接入鉴权：broker 要求账号时，通过 setCredentials() 传入用户名 / 密码，
/// CONNECT 报文会带上 User Name / Password 标志与字段（3.1.1 的明文形式，
/// 不做 TLS 时本质上等同于明文口令，只适合内网）。
class MqttConnection : public DeviceConnection
{
    Q_OBJECT

public:
    explicit MqttConnection(QObject *parent = nullptr);
    ~MqttConnection() override;

    void configure(const DeviceInfo &device) override;

    bool open(const QString &host, quint16 port) override;
    void close() override;
    bool isOpen() const override;

    bool readTag(const QString &tagId, QVariant &value) override;
    bool writeTag(const QString &tagId, const QVariant &value) override;

    void setClientId(const QString &clientId) { m_clientId = clientId; }
    void setKeepAliveSec(int seconds) { m_keepAliveSec = qMax(5, seconds); }

    /// 设置接入账号 / 密码（两者皆空即匿名接入）。
    ///
    /// 必须在 open() 之前调用 —— CONNECT 报文只在建连那一刻发一次，
    /// 连上之后再改就只剩下重连才生效，容易让人以为"填了没用"。
    void setCredentials(const QString &username, const QString &password)
    {
        m_username = username;
        m_password = password;
    }

    QString subscribeTopic() const { return m_topic; }

private:
    void sendConnect();
    void sendSubscribe();
    void sendPing();
    void onReadyRead();

    /// 解析一条 PUBLISH 并把其中的点位值抛出去。
    void handlePublish(const QByteArray &topic, const QByteArray &payload);

    /// 组包：把字节数组按 `2 字节长度 + 内容` 编码（MQTT 字符串）。
    static QByteArray encodeString(const QString &text);

    /// MQTT 剩余长度：7 位一组、最高位做延续标志。
    static QByteArray encodeLength(int length);
    static int decodeLength(const QByteArray &data, int &offset, bool &ok);

    QTcpSocket *m_socket = nullptr;
    QTimer *m_pingTimer = nullptr;

    QByteArray m_buffer; ///< 入站字节缓冲（处理 TCP 粘包 / 半包）
    QString m_host;
    quint16 m_port = 1883;
    QString m_clientId;
    QString m_topic;                      ///< 订阅主题（支持 + / # 通配）
    QString m_username;                   ///< 接入账号（空 = 匿名）
    QString m_password;                   ///< 接入密码
    int m_keepAliveSec = 60;
    int m_timeoutMs = 3000;
    quint16 m_packetId = 0;
    bool m_open = false;
    /// CONNACK 的到达情况。**必须由 onReadyRead 置位**：
    /// 建连后数据先经 readyRead → onReadyRead 进 m_buffer，
    /// open() 里再去读 socket 只会读到空 —— 这里用标志位把结论传回去。
    bool m_connAckReceived = false;
    bool m_connRejected = false;
    QHash<QString, QVariant> m_lastValues; ///< 最近一次收到的值，供 readTag 返回
};
