#pragma once

#include <QDateTime>
#include <QList>
#include <QObject>
#include <QSqlDatabase>
#include <QString>

/// 历史数据存储层（SQLite）。
///
/// 只负责“把采样值存下来、按条件查回来”，不含业务判断。
/// 服务端部署时把 QSQLITE 换成 QMYSQL 即可，接口不变。
class DataStorage : public QObject
{
    Q_OBJECT

public:
    struct Sample
    {
        QDateTime time;
        QString deviceId;
        QString tagId;
        double value = 0.0;
    };

    explicit DataStorage(QObject *parent = nullptr, const QString &databasePath = QString());
    ~DataStorage() override;

    bool open();
    void close();
    bool isOpen() const;

    /// 写入一条采样记录；失败时返回 false，错误信息见 lastError()。
    bool insertSample(const QString &deviceId,
                      const QString &tagId,
                      double value,
                      const QDateTime &time = QDateTime());

    /// 按设备 + 点位 + 时间区间查询历史数据（按时间升序）。
    QList<Sample> querySamples(const QString &deviceId,
                               const QString &tagId,
                               const QDateTime &from,
                               const QDateTime &to,
                               int limit = 10000) const;

    QString lastError() const;

private:
    bool createTables();

    QString m_databasePath;
    QSqlDatabase m_db;
    mutable QString m_lastError;
};
