#include "storage/datastorage.h"

#include "comm/protocolregistry.h"
#include "utils/configio.h"
#include "utils/logger.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>
#include <QVariant>

#include <algorithm>

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

/// 点位表 → 紧凑 JSON，存进 devices.points 列。
/// 点位表的 JSON 结构定义在 ConfigIo 里，和导出的配置文件共用同一份。
QString pointsToJson(const QList<TagPoint> &points)
{
    return QString::fromUtf8(
        QJsonDocument(ConfigIo::pointsToJson(points)).toJson(QJsonDocument::Compact));
}

/// 数据库列里的 JSON → 点位表。
QList<TagPoint> pointsFromJson(const QString &text)
{
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8());
    return doc.isArray() ? ConfigIo::pointsFromJson(doc.array()) : QList<TagPoint>{};
}

} // namespace

DataStorage::DataStorage(QObject *parent, const QString &databasePath)
    : QObject(parent)
    , m_databasePath(databasePath.isEmpty() ? defaultDatabasePath() : databasePath)
    , m_spillPath(m_databasePath + QStringLiteral(".pending.jsonl"))
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

    // 先把上次没落库的积压补传进来，再开始接收新数据 ——
    // 顺序反过来的话，历史查询里会出现"新的在前、补传的在后"的时间空洞。
    replaySpillFile();

    // 攒批提交的定时器：住在本对象所在线程（GUI），SQLite 连接不跨线程
    if (!m_flushTimer) {
        m_flushTimer = new QTimer(this);
        m_flushTimer->setInterval(kSampleFlushIntervalMs);
        connect(m_flushTimer, &QTimer::timeout, this, &DataStorage::flush);
    }
    m_flushTimer->start();

    // 数据保留策略：启动即跑一次（清理历史积压的过期数据），之后每日由定时器执行。
    startRetentionTimer();
    applyRetentionPolicy();

    Log::info(QStringLiteral("历史数据库已就绪: %1").arg(m_databasePath));
    return true;
}

void DataStorage::close()
{
    if (m_flushTimer)
        m_flushTimer->stop();

    // 关库前先落盘，否则队列里那几条就跟着进程一起没了
    flush();

    if (m_db.isOpen())
        m_db.close();
}

void DataStorage::setRetentionPolicy(int retentionDays, bool downsampleEnabled, int bucketHours)
{
    m_retentionDays = std::max(1, retentionDays);
    m_downsampleEnabled = downsampleEnabled;
    m_bucketHours = std::max(1, bucketHours);
}

void DataStorage::startRetentionTimer()
{
    if (!m_retentionTimer) {
        m_retentionTimer = new QTimer(this);
        m_retentionTimer->setInterval(int(kRetentionCheckIntervalMs));
        connect(m_retentionTimer, &QTimer::timeout, this, &DataStorage::applyRetentionPolicy);
    }
    m_retentionTimer->start();
}

bool DataStorage::applyRetentionPolicy()
{
    if (!m_db.isOpen())
        return false;

    // 超过这个时间点（对齐到桶边界）的原始采样即视为"过期"。
    // 对齐是关键：只处理"整个桶都已过期"的数据，避免把一个桶拆成两半 ——
    // 一半进了降采样表、另一半还留在原始表，会造成半桶丢失或重复计数。
    const qint64 bucketSeconds = static_cast<qint64>(m_bucketHours) * 3600;
    const qint64 cutoffSec =
        QDateTime::currentDateTime().addDays(-m_retentionDays).toSecsSinceEpoch();
    const qint64 alignedCutoffSec = (cutoffSec / bucketSeconds) * bucketSeconds;
    const QDateTime alignedCutoff = QDateTime::fromSecsSinceEpoch(alignedCutoffSec);

    // 聚合与删除放进同一个事务，要么都成、要么都不成。
    if (!m_db.transaction()) {
        m_lastError = m_db.lastError().text();
        Log::warn(QStringLiteral("数据保留策略失败（事务开启失败）: %1").arg(m_lastError));
        return false;
    }

    bool ok = true;
    if (m_downsampleEnabled) {
        // 把过期原始数据按桶聚合进降采样表。
        // INSERT OR REPLACE + (device, tag, bucket_start) 唯一键 → 幂等：
        // 每个过期桶只会在"首次完全过期"时聚合一次，聚合后原始行随即被删除，不会重现；
        // 若期间插入了更老的数据（落在已聚合过的桶里），REPLACE 会用全量数据覆盖，不重复不丢失。
        QSqlQuery aggregate(m_db);
        aggregate.prepare(QStringLiteral(
            "INSERT OR REPLACE INTO samples_downsampled "
            "(bucket_start, device, tag, avg_value, min_value, max_value, sample_count) "
            "SELECT datetime((cast(strftime('%s', ts) AS INTEGER) / :bucket) * :bucket, 'unixepoch'), "
            "       device, tag, AVG(value), MIN(value), MAX(value), COUNT(*) "
            "FROM samples WHERE ts < :cutoff "
            "GROUP BY 1, device, tag"));
        aggregate.bindValue(QStringLiteral(":bucket"), static_cast<qlonglong>(bucketSeconds));
        aggregate.bindValue(QStringLiteral(":cutoff"), alignedCutoff);
        if (!aggregate.exec()) {
            m_lastError = aggregate.lastError().text();
            ok = false;
        }
    }

    if (ok) {
        QSqlQuery del(m_db);
        del.prepare(QStringLiteral("DELETE FROM samples WHERE ts < :cutoff"));
        del.bindValue(QStringLiteral(":cutoff"), alignedCutoff);
        if (!del.exec()) {
            m_lastError = del.lastError().text();
            ok = false;
        }
    }

    if (ok && !m_db.commit()) {
        m_lastError = m_db.lastError().text();
        ok = false;
    }
    if (!ok) {
        m_db.rollback();
        Log::warn(QStringLiteral("数据保留策略执行失败，已回滚: %1").arg(m_lastError));
        return false;
    }

    Log::info(QStringLiteral("数据保留策略：保留近 %1 天，过期原始数据按 %2 小时桶%3清理完成")
                  .arg(m_retentionDays)
                  .arg(m_bucketHours)
                  .arg(m_downsampleEnabled ? QStringLiteral("聚合降级后") : QStringLiteral("（未启用降采样）")));
    return true;
}

qint64 DataStorage::downsampledRowCount() const
{
    if (!m_db.isOpen())
        return -1;
    QSqlQuery query(m_db);
    if (!query.exec(QStringLiteral("SELECT COUNT(*) FROM samples_downsampled")))
        return -1;
    return query.next() ? query.value(0).toLongLong() : -1;
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
                       "  acknowledged INTEGER  NOT NULL,"
                       "  disposition  INTEGER,"
                       "  handled_by   TEXT,"
                       "  handled_at   DATETIME,"
                       "  note         TEXT"
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
                       "  protocol_id TEXT,"
                       "  poll_ms    INTEGER,"
                       "  mqtt_topic TEXT,"
                       "  points     TEXT,"
                       "  grp        TEXT,"
                       "  username   TEXT,"
                       "  password   TEXT,"
                       "  baud_rate  INTEGER,"
                       "  data_bits  INTEGER,"
                       "  parity     INTEGER,"
                       "  stop_bits  INTEGER"
                       ")"),

        // 降采样表：过期原始数据按时间桶聚合后的"趋势行"。原始表只保留最近 N 天，
        // 更早的数据一旦整桶过期就被聚合成这里的一行（avg/min/max/计数），既不丢长期趋势也不撑爆磁盘。
        // UNIQUE(device, tag, bucket_start) 让聚合幂等：同一桶重复跑也不会产生重复行。
        QStringLiteral("CREATE TABLE IF NOT EXISTS samples_downsampled ("
                       "  id           INTEGER PRIMARY KEY AUTOINCREMENT,"
                       "  bucket_start DATETIME NOT NULL,"
                       "  device       TEXT     NOT NULL,"
                       "  tag          TEXT     NOT NULL,"
                       "  avg_value    REAL     NOT NULL,"
                       "  min_value    REAL     NOT NULL,"
                       "  max_value    REAL     NOT NULL,"
                       "  sample_count INTEGER  NOT NULL,"
                       "  UNIQUE(device, tag, bucket_start)"
                       ")"),
    };

    for (const QString &sql : statements) {
        if (!query.exec(sql)) {
            m_lastError = query.lastError().text();
            Log::error(QStringLiteral("建表失败: %1").arg(m_lastError));
            return false;
        }
    }

    // 老库里的 devices 表没有 grp 列，靠补列升级 —— 不重建表，设备配置原样保留。
    // 补出来的列是 NULL，DeviceInfo::groupName() 会把它当默认分组。
    if (!ensureColumn(QStringLiteral("devices"), QStringLiteral("grp"), QStringLiteral("TEXT")))
        return false;

    // MQTT 接入账号：同样是补列升级，老设备补出来是 NULL → 空串 → 匿名接入，行为不变。
    if (!ensureColumn(QStringLiteral("devices"), QStringLiteral("username"), QStringLiteral("TEXT")))
        return false;
    if (!ensureColumn(QStringLiteral("devices"), QStringLiteral("password"), QStringLiteral("TEXT")))
        return false;

    // Modbus RTU 串口参数：同样是补列升级，老设备补出来是 NULL → 取 DeviceInfo 的默认值
    // （9600 / 8 / 无校验 / 1 停止位），行为不变。串口参数用普通 int 存，不依赖 QtSerialPort。
    if (!ensureColumn(QStringLiteral("devices"), QStringLiteral("baud_rate"), QStringLiteral("INTEGER")))
        return false;
    if (!ensureColumn(QStringLiteral("devices"), QStringLiteral("data_bits"), QStringLiteral("INTEGER")))
        return false;
    if (!ensureColumn(QStringLiteral("devices"), QStringLiteral("parity"), QStringLiteral("INTEGER")))
        return false;
    if (!ensureColumn(QStringLiteral("devices"), QStringLiteral("stop_bits"), QStringLiteral("INTEGER")))
        return false;

    // 协议插件化：协议从整型枚举换成了字符串 id（外部插件的协议在编译期还不存在，
    // 枚举表达不了）。老库补出来的 protocol_id 是 NULL，加载时按老的 protocol
    // 整型映射一次即可 —— 不需要数据回填脚本，老设备配置照旧能用。
    if (!ensureColumn(QStringLiteral("devices"), QStringLiteral("protocol_id"), QStringLiteral("TEXT")))
        return false;

    // 告警工单闭环：老库的 alarms 表没有处理结论这几列。
    // 补出来是 NULL —— handled_at 为 NULL 即"未处理"，语义天然正确，不需要回填默认值。
    if (!ensureColumn(QStringLiteral("alarms"), QStringLiteral("disposition"), QStringLiteral("INTEGER")))
        return false;
    if (!ensureColumn(QStringLiteral("alarms"), QStringLiteral("handled_by"), QStringLiteral("TEXT")))
        return false;
    if (!ensureColumn(QStringLiteral("alarms"), QStringLiteral("handled_at"), QStringLiteral("DATETIME")))
        return false;
    if (!ensureColumn(QStringLiteral("alarms"), QStringLiteral("note"), QStringLiteral("TEXT")))
        return false;

    query.exec(QStringLiteral(
        "CREATE INDEX IF NOT EXISTS idx_samples_lookup ON samples(device, tag, ts)"));
    query.exec(QStringLiteral(
        "CREATE INDEX IF NOT EXISTS idx_alarms_lookup ON alarms(device, tag, ts)"));
    // 总览仪表盘要按"时间区间"整体统计（不分设备），上面那条以 device 打头的索引帮不上忙
    query.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_samples_ts ON samples(ts)"));
    query.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_alarms_ts ON alarms(ts)"));
    query.exec(QStringLiteral(
        "CREATE INDEX IF NOT EXISTS idx_downsampled_lookup ON samples_downsampled(device, tag, bucket_start)"));
    return true;
}

bool DataStorage::ensureColumn(const QString &table, const QString &column, const QString &definition)
{
    // 先问 PRAGMA 有没有这一列：直接 ALTER TABLE 会在列已存在时报错，
    // 每次启动都往日志里甩一条 "duplicate column name" 纯属噪音。
    QSqlQuery probe(m_db);
    if (!probe.exec(QStringLiteral("PRAGMA table_info(%1)").arg(table))) {
        m_lastError = probe.lastError().text();
        return false;
    }

    while (probe.next()) {
        if (probe.value(1).toString().compare(column, Qt::CaseInsensitive) == 0)
            return true;
    }

    QSqlQuery alter(m_db);
    if (!alter.exec(QStringLiteral("ALTER TABLE %1 ADD COLUMN %2 %3")
                        .arg(table, column, definition))) {
        m_lastError = alter.lastError().text();
        Log::error(QStringLiteral("为表 %1 补列 %2 失败: %3").arg(table, column, m_lastError));
        return false;
    }

    Log::info(QStringLiteral("数据库升级：表 %1 新增列 %2").arg(table, column));
    return true;
}

// ============================ 采样 ============================

bool DataStorage::insertSample(const QString &deviceId,
                               const QString &tagId,
                               double value,
                               const QDateTime &time)
{
    // ⚠️ 这里**故意不判断数据库是否打开**：数据一律先进内存队列，由 flush() 决定
    // 是落库还是溢出到磁盘队列。以前这里是"库没打开就 return false"，
    // 结果数据库一旦启动失败，所有采样就被逐条拒绝 —— 恰好丢掉的全部数据。
    //
    // 时间戳在这里就定下来，免得排队 200ms 后采样时刻全被"拉平"。
    PendingSample sample;
    sample.time = time.isValid() ? time : QDateTime::currentDateTime();
    sample.deviceId = deviceId;
    sample.tagId = tagId;
    sample.value = value;
    m_pendingSamples.append(sample);

    // 高频采集时不等定时器，攒够一批立刻走 —— 否则队列会被采样速度甩开
    if (m_pendingSamples.size() >= kSampleBatchSize)
        flush();

    return true;
}

void DataStorage::flush()
{
    if (m_pendingSamples.isEmpty())
        return;

    if (!m_db.isOpen()) {
        // 库不可用：**不丢数据**，整批溢出到磁盘队列，等下次 open() 时补写。
        // 以前这里是直接丢弃并打一条警告 —— 对工业数据来说，"丢掉"永远是最差的选项。
        spillBatch(m_pendingSamples);
        m_pendingSamples.clear();
        return;
    }

    const QList<PendingSample> batch = m_pendingSamples;
    m_pendingSamples.clear();

    // 一个事务装一整批：SQLite 默认每条 INSERT 单独提交（各自一次 fsync），
    // 合并后几百条数据只落一次盘 —— 这才是批量写入真正的收益所在。
    if (!m_db.transaction()) {
        m_lastError = m_db.lastError().text();
        Log::warn(QStringLiteral("采样批量写入失败（事务开启失败），转入磁盘队列: %1").arg(m_lastError));
        spillBatch(batch);
        return;
    }

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT INTO samples (ts, device, tag, value) VALUES (:ts, :device, :tag, :value)"));

    bool ok = true;
    for (const PendingSample &sample : batch) {
        query.bindValue(QStringLiteral(":ts"), sample.time);
        query.bindValue(QStringLiteral(":device"), sample.deviceId);
        query.bindValue(QStringLiteral(":tag"), sample.tagId);
        query.bindValue(QStringLiteral(":value"), sample.value);

        if (!query.exec()) {
            m_lastError = query.lastError().text();
            ok = false;
            break;
        }
    }

    if (ok && !m_db.commit()) {
        m_lastError = m_db.lastError().text();
        ok = false;
    }

    if (!ok) {
        m_db.rollback();
        // 整批回滚后**转入磁盘队列**而不是丢弃：回滚意味着这一批一条都没落库，
        // 留着它们才能保证"数据一条不少"。
        Log::warn(QStringLiteral("采样批量写入失败，本批 %1 条转入磁盘队列: %2")
                      .arg(batch.size())
                      .arg(m_lastError));
        spillBatch(batch);
    }
}

bool DataStorage::spillBatch(const QList<PendingSample> &batch)
{
    if (batch.isEmpty())
        return true;

    // 队列文件无上限早晚会把磁盘写满，所以写入前先立一道闸门。
    // 超限时的选择是"停止接收"而不是"悄悄丢掉最老的"——后者会让运维永远不知道丢了什么。
    constexpr qint64 kMaxSpillBytes = 64LL * 1024 * 1024; // 64 MB
    if (QFileInfo(m_spillPath).size() > kMaxSpillBytes) {
        Log::error(QStringLiteral("采样磁盘队列已超过 %1 MB，暂停止接收新数据以免写满磁盘")
                       .arg(kMaxSpillBytes / 1024 / 1024));
        return false;
    }

    QFile file(m_spillPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        m_lastError = file.errorString();
        Log::error(QStringLiteral("采样磁盘队列写入失败: %1").arg(m_lastError));
        return false;
    }

    // 用 JSON Lines（一行一条）而不是一个大 JSON 数组：
    // 追加写不必读回整个文件，且进程被强杀时已写完的行仍然是完整可解析的。
    QByteArray buffer;
    for (const PendingSample &sample : batch) {
        QJsonObject object;
        object.insert(QStringLiteral("t"), sample.time.toString(Qt::ISODateWithMs));
        object.insert(QStringLiteral("d"), sample.deviceId);
        object.insert(QStringLiteral("g"), sample.tagId);
        object.insert(QStringLiteral("v"), sample.value);
        buffer += QJsonDocument(object).toJson(QJsonDocument::Compact);
        buffer += '\n';
    }

    const bool written = (file.write(buffer) == buffer.size());
    file.close();

    if (!written) {
        m_lastError = file.errorString();
        Log::error(QStringLiteral("采样磁盘队列写入不完整: %1").arg(m_lastError));
        return false;
    }

    Log::warn(QStringLiteral("数据库不可用，%1 条采样已转入磁盘队列待补传").arg(batch.size()));
    return true;
}

qint64 DataStorage::spillBacklogCount() const
{
    QFile file(m_spillPath);
    if (!file.exists() || !file.open(QIODevice::ReadOnly))
        return 0;

    // 分块数换行符，不把整个文件读进内存
    qint64 lines = 0;
    constexpr int kChunkBytes = 64 * 1024;
    while (!file.atEnd())
        lines += file.read(kChunkBytes).count('\n');
    file.close();
    return lines;
}

void DataStorage::replaySpillFile()
{
    QFile file(m_spillPath);
    if (!file.exists())
        return;

    if (!file.open(QIODevice::ReadOnly)) {
        Log::warn(QStringLiteral("磁盘队列无法打开，跳过补传: %1").arg(file.errorString()));
        return;
    }

    QList<PendingSample> recovered;
    int brokenLines = 0;

    while (!file.atEnd()) {
        const QByteArray line = file.readLine().trimmed();
        if (line.isEmpty())
            continue;

        QJsonParseError parseError{};
        const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            ++brokenLines;
            continue;
        }

        const QJsonObject object = document.object();
        PendingSample sample;
        sample.time = QDateTime::fromString(object.value(QStringLiteral("t")).toString(),
                                            Qt::ISODateWithMs);
        sample.deviceId = object.value(QStringLiteral("d")).toString();
        sample.tagId = object.value(QStringLiteral("g")).toString();
        sample.value = object.value(QStringLiteral("v")).toDouble();

        if (!sample.time.isValid() || sample.deviceId.isEmpty() || sample.tagId.isEmpty()) {
            ++brokenLines;
            continue;
        }
        recovered.append(sample);
    }
    file.close();

    if (recovered.isEmpty()) {
        if (brokenLines > 0)
            Log::warn(QStringLiteral("磁盘队列里 %1 行无法解析，已连同队列文件一并清理").arg(brokenLines));
        QFile::remove(m_spillPath);
        return;
    }

    // 按时间升序补写："补传"的语义就是让这段数据回到历史序列里它原本该在的位置。
    std::sort(recovered.begin(), recovered.end(),
              [](const PendingSample &left, const PendingSample &right) {
                  return left.time < right.time;
              });

    if (!m_db.transaction()) {
        m_lastError = m_db.lastError().text();
        Log::warn(QStringLiteral("补传未执行（事务开启失败），队列保留待下次启动重试: %1")
                      .arg(m_lastError));
        return;
    }

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT INTO samples (ts, device, tag, value) VALUES (:ts, :device, :tag, :value)"));

    bool ok = true;
    for (const PendingSample &sample : recovered) {
        query.bindValue(QStringLiteral(":ts"), sample.time);
        query.bindValue(QStringLiteral(":device"), sample.deviceId);
        query.bindValue(QStringLiteral(":tag"), sample.tagId);
        query.bindValue(QStringLiteral(":value"), sample.value);
        if (!query.exec()) {
            m_lastError = query.lastError().text();
            ok = false;
            break;
        }
    }

    if (ok && !m_db.commit()) {
        m_lastError = m_db.lastError().text();
        ok = false;
    }

    if (!ok) {
        // 回滚了就等于一条都没写进去，队列文件绝对不能删，否则这批数据就真没了。
        m_db.rollback();
        Log::warn(QStringLiteral("补传失败，%1 条采样仍留在磁盘队列: %2")
                      .arg(recovered.size())
                      .arg(m_lastError));
        return;
    }

    // **确认提交成功之后**才删队列文件：先删后写，一崩就丢。
    QFile::remove(m_spillPath);

    Log::info(QStringLiteral("已补传 %1 条积压采样%2")
                  .arg(recovered.size())
                  .arg(brokenLines > 0
                           ? QStringLiteral("（另有 %1 行无法解析，已跳过）").arg(brokenLines)
                           : QString()));
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
        " message, active, acknowledged, disposition, handled_by, handled_at, note)"
        " VALUES (:ts, :device, :tag, :level, :value, :low, :high, :message, :active, :ack,"
        " :disposition, :handled_by, :handled_at, :note)"));
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

    // 未处理的记录一律写 NULL，而不是把 disposition 的默认值 0（已处理恢复）写进去 ——
    // 否则老库/未处理的告警在报表里会被算成"已处理"，统计直接失真。
    if (record.handled()) {
        query.bindValue(QStringLiteral(":disposition"), int(record.disposition));
        query.bindValue(QStringLiteral(":handled_by"), record.handledBy);
        query.bindValue(QStringLiteral(":handled_at"), record.handledAt);
        query.bindValue(QStringLiteral(":note"), record.handlingNote);
    } else {
        query.bindValue(QStringLiteral(":disposition"), QVariant());
        query.bindValue(QStringLiteral(":handled_by"), QVariant());
        query.bindValue(QStringLiteral(":handled_at"), QVariant());
        query.bindValue(QStringLiteral(":note"), QVariant());
    }

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

bool DataStorage::markAlarmHandled(const QString &deviceId,
                                   const QString &tagId,
                                   AlarmDisposition disposition,
                                   const QString &handledBy,
                                   const QString &note)
{
    if (!m_db.isOpen())
        return false;

    QSqlQuery query(m_db);
    // 处理必然意味着"已经确认过了"，所以顺手把 acknowledged 也置 1，
    // 免得出现"已处理但未确认"这种自相矛盾的状态。
    query.prepare(QStringLiteral(
        "UPDATE alarms SET disposition = :disposition, handled_by = :handled_by,"
        " handled_at = :handled_at, note = :note, acknowledged = 1"
        " WHERE id = (SELECT id FROM alarms WHERE device = :device AND tag = :tag"
        "             ORDER BY ts DESC LIMIT 1)"));
    query.bindValue(QStringLiteral(":disposition"), int(disposition));
    query.bindValue(QStringLiteral(":handled_by"), handledBy);
    // 处理时刻以落库这一刻为准；与引擎内存里的时刻最多差几毫秒，对 MTTR 无影响。
    query.bindValue(QStringLiteral(":handled_at"), QDateTime::currentDateTime());
    query.bindValue(QStringLiteral(":note"), note);
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
        "SELECT ts, device, tag, level, value, low_limit, high_limit, message, active, acknowledged,"
        " disposition, handled_by, handled_at, note"
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
        // 老库补出来的列是 NULL：toInt(0) 得到 0（已处理恢复），但 handledAt 无效，
        // 所以 handled() 仍然是 false —— 判断"是否处理过"永远以 handled_at 为准。
        record.disposition = static_cast<AlarmDisposition>(query.value(10).toInt(0));
        record.handledBy = query.value(11).toString();
        record.handledAt = query.value(12).toDateTime();
        record.handlingNote = query.value(13).toString();
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
        "REPLACE INTO devices (id, name, host, port, slave, protocol, protocol_id, poll_ms,"
        " mqtt_topic, points, grp, username, password, baud_rate, data_bits, parity, stop_bits)"
        " VALUES (:id, :name, :host, :port, :slave, :protocol, :protocol_id, :poll, :topic,"
        " :points, :grp, :user, :pass, :baud, :databits, :parity, :stopbits)"));
    query.bindValue(QStringLiteral(":id"), device.id);
    query.bindValue(QStringLiteral(":name"), device.name);
    query.bindValue(QStringLiteral(":host"), device.host);
    query.bindValue(QStringLiteral(":port"), int(device.port));
    query.bindValue(QStringLiteral(":slave"), device.slaveId);
    // 老的整型列照旧写一份（内置协议才认得出，其余写 -1）：
    // 万一要把程序降级回旧版本，那份代码还能读出设备来，而不是全部落成"默认协议"。
    query.bindValue(QStringLiteral(":protocol"),
                    ProtocolRegistry::legacyIntFromId(device.protocolId));
    query.bindValue(QStringLiteral(":protocol_id"), device.protocolId);
    query.bindValue(QStringLiteral(":poll"), device.pollIntervalMs);
    query.bindValue(QStringLiteral(":topic"), device.mqttTopic);
    query.bindValue(QStringLiteral(":points"), pointsToJson(device.points));
    query.bindValue(QStringLiteral(":grp"), device.groupName());
    query.bindValue(QStringLiteral(":user"), device.username);
    query.bindValue(QStringLiteral(":pass"), device.password);
    // 串口参数：Modbus RTU 才用，其它协议写默认值（读回时仍取默认值，行为不变）。
    query.bindValue(QStringLiteral(":baud"), device.baudRate);
    query.bindValue(QStringLiteral(":databits"), device.dataBits);
    query.bindValue(QStringLiteral(":parity"), device.parity);
    query.bindValue(QStringLiteral(":stopbits"), device.stopBits);

    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return false;
    }
    return true;
}

bool DataStorage::clearDevices()
{
    if (!m_db.isOpen())
        return false;

    QSqlQuery query(m_db);
    if (!query.exec(QStringLiteral("DELETE FROM devices"))) {
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
            "SELECT id, name, host, port, slave, protocol, poll_ms, mqtt_topic, points, grp,"
            " username, password, protocol_id, baud_rate, data_bits, parity, stop_bits"
            " FROM devices"))) {
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
        info.pollIntervalMs = query.value(6).toInt();
        info.mqttTopic = query.value(7).toString();
        info.points = pointsFromJson(query.value(8).toString());
        info.group = query.value(9).toString(); // 老库补列后是 NULL → 空串 → 默认分组
        info.username = query.value(10).toString(); // 同上：老库补列后为空 → 匿名接入
        info.password = query.value(11).toString();

        // 串口参数：老库补列后是 NULL → toInt() 落入 0，这里回退到 DeviceInfo 的默认值
        // （9600 / 8 / 无校验 / 1 停止位），保证老设备、非 RTU 设备读回来仍是合法串口参数。
        info.baudRate = query.value(13).isNull() ? 9600 : query.value(13).toInt();
        info.dataBits = query.value(14).isNull() ? 8 : query.value(14).toInt();
        info.parity = query.value(15).isNull() ? 0 : query.value(15).toInt();
        info.stopBits = query.value(16).isNull() ? 1 : query.value(16).toInt();

        // 协议：优先用字符串 id；老库那行 protocol_id 是 NULL → 按老整型映射一次。
        const QString protocolId = query.value(12).toString();
        if (!protocolId.isEmpty()) {
            info.protocolId = protocolId;
        } else {
            const int legacy = query.value(5).toInt();
            info.protocolId = ProtocolRegistry::idFromLegacyInt(legacy);
            Log::info(QStringLiteral("老库设备 %1 的协议由整型 %2 迁移为 %3")
                          .arg(info.id, QString::number(legacy), info.protocolId));
        }

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
    // 一次查询取齐告警数、已处理数与该设备的平均处理时长：
    //   - SUM(CASE ...) 数"已处理"（handled_at 非空）
    //   - AVG(CASE ...) 只对已处理的样本求平均（CASE 里的 NULL 会被 AVG 自动忽略）
    //   - julianday 差 × 86400000 = 毫秒
    // 拆成三条 SQL 当然也能写，但三次全表扫描换不来任何可读性。
    alarmQuery.prepare(QStringLiteral(
        "SELECT device, COUNT(*),"
        "       SUM(CASE WHEN handled_at IS NOT NULL THEN 1 ELSE 0 END),"
        "       AVG(CASE WHEN handled_at IS NOT NULL"
        "                THEN (julianday(handled_at) - julianday(ts)) * 86400000.0 END)"
        " FROM alarms WHERE ts BETWEEN :from AND :to GROUP BY device"));
    alarmQuery.bindValue(QStringLiteral(":from"), from);
    alarmQuery.bindValue(QStringLiteral(":to"), to);
    if (alarmQuery.exec()) {
        while (alarmQuery.next()) {
            const QString deviceId = alarmQuery.value(0).toString();
            DeviceStats stats = byDevice.value(deviceId);
            stats.deviceId = deviceId;
            stats.alarmCount = alarmQuery.value(1).toInt();
            stats.handledCount = alarmQuery.value(2).toInt();
            // 没有任何已处理样本时 AVG 返回 NULL → toLongLong() 得 0，
            // 这里统一翻成 -1，界面据此显示"—"而不是"0 ms"。
            stats.avgHandleMs = alarmQuery.value(3).isNull()
                                    ? -1
                                    : alarmQuery.value(3).toLongLong();
            byDevice.insert(deviceId, stats);
        }
    }

    result = byDevice.values();
    return result;
}

qint64 DataStorage::countSamples(const QDateTime &from, const QDateTime &to) const
{
    if (!m_db.isOpen())
        return 0;

    QSqlQuery query(m_db);
    // 两个时间都无效才走全表 COUNT：只要用户给了区间就按区间算，
    // 免得"忘了传参"静静地退化成全表扫描还看不出问题。
    if (from.isValid() && to.isValid()) {
        query.prepare(QStringLiteral("SELECT COUNT(*) FROM samples WHERE ts BETWEEN :from AND :to"));
        query.bindValue(QStringLiteral(":from"), from);
        query.bindValue(QStringLiteral(":to"), to);
    } else {
        query.prepare(QStringLiteral("SELECT COUNT(*) FROM samples"));
    }

    if (!query.exec() || !query.next()) {
        m_lastError = query.lastError().text();
        return 0;
    }
    return query.value(0).toLongLong();
}

int DataStorage::countAlarms(const QDateTime &from, const QDateTime &to) const
{
    if (!m_db.isOpen())
        return 0;

    QSqlQuery query(m_db);
    if (from.isValid() && to.isValid()) {
        query.prepare(QStringLiteral("SELECT COUNT(*) FROM alarms WHERE ts BETWEEN :from AND :to"));
        query.bindValue(QStringLiteral(":from"), from);
        query.bindValue(QStringLiteral(":to"), to);
    } else {
        query.prepare(QStringLiteral("SELECT COUNT(*) FROM alarms"));
    }

    if (!query.exec() || !query.next()) {
        m_lastError = query.lastError().text();
        return 0;
    }
    return query.value(0).toInt();
}

QString DataStorage::lastError() const
{
    return m_lastError;
}