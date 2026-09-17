#pragma once

#include "comm/deviceconnection.h"

#include <QHash>
#include <QStringList>

struct DeviceInfo;
class QTimer;

/// 模拟设备连接。
///
/// 在没有真实硬件（PLC / 仪表）的情况下，按固定周期产生带噪声的随机数据，
/// 让界面、告警、存储等上层逻辑可以先跑起来。
/// 真实设备接入时，照着这个类再写一个 ModbusConnection 即可，上层无需改动。
class MockConnection : public DeviceConnection
{
    Q_OBJECT

public:
    explicit MockConnection(QObject *parent = nullptr);
    ~MockConnection() override;

    void configure(const DeviceInfo &device) override;

    bool open(const QString &host, quint16 port) override;
    void close() override;
    bool isOpen() const override;

    bool readTag(const QString &tagId, QVariant &value) override;
    bool writeTag(const QString &tagId, const QVariant &value) override;

    /// 自定义点位列表；不设置时使用内置示例点位。
    void setTags(const QStringList &tags);
    QStringList tags() const;

    /// 数据产生周期，默认 1000ms。
    void setIntervalMs(int intervalMs);
    int intervalMs() const;

private:
    void tick();

    QTimer *m_timer = nullptr;
    QStringList m_tags;
    QHash<QString, QVariant> m_values;
    QHash<QString, double> m_base;
    QString m_host;
    quint16 m_port = 0;
    int m_intervalMs = 1000;
    bool m_open = false;
};
