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
/// 线程安全：采集线程也会打日志，所以文件写入用互斥锁串起来
/// （信号广播交给 Qt 的队列连接，接收方在哪个线程由连接类型决定）。
class Log : public QObject
{
    Q_OBJECT

public:
    enum Level { Info = 0, Warn = 1, Error = 2 };
    Q_ENUM(Level)

    static Log &instance();

    /// 初始化日志文件；filePath 为空时只输出到控制台。
    void init(const QString &filePath = QString());

    void write(Level level, const QString &message);

    static void info(const QString &message);
    static void warn(const QString &message);
    static void error(const QString &message);

signals:
    /// level 对应 Level 枚举值
    void messageLogged(int level, const QString &message);

private:
    explicit Log(QObject *parent = nullptr);

    QMutex m_mutex; ///< 保护 m_file / m_fileReady（多线程写入）
    QFile m_file;
    bool m_fileReady = false;
};
