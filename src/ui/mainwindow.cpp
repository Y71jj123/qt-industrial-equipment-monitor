#include "ui/mainwindow.h"

#include "core/acquisitionscheduler.h"
#include "storage/datastorage.h"
#include "utils/logger.h"

#include <QAbstractItemView>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QSplitter>
#include <QStatusBar>
#include <QTableWidget>
#include <QTime>
#include <QToolBar>

namespace {

/// 给模拟设备预置几条常见点位的告警规则
void installDefaultRules(AlarmEngine *engine, const QString &deviceId)
{
    engine->addRule(AlarmRule{deviceId, QStringLiteral("temperature"), 0.0, 60.0, true});
    engine->addRule(AlarmRule{deviceId, QStringLiteral("pressure"), 0.0, 80.0, true});
    engine->addRule(AlarmRule{deviceId, QStringLiteral("speed"), 0.0, 90.0, true});
}

} // namespace

MainWindow::MainWindow(DeviceManager *deviceManager,
                       AlarmEngine *alarmEngine,
                       DataStorage *storage,
                       QWidget *parent)
    : QMainWindow(parent)
    , m_deviceManager(deviceManager)
    , m_alarmEngine(alarmEngine)
    , m_storage(storage)
{
    m_scheduler = new AcquisitionScheduler(m_deviceManager, this);

    setupUi();
    setupConnections();

    // 预置一台模拟设备并开始采集，程序一启动就能看到数据流动
    onAddDevice();
    onStartAll();
}

QString MainWindow::keyOf(const QString &deviceId, const QString &tagId)
{
    return deviceId + QLatin1Char('/') + tagId;
}

void MainWindow::setupUi()
{
    setWindowTitle(QStringLiteral("工业设备远程监控管理平台  v%1").arg(QStringLiteral(APP_VERSION)));
    resize(1100, 700);

    QToolBar *toolBar = addToolBar(QStringLiteral("主工具栏"));
    toolBar->setMovable(false);
    toolBar->addAction(QStringLiteral("添加模拟设备"), this, &MainWindow::onAddDevice);
    toolBar->addAction(QStringLiteral("开始采集"), this, &MainWindow::onStartAll);
    toolBar->addAction(QStringLiteral("停止采集"), this, &MainWindow::onStopAll);
    toolBar->addSeparator();
    toolBar->addAction(QStringLiteral("清除告警"), this, [this]() {
        m_alarmEngine->acknowledgeAll();
        updateStatus();
    });

    // 左：设备列表
    m_deviceList = new QListWidget(this);
    m_deviceList->setMinimumWidth(220);

    // 右上：实时数据表
    m_valueTable = new QTableWidget(0, 4, this);
    m_valueTable->setHorizontalHeaderLabels({QStringLiteral("设备"),
                                             QStringLiteral("点位"),
                                             QStringLiteral("当前值"),
                                             QStringLiteral("更新时间")});
    m_valueTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_valueTable->verticalHeader()->setVisible(false);
    m_valueTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_valueTable->setSelectionBehavior(QAbstractItemView::SelectRows);

    // 右下：运行日志
    m_logView = new QPlainTextEdit(this);
    m_logView->setReadOnly(true);
    m_logView->setMaximumBlockCount(1000);
    m_logView->setPlaceholderText(QStringLiteral("运行日志…"));

    auto *rightSplitter = new QSplitter(Qt::Vertical, this);
    rightSplitter->addWidget(m_valueTable);
    rightSplitter->addWidget(m_logView);
    rightSplitter->setStretchFactor(0, 3);
    rightSplitter->setStretchFactor(1, 1);

    auto *mainSplitter = new QSplitter(Qt::Horizontal, this);
    mainSplitter->addWidget(m_deviceList);
    mainSplitter->addWidget(rightSplitter);
    mainSplitter->setStretchFactor(0, 0);
    mainSplitter->setStretchFactor(1, 1);

    setCentralWidget(mainSplitter);

    m_statusLabel = new QLabel(this);
    statusBar()->addWidget(m_statusLabel);

    updateStatus();
}

void MainWindow::setupConnections()
{
    // 采集 → 界面
    connect(m_scheduler, &AcquisitionScheduler::tagUpdated,
            this, &MainWindow::onTagUpdated);

    // 采集 → 告警引擎（数据进来就判一次阈值）
    connect(m_scheduler, &AcquisitionScheduler::tagUpdated,
            m_alarmEngine, &AlarmEngine::checkValue);

    connect(m_scheduler, &AcquisitionScheduler::errorOccurred, this,
            [this](const QString &deviceId, const QString &message) {
                Log::warn(QStringLiteral("[%1] %2").arg(deviceId, message));
            });

    connect(m_deviceManager, &DeviceManager::deviceAdded,
            this, &MainWindow::onDeviceAdded);
    connect(m_deviceManager, &DeviceManager::onlineChanged, this,
            [this](const QString &, bool) { updateStatus(); });

    connect(m_alarmEngine, &AlarmEngine::alarmRaised,
            this, &MainWindow::onAlarmRaised);
    connect(m_alarmEngine, &AlarmEngine::activeAlarmsChanged,
            this, &MainWindow::updateStatus);

    // 日志 → 界面
    connect(&Log::instance(), &Log::messageLogged, this, &MainWindow::appendLog);
}

void MainWindow::onAddDevice()
{
    const int index = m_deviceManager->devices().size() + 1;

    DeviceInfo info;
    info.name = QStringLiteral("模拟设备 %1").arg(index);
    info.host = QStringLiteral("127.0.0.1");
    info.port = static_cast<quint16>(502 + index - 1);

    m_deviceManager->addDevice(info);
}

void MainWindow::onStartAll()
{
    for (const DeviceInfo &device : m_deviceManager->devices()) {
        installDefaultRules(m_alarmEngine, device.id);
        m_scheduler->start(device.id);
    }
    updateStatus();
}

void MainWindow::onStopAll()
{
    m_scheduler->stopAll();
    updateStatus();
}

void MainWindow::onDeviceAdded(const DeviceInfo &device)
{
    auto *item = new QListWidgetItem(
        QStringLiteral("%1\n%2:%3").arg(device.name, device.host).arg(device.port), m_deviceList);
    item->setData(Qt::UserRole, device.id);
    item->setToolTip(device.id);
    updateStatus();
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
}

void MainWindow::onAlarmRaised(const AlarmRecord &record)
{
    statusBar()->showMessage(
        QStringLiteral("⚠ 告警：%1").arg(record.message), 8000);
    updateStatus();
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

    m_statusLabel->setText(QStringLiteral("设备 %1 台（在线 %2）  |  活动告警 %3 条")
                               .arg(deviceCount)
                               .arg(onlineCount)
                               .arg(m_alarmEngine->activeAlarms().size()));
}
