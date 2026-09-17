#include "utils/logger.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFileInfo>

namespace {

/// 日志器在 QCoreApplication 下的对象名（见 Log::instance() 的说明）。
constexpr auto kLoggerObjectName = "monitor-logger";

QString levelName(Log::Level level)
{
    switch (level) {
    case Log::Info:
        return QStringLiteral("INFO");
    case Log::Warn:
        return QStringLiteral("WARN");
    case Log::Error:
        return QStringLiteral("ERROR");
    }
    return QStringLiteral("?");
}

/// 滚动归档文件（以及归档文件的名字是 `<日志文件>.<序号>`）的路径拼接。
QString archivePath(const QString &base, int index)
{
    return QStringLiteral("%1.%2").arg(base).arg(index);
}

} // namespace

Log::Log(QObject *parent)
    : QObject(parent)
{
}

Log &Log::instance()
{
    QCoreApplication *app = QCoreApplication::instance();
    if (!app) {
        // 没有 QCoreApplication 的场合（纯逻辑单测）：退回函数内静态就够了。
        static Log fallback;
        return fallback;
    }

    // 为什么挂在 QCoreApplication 下查找：协议插件是独立动态库、**静态链接了同一份
    // 源码**，函数内静态局部变量在"一个模块一份"的情况下会各造一个实例 ——
    // 结果是两个 QFile 句柄抢同一个日志文件、两套滚动计数互相打架。
    //
    // 为什么这里按 QObject 查找、再自己向下转型，而不是直接用 findChild<Log*>：
    // 后者依赖跨模块的元对象转换，实测在插件里**找不到**主程序建的那个实例
    // （同模块内则一切正常），于是又新建一个 —— 单例就名存实亡了。
    // QObject 的元对象在 Qt6Core 里、全进程唯一，所以这一层查找一定找得到；
    // 而 monitor-logger 这个名字是本类专有的，向下转型是安全的。
    if (QObject *existing = app->findChild<QObject *>(QString::fromLatin1(kLoggerObjectName)))
        return *static_cast<Log *>(existing);

    auto *created = new Log(app);
    created->setObjectName(QString::fromLatin1(kLoggerObjectName));
    return *created;
}

void Log::init(const QString &filePath, qint64 maxBytes, int maxFiles)
{
    if (filePath.isEmpty())
        return;

    const QFileInfo info(filePath);
    QDir().mkpath(info.absolutePath());

    QMutexLocker locker(&m_mutex);

    m_filePath = filePath;
    // 下限 4KB：比一条日志还小的话会变成"每次写入都滚动"，
    // 那种配置只会把日志变成一堆碎文件，不如直接给个合理下限。
    m_maxBytes = qMax<qint64>(4096, maxBytes);
    m_maxFiles = qMax(1, maxFiles);

    m_file.setFileName(m_filePath);
    // ⚠️ 刻意**不用 QIODevice::Text**：Text 模式在 Windows 上会把 '\n' 翻译成 "\r\n"，
    // 于是"写入的字节数"和"磁盘上的字节数"每行差 1 字节 ——
    // 滚动阈值是按前者算的，结果文件会稳定地超出上限（账实不符）。
    // 日志文件本来也不需要 CRLF，保持纯 LF 即可。
    m_fileReady = m_file.open(QIODevice::Append);
    if (!m_fileReady) {
        qWarning() << "无法打开日志文件:" << filePath;
        return;
    }

    m_writtenBytes = m_file.size();

    // 上次退出时就已经超限了：启动即滚动，别等第一条日志进来
    if (m_writtenBytes >= m_maxBytes)
        rotate();
}

void Log::rotate()
{
    // 调用方已持有 m_mutex，这里不再加锁（QMutex 不可重入，加了会直接死锁）。
    m_file.close();
    m_fileReady = false;

    // 最老的一份直接删掉，给后面的腾位置
    QFile::remove(archivePath(m_filePath, m_maxFiles));

    // 从后往前挪：.N-1 → .N。**必须倒序**，正序会把还没挪的覆盖掉。
    for (int i = m_maxFiles - 1; i >= 1; --i) {
        const QString from = archivePath(m_filePath, i);
        if (!QFile::exists(from))
            continue;
        const QString to = archivePath(m_filePath, i + 1);
        QFile::remove(to);
        QFile::rename(from, to);
    }

    // 当前文件 → .1
    if (QFile::exists(m_filePath)) {
        const QString first = archivePath(m_filePath, 1);
        QFile::remove(first);
        QFile::rename(m_filePath, first);
    }

    m_file.setFileName(m_filePath);
    m_fileReady = m_file.open(QIODevice::Append);
    m_writtenBytes = 0;
}

void Log::write(Level level, const QString &message)
{
    const QString line =
        QStringLiteral("[%1] [%2] %3")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")),
                 levelName(level),
                 message);

    switch (level) {
    case Info:
        qInfo().noquote() << line;
        break;
    case Warn:
        qWarning().noquote() << line;
        break;
    case Error:
        qCritical().noquote() << line;
        break;
    }

    {
        // 锁只护住文件：多线程同时写 QFile 会串行错乱
        QMutexLocker locker(&m_mutex);
        if (m_fileReady) {
            // 一次写完再计数：分开写 "+ \n" 的话，字节统计和实际落盘对不上，
            // 滚动阈值就会越来越偏。
            const QByteArray data = line.toUtf8() + '\n';
            m_file.write(data);
            m_file.flush();
            m_writtenBytes += data.size();

            if (m_writtenBytes >= m_maxBytes)
                rotate();
        }
    }

    // 广播放在锁外：接收方可能就在别的线程排队处理，持锁发信号没有必要
    emit messageLogged(static_cast<int>(level), message);
}

qint64 Log::currentSize() const
{
    QMutexLocker locker(&m_mutex);
    return m_writtenBytes;
}

void Log::info(const QString &message)
{
    instance().write(Info, message);
}

void Log::warn(const QString &message)
{
    instance().write(Warn, message);
}

void Log::error(const QString &message)
{
    instance().write(Error, message);
}
