#include "utils/logger.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFileInfo>

namespace {

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

} // namespace

Log::Log(QObject *parent)
    : QObject(parent)
{
}

Log &Log::instance()
{
    static Log logger;
    return logger;
}

void Log::init(const QString &filePath)
{
    if (filePath.isEmpty())
        return;

    const QFileInfo info(filePath);
    QDir().mkpath(info.absolutePath());

    m_file.setFileName(filePath);
    m_fileReady = m_file.open(QIODevice::Append | QIODevice::Text);
    if (!m_fileReady)
        qWarning() << "无法打开日志文件:" << filePath;
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

    if (m_fileReady) {
        m_file.write(line.toUtf8());
        m_file.write("\n");
        m_file.flush();
    }

    emit messageLogged(static_cast<int>(level), message);
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
