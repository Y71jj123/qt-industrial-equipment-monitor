#include "storage/datastorage.h"

#include "utils/logger.h"

#include <QDir>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace {
constexpr auto kConnectionName = "app-main";
}

DataStorage::DataStorage(QObject *parent, const QString &databasePath)
    : QObject(parent)
    , m_databasePath(databasePath.isEmpty()
                         ? QDir::current().filePath(QStringLiteral("data/monitor.db"))
                         : databasePath)
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
    const QString sql = QStringLiteral(
        "CREATE TABLE IF NOT EXISTS samples ("
        "  id       INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  ts       DATETIME NOT NULL,"
        "  device   TEXT     NOT NULL,"
        "  tag      TEXT     NOT NULL,"
        "  value    REAL     NOT NULL"
        ")");

    if (!query.exec(sql)) {
        m_lastError = query.lastError().text();
        Log::error(QStringLiteral("建表失败: %1").arg(m_lastError));
        return false;
    }

    query.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_samples_lookup ON samples(device, tag, ts)"));
    return true;
}

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

QString DataStorage::lastError() const
{
    return m_lastError;
}
