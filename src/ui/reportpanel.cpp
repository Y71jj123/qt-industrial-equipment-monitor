#include "ui/reportpanel.h"

#include "core/devicemanager.h"
#include "storage/datastorage.h"

#include <QAbstractItemView>
#include <QDateTimeEdit>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {

/// 把秒数格式化成 "X 时 Y 分"。
QString formatDuration(qint64 seconds)
{
    if (seconds <= 0)
        return QStringLiteral("—");
    const qint64 hours = seconds / 3600;
    const qint64 minutes = (seconds % 3600) / 60;
    if (hours > 0)
        return QStringLiteral("%1 时 %2 分").arg(hours).arg(minutes);
    return QStringLiteral("%1 分 %2 秒").arg(minutes).arg(seconds % 60);
}

} // namespace

ReportPanel::ReportPanel(DataStorage *storage, DeviceManager *manager, QWidget *parent)
    : QWidget(parent)
    , m_storage(storage)
    , m_manager(manager)
{
    setupUi();
    setRangeToday();
    refresh();
}

void ReportPanel::setupUi()
{
    m_fromEdit = new QDateTimeEdit(this);
    m_fromEdit->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    m_fromEdit->setCalendarPopup(true);

    m_toEdit = new QDateTimeEdit(this);
    m_toEdit->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    m_toEdit->setCalendarPopup(true);

    auto *todayButton = new QPushButton(QStringLiteral("今天"), this);
    todayButton->setProperty("variant", "secondary");
    auto *weekButton = new QPushButton(QStringLiteral("近 7 天"), this);
    weekButton->setProperty("variant", "secondary");
    auto *monthButton = new QPushButton(QStringLiteral("近 30 天"), this);
    monthButton->setProperty("variant", "secondary");
    auto *refreshButton = new QPushButton(QStringLiteral("统计"), this);

    auto *filterRow = new QHBoxLayout;
    filterRow->addWidget(new QLabel(QStringLiteral("从"), this));
    filterRow->addWidget(m_fromEdit);
    filterRow->addWidget(new QLabel(QStringLiteral("到"), this));
    filterRow->addWidget(m_toEdit);
    filterRow->addSpacing(8);
    filterRow->addWidget(todayButton);
    filterRow->addWidget(weekButton);
    filterRow->addWidget(monthButton);
    filterRow->addStretch();
    filterRow->addWidget(refreshButton);

    m_summary = new QLabel(this);
    m_summary->setObjectName(QStringLiteral("summaryLabel"));

    m_table = new QTableWidget(0, 6, this);
    m_table->setHorizontalHeaderLabels({QStringLiteral("设备"),
                                        QStringLiteral("采样点数"),
                                        QStringLiteral("告警次数"),
                                        QStringLiteral("首次采样"),
                                        QStringLiteral("最后采样"),
                                        QStringLiteral("运行时长")});
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_table->verticalHeader()->setVisible(false);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(filterRow);
    layout->addWidget(m_summary);
    layout->addWidget(m_table);

    connect(todayButton, &QPushButton::clicked, this, &ReportPanel::setRangeToday);
    connect(weekButton, &QPushButton::clicked, this, &ReportPanel::setRangeWeek);
    connect(monthButton, &QPushButton::clicked, this, &ReportPanel::setRangeMonth);
    connect(refreshButton, &QPushButton::clicked, this, &ReportPanel::refresh);
}

void ReportPanel::setRangeToday()
{
    const QDateTime now = QDateTime::currentDateTime();
    m_toEdit->setDateTime(now);
    m_fromEdit->setDateTime(QDateTime(now.date(), QTime(0, 0)));
}

void ReportPanel::setRangeWeek()
{
    const QDateTime now = QDateTime::currentDateTime();
    m_toEdit->setDateTime(now);
    m_fromEdit->setDateTime(now.addDays(-7));
}

void ReportPanel::setRangeMonth()
{
    const QDateTime now = QDateTime::currentDateTime();
    m_toEdit->setDateTime(now);
    m_fromEdit->setDateTime(now.addDays(-30));
}

void ReportPanel::refresh()
{
    const QDateTime from = m_fromEdit->dateTime();
    const QDateTime to = m_toEdit->dateTime();

    const QList<DataStorage::DeviceStats> stats = m_storage->queryDeviceStats(from, to);

    m_table->setRowCount(0);
    qint64 totalSamples = 0;
    int totalAlarms = 0;

    for (const DataStorage::DeviceStats &stat : stats) {
        const int row = m_table->rowCount();
        m_table->insertRow(row);

        const DeviceInfo info = m_manager->device(stat.deviceId);
        const QString deviceName = info.name.isEmpty() ? stat.deviceId : info.name;

        m_table->setItem(row, 0, new QTableWidgetItem(deviceName));
        m_table->setItem(row, 1, new QTableWidgetItem(QString::number(stat.sampleCount)));
        m_table->setItem(row, 2, new QTableWidgetItem(QString::number(stat.alarmCount)));
        m_table->setItem(row, 3, new QTableWidgetItem(
                                     stat.firstSample.isValid()
                                         ? stat.firstSample.toString(QStringLiteral("MM-dd HH:mm:ss"))
                                         : QStringLiteral("—")));
        m_table->setItem(row, 4, new QTableWidgetItem(
                                     stat.lastSample.isValid()
                                         ? stat.lastSample.toString(QStringLiteral("MM-dd HH:mm:ss"))
                                         : QStringLiteral("—")));

        const qint64 seconds = (stat.firstSample.isValid() && stat.lastSample.isValid())
                                   ? stat.firstSample.secsTo(stat.lastSample)
                                   : 0;
        m_table->setItem(row, 5, new QTableWidgetItem(formatDuration(seconds)));

        totalSamples += stat.sampleCount;
        totalAlarms += stat.alarmCount;
    }

    m_summary->setText(QStringLiteral("共 %1 台设备有数据 ｜ 采样 %2 点 ｜ 告警 %3 次 ｜ %4 ~ %5")
                           .arg(stats.size())
                           .arg(totalSamples)
                           .arg(totalAlarms)
                           .arg(from.toString(QStringLiteral("MM-dd HH:mm")))
                           .arg(to.toString(QStringLiteral("MM-dd HH:mm"))));
}
