#pragma once

#include "core/alarmengine.h"
#include "core/devicemanager.h"

#include <QHash>
#include <QMainWindow>
#include <QString>

class QLabel;
class QListWidget;
class QPlainTextEdit;
class QTableWidget;

class AcquisitionScheduler;
class DataStorage;

/// 主窗口。
///
/// 只负责“展示”和“把用户操作转成调用”，业务逻辑一律不写在这里 ——
/// 数据来自 DeviceManager / AcquisitionScheduler，告警来自 AlarmEngine。
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(DeviceManager *deviceManager,
                        AlarmEngine *alarmEngine,
                        DataStorage *storage,
                        QWidget *parent = nullptr);
    ~MainWindow() override = default;

private slots:
    void onAddDevice();
    void onStartAll();
    void onStopAll();

    void onDeviceAdded(const DeviceInfo &device);
    void onTagUpdated(const QString &deviceId, const QString &tagId, const QVariant &value);
    void onAlarmRaised(const AlarmRecord &record);
    void appendLog(int level, const QString &message);

private:
    void setupUi();
    void setupConnections();
    void updateStatus();

    static QString keyOf(const QString &deviceId, const QString &tagId);

    DeviceManager *m_deviceManager = nullptr;
    AlarmEngine *m_alarmEngine = nullptr;
    DataStorage *m_storage = nullptr;
    AcquisitionScheduler *m_scheduler = nullptr;

    QListWidget *m_deviceList = nullptr;
    QTableWidget *m_valueTable = nullptr;
    QPlainTextEdit *m_logView = nullptr;
    QLabel *m_statusLabel = nullptr;

    QHash<QString, int> m_rowIndex; ///< key -> 表格行号
};
