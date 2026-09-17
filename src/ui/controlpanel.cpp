#include "ui/controlpanel.h"

#include "core/acquisitionscheduler.h"
#include "core/devicemanager.h"
#include "storage/datastorage.h"
#include "utils/logger.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {
QString keyOf(const QString &deviceId, const QString &tagId)
{
    return deviceId + QLatin1Char('/') + tagId;
}
} // namespace

ControlPanel::ControlPanel(AcquisitionScheduler *scheduler,
                           DeviceManager *manager,
                           DataStorage *storage,
                           QWidget *parent)
    : QWidget(parent)
    , m_scheduler(scheduler)
    , m_manager(manager)
    , m_storage(storage)
{
    setupUi();
    reload();
    refreshLog();
}

void ControlPanel::setUser(const QString &user)
{
    m_user = user;
}

void ControlPanel::setupUi()
{
    auto *control = new QGroupBox(QStringLiteral("指令下发"), this);

    m_deviceBox = new QComboBox(control);
    m_deviceBox->setMinimumWidth(170);

    m_tagBox = new QComboBox(control);
    m_tagBox->setMinimumWidth(140);

    m_valueSpin = new QDoubleSpinBox(control);
    m_valueSpin->setRange(-1000000.0, 1000000.0);
    m_valueSpin->setDecimals(2);
    m_valueSpin->setMinimumWidth(140);

    m_currentLabel = new QLabel(QStringLiteral("—"), control);
    m_currentLabel->setObjectName(QStringLiteral("summaryLabel"));

    auto *sendButton = new QPushButton(QStringLiteral("下发指令"), control);

    auto *grid = new QGridLayout(control);
    grid->addWidget(new QLabel(QStringLiteral("设备"), control), 0, 0);
    grid->addWidget(m_deviceBox, 0, 1);
    grid->addWidget(new QLabel(QStringLiteral("点位"), control), 0, 2);
    grid->addWidget(m_tagBox, 0, 3);
    grid->addWidget(new QLabel(QStringLiteral("当前值"), control), 1, 0);
    grid->addWidget(m_currentLabel, 1, 1);
    grid->addWidget(new QLabel(QStringLiteral("目标值"), control), 1, 2);
    grid->addWidget(m_valueSpin, 1, 3);
    grid->addWidget(sendButton, 1, 4);

    auto *logTitle = new QLabel(QStringLiteral("操作留痕"), this);
    logTitle->setObjectName(QStringLiteral("panelTitle"));

    auto *refreshButton = new QPushButton(QStringLiteral("刷新"), this);
    refreshButton->setProperty("variant", "secondary");

    auto *titleRow = new QHBoxLayout;
    titleRow->addWidget(logTitle);
    titleRow->addStretch();
    titleRow->addWidget(refreshButton);

    m_logTable = new QTableWidget(0, 7, this);
    m_logTable->setHorizontalHeaderLabels({QStringLiteral("时间"),
                                           QStringLiteral("操作员"),
                                           QStringLiteral("设备"),
                                           QStringLiteral("点位"),
                                           QStringLiteral("动作"),
                                           QStringLiteral("详情"),
                                           QStringLiteral("结果")});
    m_logTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_logTable->verticalHeader()->setVisible(false);
    m_logTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_logTable->setSelectionBehavior(QAbstractItemView::SelectRows);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(control);
    layout->addLayout(titleRow);
    layout->addWidget(m_logTable);

    connect(m_deviceBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ControlPanel::onDeviceChanged);
    connect(sendButton, &QPushButton::clicked, this, &ControlPanel::onSend);
    connect(refreshButton, &QPushButton::clicked, this, &ControlPanel::refreshLog);
}

void ControlPanel::reload()
{
    const QString current = m_deviceBox->currentData().toString();

    m_deviceBox->blockSignals(true);
    m_deviceBox->clear();
    for (const DeviceInfo &info : m_manager->devices())
        m_deviceBox->addItem(info.name.isEmpty() ? info.id : info.name, info.id);
    m_deviceBox->blockSignals(false);

    const int index = m_deviceBox->findData(current);
    if (index >= 0)
        m_deviceBox->setCurrentIndex(index);

    populatePoints(m_deviceBox->currentData().toString());
}

void ControlPanel::populatePoints(const QString &deviceId)
{
    const QString current = m_tagBox->currentData().toString();

    m_tagBox->clear();
    if (deviceId.isEmpty())
        return;

    const DeviceInfo info = m_manager->device(deviceId);
    const QList<TagPoint> points = info.points.isEmpty() ? defaultTagPoints() : info.points;
    for (const TagPoint &point : points)
        m_tagBox->addItem(point.displayName(), point.id);

    const int index = m_tagBox->findData(current);
    if (index >= 0)
        m_tagBox->setCurrentIndex(index);
}

void ControlPanel::onDeviceChanged()
{
    populatePoints(m_deviceBox->currentData().toString());
    m_currentLabel->setText(QStringLiteral("—"));
}

void ControlPanel::updateCurrentValue(const QString &deviceId, const QString &tagId, const QVariant &value)
{
    m_currentValues.insert(keyOf(deviceId, tagId), value.toDouble());

    if (deviceId == m_deviceBox->currentData().toString()
        && tagId == m_tagBox->currentData().toString()) {
        m_currentLabel->setText(QString::number(value.toDouble(), 'f', 2));
    }
}

void ControlPanel::onSend()
{
    const QString deviceId = m_deviceBox->currentData().toString();
    const QString tagId = m_tagBox->currentData().toString();
    if (deviceId.isEmpty() || tagId.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("下发"), QStringLiteral("请先选择设备与点位。"));
        return;
    }

    const DeviceInfo info = m_manager->device(deviceId);
    const double target = m_valueSpin->value();
    const double current = m_currentValues.value(keyOf(deviceId, tagId), 0.0);

    // ---- 二次确认 ----
    const auto answer = QMessageBox::question(
        this, QStringLiteral("确认下发指令"),
        QStringLiteral("即将向设备「%1」的点位「%2」下发指令：\n\n"
                       "    目标值：%3\n"
                       "    当前值：%4\n\n"
                       "远程控制可能改变现场设备运行状态，请确认无误后执行。")
            .arg(info.name.isEmpty() ? deviceId : info.name, tagId)
            .arg(target, 0, 'f', 2)
            .arg(current, 0, 'f', 2),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

    if (answer != QMessageBox::Yes)
        return;

    const bool ok = m_scheduler->writeTag(deviceId, tagId, target);

    // ---- 操作留痕 ----
    if (m_storage) {
        DataStorage::OperationEntry entry;
        entry.time = QDateTime::currentDateTime();
        entry.user = m_user.isEmpty() ? QStringLiteral("未知") : m_user;
        entry.deviceId = deviceId;
        entry.tagId = tagId;
        entry.action = QStringLiteral("写入点位");
        entry.detail = QStringLiteral("%1 → %2")
                           .arg(current, 0, 'f', 2)
                           .arg(target, 0, 'f', 2);
        entry.success = ok;
        m_storage->insertOperation(entry);
    }

    if (ok) {
        Log::info(QStringLiteral("下发成功 %1/%2 = %3").arg(deviceId, tagId).arg(target));
        QMessageBox::information(this, QStringLiteral("下发成功"),
                                 QStringLiteral("%1 = %2 已下发。").arg(tagId).arg(target, 0, 'f', 2));
    } else {
        QMessageBox::warning(this, QStringLiteral("下发失败"),
                             QStringLiteral("指令未能送达设备，请检查连接状态。"));
    }

    refreshLog();
}

void ControlPanel::refreshLog()
{
    m_logTable->setRowCount(0);
    if (!m_storage)
        return;

    const QList<DataStorage::OperationEntry> entries = m_storage->queryOperations(500);
    for (const DataStorage::OperationEntry &entry : entries) {
        const int row = m_logTable->rowCount();
        m_logTable->insertRow(row);

        const DeviceInfo info = m_manager->device(entry.deviceId);
        const QString deviceName = info.name.isEmpty() ? entry.deviceId : info.name;

        m_logTable->setItem(row, 0, new QTableWidgetItem(
                                       entry.time.toString(QStringLiteral("MM-dd HH:mm:ss"))));
        m_logTable->setItem(row, 1, new QTableWidgetItem(entry.user));
        m_logTable->setItem(row, 2, new QTableWidgetItem(deviceName));
        m_logTable->setItem(row, 3, new QTableWidgetItem(entry.tagId));
        m_logTable->setItem(row, 4, new QTableWidgetItem(entry.action));
        m_logTable->setItem(row, 5, new QTableWidgetItem(entry.detail));

        auto *resultItem = new QTableWidgetItem(entry.success ? QStringLiteral("成功")
                                                              : QStringLiteral("失败"));
        resultItem->setForeground(entry.success ? QColor(47, 150, 90) : QColor(200, 60, 60));
        m_logTable->setItem(row, 6, resultItem);
    }
}
