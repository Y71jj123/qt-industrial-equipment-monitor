#include "comm/mqttconnection.h"

#include "utils/logger.h"

#include <QAbstractSocket>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRandomGenerator>
#include <QTcpSocket>
#include <QTimer>

namespace {

// MQTT 控制报文类型（高 4 位）
enum MqttPacketType {
    PacketConnect = 1,
    PacketConnAck = 2,
    PacketPublish = 3,
    PacketSubscribe = 8,
};

} // namespace

MqttConnection::MqttConnection(QObject *parent)
    : DeviceConnection(parent)
{
}

MqttConnection::~MqttConnection()
{
    close();
}

QByteArray MqttConnection::encodeString(const QString &text)
{
    const QByteArray utf8 = text.toUtf8();
    QByteArray out;
    out.append(char(quint8((utf8.size() >> 8) & 0xFF)));
    out.append(char(quint8(utf8.size() & 0xFF)));
    out.append(utf8);
    return out;
}

QByteArray MqttConnection::encodeLength(int length)
{
    QByteArray out;
    do {
        quint8 byte = quint8(length % 128);
        length /= 128;
        if (length > 0)
            byte |= 0x80;
        out.append(char(byte));
    } while (length > 0);
    return out;
}

int MqttConnection::decodeLength(const QByteArray &data, int &offset, bool &ok)
{
    int multiplier = 1;
    int value = 0;
    quint8 byte = 0;

    do {
        if (offset >= data.size()) {
            ok = false;
            return 0;
        }
        byte = quint8(data.at(offset++));
        value += int(byte & 0x7F) * multiplier;
        multiplier *= 128;
        if (multiplier > 128 * 128 * 128) {
            ok = false;
            return 0;
        }
    } while (byte & 0x80);

    ok = true;
    return value;
}

void MqttConnection::configure(const DeviceInfo &device)
{
    m_topic = device.mqttTopic;
    if (m_topic.isEmpty()) {
        // 未指定主题时给一个带设备 id 前缀的通配主题，便于多设备区分。
        m_topic = QStringLiteral("dsh/%1/#").arg(device.id.left(8));
    }
}

bool MqttConnection::open(const QString &host, quint16 port)
{
    if (m_open)
        close();

    m_host = host;
    m_port = port;
    m_buffer.clear();

    if (!m_socket) {
        m_socket = new QTcpSocket(this);
        connect(m_socket, &QTcpSocket::readyRead, this, &MqttConnection::onReadyRead);
        connect(m_socket, &QTcpSocket::disconnected, this, [this]() {
            if (m_open) {
                m_open = false;
                if (m_pingTimer)
                    m_pingTimer->stop();
                emit errorOccurred(QStringLiteral("MQTT 连接已断开: %1:%2")
                                       .arg(m_host)
                                       .arg(m_port));
                emit closed();
            }
        });
    }

    m_socket->connectToHost(host, port);
    if (!m_socket->waitForConnected(m_timeoutMs)) {
        emit errorOccurred(QStringLiteral("MQTT 连接失败 %1:%2（%3）")
                               .arg(host)
                               .arg(port)
                               .arg(m_socket->errorString()));
        return false;
    }

    if (m_clientId.isEmpty())
        m_clientId = QStringLiteral("dsh-monitor-%1")
                         .arg(QRandomGenerator::global()->bounded(100000));

    sendConnect();

    // 等 CONNACK（固定头 0x20）。收不到不致命，继续往下走由 onReadyRead 兜底。
    if (m_socket->waitForReadyRead(m_timeoutMs)) {
        const QByteArray data = m_socket->readAll();
        if (data.isEmpty() || quint8(data.at(0)) != char(0x20)) {
            m_buffer.append(data); // 不是 CONNACK，交给统一解析器
        }
    } else {
        emit errorOccurred(QStringLiteral("MQTT 未收到 CONNACK，仍尝试订阅 %1").arg(m_topic));
    }

    sendSubscribe();
    m_open = true;
    Log::info(QStringLiteral("MQTT 已连接 %1:%2（订阅 %3）").arg(host).arg(port).arg(m_topic));
    emit opened();

    if (!m_pingTimer) {
        m_pingTimer = new QTimer(this);
        connect(m_pingTimer, &QTimer::timeout, this, &MqttConnection::sendPing);
    }
    m_pingTimer->start(qMax(5, m_keepAliveSec / 2) * 1000);
    return true;
}

void MqttConnection::close()
{
    if (m_pingTimer)
        m_pingTimer->stop();

    const bool wasOpen = m_open;
    m_open = false;

    if (m_socket && m_socket->state() != QAbstractSocket::UnconnectedState) {
        m_socket->disconnectFromHost();
        if (m_socket->state() != QAbstractSocket::UnconnectedState)
            m_socket->waitForDisconnected(200);
    }

    if (wasOpen) {
        Log::info(QStringLiteral("MQTT 已断开 %1:%2").arg(m_host).arg(m_port));
        emit closed();
    }
}

bool MqttConnection::isOpen() const
{
    return m_open;
}

void MqttConnection::sendConnect()
{
    QByteArray variable;
    variable += encodeString(QStringLiteral("MQTT")); // 协议名
    variable.append(char(0x04));                      // 协议级别 4 = 3.1.1
    variable.append(char(0x02));                      // 连接标志：Clean Session
    variable.append(char(quint8((m_keepAliveSec >> 8) & 0xFF)));
    variable.append(char(quint8(m_keepAliveSec & 0xFF)));

    QByteArray payload;
    payload += encodeString(m_clientId);

    QByteArray packet;
    packet.append(char(0x10)); // CONNECT
    packet += encodeLength(variable.size() + payload.size());
    packet += variable;
    packet += payload;

    m_socket->write(packet);
    m_socket->waitForBytesWritten(m_timeoutMs);
}

void MqttConnection::sendSubscribe()
{
    if (m_topic.isEmpty())
        return;

    m_packetId = quint16((m_packetId % 65535) + 1);

    QByteArray variable;
    variable.append(char(quint8((m_packetId >> 8) & 0xFF)));
    variable.append(char(quint8(m_packetId & 0xFF)));

    QByteArray payload;
    payload += encodeString(m_topic);
    payload.append(char(0x00)); // 请求 QoS 0

    QByteArray packet;
    packet.append(char(0x82)); // SUBSCRIBE
    packet += encodeLength(variable.size() + payload.size());
    packet += variable;
    packet += payload;

    m_socket->write(packet);
    m_socket->waitForBytesWritten(m_timeoutMs);
}

void MqttConnection::sendPing()
{
    if (!m_socket || m_socket->state() != QAbstractSocket::ConnectedState)
        return;

    QByteArray packet;
    packet.append(char(0xC0)); // PINGREQ
    packet.append(char(0x00));
    m_socket->write(packet);
}

void MqttConnection::onReadyRead()
{
    m_buffer += m_socket->readAll();

    while (true) {
        if (m_buffer.size() < 2)
            return;

        int offset = 1;
        bool ok = false;
        const int remaining = decodeLength(m_buffer, offset, ok);
        if (!ok || m_buffer.size() < offset + remaining)
            return; // 半包，等更多数据

        const quint8 header = quint8(m_buffer.at(0));
        const int type = header >> 4;
        const QByteArray body = m_buffer.mid(offset, remaining);
        m_buffer.remove(0, offset + remaining);

        if (type == PacketPublish) {
            if (body.size() < 2)
                continue;
            const int topicLength = (quint8(body.at(0)) << 8) | quint8(body.at(1));
            int pos = 2;
            if (body.size() < pos + topicLength)
                continue;
            const QByteArray topic = body.mid(pos, topicLength);
            pos += topicLength;
            const int qos = (header >> 1) & 0x03;
            if (qos > 0)
                pos += 2; // 跳过包标识
            if (pos > body.size())
                continue;
            handlePublish(topic, body.mid(pos));
        }
        // CONNACK / SUBACK / PINGRESP 等无需处理
    }
}

void MqttConnection::handlePublish(const QByteArray &topic, const QByteArray &payload)
{
    const QByteArray trimmed = payload.trimmed();

    // JSON 报文：逐字段上报
    if (trimmed.startsWith('{')) {
        QJsonParseError error{};
        const QJsonDocument doc = QJsonDocument::fromJson(trimmed, &error);
        if (error.error == QJsonParseError::NoError && doc.isObject()) {
            const QJsonObject object = doc.object();
            for (auto it = object.begin(); it != object.end(); ++it) {
                if (!it.value().isDouble())
                    continue;
                const double v = it.value().toDouble();
                m_lastValues.insert(it.key(), v);
                emit tagValueChanged(it.key(), v);
            }
            return;
        }
    }

    // 单值报文：主题末级即点位 id
    const QString tagId = QString::fromUtf8(topic).section(QLatin1Char('/'), -1);
    if (tagId.isEmpty())
        return;

    bool ok = false;
    const double v = QString::fromUtf8(trimmed).toDouble(&ok);
    if (!ok)
        return;

    m_lastValues.insert(tagId, v);
    emit tagValueChanged(tagId, v);
}

bool MqttConnection::readTag(const QString &tagId, QVariant &value)
{
    if (!m_lastValues.contains(tagId)) {
        emit errorOccurred(QStringLiteral("MQTT 读取失败：尚未收到点位 %1 的上报").arg(tagId));
        return false;
    }

    value = m_lastValues.value(tagId);
    return true;
}

bool MqttConnection::writeTag(const QString &tagId, const QVariant &value)
{
    if (!m_open) {
        emit errorOccurred(QStringLiteral("MQTT 写入失败：连接未建立"));
        return false;
    }

    // 发布到 `订阅主题 + / + 点位id`（去掉通配符），QoS 0。
    QString publishTopic = m_topic;
    if (publishTopic.endsWith(QLatin1Char('#')) || publishTopic.endsWith(QLatin1Char('+')))
        publishTopic.chop(1);
    if (!publishTopic.isEmpty() && !publishTopic.endsWith(QLatin1Char('/')))
        publishTopic += QLatin1Char('/');
    publishTopic += tagId;

    const QByteArray payload = value.toString().toUtf8();

    QByteArray variable;
    variable += encodeString(publishTopic);

    QByteArray packet;
    packet.append(char(0x30)); // PUBLISH, QoS 0, 不保留
    packet += encodeLength(variable.size() + payload.size());
    packet += variable;
    packet += payload;

    if (m_socket->write(packet) < 0) {
        emit errorOccurred(QStringLiteral("MQTT 发布失败：%1").arg(m_socket->errorString()));
        return false;
    }
    m_socket->waitForBytesWritten(m_timeoutMs);

    Log::info(QStringLiteral("MQTT 下发 %1 = %2").arg(publishTopic, value.toString()));
    return true;
}
