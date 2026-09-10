#pragma once

#include "core/devicemanager.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <QVariant>

class DeviceConnection;

/// 采集调度器：为每台设备建立一条连接，负责取数与下发。
///
/// 设计约束：调度器**不接触任何界面对象**，取到的数据一律通过信号抛出，
/// 由上层（界面）自行决定怎么显示。这样采集逻辑可以独立测试，也方便以后
/// 把采集放到独立线程里。
class AcquisitionScheduler : public QObject
{
    Q_OBJECT

public:
    explicit AcquisitionScheduler(DeviceManager *manager, QObject *parent = nullptr);
    ~AcquisitionScheduler() override;

    /// 开始采集指定设备；设备不存在或已有连接时返回 false。
    bool start(const QString &deviceId);

    void stop(const QString &deviceId);
    void stopAll();

    bool isRunning(const QString &deviceId) const;

    /// 向指定设备下发指令（写入一个点位）。
    bool writeTag(const QString &deviceId, const QString &tagId, const QVariant &value);

signals:
    void started(const QString &deviceId);
    void stopped(const QString &deviceId);
    void tagUpdated(const QString &deviceId, const QString &tagId, const QVariant &value);
    void errorOccurred(const QString &deviceId, const QString &message);

private:
    /// 按设备配置创建连接。
    /// 目前只有模拟实现；真实设备接入时在此按协议类型分支即可，上层无需改动。
    DeviceConnection *createConnection(const DeviceInfo &device, QObject *parent);

    DeviceManager *m_manager = nullptr;
    QHash<QString, DeviceConnection *> m_connections;
};
