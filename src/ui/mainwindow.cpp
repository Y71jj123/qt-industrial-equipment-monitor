#include "ui/mainwindow.h"

#include "core/acquisitionscheduler.h"
#include "storage/datastorage.h"
#include "ui/alarmpanel.h"
#include "ui/controlpanel.h"
#include "ui/devicedetailpanel.h"
#include "ui/devicedialog.h"
#include "ui/historypanel.h"
#include "ui/reportpanel.h"
#include "ui/rulepanel.h"
#include "utils/logger.h"

#include <QAbstractItemView>
#include <QAction>
#include <QDialog>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTime>
#include <QToolBar>

#ifdef HAVE_QT_CHARTS
#include <QColor>
#include <QDateTime>
#include <QtCharts/QChart>
#include <QtCharts/QChartView>
#include <QtCharts/QDateTimeAxis>
#include <QtCharts/QLineSeries>
#include <QtCharts/QValueAxis>
#endif

namespace {

/// 给设备的点位预置默认告警规则（阈值型）。
/// 这是"开箱可用"的默认值，用户可在「规则配置」里改。
void installDefaultRules(AlarmEngine *engine, const QString &deviceId, const QList<TagPoint> &points)
{
    for (const TagPoint &point : points) {
        if (point.boolean)
            continue; // 开关量不做阈值告警

        double high = 100.0;
        if (point.id == QLatin1String("temperature"))
            high = 60.0;
        else if (point.id == QLatin1String("pressure"))
            high = 80.0;
        else if (point.id == QLatin1String("speed"))
            high = 90.0;
        else if (point.id == QLatin1String("vibration"))
            high = 70.0;

        engine->addRule(AlarmRule{deviceId, point.id, AlarmKind::Range, 0.0, high, 0.0, true});
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
{
    m_scheduler = new AcquisitionScheduler(m_deviceManager, this);

    setupUi();
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
    setWindowTitle(QStringLiteral("工业设备远程监控管理平台  v%1").arg(QStringLiteral(APP_VERSION)));
    resize(1360, 820);

    QToolBar *toolBar = addToolBar(QStringLiteral("主工具栏"));
    toolBar->setMovable(false);

    QAction *addAction = toolBar->addAction(QStringLiteral("添加设备"), this, &MainWindow::onAddDevice);
    QAction *editAction = toolBar->addAction(QStringLiteral("编辑设备"), this, &MainWindow::onEditDevice);
    QAction *removeAction = toolBar->addAction(QStringLiteral("移除设备"), this, &MainWindow::onRemoveDevice);

    toolBar->addSeparator();
    toolBar->addAction(QStringLiteral("开始采集"), this, &MainWindow::onStartAll);
    toolBar->addAction(QStringLiteral("停止采集"), this, &MainWindow::onStopAll);

    toolBar->addSeparator();
    toolBar->addAction(QStringLiteral("确认全部告警"), this, &MainWindow::onAcknowledgeAll);
    toolBar->addAction(QStringLiteral("消警（已确认）"), this, &MainWindow::onClearAcknowledged);

    addAction->setObjectName(QStringLiteral("actionAddDevice"));
    editAction->setObjectName(QStringLiteral("actionEditDevice"));
    removeAction->setObjectName(QStringLiteral("actionRemoveDevice"));

    // 左：设备列表
    m_deviceList = new QListWidget(this);
    m_deviceList->setMinimumWidth(250);
    m_deviceList->setIconSize(QSize(14, 14));

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

    m_chartView = new QChartView(m_chart, this);
    m_chartView->setRenderHint(QPainter::Antialiasing);
    m_rightTabs->addTab(m_chartView, QStringLiteral("趋势曲线"));
#endif

    m_alarmPanel = new AlarmPanel(m_alarmEngine, m_deviceManager, this);
    m_rightTabs->addTab(m_alarmPanel, QStringLiteral("告警"));

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
    mainSplitter->addWidget(m_deviceList);
    mainSplitter->addWidget(rightSplitter);
    mainSplitter->setStretchFactor(0, 0);
    mainSplitter->setStretchFactor(1, 1);

    setCentralWidget(mainSplitter);

    connect(m_deviceList, &QListWidget::currentItemChanged,
            this, &MainWindow::onDeviceSelected);

    m_statusLabel = new QLabel(this);
    statusBar()->addWidget(m_statusLabel);

    m_userLabel = new QLabel(this);
    m_userLabel->setObjectName(QStringLiteral("summaryLabel"));
    m_userLabel->setText(QStringLiteral("用户：%1（%2）").arg(m_userName, userRoleName(m_role)));
    statusBar()->addPermanentWidget(m_userLabel);

    updateStatus();
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
                updateDeviceListItem(deviceId);
                if (m_detailPanel)
                    m_detailPanel->refresh();
            });

    connect(m_deviceManager, &DeviceManager::deviceAdded,
            this, &MainWindow::onDeviceAdded);
    connect(m_deviceManager, &DeviceManager::deviceRemoved,
            this, &MainWindow::onDeviceRemoved);
    connect(m_deviceManager, &DeviceManager::onlineChanged, this,
            [this](const QString &deviceId, bool) {
                updateStatus();
                updateDeviceListItem(deviceId);
            });
    connect(m_deviceManager, &DeviceManager::faultChanged, this,
            [this](const QString &deviceId, bool) {
                updateStatus();
                updateDeviceListItem(deviceId);
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

    // 日志 → 界面
    connect(&Log::instance(), &Log::messageLogged, this, &MainWindow::appendLog);
}

void MainWindow::applyPermissions()
{
    const bool admin = (m_role == UserRole::Administrator);

    if (QAction *action = findChild<QAction *>(QStringLiteral("actionAddDevice")))
        action->setEnabled(admin);
    if (QAction *action = findChild<QAction *>(QStringLiteral("actionEditDevice")))
        action->setEnabled(admin);
    if (QAction *action = findChild<QAction *>(QStringLiteral("actionRemoveDevice")))
        action->setEnabled(admin);

    if (m_rulePanel)
        m_rightTabs->setTabEnabled(m_rightTabs->indexOf(m_rulePanel), admin);
    if (m_controlPanel)
        m_rightTabs->setTabEnabled(m_rightTabs->indexOf(m_controlPanel), admin);

    if (m_controlPanel)
        m_controlPanel->setUser(m_userName);

    if (!admin)
        Log::info(QStringLiteral("以操作员身份登录：设备管理、规则配置、远程控制已锁定"));
}

void MainWindow::restoreOrSeedDevices()
{
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
        return;
    }

    for (const DeviceInfo &info : saved)
        m_deviceManager->addDevice(info);

    Log::info(QStringLiteral("已从数据库恢复 %1 台设备").arg(saved.size()));
}

void MainWindow::reloadDeviceDependentPanels()
{
    if (m_historyPanel)
        m_historyPanel->reloadDevices();
    if (m_rulePanel)
        m_rulePanel->reload();
    if (m_controlPanel)
        m_controlPanel->reload();
}

void MainWindow::onAddDevice()
{
    DeviceDialog dialog(this);
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

    installDefaultRules(m_alarmEngine, id,
                        saved.points.isEmpty() ? defaultTagPoints() : saved.points);

    reloadDeviceDependentPanels();
    m_scheduler->start(id);
}

void MainWindow::onEditDevice()
{
    QListWidgetItem *item = m_deviceList->currentItem();
    if (!item) {
        QMessageBox::information(this, QStringLiteral("编辑设备"),
                                 QStringLiteral("请先在左侧选择一台设备。"));
        return;
    }

    const QString id = item->data(Qt::UserRole).toString();
    const DeviceInfo info = m_deviceManager->device(id);

    DeviceDialog dialog(this);
    dialog.setDevice(info);
    if (dialog.exec() != QDialog::Accepted)
        return;

    const DeviceInfo updated = dialog.device();
    const bool wasRunning = m_scheduler->isRunning(id);

    m_scheduler->stop(id);
    m_deviceManager->updateDevice(updated);

    if (m_storage && m_storage->isOpen())
        m_storage->saveDevice(updated);

    updateDeviceListItem(id);
    reloadDeviceDependentPanels();

    if (wasRunning)
        m_scheduler->start(id);

    Log::info(QStringLiteral("设备 %1 配置已更新").arg(updated.name));
}

void MainWindow::onRemoveDevice()
{
    QListWidgetItem *item = m_deviceList->currentItem();
    if (!item) {
        QMessageBox::information(this, QStringLiteral("移除设备"),
                                 QStringLiteral("请先在左侧选择一台设备。"));
        return;
    }

    const QString id = item->data(Qt::UserRole).toString();
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
        installDefaultRules(m_alarmEngine, device.id,
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
    auto *item = new QListWidgetItem(m_deviceList);
    item->setData(Qt::UserRole, device.id);
    updateDeviceListItem(device.id);
    updateStatus();

    if (!m_deviceList->currentItem())
        m_deviceList->setCurrentRow(0); // 触发选中 → 同步趋势图与详情
}

void MainWindow::onDeviceRemoved(const QString &deviceId)
{
    for (int i = 0; i < m_deviceList->count(); ++i) {
        QListWidgetItem *item = m_deviceList->item(i);
        if (item && item->data(Qt::UserRole).toString() == deviceId) {
            delete m_deviceList->takeItem(i);
            break;
        }
    }

    // 该设备的历史采样行已失效：清空表格，数据会随下次采样重建
    m_valueTable->setRowCount(0);
    m_rowIndex.clear();

#ifdef HAVE_QT_CHARTS
    if (m_trendDeviceId == deviceId) {
        buildTrendSeries(QString());
        m_trendDeviceId.clear();
    }
#endif

    if (m_detailPanel) {
        if (m_deviceList->count() > 0)
            m_deviceList->setCurrentRow(0);
        else
            m_detailPanel->setDevice(QString());
    }

    updateStatus();
}

void MainWindow::updateDeviceListItem(const QString &deviceId)
{
    const DeviceInfo info = m_deviceManager->device(deviceId);
    if (info.id.isEmpty())
        return;

    for (int i = 0; i < m_deviceList->count(); ++i) {
        QListWidgetItem *item = m_deviceList->item(i);
        if (!item || item->data(Qt::UserRole).toString() != deviceId)
            continue;

        const DeviceState state = info.state();

        QColor color;
        switch (state) {
        case DeviceState::Online:
            color = QColor(47, 180, 107);
            break;
        case DeviceState::Fault:
            color = QColor(232, 145, 45);
            break;
        case DeviceState::Offline:
        default:
            color = QColor(149, 163, 178);
            break;
        }

        item->setIcon(dotIcon(color));
        item->setText(QStringLiteral("%1\n%2:%3  ·  %4")
                          .arg(info.name.isEmpty() ? deviceId : info.name)
                          .arg(info.host)
                          .arg(info.port)
                          .arg(deviceStateName(state)));
        item->setToolTip(QStringLiteral("%1\n协议：%2\n状态：%3\nID：%4")
                             .arg(info.name.isEmpty() ? deviceId : info.name,
                                  protocolName(info.protocol),
                                  deviceStateName(state),
                                  info.id));
        return;
    }
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
}
#endif

void MainWindow::onDeviceSelected(QListWidgetItem *current, QListWidgetItem *previous)
{
    Q_UNUSED(previous);
    if (!current)
        return;

    const QString deviceId = current->data(Qt::UserRole).toString();
    if (deviceId.isEmpty())
        return;

#ifdef HAVE_QT_CHARTS
    buildTrendSeries(deviceId);
#endif

    if (m_detailPanel)
        m_detailPanel->setDevice(deviceId);
}

void MainWindow::onTagUpdated(const QString &deviceId, const QString &tagId, const QVariant &value)
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

    const double v = value.toDouble();

    if (QTableWidgetItem *valueItem = m_valueTable->item(row, 2))
        valueItem->setText(QString::number(v, 'f', 2));
    if (QTableWidgetItem *timeItem = m_valueTable->item(row, 3))
        timeItem->setText(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")));

    if (m_storage && m_storage->isOpen())
        m_storage->insertSample(deviceId, tagId, v);

    if (m_detailPanel)
        m_detailPanel->updateValue(deviceId, tagId, value);
    if (m_controlPanel)
        m_controlPanel->updateCurrentValue(deviceId, tagId, value);

#ifdef HAVE_QT_CHARTS
    if (deviceId == m_trendDeviceId) {
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
        const qint64 now = QDateTime::currentDateTime().toMSecsSinceEpoch();
        series->append(now, v);
        if (series->count() > m_trendMaxPoints)
            series->remove(0);
        const qint64 window = 60000; // 滚动窗口 60s
        m_axisX->setRange(QDateTime::fromMSecsSinceEpoch(now - window),
                          QDateTime::fromMSecsSinceEpoch(now));
    }
#endif
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
