#include "ui/historypanel.h"

#include "core/devicemanager.h"
#include "storage/datastorage.h"
#include "ui/advancedchartview.h"
#include "utils/logger.h"

#include <QComboBox>
#include <QDateTimeEdit>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QVBoxLayout>

#ifdef HAVE_QT_CHARTS
#include <QPainter>
#include <QtCharts/QChart>
#include <QtCharts/QDateTimeAxis>
#include <QtCharts/QLineSeries>
#include <QtCharts/QValueAxis>
#endif

HistoryPanel::HistoryPanel(DataStorage *storage, DeviceManager *manager, QWidget *parent)
    : QWidget(parent)
    , m_storage(storage)
    , m_manager(manager)
{
    setupUi();
    reloadDevices();
    setRangeLastHour();
    onQuery();
}

void HistoryPanel::setupUi()
{
    auto *deviceLabel = new QLabel(QStringLiteral("设备"), this);
    m_deviceBox = new QComboBox(this);
    m_deviceBox->setMinimumWidth(180);

    auto *tagLabel = new QLabel(QStringLiteral("点位"), this);
    m_tagBox = new QComboBox(this);
    m_tagBox->setMinimumWidth(140);

    auto *fromLabel = new QLabel(QStringLiteral("从"), this);
    m_fromEdit = new QDateTimeEdit(this);
    m_fromEdit->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    m_fromEdit->setCalendarPopup(true);

    auto *toLabel = new QLabel(QStringLiteral("到"), this);
    m_toEdit = new QDateTimeEdit(this);
    m_toEdit->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    m_toEdit->setCalendarPopup(true);

    auto *lastHourButton = new QPushButton(QStringLiteral("最近 1 小时"), this);
    lastHourButton->setProperty("variant", "secondary");
    auto *todayButton = new QPushButton(QStringLiteral("今天"), this);
    todayButton->setProperty("variant", "secondary");
    auto *queryButton = new QPushButton(QStringLiteral("查询"), this);
    auto *exportButton = new QPushButton(QStringLiteral("导出 CSV"), this);
    exportButton->setProperty("variant", "secondary");

    auto *filterRow = new QHBoxLayout;
    filterRow->addWidget(deviceLabel);
    filterRow->addWidget(m_deviceBox);
    filterRow->addWidget(tagLabel);
    filterRow->addWidget(m_tagBox);
    filterRow->addSpacing(8);
    filterRow->addWidget(fromLabel);
    filterRow->addWidget(m_fromEdit);
    filterRow->addWidget(toLabel);
    filterRow->addWidget(m_toEdit);
    filterRow->addSpacing(8);
    filterRow->addWidget(lastHourButton);
    filterRow->addWidget(todayButton);
    filterRow->addStretch();
    filterRow->addWidget(queryButton);
    filterRow->addWidget(exportButton);

    m_summary = new QLabel(this);
    m_summary->setObjectName(QStringLiteral("summaryLabel"));

    m_table = new QTableWidget(0, 4, this);
    m_table->setHorizontalHeaderLabels({QStringLiteral("时间"),
                                        QStringLiteral("设备"),
                                        QStringLiteral("点位"),
                                        QStringLiteral("数值")});
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_table->verticalHeader()->setVisible(false);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);

    auto *content = new QSplitter(Qt::Vertical, this);
    content->addWidget(m_table);

#ifdef HAVE_QT_CHARTS
    m_chart = new QChart();
    m_chart->setTitle(QStringLiteral("历史曲线"));
    m_chart->legend()->setVisible(false);

    m_axisX = new QDateTimeAxis();
    m_axisX->setFormat(QStringLiteral("MM-dd hh:mm"));
    m_axisX->setTitleText(QStringLiteral("时间"));
    m_chart->addAxis(m_axisX, Qt::AlignBottom);

    m_axisY = new QValueAxis();
    m_axisY->setLabelFormat(QStringLiteral("%.1f"));
    m_axisY->setTitleText(QStringLiteral("数值"));
    m_chart->addAxis(m_axisY, Qt::AlignLeft);

    m_series = new QLineSeries();
    m_series->setColor(QColor(47, 123, 214));
    m_chart->addSeries(m_series);
    m_series->attachAxis(m_axisX);
    m_series->attachAxis(m_axisY);

    // 交互式图表视图：滚轮缩放 / 拖拽平移 / 右键复位 / 悬停读数都在它里面
    m_chartView = new AdvancedChartView(m_chart, this);
    content->addWidget(m_chartView);
#endif

    content->setStretchFactor(0, 2);
    content->setStretchFactor(1, 3);

    auto *chartHint = new QLabel(
        QStringLiteral("曲线：滚轮缩放 ｜ Ctrl+滚轮缩放纵轴 ｜ 按住左键拖拽平移 ｜ 右键复位到查询范围"),
        this);
    chartHint->setObjectName(QStringLiteral("panelHint"));

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(filterRow);
    layout->addWidget(m_summary);
    layout->addWidget(content);
    layout->addWidget(chartHint);

    connect(m_deviceBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &HistoryPanel::onDeviceChanged);
    connect(queryButton, &QPushButton::clicked, this, &HistoryPanel::onQuery);
    connect(exportButton, &QPushButton::clicked, this, &HistoryPanel::onExportCsv);
    connect(lastHourButton, &QPushButton::clicked, this, &HistoryPanel::setRangeLastHour);
    connect(todayButton, &QPushButton::clicked, this, &HistoryPanel::setRangeToday);
}

void HistoryPanel::reloadDevices()
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

void HistoryPanel::populatePoints(const QString &deviceId)
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

void HistoryPanel::onDeviceChanged()
{
    populatePoints(m_deviceBox->currentData().toString());
}

void HistoryPanel::setRangeLastHour()
{
    const QDateTime now = QDateTime::currentDateTime();
    m_toEdit->setDateTime(now);
    m_fromEdit->setDateTime(now.addSecs(-3600));
}

void HistoryPanel::setRangeToday()
{
    const QDateTime now = QDateTime::currentDateTime();
    m_toEdit->setDateTime(now);
    m_fromEdit->setDateTime(QDateTime(now.date(), QTime(0, 0)));
}

void HistoryPanel::onQuery()
{
    const QString deviceId = m_deviceBox->currentData().toString();
    const QString tagId = m_tagBox->currentData().toString();
    if (deviceId.isEmpty() || tagId.isEmpty()) {
        m_summary->setText(QStringLiteral("请选择设备与点位"));
        return;
    }

    const QDateTime from = m_fromEdit->dateTime();
    const QDateTime to = m_toEdit->dateTime();

    const QList<DataStorage::Sample> samples =
        m_storage->querySamples(deviceId, tagId, from, to, 20000);

    m_table->setRowCount(0);
#ifdef HAVE_QT_CHARTS
    m_series->clear();
#endif

    double lo = 0.0;
    double hi = 0.0;
    bool first = true;

    for (const DataStorage::Sample &sample : samples) {
        const int row = m_table->rowCount();
        m_table->insertRow(row);
        m_table->setItem(row, 0, new QTableWidgetItem(
                                     sample.time.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))));
        m_table->setItem(row, 1, new QTableWidgetItem(sample.deviceId));
        m_table->setItem(row, 2, new QTableWidgetItem(sample.tagId));
        m_table->setItem(row, 3, new QTableWidgetItem(QString::number(sample.value, 'f', 3)));

        if (first) {
            lo = hi = sample.value;
            first = false;
        } else {
            lo = qMin(lo, sample.value);
            hi = qMax(hi, sample.value);
        }

#ifdef HAVE_QT_CHARTS
        m_series->append(sample.time.toMSecsSinceEpoch(), sample.value);
#endif
    }

    m_summary->setText(QStringLiteral("共 %1 条记录 ｜ 时间范围 %2 ~ %3")
                           .arg(samples.size())
                           .arg(from.toString(QStringLiteral("MM-dd HH:mm")))
                           .arg(to.toString(QStringLiteral("MM-dd HH:mm"))));

#ifdef HAVE_QT_CHARTS
    if (!samples.isEmpty()) {
        m_axisX->setRange(from, to);
        const double pad = qMax(1.0, (hi - lo) * 0.1);
        m_axisY->setRange(lo - pad, hi + pad);

        // 记下这次查询的基准范围：用户缩放 / 平移后右键复位，回到的就是它
        m_chartView->setHomeRange(from, to, lo - pad, hi + pad);
        m_chartView->setAutoFollow(true);
    }
#endif

    Log::info(QStringLiteral("历史查询 %1/%2：%3 条").arg(deviceId, tagId).arg(samples.size()));
}

void HistoryPanel::onExportCsv()
{
    const QString deviceId = m_deviceBox->currentData().toString();
    const QString tagId = m_tagBox->currentData().toString();
    if (deviceId.isEmpty() || tagId.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("导出"), QStringLiteral("请先选择设备与点位。"));
        return;
    }

    const QString suggested = QStringLiteral("%1_%2_history.csv").arg(deviceId.left(8), tagId);
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出历史数据"), suggested, QStringLiteral("CSV 文件 (*.csv)"));
    if (path.isEmpty())
        return;

    if (m_storage->exportSamplesCsv(deviceId, tagId, m_fromEdit->dateTime(), m_toEdit->dateTime(), path)) {
        QMessageBox::information(this, QStringLiteral("导出成功"),
                                 QStringLiteral("已导出到：\n%1").arg(path));
    } else {
        QMessageBox::warning(this, QStringLiteral("导出失败"), m_storage->lastError());
    }
}
