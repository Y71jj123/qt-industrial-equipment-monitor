#include <QtTest>

#include <QDir>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

#include "storage/datastorage.h"

/// DataStorage：告警工单落库、统计口径、采样溢出队列与启动补传。
class TestDataStorage : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void unhandledAlarmStoresNull();
    void alarmHandlingRoundTrip();
    void deviceStatsMttr();
    void sampleCounts();
    void samplesSpillWhenDatabaseUnavailable();
    void spillReplaysCompletelyAndInOrder();
    void spillSkipsBrokenLines();

private:
    QString dbPath() const { return m_dir.filePath(QStringLiteral("t.db")); }
    QString spillPath() const { return dbPath() + QStringLiteral(".pending.jsonl"); }
    void removeDatabaseFiles();

    static QDateTime wideFrom() { return QDateTime::currentDateTime().addDays(-400); }
    static QDateTime wideTo() { return QDateTime::currentDateTime().addDays(1); }

    QTemporaryDir m_dir;
};

void TestDataStorage::init()
{
    removeDatabaseFiles();
}

void TestDataStorage::cleanup()
{
    removeDatabaseFiles();
}

void TestDataStorage::removeDatabaseFiles()
{
    QFile::remove(dbPath());
    QFile::remove(spillPath());
}

void TestDataStorage::unhandledAlarmStoresNull()
{
    DataStorage storage(nullptr, dbPath());
    QVERIFY(storage.open());

    AlarmRecord record;
    record.time = QDateTime::currentDateTime();
    record.deviceId = QStringLiteral("dev-1");
    record.tagId = QStringLiteral("temperature");
    record.level = AlarmLevel::Critical;
    record.value = 99.0;
    record.message = QStringLiteral("温度超高");
    record.active = true;
    QVERIFY(storage.insertAlarm(record));

    const QList<AlarmRecord> back = storage.queryAlarms(wideFrom(), wideTo(), 10);
    QCOMPARE(int(back.size()), 1);
    QVERIFY2(!back.at(0).handled(), "未处理的告警不能被当成已处理");

    // 数据库层面确认真的写的是 NULL：若落了 disposition 的默认值 0（已处理恢复），
    // 报表就会把没处理的告警算成已处理 —— 这是最容易悄悄发生的统计失真。
    {
        int nullHandled = -1;
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                        QStringLiteral("verify-null"));
            db.setDatabaseName(dbPath());
            QVERIFY(db.open());
            QSqlQuery query(db);
            if (query.exec(QStringLiteral(
                    "SELECT COUNT(*) FROM alarms WHERE handled_at IS NULL AND disposition IS NULL"))) {
                query.next();
                nullHandled = query.value(0).toInt();
            }
            db.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("verify-null"));
        QCOMPARE(nullHandled, 1);
    }

    storage.close();
}

void TestDataStorage::alarmHandlingRoundTrip()
{
    DataStorage storage(nullptr, dbPath());
    QVERIFY(storage.open());

    AlarmRecord record;
    record.time = QDateTime::currentDateTime();
    record.deviceId = QStringLiteral("dev-1");
    record.tagId = QStringLiteral("pressure");
    record.level = AlarmLevel::Warning;
    record.value = 88.0;
    record.message = QStringLiteral("压力偏高");
    record.active = true;
    QVERIFY(storage.insertAlarm(record));

    QVERIFY(storage.markAlarmHandled(QStringLiteral("dev-1"), QStringLiteral("pressure"),
                                     AlarmDisposition::FalseAlarm, QStringLiteral("王五"),
                                     QStringLiteral("传感器飘了")));

    const QList<AlarmRecord> back = storage.queryAlarms(wideFrom(), wideTo(), 10);
    QCOMPARE(int(back.size()), 1);
    QVERIFY(back.at(0).handled());
    QCOMPARE(int(back.at(0).disposition), int(AlarmDisposition::FalseAlarm));
    QCOMPARE(back.at(0).handledBy, QStringLiteral("王五"));
    QCOMPARE(back.at(0).handlingNote, QStringLiteral("传感器飘了"));
    QVERIFY2(back.at(0).acknowledged, "处理必然意味着已经确认过");

    storage.close();
}

void TestDataStorage::deviceStatsMttr()
{
    DataStorage storage(nullptr, dbPath());
    QVERIFY(storage.open());

    // 告警发生在 120 秒前、处理时间是现在 → MTTR 应约等于 120000 ms。
    // 这是对 SQL 里 julianday 换算的数值级验证，不是"看着对"。
    AlarmRecord record;
    record.time = QDateTime::currentDateTime().addSecs(-120);
    record.deviceId = QStringLiteral("dev-db");
    record.tagId = QStringLiteral("speed");
    record.level = AlarmLevel::Critical;
    record.value = 123.0;
    record.message = QStringLiteral("转速超高");
    record.active = true;
    QVERIFY(storage.insertAlarm(record));
    QVERIFY(storage.markAlarmHandled(QStringLiteral("dev-db"), QStringLiteral("speed"),
                                     AlarmDisposition::Maintained, QStringLiteral("张三"),
                                     QStringLiteral("停机换电机")));

    bool found = false;
    for (const DataStorage::DeviceStats &stat : storage.queryDeviceStats(wideFrom(), wideTo())) {
        if (stat.deviceId != QLatin1String("dev-db"))
            continue;
        found = true;
        QCOMPARE(stat.alarmCount, 1);
        QCOMPARE(stat.handledCount, 1);
        QVERIFY2(stat.avgHandleMs > 100000 && stat.avgHandleMs < 140000,
                 qPrintable(QStringLiteral("MTTR 换算错误：期望≈120000，实测 %1").arg(stat.avgHandleMs)));
    }
    QVERIFY2(found, "统计结果里应该能查到该设备");

    storage.close();
}

void TestDataStorage::sampleCounts()
{
    DataStorage storage(nullptr, dbPath());
    QVERIFY(storage.open());

    const QDateTime now = QDateTime::currentDateTime();
    for (int i = 0; i < 6; ++i) {
        QVERIFY(storage.insertSample(QStringLiteral("dev-1"), QStringLiteral("temperature"),
                                     double(i), now.addSecs(i)));
    }
    storage.flush();

    QCOMPARE(storage.countSamples(wideFrom(), wideTo()), qint64(6));
    // 区间只覆盖后半段
    QCOMPARE(storage.countSamples(now.addSecs(3), now.addSecs(10)), qint64(3));
    // 不带参数 = 全表
    QCOMPARE(storage.countSamples(), qint64(6));

    storage.close();
}

void TestDataStorage::samplesSpillWhenDatabaseUnavailable()
{
    // 刻意不 open()：模拟"数据库起不来"。采样**一条都不能丢**。
    DataStorage storage(nullptr, dbPath());
    const QDateTime base = QDateTime::currentDateTime().addSecs(-100);

    for (int i = 0; i < 5; ++i) {
        // 乱序写入，用来验证补传时会按时间排序
        const int offsets[5] = {3, 1, 5, 2, 4};
        QVERIFY(storage.insertSample(QStringLiteral("dev-1"), QStringLiteral("temperature"),
                                     double(i), base.addSecs(offsets[i])));
    }
    storage.flush();

    QVERIFY2(QFile::exists(spillPath()), "库不可用时采样应溢出到磁盘队列");
    QCOMPARE(storage.spillBacklogCount(), qint64(5));
}

void TestDataStorage::spillReplaysCompletelyAndInOrder()
{
    // 第一阶段：攒出一份积压
    {
        DataStorage storage(nullptr, dbPath());
        const QDateTime base = QDateTime::currentDateTime().addSecs(-100);
        const int offsets[5] = {3, 1, 5, 2, 4};
        const double values[5] = {11.0, 22.0, 33.0, 44.0, 55.0};
        for (int i = 0; i < 5; ++i) {
            storage.insertSample(QStringLiteral("dev-1"), QStringLiteral("temperature"),
                                 values[i], base.addSecs(offsets[i]));
        }
        storage.flush();
        QVERIFY(QFile::exists(spillPath()));
    }

    // 第二阶段：重新打开 → 自动补传
    {
        DataStorage storage(nullptr, dbPath());
        QVERIFY(storage.open());

        QCOMPARE(storage.spillBacklogCount(), qint64(0));
        QVERIFY2(!QFile::exists(spillPath()), "补传成功后队列文件必须被清理");
        QCOMPARE(storage.countSamples(wideFrom(), wideTo()), qint64(5));

        // 时间升序 (+1s..+5s) 对应 22 / 44 / 11 / 55 / 33 —— 一一对应才说明没写错位
        const QList<DataStorage::Sample> samples =
            storage.querySamples(QStringLiteral("dev-1"), QStringLiteral("temperature"),
                                 wideFrom(), wideTo(), 100);
        QCOMPARE(int(samples.size()), 5);
        const double expected[5] = {22.0, 44.0, 11.0, 55.0, 33.0};
        for (int i = 0; i < 5; ++i)
            QCOMPARE(samples.at(i).value, expected[i]);

        storage.close();
    }

    // 第三阶段：物理写入顺序（rowid）也必须是时间升序 ——
    // 光靠"查询按 ts 排序"证明不了补传时排过序。
    {
        bool orderChecked = false;
        bool ascending = false;
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                        QStringLiteral("verify-order"));
            db.setDatabaseName(dbPath());
            if (db.open()) {
                ascending = true;
                bool first = true;
                QDateTime previous;
                QSqlQuery query(db);
                if (query.exec(QStringLiteral("SELECT ts FROM samples ORDER BY rowid"))) {
                    while (query.next()) {
                        const QDateTime ts = query.value(0).toDateTime();
                        if (!ts.isValid() || (!first && ts < previous)) {
                            ascending = false;
                            break;
                        }
                        previous = ts;
                        first = false;
                    }
                    orderChecked = true;
                }
                db.close();
            }
        }
        QSqlDatabase::removeDatabase(QStringLiteral("verify-order"));
        QVERIFY(orderChecked);
        QVERIFY2(ascending, "补传必须按时间升序物理写入");
    }

    // 第四阶段：再次启动不重复补传
    {
        DataStorage storage(nullptr, dbPath());
        QVERIFY(storage.open());
        QCOMPARE(storage.countSamples(wideFrom(), wideTo()), qint64(5));
        storage.close();
    }
}

void TestDataStorage::spillSkipsBrokenLines()
{
    // 队列文件被截断/写坏是常态（磁盘满、进程被强杀），坏行不能拖垮整批补传。
    {
        QFile file(spillPath());
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        const QByteArray good = QByteArrayLiteral(
            "{\"t\":\"2026-09-17T10:00:00.000\",\"d\":\"dev-9\",\"g\":\"speed\",\"v\":10.5}\n");
        file.write(good);
        file.write("this line is not json at all\n");
        file.write("{\"t\":\"not-a-date\",\"d\":\"dev-9\",\"g\":\"speed\",\"v\":1.0}\n");
        file.write(good);
        file.close();
    }

    DataStorage storage(nullptr, dbPath());
    QVERIFY(storage.open());

    QCOMPARE(storage.countSamples(wideFrom(), wideTo()), qint64(2));
    QVERIFY2(!QFile::exists(spillPath()), "含坏行的队列文件补传后同样要清理");

    storage.close();
}

QTEST_GUILESS_MAIN(TestDataStorage)

#include "tst_datastorage.moc"
