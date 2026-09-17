#include "ui/devicedetailpanel.h"

#include "core/acquisitionscheduler.h"
#include "core/devicemanager.h"

#include <QAbstractItemView>
#include <QFormLayout>
#include <QFont>
#include <QHeaderView>
#include <QLabel>
#include <QTableWidget>
#include <QVBoxLayout>

DeviceDetailPanel::DeviceDetailPanel(DeviceManager *manager,
                                     AcquisitionScheduler *scheduler,
                                     QWidget *parent)
    : QWidget(parent)
    , m_manager(manager)
    , m_scheduler(scheduler)
{
    setupUi();
    setDevice(QString());
}

void DeviceDetailPanel::setupUi()
{
    m_title = new QLabel(this);
    QFont titleFont = m_title->font();
    titleFont.setPointSize(titleFont.pointSize() + 3);
    titleFont.setBold(true);
    m_title->setFont(titleFont);

    m_protocol = new QLabel(this);
    m_endpoint = new QLabel(this);
    m_interval = new QLabel(this);
    m_state = new QLabel(this);

    auto *infoForm = new QFormLayout;
    infoForm->addRow(QStringLiteral("通信协议"), m_protocol);
    infoForm->addRow(QStringLiteral("地址 / 端口"), m_endpoint);
    infoForm->addRow(QStringLiteral("采集周期"), m_interval);
    infoForm->addRow(QStringLiteral("采集状态"), m_state);

    m_pointTable = new QTableWidget(0, 5, this);
    m_pointTable->setHorizontalHeaderLabels({QStringLiteral("点位"),
                                             QStringLiteral("显示名"),
                                             QStringLiteral("当前值"),
                                             QStringLiteral("单位"),
                                             QStringLiteral("寄存器地址")});
    m_pointTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_pointTable->verticalHeader()->setVisible(false);
    m_pointTable->setEditTriggers(QAbstractItemView::NoEditTriggers);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_title);
    layout->addLayout(infoForm);
    layout->addWidget(m_pointTable);
}

void DeviceDetailPanel::setDevice(const QString &deviceId)
{
    m_deviceId = deviceId;
    m_rowIndex.clear();
    m_pointTable->setRowCount(0);

    if (deviceId.isEmpty()) {
        m_title->setText(QStringLiteral("未选择设备"));
        m_protocol->setText(QStringLiteral("—"));
        m_endpoint->setText(QStringLiteral("—"));
        m_interval->setText(QStringLiteral("—"));
        m_state->setText(QStringLiteral("—"));
        return;
    }

    const DeviceInfo info = m_manager->device(deviceId);

    m_title->setText(info.name.isEmpty() ? deviceId : info.name);
    m_protocol->setText(protocolName(info.protocolId));
    m_endpoint->setText(QStringLiteral("%1:%2").arg(info.host).arg(info.port));
    m_interval->setText(QStringLiteral("%1 ms").arg(info.pollIntervalMs));

    const QList<TagPoint> points = info.points.isEmpty() ? defaultTagPoints() : info.points;
    for (const TagPoint &point : points) {
        const int row = m_pointTable->rowCount();
        m_pointTable->insertRow(row);
        m_rowIndex.insert(point.id, row);

        m_pointTable->setItem(row, 0, new QTableWidgetItem(point.id));
        m_pointTable->setItem(row, 1, new QTableWidgetItem(point.displayName()));
        m_pointTable->setItem(row, 2, new QTableWidgetItem(QStringLiteral("—")));
        m_pointTable->setItem(row, 3, new QTableWidgetItem(point.unit));
        m_pointTable->setItem(row, 4, new QTableWidgetItem(QString::number(point.address)));
    }

    refresh();
}

void DeviceDetailPanel::refresh()
{
    if (m_deviceId.isEmpty())
        return;

    const DeviceInfo info = m_manager->device(m_deviceId);

    QString stateText = QStringLiteral("已停止");
    if (m_scheduler && m_scheduler->isRunning(m_deviceId))
        stateText = info.online ? QStringLiteral("采集中（在线）") : QStringLiteral("采集中（离线）");
    m_state->setText(stateText);
}

void DeviceDetailPanel::updateValue(const QString &deviceId, const QString &tagId, const QVariant &value)
{
    if (deviceId != m_deviceId)
        return;

    const int row = m_rowIndex.value(tagId, -1);
    if (row < 0)
        return;

    if (QTableWidgetItem *item = m_pointTable->item(row, 2))
        item->setText(QString::number(value.toDouble(), 'f', 2));
}
