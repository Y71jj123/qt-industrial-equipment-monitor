#include "ui/mainwindow.h"

#include "core/acquisitionscheduler.h"
#include "storage/datastorage.h"
#include "ui/advancedchartview.h"
#include "ui/alarmhistorypanel.h"
#include "ui/alarmnotifier.h"
#include "ui/alarmpanel.h"
#include "ui/controlpanel.h"
#include "ui/devicedetailpanel.h"
#include "ui/devicedialog.h"
#include "ui/historypanel.h"
#include "ui/overviewpanel.h"
#include "ui/reportpanel.h"
#include "ui/rulepanel.h"
#include "utils/logger.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QBrush>
#include <QCloseEvent>
#include <QDialog>
#include <QFileDialog>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QSettings>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTime>
#include <QTimer>
#include <QToolBar>
#include <QTreeWidget>

#include <utility>

#ifdef HAVE_QT_CHARTS
#include <QColor>
#include <QDateTime>
#include <QtCharts/QAbstractAxis>
#include <QtCharts/QChart>
#include <QtCharts/QDateTimeAxis>
#include <QtCharts/QLegend>
#include <QtCharts/QLineSeries>
#include <QtCharts/QValueAxis>
#endif

namespace {

/// 设备树节点的类型标记。分组节点没有设备 id，光靠 UserRole 分不清
/// "分组"和"一个 id 为空的设备"，所以单独给一个角色。
enum TreeItemType {
    ItemDevice = 0,
    ItemGroup = 1
};

constexpr int kRoleDeviceId = Qt::UserRole;      ///< 设备节点：设备 id；分组节点：空
constexpr int kRoleItemType = Qt::UserRole + 1;  ///< TreeItemType
constexpr int kRoleGroupName = Qt::UserRole + 2; ///< 分组节点：分组名

/// 分组名列表在 QSettings 里的键。
constexpr auto kGroupsKey = "devices/groups";

/// 某个点位对应的默认告警上限（与 MockConnection 的数据量程保持一致）。
double defaultHighLimit(const TagPoint &point)
{
    if (point.id == QLatin1String("temperature"))
        return 60.0;
    if (point.id == QLatin1String("pressure"))
        return 80.0;
    if (point.id == QLatin1String("speed"))
        return 90.0;
    if (point.id == QLatin1String("vibration"))
        return 70.0;
    return 100.0;
}

/// 给还没有规则的点位补上默认告警规则（阈值型）。
///
/// **只补缺、不覆盖**：以前这里是"无条件重设"，于是点一次「开始采集」
/// 就把用户在「规则配置」里改过的阈值全部打回默认值，导入进来的规则也保不住。
/// 默认值只是"开箱可用"的起点，不是每次启动都要执行的复位动作。
void ensureDefaultRules(AlarmEngine *engine, const QString &deviceId, const QList<TagPoint> &points)
{
    const QList<AlarmRule> existing = engine->rules();

    for (const TagPoint &point : points) {
        if (point.boolean)
            continue; // 开关量不做阈值告警

        bool hasRule = false;
        for (const AlarmRule &rule : existing) {
            if (rule.deviceId == deviceId && rule.tagId == point.id) {
                hasRule = true;
                break;
            }
        }
        if (hasRule)
            continue;

        engine->addRule(
            AlarmRule{deviceId, point.id, AlarmKind::Range, 0.0, defaultHighLimit(point), 0.0, true});
    }
}

/// 画一个状态灯圆点。
QIcon dotIcon(const QColor &color)
{
    QPixmap pixmap(14, 14);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawEllipse(2, 2, 10, 10);

    return QIcon(pixmap);
}

/// 设备在线状态对应的灯色。
QColor stateColor(DeviceState state)
{
    switch (state) {
    case DeviceState::Online:
        return QColor(47, 180, 107);
    case DeviceState::Fault:
        return QColor(232, 145, 45);
    case DeviceState::Offline:
        break;
    }
    return QColor(149, 163, 178);
}

} // namespace

MainWindow::MainWindow(DeviceManager *deviceManager,
                       AlarmEngine *alarmEngine,
                       DataStorage *storage,
                       const QString &userName,
                       UserRole role,
                       QWidget *parent)
    : QMainWindow(parent)
    , m_deviceManager(deviceManager)
    , m_alarmEngine(alarmEngine)
    , m_storage(storage)
    , m_userName(userName)
    , m_role(role)
    , m_theme(loadSavedTheme()) // 沿用用户上次选的主题
{
    m_scheduler = new AcquisitionScheduler(m_deviceManager, this);

    setupUi();
    polishTables();
    refreshTheme();
    setupConnections();
    applyPermissions();

    restoreOrSeedDevices();
    reloadDeviceDependentPanels();
    onStartAll();
}

QString MainWindow::keyOf(const QString &deviceId, const QString &tagId)
{
    return deviceId + QLatin1Char('/') + tagId;
}

void MainWindow::setupUi()
{
    // 标题只留名字：版本号对使用者没有信息量，挂在标题上只是噪音。
    // 版本仍在 QApplication::setApplicationVersion() 与启动日志里，排查时查得到。
    setWindowTitle(QStringLiteral("工业设备远程监控管理平台"));
    resize(1360, 820);

    QToolBar *toolBar = addToolBar(QStringLiteral("主工具栏"));
    toolBar->setMovable(false);
    toolBar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toolBar->setIconSize(QSize(18, 18));

    // 图标在 refreshTheme() 里按当前主题色统一画上去，这里只管建动作
    m_actionAddDevice = toolBar->addAction(QStringLiteral("添加设备"),
                                           this, &MainWindow::onAddDevice);
    m_actionEditDevice = toolBar->addAction(QStringLiteral("编辑设备"),
                                            this, &MainWindow::onEditDevice);
    m_actionRemoveDevice = toolBar->addAction(QStringLiteral("移除设备"),
                                              this, &MainWindow::onRemoveDevice);

    toolBar->addSeparator();
    m_actionStartAcquisition = toolBar->addAction(QStringLiteral("开始采集"),
                                                  this, &MainWindow::onStartAll);
    m_actionStopAcquisition = toolBar->addAction(QStringLiteral("停止采集"),
                                                 this, &MainWindow::onStopAll);

    toolBar->addSeparator();
    m_actionAcknowledgeAll = toolBar->addAction(QStringLiteral("确认全部告警"),
                                                this, &MainWindow::onAcknowledgeAll);
    m_actionClearAcknowledged = toolBar->addAction(QStringLiteral("消警（已确认）"),
                                                   this, &MainWindow::onClearAcknowledged);

    toolBar->addSeparator();
    m_actionExportConfig = toolBar->addAction(QStringLiteral("导出配置"),
                                              this, &MainWindow::onExportConfig);
    m_actionExportConfig->setToolTip(
        QStringLiteral("把全部设备（含点位表）、分组与告警规则导出成一个 JSON 文件"));
    m_actionImportConfig = toolBar->addAction(QStringLiteral("导入配置"),
                                              this, &MainWindow::onImportConfig);
    m_actionImportConfig->setToolTip(
        QStringLiteral("从 JSON 文件读回配置，将整体替换当前设备台账并重新开始采集"));

    toolBar->addSeparator();
    // 声音开关的初始状态来自 QSettings；先设好再连信号，免得初始化时反过来写一遍配置
    m_actionSoundAlarm = toolBar->addAction(QStringLiteral("声音报警"));
    m_actionSoundAlarm->setCheckable(true);
    m_actionSoundAlarm->setChecked(AlarmNotifier::soundEnabled());
    connect(m_actionSoundAlarm, &QAction::toggled, this, &MainWindow::onToggleSoundAlarm);

    m_actionToggleTheme = toolBar->addAction(QStringLiteral("切换主题"),
                                             this, &MainWindow::onToggleTheme);
    m_actionToggleTheme->setToolTip(QStringLiteral("在浅色 / 暗色主题之间切换（会记住选择）"));

    m_actionAddDevice->setObjectName(QStringLiteral("actionAddDevice"));
    m_actionEditDevice->setObjectName(QStringLiteral("actionEditDevice"));
    m_actionRemoveDevice->setObjectName(QStringLiteral("actionRemoveDevice"));

    // 左：设备树 —— 两层「分组 → 设备」。
    // 用树而不是列表，是因为分组本身也要能被点中（展开 / 折叠、右键管理），
    // 而"点分组"和"点设备"在业务上完全是两回事，详见 onDeviceTreeSelectionChanged。
    m_deviceTree = new QTreeWidget(this);
    m_deviceTree->setMinimumWidth(250);
    m_deviceTree->setIconSize(QSize(14, 14));
    m_deviceTree->setHeaderHidden(true);
    m_deviceTree->setIndentation(14);
    m_deviceTree->setAnimated(true);
    m_deviceTree->setUniformRowHeights(true);
    m_deviceTree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_deviceTree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_deviceTree->setExpandsOnDoubleClick(true);

    // 实时数据表
    m_valueTable = new QTableWidget(0, 4, this);
    m_valueTable->setHorizontalHeaderLabels({QStringLiteral("设备"),
                                             QStringLiteral("点位"),
                                             QStringLiteral("当前值"),
                                             QStringLiteral("更新时间")});
    m_valueTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_valueTable->verticalHeader()->setVisible(false);
    m_valueTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_valueTable->setSelectionBehavior(QAbstractItemView::SelectRows);

    // 运行日志
    m_logView = new QPlainTextEdit(this);
    m_logView->setObjectName(QStringLiteral("logView"));
    m_logView->setReadOnly(true);
    m_logView->setMaximumBlockCount(1000);
    m_logView->setPlaceholderText(QStringLiteral("运行日志…"));

    // 右侧功能页
    m_rightTabs = new QTabWidget(this);

    // 「总览」放第一个：打开软件第一眼就是全局态势（在线率 / 告警 / 数据量），
    // 而不是一张等着刷新的空表格。
    m_overviewPanel = new OverviewPanel(m_deviceManager, m_alarmEngine, m_storage, m_scheduler, this);
    m_rightTabs->addTab(m_overviewPanel, QStringLiteral("总览"));

    m_rightTabs->addTab(m_valueTable, QStringLiteral("实时数据"));

#ifdef HAVE_QT_CHARTS
    m_chart = new QChart();
    m_chart->setTitle(QStringLiteral("实时趋势"));
    m_chart->legend()->setVisible(true);
    m_chart->legend()->setAlignment(Qt::AlignBottom);

    m_axisX = new QDateTimeAxis();
    m_axisX->setTitleText(QStringLiteral("时间"));
    m_axisX->setFormat(QStringLiteral("hh:mm:ss"));
    m_chart->addAxis(m_axisX, Qt::AlignBottom);

    m_axisY = new QValueAxis();
    m_axisY->setTitleText(QStringLiteral("数值"));
    m_axisY->setLabelFormat(QStringLiteral("%.1f"));
    m_chart->addAxis(m_axisY, Qt::AlignLeft);

    // 交互式视图：滚轮缩放 / 拖拽平移 / 右键复位 / 悬停读数
    m_chartView = new AdvancedChartView(m_chart, this);
    m_rightTabs->addTab(m_chartView, QStringLiteral("趋势曲线"));
#endif

    m_alarmPanel = new AlarmPanel(m_alarmEngine, m_deviceManager, this);
    m_rightTabs->addTab(m_alarmPanel, QStringLiteral("告警"));

    // 「告警」看的是当前活动告警，紧挨着的「告警历史」查的是库里存下来的历史
    m_alarmHistoryPanel = new AlarmHistoryPanel(m_storage, m_deviceManager, this);
    m_rightTabs->addTab(m_alarmHistoryPanel, QStringLiteral("告警历史"));

    m_historyPanel = new HistoryPanel(m_storage, m_deviceManager, this);
    m_rightTabs->addTab(m_historyPanel, QStringLiteral("历史查询"));

    m_controlPanel = new ControlPanel(m_scheduler, m_deviceManager, m_storage, this);
    m_rightTabs->addTab(m_controlPanel, QStringLiteral("远程控制"));

    m_rulePanel = new RulePanel(m_alarmEngine, m_deviceManager, this);
    m_rightTabs->addTab(m_rulePanel, QStringLiteral("规则配置"));

    m_reportPanel = new ReportPanel(m_storage, m_deviceManager, this);
    m_rightTabs->addTab(m_reportPanel, QStringLiteral("报表统计"));

    m_detailPanel = new DeviceDetailPanel(m_deviceManager, m_scheduler, this);
    m_rightTabs->addTab(m_detailPanel, QStringLiteral("设备详情"));

    auto *rightSplitter = new QSplitter(Qt::Vertical, this);
    rightSplitter->addWidget(m_rightTabs);
    rightSplitter->addWidget(m_logView);
    rightSplitter->setStretchFactor(0, 4);
    rightSplitter->setStretchFactor(1, 1);

    auto *mainSplitter = new QSplitter(Qt::Horizontal, this);
    mainSplitter->addWidget(m_deviceTree);
    mainSplitter->addWidget(rightSplitter);
    mainSplitter->setStretchFactor(0, 0);
    mainSplitter->setStretchFactor(1, 1);

    setCentralWidget(mainSplitter);

    connect(m_deviceTree, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem *, QTreeWidgetItem *) { onDeviceTreeSelectionChanged(); });
    connect(m_deviceTree, &QTreeWidget::customContextMenuRequested,
            this, &MainWindow::onDeviceTreeContextMenu);

    // 状态栏分两组：左边是设备 / 告警统计，右边是"主题 + 用户"的信息区，
    // 中间用一条细线断开；用户标签最弱化，避免和运行状态抢注意力。
    m_statusLabel = new QLabel(this);
    m_statusLabel->setObjectName(QStringLiteral("statusInfo"));
    statusBar()->addWidget(m_statusLabel);

    auto *separator = new QLabel(this);
    separator->setObjectName(QStringLiteral("statusSeparator"));
    separator->setFixedSize(1, 14);
    statusBar()->addPermanentWidget(separator);

    m_themeBadge = new QLabel(this);
    m_themeBadge->setObjectName(QStringLiteral("statusBadge"));
    statusBar()->addPermanentWidget(m_themeBadge);

    m_userLabel = new QLabel(this);
    m_userLabel->setObjectName(QStringLiteral("userBadge"));
    m_userLabel->setText(QStringLiteral("用户：%1（%2）").arg(m_userName, userRoleName(m_role)));
    statusBar()->addPermanentWidget(m_userLabel);

    updateStatus();

    // 界面刷新节流：单次触发，数据到了才启动 —— 没数据时一个定时器都不转。
    // 单次而非循环，是为了让"第一批数据"立刻（100ms 后）上屏，而不是干等一个整周期。
    m_uiRefreshTimer = new QTimer(this);
    m_uiRefreshTimer->setSingleShot(true);
    m_uiRefreshTimer->setInterval(kUiRefreshIntervalMs);
    connect(m_uiRefreshTimer, &QTimer::timeout, this, &MainWindow::flushPendingValues);
}

void MainWindow::setupConnections()
{
    // 采集 → 界面
    connect(m_scheduler, &AcquisitionScheduler::tagUpdated,
            this, &MainWindow::onTagUpdated);

    // 采集 → 告警引擎（数据进来就判一次阈值 / 变化率）
    connect(m_scheduler, &AcquisitionScheduler::tagUpdated,
            m_alarmEngine, &AlarmEngine::checkValue);

    connect(m_scheduler, &AcquisitionScheduler::errorOccurred, this,
            [this](const QString &deviceId, const QString &message) {
                Log::warn(QStringLiteral("[%1] %2").arg(deviceId, message));
            });

    // 意外掉线 → 离线告警；重连成功 → 清除离线告警
    connect(m_scheduler, &AcquisitionScheduler::deviceOffline, this,
            [this](const QString &deviceId) {
                const DeviceInfo info = m_deviceManager->device(deviceId);
                m_alarmEngine->raiseOfflineAlarm(deviceId, info.name);
            });
    connect(m_scheduler, &AcquisitionScheduler::connectionStateChanged, this,
            [this](const QString &deviceId, bool connected) {
                if (connected)
                    m_alarmEngine->clearOfflineAlarm(deviceId);
                updateDeviceTreeItem(deviceId);
                if (m_detailPanel)
                    m_detailPanel->refresh();
            });

    connect(m_deviceManager, &DeviceManager::deviceAdded,
            this, &MainWindow::onDeviceAdded);
    connect(m_deviceManager, &DeviceManager::deviceRemoved,
            this, &MainWindow::onDeviceRemoved);
    connect(m_deviceManager, &DeviceManager::deviceUpdated, this,
            [this](const DeviceInfo &) { updateGroupItems(); });
    connect(m_deviceManager, &DeviceManager::groupsChanged, this, [this]() {
        rebuildDeviceTree();
        saveGroupsToSettings();
    });
    connect(m_deviceManager, &DeviceManager::onlineChanged, this,
            [this](const QString &deviceId, bool) {
                updateStatus();
                updateDeviceTreeItem(deviceId);
                updateGroupItems();
            });
    connect(m_deviceManager, &DeviceManager::faultChanged, this,
            [this](const QString &deviceId, bool) {
                updateStatus();
                updateDeviceTreeItem(deviceId);
                updateGroupItems();
            });

    connect(m_alarmEngine, &AlarmEngine::alarmRaised,
            this, &MainWindow::onAlarmRaised);
    connect(m_alarmEngine, &AlarmEngine::alarmCleared,
            this, &MainWindow::onAlarmCleared);
    connect(m_alarmEngine, &AlarmEngine::activeAlarmsChanged, this,
            [this]() {
                updateStatus();
                onSyncAlarmAck();
            });
    connect(m_alarmEngine, &AlarmEngine::historyChanged,
            m_alarmPanel, &AlarmPanel::refresh);

    // 告警通知（浮层 + 声音 + 系统托盘）：自己订阅 AlarmEngine，这里只接浮层的点击
    m_notifier = new AlarmNotifier(this, m_alarmEngine, m_deviceManager);
    connect(m_notifier, &AlarmNotifier::alarmActivated,
            this, &MainWindow::onAlarmToastActivated);

    // 总览页点某台设备的卡片 → 在左树里选中它，并下钻到「实时数据」
    connect(m_overviewPanel, &OverviewPanel::deviceActivated, this,
            [this](const QString &deviceId) {
                if (QTreeWidgetItem *item = findDeviceItem(deviceId)) {
                    m_deviceTree->setCurrentItem(item);
                    m_deviceTree->scrollToItem(item);
                }
                m_rightTabs->setCurrentWidget(m_valueTable);
            });

#ifdef HAVE_QT_CHARTS
    // 趋势图的交互：暂停跟随时把状态写进标题，"曲线为什么不动了"要一眼看得出来
    connect(m_chartView, &AdvancedChartView::autoFollowChanged, this, [this](bool on) {
        const QString name = m_trendDeviceId.isEmpty()
                                 ? QStringLiteral("实时趋势")
                                 : QStringLiteral("实时趋势 — %1")
                                       .arg(m_deviceManager->device(m_trendDeviceId).name);
        m_chart->setTitle(on ? name
                             : QStringLiteral("%1（已暂停跟随，右键复位）").arg(name));
    });
    // 右键复位：立刻把窗口摆回去，不用干等下一拍数据
    connect(m_chartView, &AdvancedChartView::homeRequested, this, [this]() {
        if (!m_trendDeviceId.isEmpty())
            applyTrendHomeRange();
    });
#endif

    // 日志 → 界面
    connect(&Log::instance(), &Log::messageLogged, this, &MainWindow::appendLog);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    // 托盘可用时关闭只是收进托盘：采集和告警提醒都还在跑，
    // 真正的退出走托盘菜单的"退出"。
    if (m_notifier && m_notifier->handleCloseRequest()) {
        event->ignore();
        return;
    }

    QMainWindow::closeEvent(event);
}

void MainWindow::applyPermissions()
{
    const bool admin = (m_role == UserRole::Administrator);

    m_actionAddDevice->setEnabled(admin);
    m_actionEditDevice->setEnabled(admin);
    m_actionRemoveDevice->setEnabled(admin);

    // 导出是只读的，谁都能用来备份；导入会整份替换设备台账，只给管理员
    m_actionImportConfig->setEnabled(admin);

    if (m_rulePanel)
        m_rightTabs->setTabEnabled(m_rightTabs->indexOf(m_rulePanel), admin);
    if (m_controlPanel)
        m_rightTabs->setTabEnabled(m_rightTabs->indexOf(m_controlPanel), admin);

    if (m_controlPanel)
        m_controlPanel->setUser(m_userName);

    if (!admin)
        Log::info(QStringLiteral("以操作员身份登录：设备管理、规则配置、远程控制已锁定"));
}

void MainWindow::onToggleTheme()
{
    m_theme = (m_theme == Theme::Dark) ? Theme::Light : Theme::Dark;
    saveTheme(m_theme);

    refreshTheme();

    Log::info(QStringLiteral("已切换到%1主题").arg(themeName(m_theme)));
}

void MainWindow::onToggleSoundAlarm(bool enabled)
{
    AlarmNotifier::setSoundEnabled(enabled);
    updateSoundAction();

    Log::info(enabled ? QStringLiteral("声音报警已开启") : QStringLiteral("声音报警已关闭"));
}

void MainWindow::updateSoundAction()
{
    if (!m_actionSoundAlarm)
        return;

    const bool enabled = m_actionSoundAlarm->isChecked();
    m_actionSoundAlarm->setIcon(makeIcon(enabled ? IconType::SoundOn : IconType::SoundOff,
                                         themeIconColor(m_theme)));
    m_actionSoundAlarm->setToolTip(enabled
                                       ? QStringLiteral("严重告警时响一声（已开启，点击关闭）")
                                       : QStringLiteral("声音报警已关闭，点击开启"));
}

void MainWindow::onAlarmToastActivated(const QString &deviceId, const QString &tagId)
{
    Q_UNUSED(deviceId);
    Q_UNUSED(tagId);

    // 浮层可能是窗口缩在托盘里的时候弹出来的，点一下就该把窗口叫回来
    if (m_notifier)
        m_notifier->showHost();

    if (m_alarmPanel)
        m_rightTabs->setCurrentWidget(m_alarmPanel);
}

void MainWindow::refreshTheme()
{
    // 样式表与调色板一起换，全局立即生效；各面板不需要知道自己被换肤了
    applyTheme(m_theme);

    // 图标是位图，颜色在绘制时就定死了 —— 换主题必须整批重画
    const QColor iconColor = themeIconColor(m_theme);
    m_actionAddDevice->setIcon(makeIcon(IconType::AddDevice, iconColor));
    m_actionEditDevice->setIcon(makeIcon(IconType::EditDevice, iconColor));
    m_actionRemoveDevice->setIcon(makeIcon(IconType::RemoveDevice, iconColor));
    m_actionStartAcquisition->setIcon(makeIcon(IconType::StartAcquisition, iconColor));
    m_actionStopAcquisition->setIcon(makeIcon(IconType::StopAcquisition, iconColor));
    m_actionAcknowledgeAll->setIcon(makeIcon(IconType::AcknowledgeAll, iconColor));
    m_actionClearAcknowledged->setIcon(makeIcon(IconType::ClearAlarms, iconColor));
    m_actionExportConfig->setIcon(makeIcon(IconType::ExportConfig, iconColor));
    m_actionImportConfig->setIcon(makeIcon(IconType::ImportConfig, iconColor));
    m_actionToggleTheme->setIcon(makeIcon(IconType::ToggleTheme, iconColor));

    // 声音开关的图标取决于勾选状态，一并交给它自己刷新
    updateSoundAction();

    if (m_themeBadge)
        m_themeBadge->setText(QStringLiteral("主题：%1").arg(themeName(m_theme)));

#ifdef HAVE_QT_CHARTS
    refreshChartTheme();
#endif
}

void MainWindow::polishTables()
{
    // 斑马纹 / 行高 / 表头这些是控件属性，QSS 里写不了，统一在这里设一遍。
    // 所有表格都在构造期就建好了，所以只需做一次，换主题时不必重来。
    const QList<QTableWidget *> tables = findChildren<QTableWidget *>();
    for (QTableWidget *table : tables) {
        table->setAlternatingRowColors(true);
        table->setShowGrid(false); // 靠斑马纹分行，比满屏网格线干净
        table->setWordWrap(false);
        table->verticalHeader()->setDefaultSectionSize(30);
        table->horizontalHeader()->setHighlightSections(false); // 免得选中单元格时表头忽粗忽细
        table->horizontalHeader()->setMinimumHeight(34);
    }
}

#ifdef HAVE_QT_CHARTS
void MainWindow::refreshChartTheme()
{
    const ChartColors colors = themeChartColors(m_theme);

    // 十字光标画在图表之上，颜色得跟着主题走，否则暗色主题下浅灰十字几乎看不见
    const QList<AdvancedChartView *> interactive = findChildren<AdvancedChartView *>();
    for (AdvancedChartView *view : interactive)
        view->setCrosshairColor(colors.text);

    // 图表不止一个（实时趋势 / 历史查询），按窗口递归找，省得去动各个面板的代码
    const QList<QChartView *> views = findChildren<QChartView *>();
    for (QChartView *view : views) {
        QChart *chart = view->chart();
        if (!chart)
            continue;

        chart->setBackgroundBrush(QBrush(colors.background));
        chart->setBackgroundPen(Qt::NoPen);
        chart->setTitleBrush(QBrush(colors.text));
        chart->setPlotAreaBackgroundVisible(false);

        if (chart->legend())
            chart->legend()->setLabelColor(colors.text);

        const QList<QAbstractAxis *> axes = chart->axes();
        for (QAbstractAxis *axis : axes) {
            axis->setLabelsColor(colors.text);
            axis->setTitleBrush(QBrush(colors.text));
            axis->setLinePenColor(colors.grid);
            axis->setGridLineColor(colors.grid);
        }
    }
}
#endif

void MainWindow::restoreOrSeedDevices()
{
    // 分组列表先于设备恢复：设备在 addDevice 时会自动补建自己所属的分组，
    // 但它认不出"用户建过、现在还是空的"那些组 —— 那部分只能靠 QSettings。
    m_deviceManager->setGroups(loadGroupsFromSettings());

    QList<DeviceInfo> saved;
    if (m_storage && m_storage->isOpen())
        saved = m_storage->loadDevices();

    if (saved.isEmpty()) {
        // 空库 → 预置一台模拟设备，保证一打开就能看到数据流动
        DeviceInfo info;
        info.name = QStringLiteral("模拟设备 1");
        info.host = QStringLiteral("127.0.0.1");
        info.port = 502;
        info.protocol = DeviceProtocol::Mock;
        info.points = defaultTagPoints();

        const QString id = m_deviceManager->addDevice(info);
        if (m_storage && m_storage->isOpen())
            m_storage->saveDevice(m_deviceManager->device(id));
    } else {
        for (const DeviceInfo &info : saved)
            m_deviceManager->addDevice(info);

        Log::info(QStringLiteral("已从数据库恢复 %1 台设备").arg(saved.size()));
    }

    rebuildDeviceTree();
}

void MainWindow::saveGroupsToSettings()
{
    QSettings settings(QStringLiteral("Y71jj123"),
                       QStringLiteral("qt-industrial-equipment-monitor"));
    settings.setValue(QLatin1String(kGroupsKey), m_deviceManager->groups());
}

QStringList MainWindow::loadGroupsFromSettings() const
{
    QSettings settings(QStringLiteral("Y71jj123"),
                       QStringLiteral("qt-industrial-equipment-monitor"));
    return settings.value(QLatin1String(kGroupsKey)).toStringList();
}

QTreeWidgetItem *MainWindow::findDeviceItem(const QString &deviceId) const
{
    if (deviceId.isEmpty())
        return nullptr;

    for (int i = 0; i < m_deviceTree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *groupItem = m_deviceTree->topLevelItem(i);
        for (int j = 0; j < groupItem->childCount(); ++j) {
            QTreeWidgetItem *child = groupItem->child(j);
            if (child->data(0, kRoleDeviceId).toString() == deviceId)
                return child;
        }
    }
    return nullptr;
}

QString MainWindow::currentDeviceId() const
{
    const QTreeWidgetItem *item = m_deviceTree->currentItem();
    if (!item)
        return QString();

    // 分组节点的 UserRole 是空的，这里天然返回空串
    return item->data(0, kRoleDeviceId).toString();
}

void MainWindow::rebuildDeviceTree()
{
    // 重建会丢了选中状态，先记下来；分组折叠状态也一并保住，
    // 否则每加一台设备、每改一个分组，用户展开的树就全塌回去了。
    const QString keepDevice = currentDeviceId();
    QStringList collapsedGroups;
    for (int i = 0; i < m_deviceTree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *groupItem = m_deviceTree->topLevelItem(i);
        if (!groupItem->isExpanded())
            collapsedGroups.append(groupItem->data(0, kRoleGroupName).toString());
    }

    // 重建期间屏蔽信号：中途的空树会触发一串选中变化，
    // 让趋势图和详情面板跟着闪，而这一切都发生在"用户什么都没点"的时候。
    const QSignalBlocker blocker(m_deviceTree);
    m_deviceTree->clear();

    const QList<DeviceInfo> devices = m_deviceManager->devices();
    const QStringList groups = m_deviceManager->groups();

    for (const QString &group : groups) {
        auto *groupItem = new QTreeWidgetItem(m_deviceTree);
        groupItem->setData(0, kRoleDeviceId, QString());
        groupItem->setData(0, kRoleItemType, int(ItemGroup));
        groupItem->setData(0, kRoleGroupName, group);

        QFont font = groupItem->font(0);
        font.setBold(true);
        groupItem->setFont(0, font);
        groupItem->setIcon(0, makeIcon(IconType::DeviceGroup, themeIconColor(m_theme)));

        for (const DeviceInfo &device : devices) {
            if (device.groupName() != group)
                continue;

            auto *deviceItem = new QTreeWidgetItem(groupItem);
            deviceItem->setData(0, kRoleDeviceId, device.id);
            deviceItem->setData(0, kRoleItemType, int(ItemDevice));
        }

        groupItem->setExpanded(!collapsedGroups.contains(group));
    }

    // 逐个设备补状态灯与文字（分组节点的"在线数 / 总数"也在里面刷）
    for (const DeviceInfo &device : devices)
        updateDeviceTreeItem(device.id);
    updateGroupItems();

    // 恢复选中：原设备还在就选回它，否则退回第一台设备 ——
    // 删掉当前选中的设备后，右侧面板不该停在"没有设备"的空状态。
    QTreeWidgetItem *target = findDeviceItem(keepDevice);
    if (!target) {
        for (int i = 0; i < m_deviceTree->topLevelItemCount() && !target; ++i) {
            QTreeWidgetItem *groupItem = m_deviceTree->topLevelItem(i);
            if (groupItem->childCount() > 0)
                target = groupItem->child(0);
        }
    }

    m_deviceTree->setCurrentItem(target); // 信号已解除屏蔽，这里会正常同步右侧面板
    if (!target)
        syncDeviceSelection(QString());
}

void MainWindow::updateDeviceTreeItem(const QString &deviceId)
{
    QTreeWidgetItem *item = findDeviceItem(deviceId);
    if (!item)
        return;

    const DeviceInfo info = m_deviceManager->device(deviceId);
    if (info.id.isEmpty())
        return;

    const DeviceState state = info.state();

    item->setIcon(0, dotIcon(stateColor(state)));
    item->setText(0, QStringLiteral("%1  ·  %2")
                         .arg(info.name.isEmpty() ? deviceId : info.name, deviceStateName(state)));
    item->setToolTip(0, QStringLiteral("%1\n分组：%2\n协议：%3\n地址：%4:%5\n状态：%6\nID：%7")
                            .arg(info.name.isEmpty() ? deviceId : info.name,
                                 info.groupName(),
                                 protocolName(info.protocol),
                                 info.host)
                            .arg(info.port)
                            .arg(deviceStateName(state), info.id));
}

void MainWindow::updateGroupItems()
{
    const QList<DeviceInfo> devices = m_deviceManager->devices();

    for (int i = 0; i < m_deviceTree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *groupItem = m_deviceTree->topLevelItem(i);
        const QString group = groupItem->data(0, kRoleGroupName).toString();

        int total = 0;
        int online = 0;
        for (const DeviceInfo &device : devices) {
            if (device.groupName() != group)
                continue;
            ++total;
            if (device.online)
                ++online;
        }

        groupItem->setText(0, QStringLiteral("%1  (%2/%3)").arg(group).arg(online).arg(total));
        groupItem->setToolTip(
            0, QStringLiteral("分组「%1」：%2 台设备，在线 %3 台\n右键可重命名 / 删除该分组")
                   .arg(group)
                   .arg(total)
                   .arg(online));
    }
}

void MainWindow::syncDeviceSelection(const QString &deviceId)
{
#ifdef HAVE_QT_CHARTS
    if (m_trendDeviceId != deviceId)
        buildTrendSeries(deviceId);
#endif

    if (m_detailPanel)
        m_detailPanel->setDevice(deviceId);
}

void MainWindow::onDeviceTreeSelectionChanged()
{
    const QString deviceId = currentDeviceId();
    if (deviceId.isEmpty())
        return; // 分组节点：只展开 / 折叠，右侧面板保持原样

    syncDeviceSelection(deviceId);
}

void MainWindow::reloadDeviceDependentPanels()
{
    if (m_overviewPanel)
        m_overviewPanel->refresh();
    if (m_historyPanel)
        m_historyPanel->reloadDevices();
    if (m_alarmHistoryPanel)
        m_alarmHistoryPanel->reloadDevices();
    if (m_rulePanel)
        m_rulePanel->reload();
    if (m_controlPanel)
        m_controlPanel->reload();
}

void MainWindow::onAddDevice()
{
    DeviceDialog dialog(this);
    dialog.setGroups(m_deviceManager->groups());
    if (dialog.exec() != QDialog::Accepted)
        return;

    const DeviceInfo info = dialog.device();
    const QString id = m_deviceManager->addDevice(info);
    if (id.isEmpty())
        return;

    const DeviceInfo saved = m_deviceManager->device(id);

    // 持久化 + 默认规则
    if (m_storage && m_storage->isOpen())
        m_storage->saveDevice(saved);

    ensureDefaultRules(m_alarmEngine, id,
                       saved.points.isEmpty() ? defaultTagPoints() : saved.points);

    reloadDeviceDependentPanels();
    m_scheduler->start(id);
}

void MainWindow::onEditDevice()
{
    const QString id = currentDeviceId();
    if (id.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("编辑设备"),
                                 QStringLiteral("请先在左侧选择一台设备（点分组节点是不算的）。"));
        return;
    }

    const DeviceInfo info = m_deviceManager->device(id);

    DeviceDialog dialog(this);
    dialog.setGroups(m_deviceManager->groups());
    dialog.setDevice(info);
    if (dialog.exec() != QDialog::Accepted)
        return;

    const DeviceInfo updated = dialog.device();
    const bool wasRunning = m_scheduler->isRunning(id);

    m_scheduler->stop(id);
    m_deviceManager->updateDevice(updated);

    if (m_storage && m_storage->isOpen())
        m_storage->saveDevice(updated);

    reloadDeviceDependentPanels();

    if (wasRunning)
        m_scheduler->start(id);

    Log::info(QStringLiteral("设备 %1 配置已更新").arg(updated.name));
}

void MainWindow::onRemoveDevice()
{
    const QString id = currentDeviceId();
    if (id.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("移除设备"),
                                 QStringLiteral("请先在左侧选择一台设备（点分组节点是不算的）。"));
        return;
    }

    const DeviceInfo info = m_deviceManager->device(id);
    const QString name = info.name.isEmpty() ? id : info.name;

    const auto answer = QMessageBox::question(
        this, QStringLiteral("移除设备"),
        QStringLiteral("确定移除设备「%1」吗？其采集将停止。\n（历史数据不会被删除）").arg(name),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;

    m_scheduler->stop(id);

    // 清掉该设备的告警规则，避免留下悬空规则
    const QList<AlarmRule> rules = m_alarmEngine->rules();
    for (const AlarmRule &rule : rules) {
        if (rule.deviceId == id)
            m_alarmEngine->removeRule(id, rule.tagId);
    }

    if (m_storage && m_storage->isOpen())
        m_storage->removeDevice(id);

    m_deviceManager->removeDevice(id);
    reloadDeviceDependentPanels();
    Log::info(QStringLiteral("已移除设备 %1").arg(name));
}

void MainWindow::onStartAll()
{
    for (const DeviceInfo &device : m_deviceManager->devices()) {
        // 只给还没有规则的点位补默认值 —— 否则每点一次「开始采集」，
        // 用户改过的阈值和导入进来的规则都会被默认值冲掉
        ensureDefaultRules(m_alarmEngine, device.id,
                           device.points.isEmpty() ? defaultTagPoints() : device.points);
        m_scheduler->start(device.id);
    }
    updateStatus();
}

void MainWindow::onStopAll()
{
    m_scheduler->stopAll();
    updateStatus();
    if (m_detailPanel)
        m_detailPanel->refresh();
}

void MainWindow::onAcknowledgeAll()
{
    m_alarmEngine->acknowledgeAll();
    updateStatus();
}

void MainWindow::onClearAcknowledged()
{
    m_alarmEngine->clearAcknowledged();
    updateStatus();
}

void MainWindow::onDeviceAdded(const DeviceInfo &device)
{
    Q_UNUSED(device);
    rebuildDeviceTree();
    updateStatus();
}

void MainWindow::onDeviceRemoved(const QString &deviceId)
{
#ifdef HAVE_QT_CHARTS
    // 顺序要紧：重建树会恢复选中并触发 buildTrendSeries，
    // 必须先把"当前设备就是被删的那台"这个状态清掉，
    // 否则重建后 m_trendDeviceId 还指着已删的设备，曲线会留着一份幽灵数据。
    if (m_trendDeviceId == deviceId) {
        buildTrendSeries(QString());
        m_trendDeviceId.clear();
    }
#endif

    rebuildDeviceTree();

    // 该设备的历史采样行已失效：清空表格，数据会随下次采样重建
    m_valueTable->setRowCount(0);
    m_rowIndex.clear();

    // 待刷新队列里属于这台设备的条目也要丢掉，否则下一拍会把刚清掉的行又建回来
    for (auto it = m_pendingValues.begin(); it != m_pendingValues.end();) {
        if (it->deviceId == deviceId)
            it = m_pendingValues.erase(it);
        else
            ++it;
    }

    updateStatus();
}

#ifdef HAVE_QT_CHARTS
void MainWindow::buildTrendSeries(const QString &deviceId)
{
    const auto keys = m_trendSeries.keys();
    for (const QString &k : keys) {
        QLineSeries *s = m_trendSeries.value(k);
        m_chart->removeSeries(s);
        delete s;
    }
    m_trendSeries.clear();
    m_trendDeviceId = deviceId;

    // 换设备等于换了一张图：缩放 / 平移的痕迹不该带过去，恢复默认跟随
    m_chartView->setAutoFollow(true);
    m_chartView->setHomeRange(QDateTime(), QDateTime(), 0.0, 0.0);

    m_chart->setTitle(deviceId.isEmpty()
                          ? QStringLiteral("实时趋势")
                          : QStringLiteral("实时趋势 — %1")
                                .arg(m_deviceManager->device(deviceId).name));
}

void MainWindow::applyTrendHomeRange()
{
    const qint64 now = QDateTime::currentDateTime().toMSecsSinceEpoch();
    const QDateTime from = QDateTime::fromMSecsSinceEpoch(now - m_trendWindowMs);
    const QDateTime to = QDateTime::fromMSecsSinceEpoch(now);

    // 纵轴跟着数据走。不给范围的话 QValueAxis 会停在默认的 0~10，
    // 温度、转速这些几十上百的量纲会被整条切掉，曲线看起来"什么都没有"。
    double lo = 0.0;
    double hi = 0.0;
    bool first = true;
    for (QLineSeries *series : std::as_const(m_trendSeries)) {
        const QList<QPointF> points = series->points();
        for (const QPointF &point : points) {
            if (first) {
                lo = hi = point.y();
                first = false;
            } else {
                lo = qMin(lo, point.y());
                hi = qMax(hi, point.y());
            }
        }
    }

    const double pad = qMax(1.0, (hi - lo) * 0.15);
    m_axisX->setRange(from, to);
    m_axisY->setRange(lo - pad, hi + pad);
    m_chartView->setHomeRange(from, to, lo - pad, hi + pad);
}
#endif

void MainWindow::onTagUpdated(const QString &deviceId, const QString &tagId, const QVariant &value)
{
    // 落库不跟着节流走：数据一条都不能少（攒批提交是 DataStorage 内部的事）
    if (m_storage && m_storage->isOpen())
        m_storage->insertSample(deviceId, tagId, value.toDouble());

    // 表格 / 曲线 / 各面板统一攒到 100ms 后合并刷新。
    // 同一个点位在这 100ms 里来 10 个值，也只画最后一个 —— 中间值肉眼看不到。
    const QString key = keyOf(deviceId, tagId);
    m_pendingValues.insert(key, PendingValue{deviceId, tagId, value});

    if (m_uiRefreshTimer && !m_uiRefreshTimer->isActive())
        m_uiRefreshTimer->start();
}

void MainWindow::flushPendingValues()
{
    if (m_pendingValues.isEmpty())
        return;

    // 先摘出来再刷：刷新过程中若有新数据到达，会重新起一个 100ms 的窗口，
    // 不会挤进这一批里来回改。
    const QList<PendingValue> batch = m_pendingValues.values();
    m_pendingValues.clear();

    for (const PendingValue &pending : batch) {
        updateValueRow(pending.deviceId, pending.tagId, pending.value);

        if (m_detailPanel)
            m_detailPanel->updateValue(pending.deviceId, pending.tagId, pending.value);
        if (m_controlPanel)
            m_controlPanel->updateCurrentValue(pending.deviceId, pending.tagId, pending.value);

#ifdef HAVE_QT_CHARTS
        appendTrendPoint(pending.deviceId, pending.tagId, pending.value.toDouble());
#endif
    }
}

void MainWindow::updateValueRow(const QString &deviceId, const QString &tagId, const QVariant &value)
{
    const QString key = keyOf(deviceId, tagId);
    int row = m_rowIndex.value(key, -1);

    if (row < 0) {
        row = m_valueTable->rowCount();
        m_valueTable->insertRow(row);
        m_rowIndex.insert(key, row);

        const DeviceInfo info = m_deviceManager->device(deviceId);
        m_valueTable->setItem(row, 0, new QTableWidgetItem(info.name));
        m_valueTable->setItem(row, 1, new QTableWidgetItem(tagId));
        m_valueTable->setItem(row, 2, new QTableWidgetItem());
        m_valueTable->setItem(row, 3, new QTableWidgetItem());
    }

    if (QTableWidgetItem *valueItem = m_valueTable->item(row, 2))
        valueItem->setText(QString::number(value.toDouble(), 'f', 2));
    if (QTableWidgetItem *timeItem = m_valueTable->item(row, 3))
        timeItem->setText(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")));
}

#ifdef HAVE_QT_CHARTS
void MainWindow::appendTrendPoint(const QString &deviceId, const QString &tagId, double value)
{
    if (deviceId != m_trendDeviceId)
        return;

    QLineSeries *series = m_trendSeries.value(tagId, nullptr);
    if (!series) {
        static const QColor palette[] = {
            QColor(220, 80, 80), QColor(80, 140, 220), QColor(80, 180, 120),
            QColor(230, 180, 60), QColor(160, 90, 200), QColor(80, 190, 200)};
        const int n = static_cast<int>(sizeof(palette) / sizeof(palette[0]));
        series = new QLineSeries();
        series->setName(tagId);
        series->setColor(palette[m_trendSeries.size() % n]);
        m_chart->addSeries(series);
        series->attachAxis(m_axisX);
        series->attachAxis(m_axisY);
        m_trendSeries.insert(tagId, series);
    }

    // 横轴用刷新时刻：同一批里的点相差不超过一个节流周期，肉眼分辨不出
    const qint64 now = QDateTime::currentDateTime().toMSecsSinceEpoch();
    series->append(now, value);
    if (series->count() > m_trendMaxPoints)
        series->remove(0);

    // 用户一旦缩放 / 平移过，就不再抢方向盘 —— 否则这一拍刚把视图摆好，
    // 100ms 后又被拽回"最新 60 秒"，缩放等于白做。右键复位会重新打开跟随。
    if (m_chartView->autoFollow())
        applyTrendHomeRange();
}
#endif

void MainWindow::onDeviceTreeContextMenu(const QPoint &pos)
{
    QTreeWidgetItem *item = m_deviceTree->itemAt(pos);
    const bool admin = (m_role == UserRole::Administrator);

    // 右键不改选中项：菜单操作的对象由 itemAt 决定。
    // 若让它顺带改选中，用户在设备上右键想"移到分组"，右侧趋势图会先跳一下。
    QMenu menu(this);

    QAction *newGroupAction = menu.addAction(QStringLiteral("新建分组…"));
    newGroupAction->setEnabled(admin);

    QAction *renameAction = nullptr;
    QAction *removeGroupAction = nullptr;
    QAction *editAction = nullptr;
    QAction *removeDeviceAction = nullptr;

    const QString groupName =
        item && item->data(0, kRoleItemType).toInt() == int(ItemGroup)
            ? item->data(0, kRoleGroupName).toString()
            : QString();
    const QString deviceId = item ? item->data(0, kRoleDeviceId).toString() : QString();

    if (!groupName.isEmpty()) {
        menu.addSeparator();
        renameAction = menu.addAction(QStringLiteral("重命名分组「%1」…").arg(groupName));
        removeGroupAction = menu.addAction(QStringLiteral("删除分组「%1」").arg(groupName));

        // 默认分组是"回落到默认分组"这句话的落脚点，删了它设备就无处可去
        const bool isDefault = (groupName == defaultGroupName());
        renameAction->setEnabled(admin && !isDefault);
        removeGroupAction->setEnabled(admin && !isDefault);
    } else if (!deviceId.isEmpty()) {
        menu.addSeparator();

        QMenu *moveMenu = menu.addMenu(QStringLiteral("移动到分组"));
        const QString current = m_deviceManager->device(deviceId).groupName();
        for (const QString &group : m_deviceManager->groups()) {
            QAction *action = moveMenu->addAction(group);
            action->setCheckable(true);
            action->setChecked(group == current);
            action->setEnabled(admin && group != current);
            connect(action, &QAction::triggered, this, [this, deviceId, group]() {
                if (!m_deviceManager->setDeviceGroup(deviceId, group))
                    return;

                const DeviceInfo info = m_deviceManager->device(deviceId);
                if (m_storage && m_storage->isOpen())
                    m_storage->saveDevice(info);
                Log::info(QStringLiteral("设备 %1 已移动到分组 %2").arg(info.name, group));
            });
        }

        menu.addSeparator();
        editAction = menu.addAction(QStringLiteral("编辑设备…"));
        removeDeviceAction = menu.addAction(QStringLiteral("移除设备…"));
        editAction->setEnabled(admin);
        removeDeviceAction->setEnabled(admin);
    }

    QAction *chosen = menu.exec(m_deviceTree->viewport()->mapToGlobal(pos));
    if (!chosen)
        return;

    if (chosen == newGroupAction) {
        bool ok = false;
        const QString name = QInputDialog::getText(this, QStringLiteral("新建分组"),
                                                   QStringLiteral("分组名称："),
                                                   QLineEdit::Normal, QString(), &ok)
                                 .trimmed();
        if (!ok || name.isEmpty())
            return;

        if (!m_deviceManager->addGroup(name)) {
            QMessageBox::information(this, QStringLiteral("新建分组"),
                                     QStringLiteral("分组「%1」已存在。").arg(name));
        }
    } else if (chosen == renameAction) {
        bool ok = false;
        const QString name =
            QInputDialog::getText(this, QStringLiteral("重命名分组"),
                                  QStringLiteral("新的分组名称："),
                                  QLineEdit::Normal, groupName, &ok)
                .trimmed();
        if (!ok || name.isEmpty() || name == groupName)
            return;

        if (!m_deviceManager->renameGroup(groupName, name)) {
            QMessageBox::information(this, QStringLiteral("重命名分组"),
                                     QStringLiteral("重命名失败：名称「%1」已被占用。").arg(name));
            return;
        }

        // 组内设备的 group 字段变了，库里的记录也得跟着改
        for (const DeviceInfo &device : m_deviceManager->devices()) {
            if (device.groupName() != name)
                continue;
            if (m_storage && m_storage->isOpen())
                m_storage->saveDevice(device);
        }
    } else if (chosen == removeGroupAction) {
        int affected = 0;
        for (const DeviceInfo &device : m_deviceManager->devices()) {
            if (device.groupName() == groupName)
                ++affected;
        }

        const auto answer = QMessageBox::question(
            this, QStringLiteral("删除分组"),
            QStringLiteral("确定删除分组「%1」吗？\n\n"
                           "该分组下的 %2 台设备会回落到「%3」，**设备本身不会被删除**，"
                           "采集也会照常继续。")
                .arg(groupName)
                .arg(affected)
                .arg(defaultGroupName()),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes)
            return;

        if (!m_deviceManager->removeGroup(groupName))
            return;

        for (const DeviceInfo &device : m_deviceManager->devices()) {
            if (device.groupName() != defaultGroupName())
                continue;
            if (m_storage && m_storage->isOpen())
                m_storage->saveDevice(device);
        }
    } else if (editAction && chosen == editAction) {
        // 树里没选中这台设备，先选中再走统一的编辑流程
        if (QTreeWidgetItem *target = findDeviceItem(deviceId))
            m_deviceTree->setCurrentItem(target);
        onEditDevice();
    } else if (removeDeviceAction && chosen == removeDeviceAction) {
        if (QTreeWidgetItem *target = findDeviceItem(deviceId))
            m_deviceTree->setCurrentItem(target);
        onRemoveDevice();
    }
}

void MainWindow::onExportConfig()
{
    const QString suggested =
        QStringLiteral("monitor-config-%1.json")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")));

    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("导出配置"),
                                                      suggested,
                                                      QStringLiteral("JSON 配置文件 (*.json)"));
    if (path.isEmpty())
        return;

    const QList<DeviceInfo> devices = m_deviceManager->devices();
    const QJsonObject object =
        ConfigIo::toJson(devices, m_deviceManager->groups(), m_alarmEngine->rules());

    QString error;
    if (!ConfigIo::saveToFile(path, object, &error)) {
        QMessageBox::warning(this, QStringLiteral("导出失败"), error);
        Log::error(QStringLiteral("配置导出失败: %1").arg(error));
        return;
    }

    Log::info(QStringLiteral("配置已导出到 %1（%2 台设备）").arg(path).arg(devices.size()));
    QMessageBox::information(this, QStringLiteral("导出成功"),
                             QStringLiteral("已导出 %1 台设备（含点位表）、%2 个分组、%3 条告警规则到：\n%4")
                                 .arg(devices.size())
                                 .arg(m_deviceManager->groups().size())
                                 .arg(m_alarmEngine->rules().size())
                                 .arg(path));
}

void MainWindow::onImportConfig()
{
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("导入配置"),
                                                      QString(),
                                                      QStringLiteral("JSON 配置文件 (*.json)"));
    if (path.isEmpty())
        return;

    QJsonObject object;
    QString error;
    if (!ConfigIo::loadFromFile(path, &object, &error)) {
        QMessageBox::warning(this, QStringLiteral("导入失败"), error);
        return;
    }

    ConfigIo::Config config;
    if (!ConfigIo::fromJson(object, &config, &error)) {
        QMessageBox::warning(this, QStringLiteral("导入失败"), error);
        return;
    }

    const int currentCount = m_deviceManager->devices().size();
    const auto answer = QMessageBox::question(
        this, QStringLiteral("导入配置"),
        QStringLiteral("即将从文件导入：\n"
                       "  设备 %1 台（当前 %2 台）\n"
                       "  分组 %3 个\n"
                       "  告警规则 %4 条\n\n"
                       "导入会**整体替换**当前的设备台账：当前的 %2 台设备及其告警规则"
                       "将被移除，采集会停止并按新配置重新开始。\n"
                       "历史采样数据与告警历史**不受影响**。\n\n确定继续吗？")
            .arg(config.devices.size())
            .arg(currentCount)
            .arg(config.groups.size())
            .arg(config.rules.size()),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;

    applyImportedConfig(config);
}

void MainWindow::applyImportedConfig(const ConfigIo::Config &config)
{
    // 先停采集：设备对象一会儿要被换掉，线程还挂在上面的会很难收场
    m_scheduler->stopAll();

    m_alarmEngine->clearRules();
    m_deviceManager->clear(); // 逐台发 deviceRemoved，树与各面板跟着清空
    if (m_storage && m_storage->isOpen())
        m_storage->clearDevices();

    m_deviceManager->setGroups(config.groups);

    int imported = 0;
    for (const DeviceInfo &device : config.devices) {
        const QString id = m_deviceManager->addDevice(device);
        if (id.isEmpty())
            continue;

        ++imported;
        const DeviceInfo saved = m_deviceManager->device(id);
        if (m_storage && m_storage->isOpen())
            m_storage->saveDevice(saved);
    }

    for (const AlarmRule &rule : config.rules)
        m_alarmEngine->addRule(rule);

    // 文件里没带规则的点位补默认值（手工写的最小配置也能直接用）
    for (const DeviceInfo &device : m_deviceManager->devices()) {
        ensureDefaultRules(m_alarmEngine, device.id,
                           device.points.isEmpty() ? defaultTagPoints() : device.points);
    }

    saveGroupsToSettings();
    rebuildDeviceTree();
    reloadDeviceDependentPanels();
    onStartAll(); // 停过的采集重新拉起来

    // 表格与曲线里的旧设备数据已经失效，清掉等新数据重建
    m_valueTable->setRowCount(0);
    m_rowIndex.clear();
    m_pendingValues.clear();

    Log::info(QStringLiteral("配置导入完成：%1 台设备、%2 条告警规则")
                  .arg(imported)
                  .arg(m_alarmEngine->rules().size()));

    QMessageBox::information(this, QStringLiteral("导入完成"),
                             QStringLiteral("已导入 %1 台设备并重新开始采集。").arg(imported));
}

void MainWindow::onAlarmRaised(const AlarmRecord &record)
{
    statusBar()->showMessage(
        QStringLiteral("⚠ [%1] %2").arg(alarmLevelName(record.level), record.message), 8000);

    // 持久化告警历史
    if (m_storage && m_storage->isOpen())
        m_storage->insertAlarm(record);

    updateStatus();
}

void MainWindow::onAlarmCleared(const QString &deviceId, const QString &tagId)
{
    if (m_storage && m_storage->isOpen())
        m_storage->markAlarmInactive(deviceId, tagId);
}

void MainWindow::onSyncAlarmAck()
{
    if (!m_storage || !m_storage->isOpen())
        return;

    // 活动告警里已确认的，同步到数据库（告警数量很少，全量同步开销可忽略）
    const QList<AlarmRecord> alarms = m_alarmEngine->activeAlarms();
    for (const AlarmRecord &record : alarms) {
        if (record.acknowledged)
            m_storage->markAlarmAcknowledged(record.deviceId, record.tagId);
    }
}

void MainWindow::appendLog(int level, const QString &message)
{
    const QString prefix = level == Log::Error ? QStringLiteral("[错误] ")
                           : level == Log::Warn ? QStringLiteral("[警告] ")
                                                : QStringLiteral("[信息] ");
    m_logView->appendPlainText(prefix + message);
}

void MainWindow::updateStatus()
{
    const int deviceCount = m_deviceManager->devices().size();
    int onlineCount = 0;
    for (const DeviceInfo &device : m_deviceManager->devices()) {
        if (device.online)
            ++onlineCount;
    }

    int activeCount = 0;
    int unacknowledged = 0;
    const QList<AlarmRecord> alarms = m_alarmEngine->activeAlarms();
    for (const AlarmRecord &record : alarms) {
        ++activeCount;
        if (!record.acknowledged)
            ++unacknowledged;
    }

    m_statusLabel->setText(QStringLiteral("设备 %1 台（在线 %2）  |  活动告警 %3 条（未确认 %4）")
                               .arg(deviceCount)
                               .arg(onlineCount)
                               .arg(activeCount)
                               .arg(unacknowledged));
}
