#include "storage/datastorage.h"

#include "utils/logger.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTextStream>
#include <QVariant>

namespace {

constexpr auto kConnectionName = "app-main";

/// 默认数据库位置：系统标准的应用数据目录
/// （Windows 上形如 %APPDATA%\<组织>\<应用>\monitor.db）。
/// 不能用相对路径 —— 那会跟着程序的工作目录跑，换一种启动方式就找不到数据。
QString defaultDatabasePath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(dir).filePath(QStringLiteral("monitor.db"));
}

/// 点位表 → JSON（存进 devices.points 列）。
QString pointsToJson(const QList<TagPoint> &points)
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
    return QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
}

/// JSON → 点位表。
QList<TagPoint> pointsFromJson(const QString &text)
{
    QList<TagPoint> points;
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8());
    if (!doc.isArray())
        return points;

    const QJsonArray array = doc.array();
    for (const QJsonValue &value : array) {
        const QJsonObject object = value.toObject();

        TagPoint point;
        point.id = object.value(QStringLiteral("id")).toString();
        if (point.id.isEmpty())
            continue;
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

} // namespace

DataStorage::DataStorage(QObject *parent, const QString &databasePath)
    : QObject(parent)
    , m_databasePath(databasePath.isEmpty() ? defaultDatabasePath() : databasePath)
{
}

DataStorage::~DataStorage()
{
    close();
}

bool DataStorage::open()
{
    if (m_db.isOpen())
        return true;

    const QFileInfo info(m_databasePath);
    QDir().mkpath(info.absolutePath());

    if (QSqlDatabase::contains(QLatin1String(kConnectionName)))
        m_db = QSqlDatabase::database(QLatin1String(kConnectionName));
    else
        m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QLatin1String(kConnectionName));

    m_db.setDatabaseName(m_databasePath);

    if (!m_db.open()) {
        m_lastError = m_db.lastError().text();
        Log::error(QStringLiteral("数据库打开失败: %1").arg(m_lastError));
        return false;
    }

    if (!createTables()) {
        m_db.close();
        return false;
    }

    Log::info(QStringLiteral("历史数据库已就绪: %1").arg(m_databasePath));
    return true;
}

void DataStorage::close()
{
    if (m_db.isOpen())
        m_db.close();
}

bool DataStorage::isOpen() const
{
    return m_db.isOpen();
}

bool DataStorage::createTables()
{
    QSqlQuery query(m_db);

    const QStringList statements = {
        QStringLiteral("CREATE TABLE IF NOT EXISTS samples ("
                       "  id     INTEGER PRIMARY KEY AUTOINCREMENT,"
                       "  ts     DATETIME NOT NULL,"
                       "  device TEXT     NOT NULL,"
                       "  tag    TEXT     NOT NULL,"
                       "  value  REAL     NOT NULL"
                       ")"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS alarms ("
                       "  id           INTEGER PRIMARY KEY AUTOINCREMENT,"
                       "  ts           DATETIME NOT NULL,"
                       "  device       TEXT     NOT NULL,"
                       "  tag          TEXT     NOT NULL,"
                       "  level        INTEGER  NOT NULL,"
                       "  value        REAL     NOT NULL,"
                       "  low_limit    REAL     NOT NULL,"
                       "  high_limit   REAL     NOT NULL,"
                       "  message      TEXT,"
                       "  active       INTEGER  NOT NULL,"
                       "  acknowledged INTEGER  NOT NULL"
                       ")"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS operations ("
                       "  id       INTEGER PRIMARY KEY AUTOINCREMENT,"
                       "  ts       DATETIME NOT NULL,"
                       "  username TEXT,"
                       "  device   TEXT,"
                       "  tag      TEXT,"
                       "  action   TEXT,"
                       "  detail   TEXT,"
                       "  success  INTEGER"
                       ")"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS devices ("
                       "  id         TEXT PRIMARY KEY,"
                       "  name       TEXT,"
                       "  host       TEXT,"
                       "  port       INTEGER,"
                       "  slave      INTEGER,"
                       "  protocol   INTEGER,"
                       "  poll_ms    INTEGER,"
                       "  mqtt_topic TEXT,"
                       "  points     TEXT"
                       ")"),
    };

    for (const QString &sql : statements) {
        if (!query.exec(sql)) {
            m_lastError = query.lastError().text();
            Log::error(QStringLiteral("建表失败: %1").arg(m_lastError));
            return false;
        }
    }

    query.exec(QStringLiteral(
        "CREATE INDEX IF NOT EXISTS idx_samples_lookup ON samples(device, tag, ts)"));
    query.exec(QStringLiteral(
        "CREATE INDEX IF NOT EXISTS idx_alarms_lookup ON alarms(device, tag, ts)"));
    return true;
}

// ============================ 采样 ============================

bool DataStorage::insertSample(const QString &deviceId,
                               const QString &tagId,
                               double value,
                               const QDateTime &time)
{
    if (!m_db.isOpen()) {
        m_lastError = QStringLiteral("数据库未打开");
        return false;
    }

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT INTO samples (ts, device, tag, value) VALUES (:ts, :device, :tag, :value)"));
    query.bindValue(QStringLiteral(":ts"), (time.isValid() ? time : QDateTime::currentDateTime()));
    query.bindValue(QStringLiteral(":device"), deviceId);
    query.bindValue(QStringLiteral(":tag"), tagId);
    query.bindValue(QStringLiteral(":value"), value);

    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return false;
    }
    return true;
}

QList<DataStorage::Sample> DataStorage::querySamples(const QString &deviceId,
                                                     const QString &tagId,
                                                     const QDateTime &from,
                                                     const QDateTime &to,
                                                     int limit) const
{
    QList<Sample> result;
    if (!m_db.isOpen())
        return result;

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "SELECT ts, device, tag, value FROM samples"
        " WHERE device = :device AND tag = :tag AND ts BETWEEN :from AND :to"
        " ORDER BY ts ASC LIMIT :limit"));
    query.bindValue(QStringLiteral(":device"), deviceId);
    query.bindValue(QStringLiteral(":tag"), tagId);
    query.bindValue(QStringLiteral(":from"), from);
    query.bindValue(QStringLiteral(":to"), to);
    query.bindValue(QStringLiteral(":limit"), limit);

    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return result;
    }

    while (query.next()) {
        Sample sample;
        sample.time = query.value(0).toDateTime();
        sample.deviceId = query.value(1).toString();
        sample.tagId = query.value(2).toString();
        sample.value = query.value(3).toDouble();
        result.append(sample);
    }
    return result;
}

bool DataStorage::exportSamplesCsv(const QString &deviceId,
                                   const QString &tagId,
                                   const QDateTime &from,
                                   const QDateTime &to,
                                   const QString &filePath) const
{
    const QList<Sample> samples = querySamples(deviceId, tagId, from, to, 1000000);

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        m_lastError = file.errorString();
        return false;
    }

    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);
    out << QChar(0xFEFF); // BOM：让 Excel 正确识别 UTF-8 中文

    out << QStringLiteral("时间,设备,点位,数值\n");
    for (const Sample &sample : samples) {
        out << sample.time.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) << ','
            << sample.deviceId << ','
            << sample.tagId << ','
            << QString::number(sample.value, 'f', 3) << '\n';
    }

    file.close();
    Log::info(QStringLiteral("已导出 %1 条历史数据到 %2").arg(samples.size()).arg(filePath));
    return true;
}

// ============================ 告警历史 ============================

bool DataStorage::insertAlarm(const AlarmRecord &record)
{
    if (!m_db.isOpen())
        return false;

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT INTO alarms (ts, device, tag, level, value, low_limit, high_limit,"
        " message, active, acknowledged)"
        " VALUES (:ts, :device, :tag, :level, :value, :low, :high, :message, :active, :ack)"));
    query.bindValue(QStringLiteral(":ts"), record.time);
    query.bindValue(QStringLiteral(":device"), record.deviceId);
    query.bindValue(QStringLiteral(":tag"), record.tagId);
    query.bindValue(QStringLiteral(":level"), int(record.level));
    query.bindValue(QStringLiteral(":value"), record.value);
    query.bindValue(QStringLiteral(":low"), record.lowLimit);
    query.bindValue(QStringLiteral(":high"), record.highLimit);
    query.bindValue(QStringLiteral(":message"), record.message);
    query.bindValue(QStringLiteral(":active"), record.active ? 1 : 0);
    query.bindValue(QStringLiteral(":ack"), record.acknowledged ? 1 : 0);

    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return false;
    }
    return true;
}

bool DataStorage::markAlarmInactive(const QString &deviceId, const QString &tagId)
{
    if (!m_db.isOpen())
        return false;

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "UPDATE alarms SET active = 0"
        " WHERE id = (SELECT id FROM alarms WHERE device = :device AND tag = :tag"
        "             ORDER BY ts DESC LIMIT 1)"));
    query.bindValue(QStringLiteral(":device"), deviceId);
    query.bindValue(QStringLiteral(":tag"), tagId);

    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return false;
    }
    return true;
}

bool DataStorage::markAlarmAcknowledged(const QString &deviceId, const QString &tagId)
{
    if (!m_db.isOpen())
        return false;

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "UPDATE alarms SET acknowledged = 1"
        " WHERE id = (SELECT id FROM alarms WHERE device = :device AND tag = :tag"
        "             ORDER BY ts DESC LIMIT 1)"));
    query.bindValue(QStringLiteral(":device"), deviceId);
    query.bindValue(QStringLiteral(":tag"), tagId);

    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return false;
    }
    return true;
}

QList<AlarmRecord> DataStorage::queryAlarms(const QDateTime &from,
                                            const QDateTime &to,
                                            int limit) const
{
    QList<AlarmRecord> result;
    if (!m_db.isOpen())
        return result;

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "SELECT ts, device, tag, level, value, low_limit, high_limit, message, active, acknowledged"
        " FROM alarms WHERE ts BETWEEN :from AND :to ORDER BY ts DESC LIMIT :limit"));
    query.bindValue(QStringLiteral(":from"), from);
    query.bindValue(QStringLiteral(":to"), to);
    query.bindValue(QStringLiteral(":limit"), limit);

    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return result;
    }

    while (query.next()) {
        AlarmRecord record;
        record.time = query.value(0).toDateTime();
        record.deviceId = query.value(1).toString();
        record.tagId = query.value(2).toString();
        record.level = static_cast<AlarmLevel>(query.value(3).toInt());
        record.value = query.value(4).toDouble();
        record.lowLimit = query.value(5).toDouble();
        record.highLimit = query.value(6).toDouble();
        record.message = query.value(7).toString();
        record.active = query.value(8).toInt() != 0;
        record.acknowledged = query.value(9).toInt() != 0;
        result.append(record);
    }
    return result;
}

bool DataStorage::clearAlarms()
{
    if (!m_db.isOpen())
        return false;

    QSqlQuery query(m_db);
    if (!query.exec(QStringLiteral("DELETE FROM alarms"))) {
        m_lastError = query.lastError().text();
        return false;
    }
    return true;
}

// ============================ 操作留痕 ============================

bool DataStorage::insertOperation(const OperationEntry &entry)
{
    if (!m_db.isOpen())
        return false;

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT INTO operations (ts, username, device, tag, action, detail, success)"
        " VALUES (:ts, :user, :device, :tag, :action, :detail, :success)"));
    query.bindValue(QStringLiteral(":ts"), entry.time.isValid() ? entry.time : QDateTime::currentDateTime());
    query.bindValue(QStringLiteral(":user"), entry.user);
    query.bindValue(QStringLiteral(":device"), entry.deviceId);
    query.bindValue(QStringLiteral(":tag"), entry.tagId);
    query.bindValue(QStringLiteral(":action"), entry.action);
    query.bindValue(QStringLiteral(":detail"), entry.detail);
    query.bindValue(QStringLiteral(":success"), entry.success ? 1 : 0);

    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return false;
    }
    return true;
}

QList<DataStorage::OperationEntry> DataStorage::queryOperations(int limit) const
{
    QList<OperationEntry> result;
    if (!m_db.isOpen())
        return result;

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "SELECT ts, username, device, tag, action, detail, success"
        " FROM operations ORDER BY ts DESC LIMIT :limit"));
    query.bindValue(QStringLiteral(":limit"), limit);

    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return result;
    }

    while (query.next()) {
        OperationEntry entry;
        entry.time = query.value(0).toDateTime();
        entry.user = query.value(1).toString();
        entry.deviceId = query.value(2).toString();
        entry.tagId = query.value(3).toString();
        entry.action = query.value(4).toString();
        entry.detail = query.value(5).toString();
        entry.success = query.value(6).toInt() != 0;
        result.append(entry);
    }
    return result;
}

// ============================ 设备台账 ============================

bool DataStorage::saveDevice(const DeviceInfo &device)
{
    if (!m_db.isOpen())
        return false;

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "REPLACE INTO devices (id, name, host, port, slave, protocol, poll_ms, mqtt_topic, points)"
        " VALUES (:id, :name, :host, :port, :slave, :protocol, :poll, :topic, :points)"));
    query.bindValue(QStringLiteral(":id"), device.id);
    query.bindValue(QStringLiteral(":name"), device.name);
    query.bindValue(QStringLiteral(":host"), device.host);
    query.bindValue(QStringLiteral(":port"), int(device.port));
    query.bindValue(QStringLiteral(":slave"), device.slaveId);
    query.bindValue(QStringLiteral(":protocol"), int(device.protocol));
    query.bindValue(QStringLiteral(":poll"), device.pollIntervalMs);
    query.bindValue(QStringLiteral(":topic"), device.mqttTopic);
    query.bindValue(QStringLiteral(":points"), pointsToJson(device.points));

    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return false;
    }
    return true;
}

bool DataStorage::removeDevice(const QString &id)
{
    if (!m_db.isOpen())
        return false;

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("DELETE FROM devices WHERE id = :id"));
    query.bindValue(QStringLiteral(":id"), id);

    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return false;
    }
    return true;
}

QList<DeviceInfo> DataStorage::loadDevices() const
{
    QList<DeviceInfo> result;
    if (!m_db.isOpen())
        return result;

    QSqlQuery query(m_db);
    if (!query.exec(QStringLiteral(
            "SELECT id, name, host, port, slave, protocol, poll_ms, mqtt_topic, points FROM devices"))) {
        m_lastError = query.lastError().text();
        return result;
    }

    while (query.next()) {
        DeviceInfo info;
        info.id = query.value(0).toString();
        info.name = query.value(1).toString();
        info.host = query.value(2).toString();
        info.port = static_cast<quint16>(query.value(3).toInt());
        info.slaveId = query.value(4).toInt();
        info.protocol = static_cast<DeviceProtocol>(query.value(5).toInt());
        info.pollIntervalMs = query.value(6).toInt();
        info.mqttTopic = query.value(7).toString();
        info.points = pointsFromJson(query.value(8).toString());
        result.append(info);
    }
    return result;
}

// ============================ 统计 ============================

QList<DataStorage::DeviceStats> DataStorage::queryDeviceStats(const QDateTime &from,
                                                              const QDateTime &to) const
{
    QList<DeviceStats> result;
    if (!m_db.isOpen())
        return result;

    QHash<QString, DeviceStats> byDevice;

    QSqlQuery sampleQuery(m_db);
    sampleQuery.prepare(QStringLiteral(
        "SELECT device, COUNT(*), MIN(ts), MAX(ts) FROM samples"
        " WHERE ts BETWEEN :from AND :to GROUP BY device"));
    sampleQuery.bindValue(QStringLiteral(":from"), from);
    sampleQuery.bindValue(QStringLiteral(":to"), to);
    if (sampleQuery.exec()) {
        while (sampleQuery.next()) {
            DeviceStats stats;
            stats.deviceId = sampleQuery.value(0).toString();
            stats.sampleCount = sampleQuery.value(1).toLongLong();
            stats.firstSample = sampleQuery.value(2).toDateTime();
            stats.lastSample = sampleQuery.value(3).toDateTime();
            byDevice.insert(stats.deviceId, stats);
        }
    }

    QSqlQuery alarmQuery(m_db);
    alarmQuery.prepare(QStringLiteral(
        "SELECT device, COUNT(*) FROM alarms WHERE ts BETWEEN :from AND :to GROUP BY device"));
    alarmQuery.bindValue(QStringLiteral(":from"), from);
    alarmQuery.bindValue(QStringLiteral(":to"), to);
    if (alarmQuery.exec()) {
        while (alarmQuery.next()) {
            const QString deviceId = alarmQuery.value(0).toString();
            DeviceStats stats = byDevice.value(deviceId);
            stats.deviceId = deviceId;
            stats.alarmCount = alarmQuery.value(1).toInt();
            byDevice.insert(deviceId, stats);
        }
    }

    result = byDevice.values();
    return result;
}

QString DataStorage::lastError() const
{
    return m_lastError;
}
