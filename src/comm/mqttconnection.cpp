#include "comm/mqttconnection.h"

#include "utils/logger.h"

#include <QAbstractSocket>
#include <QElapsedTimer>
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

/// CONNACK 返回码的中文解释（3.1.1 规范）。
///
/// 单独翻译的意义在于：填错账号密码时 broker 会回返回码 4/5，
/// 不翻译的话界面上只剩"未收到 CONNACK"，让人以为是网络问题。
QString connAckReason(int code)
{
    switch (code) {
    case 0:
        return QStringLiteral("连接已接受");
    case 1:
        return QStringLiteral("协议版本不支持");
    case 2:
        return QStringLiteral("客户端标识不合法");
    case 3:
        return QStringLiteral("服务端不可用");
    case 4:
        return QStringLiteral("用户名或密码错误");
    case 5:
        return QStringLiteral("未授权");
    default:
        return QStringLiteral("未知原因");
    }
}

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

    // 接入账号：空即匿名。密码单独判空 —— 有些 broker 允许"有用户名、无密码"。
    setCredentials(device.username, device.password);
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

    // 等 CONNACK。
    //
    // ⚠️ 这里**不能**去读 socket 缓冲区：CONNACK 会先被 readyRead → onReadyRead 收走
    // 并放进 m_buffer，open() 里再 readAll() 只会拿到空数组 —— 早先就是这么
    // 把"账号密码错误"当成连接成功的。正确做法是等 onReadyRead 把结论写进标志位。
    m_connAckReceived = false;
    m_connRejected = false;

    QElapsedTimer elapsed;
    elapsed.start();
    while (!m_connAckReceived && !m_connRejected && elapsed.elapsed() < m_timeoutMs) {
        const qint64 remaining = m_timeoutMs - elapsed.elapsed();
        m_socket->waitForReadyRead(int(qMax<qint64>(1, remaining)));
    }

    if (m_connRejected) {
        // 具体原因由 onReadyRead 报过了，这里只负责把"建连失败"传回去
        close();
        return false;
    }

    if (!m_connAckReceived)
        emit errorOccurred(QStringLiteral("MQTT 未收到 CONNACK，仍尝试订阅 %1").arg(m_topic));

    sendSubscribe();
    m_open = true;
    Log::info(QStringLiteral("MQTT 已连接 %1:%2（订阅 %3%4）")
                  .arg(host)
                  .arg(port)
                  .arg(m_topic)
                  .arg(m_username.isEmpty() ? QString()
                                            : QStringLiteral("，账号 %1").arg(m_username)));
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
    const bool hasUser = !m_username.isEmpty();
    const bool hasPass = !m_password.isEmpty();

    // 连接标志（3.1.1）第 7 位 = 有用户名，第 6 位 = 有密码。
    // 规范要求"密码标志置位时用户名标志必须置位"，所以只填了密码也要把用户名带上（空串）。
    quint8 flags = 0x02; // Clean Session
    if (hasUser || hasPass)
        flags |= 0x80;
    if (hasPass)
        flags |= 0x40;

    QByteArray variable;
    variable += encodeString(QStringLiteral("MQTT")); // 协议名
    variable.append(char(0x04));                      // 协议级别 4 = 3.1.1
    variable.append(char(flags));                     // 连接标志
    variable.append(char(quint8((m_keepAliveSec >> 8) & 0xFF)));
    variable.append(char(quint8(m_keepAliveSec & 0xFF)));

    // 载荷顺序是规范定死的：ClientId → [Will] → UserName → Password
    QByteArray payload;
    payload += encodeString(m_clientId);
    if (flags & 0x80)
        payload += encodeString(m_username);
    if (flags & 0x40)
        payload += encodeString(m_password);

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

    bool rejected = false;

    while (true) {
        if (m_buffer.size() < 2)
            break;

        int offset = 1;
        bool ok = false;
        const int remaining = decodeLength(m_buffer, offset, ok);
        if (!ok || m_buffer.size() < offset + remaining)
            break; // 半包，等更多数据

        const quint8 header = quint8(m_buffer.at(0));
        const int type = header >> 4;
        const QByteArray body = m_buffer.mid(offset, remaining);
        m_buffer.remove(0, offset + remaining);

        if (type == PacketConnAck) {
            // 载荷：连接确认标志(1 字节) + 返回码(1 字节)
            m_connAckReceived = true;
            if (body.size() >= 2) {
                const int code = quint8(body.at(1));
                if (code != 0) {
                    m_connRejected = true;
                    emit errorOccurred(QStringLiteral("MQTT 接入被拒绝：%1（返回码 %2）")
                                           .arg(connAckReason(code))
                                           .arg(code));
                    rejected = true;
                    break;
                }
            }
            continue;
        }

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
        // SUBACK / PINGRESP 等无需处理
    }

    // close() 会动到 socket，放在解析循环之外调用，免得边解析边把管子拆了
    if (rejected)
        close();
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
