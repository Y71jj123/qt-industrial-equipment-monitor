#include "ui/reportpanel.h"

#include "core/devicemanager.h"
#include "storage/datastorage.h"
#include "utils/excelexporter.h"
#include "utils/logger.h"

#include <QAbstractItemView>
#include <QDateTimeEdit>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>

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

/// 统计表各列的下标（导出时用来标数字列，也让表格构造少几处魔法数字）。
enum StatsColumn {
    ColDevice = 0,
    ColSamples,
    ColAlarms,
    ColHandled,
    ColMttr,
    ColFirst,
    ColLast,
    ColDuration,
    StatsColumnCount
};

QString formatTime(const QDateTime &time)
{
    return time.isValid() ? time.toString(QStringLiteral("MM-dd HH:mm:ss")) : QStringLiteral("—");
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
    auto *exportButton = new QPushButton(QStringLiteral("导出 Excel"), this);
    exportButton->setProperty("variant", "secondary");

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
    filterRow->addWidget(exportButton);

    m_summary = new QLabel(this);
    m_summary->setObjectName(QStringLiteral("summaryLabel"));

    m_table = new QTableWidget(0, StatsColumnCount, this);
    m_table->setHorizontalHeaderLabels({QStringLiteral("设备"),
                                        QStringLiteral("采样点数"),
                                        QStringLiteral("告警次数"),
                                        QStringLiteral("已处理"),
                                        QStringLiteral("MTTR"),
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
    connect(exportButton, &QPushButton::clicked, this, &ReportPanel::onExportExcel);
}

QString ReportPanel::deviceName(const QString &deviceId) const
{
    const DeviceInfo info = m_manager->device(deviceId);
    return info.name.isEmpty() ? deviceId : info.name;
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
    int totalHandled = 0;
    // 整体 MTTR 用"按已处理条数加权"求，不能把各设备的平均值再平均一次 ——
    // 那样一台只处理过 1 条告警的设备会和一台处理过 500 条的设备等权。
    double weightedHandleMs = 0.0;

    for (const DataStorage::DeviceStats &stat : stats) {
        const int row = m_table->rowCount();
        m_table->insertRow(row);

        m_table->setItem(row, ColDevice, new QTableWidgetItem(deviceName(stat.deviceId)));
        m_table->setItem(row, ColSamples, new QTableWidgetItem(QString::number(stat.sampleCount)));
        m_table->setItem(row, ColAlarms, new QTableWidgetItem(QString::number(stat.alarmCount)));
        m_table->setItem(row, ColHandled, new QTableWidgetItem(QString::number(stat.handledCount)));
        m_table->setItem(row, ColMttr, new QTableWidgetItem(formatHandleDuration(stat.avgHandleMs)));
        m_table->setItem(row, ColFirst, new QTableWidgetItem(formatTime(stat.firstSample)));
        m_table->setItem(row, ColLast, new QTableWidgetItem(formatTime(stat.lastSample)));

        const qint64 seconds = (stat.firstSample.isValid() && stat.lastSample.isValid())
                                   ? stat.firstSample.secsTo(stat.lastSample)
                                   : 0;
        m_table->setItem(row, ColDuration, new QTableWidgetItem(formatDuration(seconds)));

        totalSamples += stat.sampleCount;
        totalAlarms += stat.alarmCount;
        totalHandled += stat.handledCount;
        if (stat.avgHandleMs >= 0)
            weightedHandleMs += double(stat.avgHandleMs) * stat.handledCount;
    }

    const qint64 overallMttr = totalHandled > 0
                                   ? qint64(weightedHandleMs / totalHandled)
                                   : -1;

    m_summary->setText(QStringLiteral("共 %1 台设备有数据 ｜ 采样 %2 点 ｜ 告警 %3 次 ｜ "
                                      "已处理 %4 ｜ MTTR %5 ｜ %6 ~ %7")
                           .arg(stats.size())
                           .arg(totalSamples)
                           .arg(totalAlarms)
                           .arg(totalHandled)
                           .arg(formatHandleDuration(overallMttr))
                           .arg(from.toString(QStringLiteral("MM-dd HH:mm")))
                           .arg(to.toString(QStringLiteral("MM-dd HH:mm"))));
}

void ReportPanel::onExportExcel()
{
    const QDateTime from = m_fromEdit->dateTime();
    const QDateTime to = m_toEdit->dateTime();

    // 重新取一遍而不是复用表格内容：表格里的"—"、格式化过的时长都没法还原成数字，
    // 导出要的是能求和、能排序的原始值。
    const QList<DataStorage::DeviceStats> stats = m_storage->queryDeviceStats(from, to);
    if (stats.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("导出 Excel"),
                                 QStringLiteral("当前时间范围内没有任何数据，无需导出。"));
        return;
    }

    const QString suggested =
        QStringLiteral("设备报表_%1_%2.xls")
            .arg(from.toString(QStringLiteral("yyyyMMdd")), to.toString(QStringLiteral("yyyyMMdd")));
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出报表"),
        suggested, QStringLiteral("Excel 工作簿 (*.xls)"));
    if (path.isEmpty())
        return;

    const QString rangeText = QStringLiteral("%1 ~ %2")
                                  .arg(from.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")),
                                       to.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));

    // ---- 表 1：设备统计 ----
    ExcelExport::Sheet statsSheet;
    statsSheet.name = QStringLiteral("设备统计");
    statsSheet.title = QStringLiteral("设备运行报表  %1").arg(rangeText);
    statsSheet.headers = {QStringLiteral("设备"),
                          QStringLiteral("采样点数"),
                          QStringLiteral("告警次数"),
                          QStringLiteral("已处理"),
                          QStringLiteral("MTTR"),
                          QStringLiteral("首次采样"),
                          QStringLiteral("最后采样"),
                          QStringLiteral("运行时长")};
    statsSheet.numericColumns = {ColSamples, ColAlarms, ColHandled};

    qint64 totalSamples = 0;
    int totalAlarms = 0;
    int totalHandled = 0;
    double weightedHandleMs = 0.0;
    for (const DataStorage::DeviceStats &stat : stats) {
        const qint64 seconds = (stat.firstSample.isValid() && stat.lastSample.isValid())
                                   ? stat.firstSample.secsTo(stat.lastSample)
                                   : 0;

        statsSheet.rows.append({deviceName(stat.deviceId),
                                QString::number(stat.sampleCount),
                                QString::number(stat.alarmCount),
                                QString::number(stat.handledCount),
                                formatHandleDuration(stat.avgHandleMs),
                                formatTime(stat.firstSample),
                                formatTime(stat.lastSample),
                                formatDuration(seconds)});

        totalSamples += stat.sampleCount;
        totalAlarms += stat.alarmCount;
        totalHandled += stat.handledCount;
        if (stat.avgHandleMs >= 0)
            weightedHandleMs += double(stat.avgHandleMs) * stat.handledCount;
    }

    const qint64 overallMttr = totalHandled > 0 ? qint64(weightedHandleMs / totalHandled) : -1;

    statsSheet.rows.append({QStringLiteral("合计（%1 台）").arg(stats.size()),
                            QString::number(totalSamples),
                            QString::number(totalAlarms),
                            QString::number(totalHandled),
                            formatHandleDuration(overallMttr),
                            QString(),
                            QString(),
                            QString()});

    // ---- 表 2：告警明细 ----
    ExcelExport::Sheet alarmSheet;
    alarmSheet.name = QStringLiteral("告警明细");
    alarmSheet.title = QStringLiteral("告警明细  %1").arg(rangeText);
    alarmSheet.headers = {QStringLiteral("时间"),
                          QStringLiteral("设备"),
                          QStringLiteral("点位"),
                          QStringLiteral("类型"),
                          QStringLiteral("级别"),
                          QStringLiteral("触发值"),
                          QStringLiteral("下限"),
                          QStringLiteral("上限"),
                          QStringLiteral("状态"),
                          QStringLiteral("处理结论"),
                          QStringLiteral("处理人"),
                          QStringLiteral("处理时间"),
                          QStringLiteral("处理说明"),
                          QStringLiteral("告警说明")};
    alarmSheet.numericColumns = {5, 6, 7};

    QList<AlarmRecord> alarms = m_storage->queryAlarms(from, to, 100000);
    // queryAlarms 是"最新在前"，报表按时间正序读起来才顺
    std::reverse(alarms.begin(), alarms.end());
    for (const AlarmRecord &record : alarms) {
        alarmSheet.rows.append(
            {record.time.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")),
             deviceName(record.deviceId),
             record.tagId,
             alarmKindName(record.kind),
             alarmLevelName(record.level),
             QString::number(record.value, 'f', 3),
             QString::number(record.lowLimit, 'f', 3),
             QString::number(record.highLimit, 'f', 3),
             record.active ? QStringLiteral("活动") : QStringLiteral("已恢复"),
             record.handled() ? alarmDispositionName(record.disposition) : QStringLiteral("—"),
             record.handledBy,
             record.handled() ? record.handledAt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
                              : QString(),
             record.handlingNote,
             record.message});
    }

    QString error;
    if (!ExcelExport::writeWorkbook(path, {statsSheet, alarmSheet}, &error)) {
        QMessageBox::warning(this, QStringLiteral("导出失败"), error);
        Log::error(QStringLiteral("报表导出失败: %1").arg(error));
        return;
    }

    Log::info(QStringLiteral("报表已导出到 %1（%2 台设备 / %3 条告警）")
                  .arg(path)
                  .arg(stats.size())
                  .arg(alarms.size()));
    QMessageBox::information(this,
                             QStringLiteral("导出成功"),
                             QStringLiteral("已导出到：\n%1\n\n含「设备统计」与「告警明细」两张工作表，"
                                            "共 %2 台设备、%3 条告警记录。")
                                 .arg(path)
                                 .arg(stats.size())
                                 .arg(alarms.size()));
}
