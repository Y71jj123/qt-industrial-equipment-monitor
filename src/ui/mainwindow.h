#pragma once

#include "core/alarmengine.h"
#include "core/devicemanager.h"
#include "ui/logindialog.h" // UserRole

#include <QHash>
#include <QMainWindow>
#include <QString>

class QLabel;
class QListWidget;
class QListWidgetItem;
class QPlainTextEdit;
class QTabWidget;
class QTableWidget;

#ifdef HAVE_QT_CHARTS
class QChart;
class QChartView;
class QDateTimeAxis;
class QLineSeries;
class QValueAxis;
#endif

class AcquisitionScheduler;
class AlarmPanel;
class ControlPanel;
class DataStorage;
class DeviceDetailPanel;
class HistoryPanel;
class ReportPanel;
class RulePanel;

/// 主窗口。
///
/// 只负责"展示"和"把用户操作转成调用"，业务逻辑一律不写在这里 ——
/// 数据来自 DeviceManager / AcquisitionScheduler，告警来自 AlarmEngine，
/// 持久化通过 DataStorage。
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(DeviceManager *deviceManager,
                        AlarmEngine *alarmEngine,
                        DataStorage *storage,
                        const QString &userName,
                        UserRole role,
                        QWidget *parent = nullptr);
    ~MainWindow() override = default;

private slots:
    void onAddDevice();
    void onEditDevice();
    void onRemoveDevice();
    void onStartAll();
    void onStopAll();
    void onAcknowledgeAll();
    void onClearAcknowledged();

    void onDeviceAdded(const DeviceInfo &device);
    void onDeviceRemoved(const QString &deviceId);
    void onTagUpdated(const QString &deviceId, const QString &tagId, const QVariant &value);
    void onAlarmRaised(const AlarmRecord &record);
    void onAlarmCleared(const QString &deviceId, const QString &tagId);
    void onSyncAlarmAck();
    void appendLog(int level, const QString &message);

    /// 左侧设备列表点击 → 同步趋势图与设备详情面板
    void onDeviceSelected(QListWidgetItem *current, QListWidgetItem *previous);

private:
    void setupUi();
    void setupConnections();
    void updateStatus();
    void applyPermissions();

    /// 启动时从数据库恢复设备台账；空库则预置一台模拟设备。
    void restoreOrSeedDevices();

    /// 刷新左侧某一项的显示（名称 + 状态灯）。
    void updateDeviceListItem(const QString &deviceId);

    /// 设备列表 / 规则 / 控制等下拉需要跟随设备变化刷新。
    void reloadDeviceDependentPanels();

    static QString keyOf(const QString &deviceId, const QString &tagId);

    DeviceManager *m_deviceManager = nullptr;
    AlarmEngine *m_alarmEngine = nullptr;
    DataStorage *m_storage = nullptr;
    AcquisitionScheduler *m_scheduler = nullptr;

    QString m_userName;
    UserRole m_role = UserRole::Operator;

    QListWidget *m_deviceList = nullptr;
    QTableWidget *m_valueTable = nullptr;
    QPlainTextEdit *m_logView = nullptr;
    QLabel *m_statusLabel = nullptr;
    QLabel *m_userLabel = nullptr;

    QTabWidget *m_rightTabs = nullptr;
    AlarmPanel *m_alarmPanel = nullptr;
    DeviceDetailPanel *m_detailPanel = nullptr;
    HistoryPanel *m_historyPanel = nullptr;
    RulePanel *m_rulePanel = nullptr;
    ControlPanel *m_controlPanel = nullptr;
    ReportPanel *m_reportPanel = nullptr;

    QHash<QString, int> m_rowIndex; ///< key -> 表格行号

#ifdef HAVE_QT_CHARTS
    // —— 实时趋势曲线（跟随左侧选中的设备）——
    QChart *m_chart = nullptr;
    QChartView *m_chartView = nullptr;
    QDateTimeAxis *m_axisX = nullptr;
    QValueAxis *m_axisY = nullptr;
    QHash<QString, QLineSeries *> m_trendSeries; ///< tagId -> 折线
    QString m_trendDeviceId;                     ///< 当前趋势图跟随的设备
    static constexpr int m_trendMaxPoints = 120; ///< 每条线最多保留的点数

    void buildTrendSeries(const QString &deviceId);
#endif
};
