#pragma once

#include "core/alarmengine.h"
#include "core/devicemanager.h"
#include "ui/logindialog.h" // UserRole
#include "ui/theme.h"
#include "utils/configio.h"

#include <QHash>
#include <QMainWindow>
#include <QString>

class QAction;
class QCloseEvent;
class QLabel;
class QPlainTextEdit;
class QPoint;
class QTabWidget;
class QTableWidget;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

#ifdef HAVE_QT_CHARTS
class AdvancedChartView;
class QChart;
class QDateTimeAxis;
class QLineSeries;
class QValueAxis;
#endif

class AcquisitionScheduler;
class AlarmHistoryPanel;
class AlarmNotifier;
class AlarmPanel;
class ControlPanel;
class DataStorage;
class DeviceDetailPanel;
class HistoryPanel;
class OverviewPanel;
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

protected:
    /// 关闭时收进托盘而不是退出 —— 监控程序被顺手关掉比占着任务栏麻烦得多。
    /// 托盘不可用时照常关闭（否则窗口一藏就再也叫不回来了）。
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onAddDevice();
    void onEditDevice();
    void onRemoveDevice();
    void onStartAll();
    void onStopAll();
    void onAcknowledgeAll();
    void onClearAcknowledged();
    void onToggleTheme();
    void onToggleSoundAlarm(bool enabled);

    /// 导出 / 导入整份配置（设备 + 点位表 + 分组 + 告警规则）。
    void onExportConfig();
    void onImportConfig();

    /// 点击告警浮层 → 跳到告警页并把窗口叫到前台。
    void onAlarmToastActivated(const QString &deviceId, const QString &tagId);

    void onDeviceAdded(const DeviceInfo &device);
    void onDeviceRemoved(const QString &deviceId);
    void onTagUpdated(const QString &deviceId, const QString &tagId, const QVariant &value);
    void onAlarmRaised(const AlarmRecord &record);
    void onAlarmCleared(const QString &deviceId, const QString &tagId);
    void onSyncAlarmAck();
    void appendLog(int level, const QString &message);

    /// 左侧设备树选中项变化 → 同步趋势图与设备详情面板。
    /// 选中分组节点时**刻意什么都不做** —— 点分组只是展开 / 折叠，不该切换设备。
    void onDeviceTreeSelectionChanged();

    /// 设备树右键菜单：分组的新建 / 重命名 / 删除，设备的移动分组。
    void onDeviceTreeContextMenu(const QPoint &pos);

private:
    void setupUi();
    void setupConnections();
    void updateStatus();
    void applyPermissions();

    /// 按 m_theme 重建样式表与全部矢量图标（主题切换时调用）。
    void refreshTheme();

    /// "声音报警"开关的图标与说明要跟着勾选状态走（QSS 的选中态不够醒目）。
    void updateSoundAction();

    /// 表格的行高 / 斑马纹 / 表头 —— QSS 管不到，只能在代码里统一设置。
    void polishTables();

#ifdef HAVE_QT_CHARTS
    /// 图表背景与坐标轴配色不归 QSS 管，随主题一起刷新。
    void refreshChartTheme();
#endif

    /// 启动时从数据库恢复设备台账（含分组）；空库则预置一台模拟设备。
    void restoreOrSeedDevices();

    /// 按 DeviceManager 的台账整棵重建设备树，并尽量保住原来的选中项。
    void rebuildDeviceTree();

    /// 刷新某个设备节点的状态灯与文字（名称 / 在线状态）。
    void updateDeviceTreeItem(const QString &deviceId);

    /// 刷新所有分组节点的「在线数 / 总数」。
    void updateGroupItems();

    /// 在树里按设备 id 找节点；找不到返回 nullptr。
    QTreeWidgetItem *findDeviceItem(const QString &deviceId) const;

    /// 当前选中的**设备** id；选中的是分组节点或没有选中时返回空串。
    QString currentDeviceId() const;

    /// 把右侧趋势图 / 详情面板切到指定设备（设备 id 为空则清空）。
    void syncDeviceSelection(const QString &deviceId);

    /// 分组列表的持久化（跟着设备台账走，但空分组也得留住，所以单独存一份）。
    void saveGroupsToSettings();
    QStringList loadGroupsFromSettings() const;

    /// 把一份导入的配置整体应用到运行时（停采集 → 换台账 → 存库 → 重启采集）。
    void applyImportedConfig(const ConfigIo::Config &config);

    /// 设备列表 / 规则 / 控制等下拉需要跟随设备变化刷新。
    void reloadDeviceDependentPanels();

    /// 定时器到期：把攒下来的值一次性写进表格 / 曲线 / 各面板。
    void flushPendingValues();

    /// 刷新实时数据表里的一行（第一次见到该点位就新建行）。
    void updateValueRow(const QString &deviceId, const QString &tagId, const QVariant &value);

#ifdef HAVE_QT_CHARTS
    /// 往当前选中设备的趋势曲线上补一个点。
    void appendTrendPoint(const QString &deviceId, const QString &tagId, double value);
#endif

    static QString keyOf(const QString &deviceId, const QString &tagId);

    DeviceManager *m_deviceManager = nullptr;
    AlarmEngine *m_alarmEngine = nullptr;
    DataStorage *m_storage = nullptr;
    AcquisitionScheduler *m_scheduler = nullptr;

    QString m_userName;
    UserRole m_role = UserRole::Operator;
    Theme m_theme = Theme::Light;

    // 工具栏动作要留着引用：换主题时得逐个换掉按旧主题色画出来的图标
    QAction *m_actionAddDevice = nullptr;
    QAction *m_actionEditDevice = nullptr;
    QAction *m_actionRemoveDevice = nullptr;
    QAction *m_actionStartAcquisition = nullptr;
    QAction *m_actionStopAcquisition = nullptr;
    QAction *m_actionAcknowledgeAll = nullptr;
    QAction *m_actionClearAcknowledged = nullptr;
    QAction *m_actionExportConfig = nullptr;
    QAction *m_actionImportConfig = nullptr;
    QAction *m_actionToggleTheme = nullptr;
    QAction *m_actionSoundAlarm = nullptr;

    /// 两层结构「分组 → 设备」的设备树。
    QTreeWidget *m_deviceTree = nullptr;
    QTableWidget *m_valueTable = nullptr;
    QPlainTextEdit *m_logView = nullptr;
    QLabel *m_statusLabel = nullptr;
    QLabel *m_themeBadge = nullptr;
    QLabel *m_userLabel = nullptr;

    QTabWidget *m_rightTabs = nullptr;
    /// 总览仪表盘（第一个页签，即"首页"）。
    OverviewPanel *m_overviewPanel = nullptr;
    AlarmPanel *m_alarmPanel = nullptr;
    AlarmHistoryPanel *m_alarmHistoryPanel = nullptr;
    DeviceDetailPanel *m_detailPanel = nullptr;
    HistoryPanel *m_historyPanel = nullptr;
    RulePanel *m_rulePanel = nullptr;
    ControlPanel *m_controlPanel = nullptr;
    ReportPanel *m_reportPanel = nullptr;

    /// 浮层 / 声音 / 托盘的统一入口，自己订阅 AlarmEngine 的告警信号。
    AlarmNotifier *m_notifier = nullptr;

    QHash<QString, int> m_rowIndex; ///< key -> 表格行号

    /// 界面刷新节流间隔（毫秒）：高频采样下每来一个点都重绘表格与曲线，
    /// 界面会被绘制拖垮，合并成 100ms 一批后视觉上反而更稳。
    static constexpr int kUiRefreshIntervalMs = 100;

    /// 待刷新队列：key 与 m_rowIndex 同构，同一个点位的新值直接覆盖旧值。
    struct PendingValue
    {
        QString deviceId;
        QString tagId;
        QVariant value;
    };

    QTimer *m_uiRefreshTimer = nullptr;
    QHash<QString, PendingValue> m_pendingValues;

#ifdef HAVE_QT_CHARTS
    // —— 实时趋势曲线（跟随左侧选中的设备）——
    QChart *m_chart = nullptr;
    AdvancedChartView *m_chartView = nullptr;
    QDateTimeAxis *m_axisX = nullptr;
    QValueAxis *m_axisY = nullptr;
    QHash<QString, QLineSeries *> m_trendSeries; ///< tagId -> 折线
    QString m_trendDeviceId;                     ///< 当前趋势图跟随的设备
    static constexpr int m_trendMaxPoints = 120; ///< 每条线最多保留的点数

    /// 滚动窗口宽度（毫秒）：横轴只显示最近这么长一段，曲线才有"流动感"。
    static constexpr qint64 m_trendWindowMs = 60000;

    void buildTrendSeries(const QString &deviceId);

    /// 把横轴 / 纵轴拉回"跟着最新数据滚动"的基准范围（右键复位时也走这里）。
    void applyTrendHomeRange();
#endif
};
