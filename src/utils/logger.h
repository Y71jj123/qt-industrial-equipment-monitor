#pragma once

#include <QFile>
#include <QMutex>
#include <QObject>
#include <QString>

/// 轻量日志器。
///
/// 同时输出到控制台（qDebug/qWarning/qCritical）、可选的日志文件，
/// 并通过 messageLogged 信号广播给界面，供日志窗口实时显示。
///
/// **日志会滚动**：单文件超过 maxBytes 就改名成 `<文件>.1`，旧的依次往后挪，
/// 只保留 maxFiles 份历史。现场设备是长期运行的，没人会手动清日志 ——
/// 不滚动最后一定会把磁盘写满，而且是悄悄写满的。
///
/// 线程安全：采集线程也会打日志，所以文件写入用互斥锁串起来
/// （信号广播交给 Qt 的队列连接，接收方在哪个线程由连接类型决定）。
class Log : public QObject
{
    Q_OBJECT

public:
    enum Level { Info = 0, Warn = 1, Error = 2 };
    Q_ENUM(Level)

    /// 单个日志文件的体积上限（字节），超过即滚动。
    static constexpr qint64 kDefaultMaxBytes = 2 * 1024 * 1024;

    /// 保留的历史日志份数（不含当前文件）。超出的最老一份被删除。
    static constexpr int kDefaultMaxFiles = 5;

    static Log &instance();

    /// 初始化日志文件；filePath 为空时只输出到控制台。
    /// @param maxBytes 单文件上限（会兜一个下限，太小会导致"刚开就滚"）
    /// @param maxFiles 保留的历史份数
    void init(const QString &filePath = QString(),
              qint64 maxBytes = kDefaultMaxBytes,
              int maxFiles = kDefaultMaxFiles);

    void write(Level level, const QString &message);

    /// 当前日志文件已写入的字节数（供自检与测试）。
    qint64 currentSize() const;

    static void info(const QString &message);
    static void warn(const QString &message);
    static void error(const QString &message);

signals:
    /// level 对应 Level 枚举值
    void messageLogged(int level, const QString &message);

private:
    explicit Log(QObject *parent = nullptr);

    /// 滚动日志文件。**调用方必须已持有 m_mutex**（内部会关掉再重开文件）。
    void rotate();

    mutable QMutex m_mutex; ///< 保护下面几个成员（多线程写入）
    QFile m_file;
    bool m_fileReady = false;

    QString m_filePath;
    qint64 m_maxBytes = kDefaultMaxBytes;
    int m_maxFiles = kDefaultMaxFiles;
    qint64 m_writtenBytes = 0;
};
