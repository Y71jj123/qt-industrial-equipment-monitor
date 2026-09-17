// 性能基线：把"能扛多少设备"变成数字，而不是感觉。
//
// 两类测量：
//   A. 端到端吞吐 —— N 台模拟设备 × P 个点位 × 1s 周期，跑满一个测量窗口，
//      记录采样点速率、进程 CPU 占用、内存增长。
//      复刻的是"采集调度器 → 落库"这条链路（不含界面渲染），
//      因为界面部分受显示器与窗口大小影响，测出来的数字没有可比性。
//   B. 写库微基准 —— 同样条数的采样，"攒批一个事务" vs "逐条各提交一次事务"。
//      这是 README 里"批量写入真正收益在减少 fsync 次数"那句话的证据。
//
// 用法：monitor_bench [--devices 100] [--points 10] [--poll 1000]
//                     [--warmup 5] [--seconds 20] [--rows 3000]

#include "comm/protocolregistry.h"
#include "core/acquisitionscheduler.h"
#include "core/devicemanager.h"
#include "storage/datastorage.h"
#include "utils/logger.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTextStream>
#include <QThread>

#ifdef Q_OS_WIN
#include <windows.h>
// psapi.h 里的 K32* 版本由 kernel32 导出，不需要额外链接 psapi
#include <psapi.h>
#endif

namespace {

QTextStream &out()
{
    static QTextStream stream(stdout);
    return stream;
}

/// 进程累计 CPU 时间（秒）。返回 -1 表示本平台不支持。
double processCpuSeconds()
{
#ifdef Q_OS_WIN
    FILETIME creation{};
    FILETIME exitTime{};
    FILETIME kernelTime{};
    FILETIME userTime{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exitTime, &kernelTime, &userTime))
        return -1.0;

    const auto toSeconds = [](const FILETIME &time) {
        ULARGE_INTEGER value;
        value.LowPart = time.dwLowDateTime;
        value.HighPart = time.dwHighDateTime;
        return double(value.QuadPart) / 10000000.0; // 100ns 单位
    };
    return toSeconds(kernelTime) + toSeconds(userTime);
#else
    return -1.0;
#endif
}

/// 当前工作集（字节）。返回 0 表示本平台不支持。
quint64 workingSetBytes()
{
#ifdef Q_OS_WIN
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (!K32GetProcessMemoryInfo(GetCurrentProcess(),
                                 reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters),
                                 sizeof(counters)))
        return 0;
    return quint64(counters.WorkingSetSize);
#else
    return 0;
#endif
}

quint64 peakWorkingSetBytes()
{
#ifdef Q_OS_WIN
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (!K32GetProcessMemoryInfo(GetCurrentProcess(),
                                 reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters),
                                 sizeof(counters)))
        return 0;
    return quint64(counters.PeakWorkingSetSize);
#else
    return 0;
#endif
}

int logicalCores()
{
    const int cores = QThread::idealThreadCount();
    return cores > 0 ? cores : 1;
}

QString humanBytes(quint64 bytes)
{
    if (bytes == 0)
        return QStringLiteral("n/a");
    return QStringLiteral("%1 MB").arg(QString::number(double(bytes) / (1024.0 * 1024.0), 'f', 1));
}

/// 按点位条数生成点位表：先取内置模板，不够的补出来。
/// 补出来的点位 id 用兜底数据源（MockConnection 对认不出的 id 走默认波动）。
QList<TagPoint> makePoints(int count)
{
    QList<TagPoint> points = defaultTagPoints();
    static const char *const extraIds[] = {"current", "voltage", "torque", "flow", "level",
                                           "humidity", "power", "rpm2", "oil_temp", "noise"};
    const int extraCount = int(sizeof(extraIds) / sizeof(extraIds[0]));

    for (int i = 0; points.size() < count; ++i) {
        TagPoint point;
        point.id = QString::fromLatin1(extraIds[i % extraCount])
                   + (i >= extraCount ? QString::number(i / extraCount) : QString());
        point.name = point.id;
        point.unit = QStringLiteral("-");
        points.append(point);
    }
    if (points.size() > count)
        points = points.mid(0, count);
    return points;
}

/// 测量窗口：跑满 seconds 秒并让事件循环转起来。
void runFor(double seconds)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < qint64(seconds * 1000.0)) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
}

void printHeader(const QString &title)
{
    out() << Qt::endl
          << QStringLiteral("========================================") << Qt::endl
          << title << Qt::endl
          << QStringLiteral("========================================") << Qt::endl;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("monitor_bench"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("性能基线：采集吞吐 + 写库批量收益"));
    parser.addHelpOption();
    parser.addOption({QStringLiteral("devices"), QStringLiteral("模拟设备台数"), QStringLiteral("n"), QStringLiteral("100")});
    parser.addOption({QStringLiteral("points"), QStringLiteral("每台设备点位数"), QStringLiteral("n"), QStringLiteral("10")});
    parser.addOption({QStringLiteral("poll"), QStringLiteral("采集周期(ms)"), QStringLiteral("ms"), QStringLiteral("1000")});
    parser.addOption({QStringLiteral("warmup"), QStringLiteral("预热秒数（不计入统计）"), QStringLiteral("s"), QStringLiteral("5")});
    parser.addOption({QStringLiteral("seconds"), QStringLiteral("测量窗口秒数"), QStringLiteral("s"), QStringLiteral("20")});
    parser.addOption({QStringLiteral("rows"), QStringLiteral("写库微基准的条数"), QStringLiteral("n"), QStringLiteral("3000")});
    parser.process(app);

    const int deviceCount = parser.value(QStringLiteral("devices")).toInt();
    const int pointCount = parser.value(QStringLiteral("points")).toInt();
    const int pollMs = parser.value(QStringLiteral("poll")).toInt();
    const double warmupSec = parser.value(QStringLiteral("warmup")).toDouble();
    const double measureSec = parser.value(QStringLiteral("seconds")).toDouble();
    const int benchRows = parser.value(QStringLiteral("rows")).toInt();

    const QString workDir = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                                .filePath(QStringLiteral("monitor-bench"));
    QDir().mkpath(workDir);
    const QString dbPath = workDir + QStringLiteral("/bench.db");
    QFile::remove(dbPath);

    Log::instance().init(workDir + QStringLiteral("/bench.log"), 4 * 1024 * 1024, 2);

    // 协议注册表必须先装好：采集调度器只会问注册表，没注册就一台设备都起不来
    // （而且会明确报"未知协议"，不会静默兜底 —— 所以漏了这一步会立刻暴露）。
    registerBuiltinProtocols();
    if (ProtocolRegistry::instance().ids().isEmpty()) {
        out() << QStringLiteral("协议注册表是空的，夹具无法继续") << Qt::endl;
        return 1;
    }

    out() << QStringLiteral("设备 %1 台 × 点位 %2 个 × 周期 %3 ms ｜ 预热 %4 s + 测量 %5 s")
                 .arg(deviceCount)
                 .arg(pointCount)
                 .arg(pollMs)
                 .arg(warmupSec)
                 .arg(measureSec)
          << Qt::endl;

    // ---------------- A. 端到端吞吐 ----------------
    printHeader(QStringLiteral("A. 采集 → 落库 端到端"));

    DataStorage storage(nullptr, dbPath);
    if (!storage.open()) {
        out() << QStringLiteral("数据库打不开：%1").arg(storage.lastError()) << Qt::endl;
        return 1;
    }

    DeviceManager manager;
    const QList<TagPoint> points = makePoints(pointCount);
    QStringList deviceIds;
    for (int i = 0; i < deviceCount; ++i) {
        DeviceInfo device;
        device.name = QStringLiteral("压测设备 %1").arg(i + 1);
        device.protocolId = ProtocolRegistry::defaultProtocolId();
        device.pollIntervalMs = pollMs;
        device.points = points;
        deviceIds << manager.addDevice(device);
    }

    AcquisitionScheduler scheduler(&manager);

    // 采样点的落地：与主窗口的做法一致（调度器发信号 → 存储入队攒批）。
    qint64 sampleCounter = 0;
    QObject::connect(&scheduler, &AcquisitionScheduler::tagUpdated, &storage,
                     [&storage, &sampleCounter](const QString &deviceId, const QString &tagId,
                                                const QVariant &value) {
                         ++sampleCounter;
                         storage.insertSample(deviceId, tagId, value.toDouble());
                     });

    QElapsedTimer wall;
    wall.start();
    for (const QString &id : std::as_const(deviceIds))
        scheduler.start(id);

    out() << QStringLiteral("预热 %1 秒…").arg(warmupSec) << Qt::endl;
    runFor(warmupSec);

    const qint64 warmupSamples = sampleCounter;
    const double cpuStart = processCpuSeconds();
    const qint64 wallStart = wall.elapsed();
    const quint64 memStart = workingSetBytes();

    runFor(measureSec);

    const double cpuSeconds = processCpuSeconds() - cpuStart;
    const double wallSeconds = double(wall.elapsed() - wallStart) / 1000.0;
    const quint64 memEnd = workingSetBytes();
    const qint64 measuredSamples = sampleCounter - warmupSamples;

    scheduler.stopAll();
    storage.flush();

    const qint64 rowsInDb = storage.countSamples();
    const double pointsPerSecond = measuredSamples / wallSeconds;
    const double cpuPercent = cpuSeconds / wallSeconds * 100.0;
    const int cores = logicalCores();
    const quint64 dbBytes = quint64(QFileInfo(dbPath).size());

    out() << QStringLiteral("预热期间采样      : %1 点").arg(warmupSamples) << Qt::endl
          << QStringLiteral("测量期间采样      : %1 点（%2 点/秒）")
                 .arg(measuredSamples)
                 .arg(QString::number(pointsPerSecond, 'f', 0))
          << Qt::endl
          << QStringLiteral("进程 CPU 时间     : %1 秒 / 挂钟 %2 秒 → **%3 %**（单核口径；本机 %4 核，占整机 %5 %）")
                 .arg(QString::number(cpuSeconds, 'f', 2),
                      QString::number(wallSeconds, 'f', 2),
                      QString::number(cpuPercent, 'f', 1))
                 .arg(cores)
                 .arg(QString::number(cpuPercent / cores, 'f', 1))
          << Qt::endl
          << QStringLiteral("内存（工作集）    : %1 → %2（峰值 %3，窗口内增长 %4）")
                 .arg(humanBytes(memStart), humanBytes(memEnd), humanBytes(peakWorkingSetBytes()),
                      humanBytes(memEnd > memStart ? memEnd - memStart : 0))
          << Qt::endl
          << QStringLiteral("数据库落库        : %1 行（%2，平均每行 %3 字节）")
                 .arg(rowsInDb)
                 .arg(humanBytes(dbBytes))
                 .arg(rowsInDb > 0 ? QString::number(dbBytes / quint64(rowsInDb)) : QStringLiteral("n/a"))
          << Qt::endl
          << QStringLiteral("丢点情况          : 采集 %1 点 / 落库 %2 行 → %3")
                 .arg(sampleCounter)
                 .arg(rowsInDb)
                 .arg(sampleCounter == rowsInDb ? QStringLiteral("一致，无丢失")
                                                : QStringLiteral("不一致，需排查"))
          << Qt::endl;

    // ---------------- B. 写库微基准 ----------------
    printHeader(QStringLiteral("B. 写库：批量事务 vs 逐条事务"));

    const QString deviceId = QStringLiteral("bench");
    const QString tagId = QStringLiteral("temperature");
    const QDateTime base = QDateTime::currentDateTime();

    // B1：走 DataStorage 的攒批路径（200ms / 200 条一个事务）
    QElapsedTimer timer;
    timer.start();
    for (int i = 0; i < benchRows; ++i)
        storage.insertSample(deviceId, tagId, 40.0 + (i % 20), base.addMSecs(i));
    storage.flush();
    const qint64 batchedMs = timer.elapsed();

    // B2：同一张表、同一个文件，但每条各开一个事务提交（"没做批量优化"的样子）
    int perRowMs = -1;
    {
        QSqlDatabase raw = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                     QStringLiteral("bench-raw"));
        raw.setDatabaseName(dbPath);
        if (raw.open()) {
            QSqlQuery query(raw);
            query.prepare(QStringLiteral(
                "INSERT INTO samples (ts, device, tag, value) VALUES (:ts, :device, :tag, :value)"));

            timer.restart();
            for (int i = 0; i < benchRows; ++i) {
                raw.transaction();
                query.bindValue(QStringLiteral(":ts"), base.addMSecs(i));
                query.bindValue(QStringLiteral(":device"), deviceId);
                query.bindValue(QStringLiteral(":tag"), tagId);
                query.bindValue(QStringLiteral(":value"), 40.0 + (i % 20));
                query.exec();
                raw.commit();
            }
            perRowMs = int(timer.elapsed());
            raw.close();
        }
    }
    QSqlDatabase::removeDatabase(QStringLiteral("bench-raw"));

    const double batchedRate = batchedMs > 0 ? benchRows * 1000.0 / batchedMs : 0.0;
    const double perRowRate = perRowMs > 0 ? benchRows * 1000.0 / perRowMs : 0.0;

    out() << QStringLiteral("写入 %1 条采样：").arg(benchRows) << Qt::endl
          << QStringLiteral("  攒批一个事务（本项目做法）: %1 ms → %2 条/秒")
                 .arg(batchedMs)
                 .arg(QString::number(batchedRate, 'f', 0))
          << Qt::endl
          << QStringLiteral("  逐条各提交一次事务        : %1 ms → %2 条/秒")
                 .arg(perRowMs)
                 .arg(QString::number(perRowRate, 'f', 0))
          << Qt::endl
          << QStringLiteral("  提速                      : %1 倍")
                 .arg(perRowMs > 0 && batchedMs > 0
                          ? QString::number(double(perRowMs) / double(batchedMs), 'f', 1)
                          : QStringLiteral("n/a"))
          << Qt::endl;

    out() << Qt::endl
          << QStringLiteral("说明：A 段不含界面渲染（受显示器与窗口大小影响，测出来没有可比性）；")
          << Qt::endl
          << QStringLiteral("      B 段的两种写法用的是同一张表、同一个数据库文件。")
          << Qt::endl;
    out().flush();
    return 0;
}
