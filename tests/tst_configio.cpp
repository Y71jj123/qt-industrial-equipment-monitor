#include <QtTest>

#include "utils/configio.h"

/// ConfigIo：点位表序列化与整份配置的往返。
///
/// 这块一旦漂移，症状是"导出的配置文件导不回来" —— 而且往往要到现场
/// 批量下发配置时才发现，所以必须有测试守着。
class TestConfigIo : public QObject
{
    Q_OBJECT

private slots:
    void pointsRoundTrip();
    void fullConfigRoundTrip();
    void keepsMqttCredentials();
    void rejectsFutureVersion();
    void rejectsEmptyDeviceList();
};

void TestConfigIo::pointsRoundTrip()
{
    const QList<TagPoint> points = defaultTagPoints();
    QVERIFY(!points.isEmpty());

    const QList<TagPoint> back = ConfigIo::pointsFromJson(ConfigIo::pointsToJson(points));
    QCOMPARE(int(back.size()), int(points.size()));

    for (int i = 0; i < points.size(); ++i) {
        QCOMPARE(back.at(i).id, points.at(i).id);
        QCOMPARE(back.at(i).name, points.at(i).name);
        QCOMPARE(back.at(i).unit, points.at(i).unit);
        QCOMPARE(back.at(i).address, points.at(i).address);
        QCOMPARE(back.at(i).registerType, points.at(i).registerType);
        QCOMPARE(back.at(i).scale, points.at(i).scale);
        QCOMPARE(back.at(i).boolean, points.at(i).boolean);
    }
}

void TestConfigIo::fullConfigRoundTrip()
{
    DeviceInfo device;
    device.id = QStringLiteral("dev-1");
    device.name = QStringLiteral("1 号空压机");
    device.group = QStringLiteral("空压站");
    device.protocol = DeviceProtocol::ModbusTcp;
    device.host = QStringLiteral("192.168.1.10");
    device.port = 502;
    device.slaveId = 3;
    device.pollIntervalMs = 500;
    device.points = defaultTagPoints();

    AlarmRule rule;
    rule.deviceId = device.id;
    rule.tagId = QStringLiteral("temperature");
    rule.highLimit = 65.0;
    rule.rateLimit = 2.5;

    const QJsonObject json =
        ConfigIo::toJson({device}, {QStringLiteral("空压站"), QStringLiteral("水泵房")}, {rule});

    ConfigIo::Config parsed;
    QString error;
    QVERIFY2(ConfigIo::fromJson(json, &parsed, &error), qPrintable(error));

    QCOMPARE(int(parsed.devices.size()), 1);
    QCOMPARE(int(parsed.groups.size()), 2);
    QCOMPARE(int(parsed.rules.size()), 1);

    const DeviceInfo back = parsed.devices.at(0);
    QCOMPARE(back.id, device.id);
    QCOMPARE(back.name, device.name);
    QCOMPARE(back.groupName(), device.group);
    QCOMPARE(int(back.protocol), int(DeviceProtocol::ModbusTcp));
    QCOMPARE(back.host, device.host);
    QCOMPARE(int(back.port), int(device.port));
    QCOMPARE(back.slaveId, device.slaveId);
    QCOMPARE(back.pollIntervalMs, device.pollIntervalMs);
    QCOMPARE(int(back.points.size()), int(device.points.size()));

    QCOMPARE(parsed.rules.at(0).tagId, rule.tagId);
    QCOMPARE(parsed.rules.at(0).highLimit, rule.highLimit);
    QCOMPARE(parsed.rules.at(0).rateLimit, rule.rateLimit);
}

void TestConfigIo::keepsMqttCredentials()
{
    DeviceInfo device;
    device.id = QStringLiteral("dev-mqtt");
    device.name = QStringLiteral("MQTT 网关");
    device.protocol = DeviceProtocol::Mqtt;
    device.host = QStringLiteral("broker.emqx.io");
    device.port = 1883;
    device.mqttTopic = QStringLiteral("factory/line1/#");
    device.username = QStringLiteral("factory");
    device.password = QStringLiteral("s3cret");
    device.points = defaultTagPoints();

    const QJsonObject json = ConfigIo::toJson({device}, {}, {});

    ConfigIo::Config parsed;
    QString error;
    QVERIFY2(ConfigIo::fromJson(json, &parsed, &error), qPrintable(error));
    QCOMPARE(int(parsed.devices.size()), 1);

    const DeviceInfo back = parsed.devices.at(0);
    QCOMPARE(back.username, device.username);
    QCOMPARE(back.password, device.password);
    QCOMPARE(back.mqttTopic, device.mqttTopic);
    QVERIFY(back.hasCredentials());
}

void TestConfigIo::rejectsFutureVersion()
{
    DeviceInfo device;
    device.id = QStringLiteral("dev-1");
    device.name = QStringLiteral("设备");
    device.host = QStringLiteral("127.0.0.1");

    QJsonObject json = ConfigIo::toJson({device}, {}, {});
    json.insert(QStringLiteral("version"), ConfigIo::kFormatVersion + 1);

    ConfigIo::Config parsed;
    QString error;
    // 宁可报错，也不要按老规则去误读将来才有的字段
    QVERIFY(!ConfigIo::fromJson(json, &parsed, &error));
    QVERIFY(!error.isEmpty());
}

void TestConfigIo::rejectsEmptyDeviceList()
{
    // 一份"没有任何设备"的配置如果被放行，导入就等于把现场台账清空了。
    const QJsonObject json = ConfigIo::toJson({}, {QStringLiteral("空压站")}, {});

    ConfigIo::Config parsed;
    QString error;
    QVERIFY(!ConfigIo::fromJson(json, &parsed, &error));
    QVERIFY(!error.isEmpty());
}

QTEST_GUILESS_MAIN(TestConfigIo)

#include "tst_configio.moc"
