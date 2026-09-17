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
    int m_keepAliveSec = 60;
    int m_timeoutMs = 3000;
    quint16 m_packetId = 0;
    bool m_open = false;
    QHash<QString, QVariant> m_lastValues; ///< 最近一次收到的值，供 readTag 返回
};
