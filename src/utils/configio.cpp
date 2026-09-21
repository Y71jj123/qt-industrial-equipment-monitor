#include "utils/configio.h"

#include "comm/protocolregistry.h"

#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QJsonValue>

namespace ConfigIo {

QJsonArray pointsToJson(const QList<TagPoint> &points)
{
    QJsonArray array;
    for (const TagPoint &point : points) {
        QJsonObject object;
        object.insert(QStringLiteral("id"), point.id);
        object.insert(QStringLiteral("name"), point.name);
        object.insert(QStringLiteral("unit"), point.unit);
        object.insert(QStringLiteral("address"), point.address);
        object.insert(QStringLiteral("registerType"), point.registerType);
        object.insert(QStringLiteral("scale"), point.scale);
        object.insert(QStringLiteral("boolean"), point.boolean);
        array.append(object);
    }
    return array;
}

QList<TagPoint> pointsFromJson(const QJsonArray &array)
{
    QList<TagPoint> points;
    for (const QJsonValue &value : array) {
        const QJsonObject object = value.toObject();

        TagPoint point;
        point.id = object.value(QStringLiteral("id")).toString().trimmed();
        if (point.id.isEmpty())
            continue; // 没有 ID 的点位无法寻址，直接跳过而不是留个空壳

        point.name = object.value(QStringLiteral("name")).toString();
        point.unit = object.value(QStringLiteral("unit")).toString();
        point.address = object.value(QStringLiteral("address")).toInt();
        point.registerType = object.value(QStringLiteral("registerType")).toInt(3);
        point.scale = object.value(QStringLiteral("scale")).toDouble(1.0);
        point.boolean = object.value(QStringLiteral("boolean")).toBool();
        points.append(point);
    }
    return points;
}

namespace {

QJsonObject deviceToJson(const DeviceInfo &device)
{
    QJsonObject object;
    object.insert(QStringLiteral("id"), device.id);
    object.insert(QStringLiteral("name"), device.name);
    object.insert(QStringLiteral("group"), device.groupName());
    object.insert(QStringLiteral("host"), device.host);
    object.insert(QStringLiteral("port"), int(device.port));
    object.insert(QStringLiteral("slaveId"), device.slaveId);
    object.insert(QStringLiteral("pollIntervalMs"), device.pollIntervalMs);
    object.insert(QStringLiteral("protocolId"), device.protocolId);
    // 老的整型字段照旧写一份：旧版本的导出文件能被新版本读，
    // 新版本的导出文件也尽量别让旧版本reader一头雾水（认不出就落默认协议）。
    object.insert(QStringLiteral("protocol"), ProtocolRegistry::legacyIntFromId(device.protocolId));
    object.insert(QStringLiteral("mqttTopic"), device.mqttTopic);
    object.insert(QStringLiteral("username"), device.username);
    object.insert(QStringLiteral("password"), device.password);
    object.insert(QStringLiteral("baudRate"), device.baudRate);
    object.insert(QStringLiteral("dataBits"), device.dataBits);
    object.insert(QStringLiteral("parity"), device.parity);
    object.insert(QStringLiteral("stopBits"), device.stopBits);
    object.insert(QStringLiteral("points"), pointsToJson(device.points));
    return object;
}

DeviceInfo deviceFromJson(const QJsonObject &object)
{
    DeviceInfo device;
    device.id = object.value(QStringLiteral("id")).toString();
    device.name = object.value(QStringLiteral("name")).toString();
    device.group = object.value(QStringLiteral("group")).toString();
    device.host = object.value(QStringLiteral("host")).toString();
    device.port = static_cast<quint16>(object.value(QStringLiteral("port")).toInt(502));
    device.slaveId = object.value(QStringLiteral("slaveId")).toInt(1);
    device.pollIntervalMs = object.value(QStringLiteral("pollIntervalMs")).toInt(1000);
    // 协议：新格式是字符串 id；老格式只有整型枚举，读到时按冻结表映射一次，
    // 这样老导出文件不用改也还能导入。
    const QString protocolId = object.value(QStringLiteral("protocolId")).toString();
    if (!protocolId.isEmpty()) {
        device.protocolId = protocolId;
    } else {
        device.protocolId = ProtocolRegistry::idFromLegacyInt(
            object.value(QStringLiteral("protocol")).toInt(0));
    }
    device.mqttTopic = object.value(QStringLiteral("mqttTopic")).toString();
    device.username = object.value(QStringLiteral("username")).toString();
    device.password = object.value(QStringLiteral("password")).toString();
    device.baudRate = object.value(QStringLiteral("baudRate")).toInt(9600);
    device.dataBits = object.value(QStringLiteral("dataBits")).toInt(8);
    device.parity = object.value(QStringLiteral("parity")).toInt(0);
    device.stopBits = object.value(QStringLiteral("stopBits")).toInt(1);
    device.points = pointsFromJson(object.value(QStringLiteral("points")).toArray());

    // 采集周期 0 会把采集定时器变成忙循环，兜一个下限
    if (device.pollIntervalMs < 100)
        device.pollIntervalMs = 1000;

    return device;
}

QJsonObject ruleToJson(const AlarmRule &rule)
{
    QJsonObject object;
    object.insert(QStringLiteral("deviceId"), rule.deviceId);
    object.insert(QStringLiteral("tagId"), rule.tagId);
    object.insert(QStringLiteral("kind"), int(rule.kind));
    object.insert(QStringLiteral("lowLimit"), rule.lowLimit);
    object.insert(QStringLiteral("highLimit"), rule.highLimit);
    object.insert(QStringLiteral("rateLimit"), rule.rateLimit);
    object.insert(QStringLiteral("enabled"), rule.enabled);
    return object;
}

AlarmRule ruleFromJson(const QJsonObject &object)
{
    AlarmRule rule;
    rule.deviceId = object.value(QStringLiteral("deviceId")).toString();
    rule.tagId = object.value(QStringLiteral("tagId")).toString();
    rule.kind = static_cast<AlarmKind>(object.value(QStringLiteral("kind")).toInt(0));
    rule.lowLimit = object.value(QStringLiteral("lowLimit")).toDouble(0.0);
    rule.highLimit = object.value(QStringLiteral("highLimit")).toDouble(100.0);
    rule.rateLimit = object.value(QStringLiteral("rateLimit")).toDouble(0.0);
    rule.enabled = object.value(QStringLiteral("enabled")).toBool(true);
    return rule;
}

} // namespace

QJsonObject toJson(const QList<DeviceInfo> &devices,
                   const QStringList &groups,
                   const QList<AlarmRule> &rules)
{
    QJsonArray deviceArray;
    for (const DeviceInfo &device : devices)
        deviceArray.append(deviceToJson(device));

    QJsonArray groupArray;
    for (const QString &group : groups)
        groupArray.append(group);

    QJsonArray ruleArray;
    for (const AlarmRule &rule : rules)
        ruleArray.append(ruleToJson(rule));

    QJsonObject root;
    root.insert(QStringLiteral("format"), QStringLiteral("qt-industrial-equipment-monitor/config"));
    root.insert(QStringLiteral("version"), kFormatVersion);
    root.insert(QStringLiteral("exportedAt"),
                QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
    root.insert(QStringLiteral("groups"), groupArray);
    root.insert(QStringLiteral("devices"), deviceArray);
    root.insert(QStringLiteral("rules"), ruleArray);
    return root;
}

bool fromJson(const QJsonObject &object, Config *config, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };

    if (!config)
        return fail(QStringLiteral("内部错误：配置输出参数为空"));

    const int version = object.value(QStringLiteral("version")).toInt(0);
    if (version <= 0)
        return fail(QStringLiteral("不是有效的配置文件：缺少 version 字段"));
    if (version > kFormatVersion)
        return fail(QStringLiteral("配置文件版本 %1 高于本程序支持的 %2，请升级软件后再导入")
                        .arg(version)
                        .arg(kFormatVersion));

    Config parsed;
    for (const QJsonValue &value : object.value(QStringLiteral("groups")).toArray()) {
        const QString name = value.toString().trimmed();
        if (!name.isEmpty() && !parsed.groups.contains(name))
            parsed.groups.append(name);
    }

    for (const QJsonValue &value : object.value(QStringLiteral("devices")).toArray()) {
        const DeviceInfo device = deviceFromJson(value.toObject());
        if (device.name.isEmpty() && device.id.isEmpty())
            continue; // 空对象，跳过

        parsed.devices.append(device);
    }

    for (const QJsonValue &value : object.value(QStringLiteral("rules")).toArray()) {
        const AlarmRule rule = ruleFromJson(value.toObject());
        if (rule.deviceId.isEmpty() || rule.tagId.isEmpty())
            continue;

        parsed.rules.append(rule);
    }

    if (parsed.devices.isEmpty())
        return fail(QStringLiteral("配置文件里没有任何设备，已取消导入"));

    *config = parsed;
    return true;
}

bool saveToFile(const QString &filePath, const QJsonObject &object, QString *error)
{
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error)
            *error = QStringLiteral("无法写入文件：%1").arg(file.errorString());
        return false;
    }

    // 缩进输出：现场排查时用记事本就能看懂，也方便手工改两台设备再发下去
    const QByteArray data = QJsonDocument(object).toJson(QJsonDocument::Indented);
    if (file.write(data) != data.size()) {
        if (error)
            *error = QStringLiteral("写入不完整：%1").arg(file.errorString());
        return false;
    }

    file.close();
    return true;
}

bool loadFromFile(const QString &filePath, QJsonObject *object, QString *error)
{
    if (!object) {
        if (error)
            *error = QStringLiteral("内部错误：输出参数为空");
        return false;
    }

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("无法打开文件：%1").arg(file.errorString());
        return false;
    }

    const QByteArray data = file.readAll();
    file.close();

    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(data, &parseError);
    if (document.isNull() || !document.isObject()) {
        if (error)
            *error = QStringLiteral("JSON 解析失败：%1").arg(parseError.errorString());
        return false;
    }

    *object = document.object();
    return true;
}

} // namespace ConfigIo
