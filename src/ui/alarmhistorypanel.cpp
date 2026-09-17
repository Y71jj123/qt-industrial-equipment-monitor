#include "ui/alarmhistorypanel.h"

#include "core/devicemanager.h"
#include "storage/datastorage.h"
#include "utils/logger.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QDateTime>
#include <QDateTimeEdit>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QStringConverter>
#include <QTableWidget>
#include <QTextStream>
#include <QVBoxLayout>

namespace {

/// 单次查询条数上限 —— 再大就会把界面卡住了，超了要在界面上说清楚。
constexpr int kQueryLimit = 5000;

/// 表格列号，避免到处写魔数。
enum Column {
    ColumnTime = 0,
    ColumnLevel,
    ColumnDevice,
    ColumnTag,
    ColumnValue,
    ColumnState,
    ColumnMessage,
    ColumnCount
};

/// 级别文案对应的底色（未恢复的活动告警才上色）。
QColor levelBackground(AlarmLevel level)
{
    switch (level) {
    case AlarmLevel::Critical:
        return QColor(255, 224, 224);
    case AlarmLevel::Warning:
        return QColor(255, 240, 214);
    case AlarmLevel::Info:
        break;
    }
    return QColor(233, 244, 255);
}

QColor levelForeground(AlarmLevel level)
{
    switch (level) {
    case AlarmLevel::Critical:
        return QColor(170, 30, 30);
    case AlarmLevel::Warning:
        return QColor(158, 96, 0);
    case AlarmLevel::Info:
        break;
    }
    return QColor(28, 78, 150);
}

/// 数值单元格：显示成文字，但排序按真正的数字比。
///
/// 直接用字符串排的话 "9" 会排在 "100" 后面 —— 查历史时按数值找异常点就没法用了。
class NumericItem : public QTableWidgetItem
{
public:
    NumericItem(const QString &text, double sortKey)
        : QTableWidgetItem(text)
        , m_sortKey(sortKey)
    {
    }

    bool operator<(const QTableWidgetItem &other) const override
    {
        const auto *peer = dynamic_cast<const NumericItem *>(&other);
        return peer ? m_sortKey < peer->m_sortKey : QTableWidgetItem::operator<(other);
    }

private:
    double m_sortKey = 0.0;
};

/// CSV 字段转义：描述是自由文本，可能带逗号或引号，按 RFC 4180 用双引号包起来。
/// 注意只处理半角逗号 —— 全角"，"不是分隔符，不需要动。
QString csvField(const QString &text)
{
    if (!text.contains(QLatin1Char(',')) && !text.contains(QLatin1Char('"'))
        && !text.contains(QLatin1Char('\n'))) {
        return text;
    }

    QString escaped = text;
    escaped.replace(QLatin1Char('"'), QLatin1String("\"\""));
    return QLatin1Char('"') + escaped + QLatin1Char('"');
}

QString stateText(const AlarmRecord &record)
{
    if (!record.active)
        return QStringLiteral("已恢复");
    return record.acknowledged ? QStringLiteral("已确认") : QStringLiteral("活动");
}

} // namespace

AlarmHistoryPanel::AlarmHistoryPanel(DataStorage *storage, DeviceManager *manager, QWidget *parent)
    : QWidget(parent)
    , m_storage(storage)
    , m_manager(manager)
{
    setupUi();
    reloadDevices();
    setRangeLastWeek();
    onQuery();
}

void AlarmHistoryPanel::setupUi()
{
    auto *deviceLabel = new QLabel(QStringLiteral("设备"), this);
    m_deviceBox = new QComboBox(this);
    m_deviceBox->setMinimumWidth(170);

    auto *levelLabel = new QLabel(QStringLiteral("级别"), this);
    m_levelBox = new QComboBox(this);
    m_levelBox->setMinimumWidth(100);
    m_levelBox->addItem(QStringLiteral("全部"), -1);
    m_levelBox->addItem(alarmLevelName(AlarmLevel::Info), static_cast<int>(AlarmLevel::Info));
    m_levelBox->addItem(alarmLevelName(AlarmLevel::Warning), static_cast<int>(AlarmLevel::Warning));
    m_levelBox->addItem(alarmLevelName(AlarmLevel::Critical), static_cast<int>(AlarmLevel::Critical));

    auto *fromLabel = new QLabel(QStringLiteral("从"), this);
    m_fromEdit = new QDateTimeEdit(this);
    m_fromEdit->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    m_fromEdit->setCalendarPopup(true);

    auto *toLabel = new QLabel(QStringLiteral("到"), this);
    m_toEdit = new QDateTimeEdit(this);
    m_toEdit->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    m_toEdit->setCalendarPopup(true);

    auto *todayButton = new QPushButton(QStringLiteral("今天"), this);
    todayButton->setProperty("variant", "secondary");
    auto *weekButton = new QPushButton(QStringLiteral("近 7 天"), this);
    weekButton->setProperty("variant", "secondary");

    auto *queryButton = new QPushButton(QStringLiteral("查询"), this);
    auto *exportButton = new QPushButton(QStringLiteral("导出 CSV"), this);
    exportButton->setProperty("variant", "secondary");

    auto *filterRow = new QHBoxLayout;
    filterRow->addWidget(deviceLabel);
    filterRow->addWidget(m_deviceBox);
    filterRow->addWidget(levelLabel);
    filterRow->addWidget(m_levelBox);
    filterRow->addSpacing(8);
    filterRow->addWidget(fromLabel);
    filterRow->addWidget(m_fromEdit);
    filterRow->addWidget(toLabel);
    filterRow->addWidget(m_toEdit);
    filterRow->addSpacing(8);
    filterRow->addWidget(todayButton);
    filterRow->addWidget(weekButton);
    filterRow->addStretch();
    filterRow->addWidget(queryButton);
    filterRow->addWidget(exportButton);

    m_summary = new QLabel(this);
    m_summary->setObjectName(QStringLiteral("summaryLabel"));

    m_table = new QTableWidget(0, ColumnCount, this);
    m_table->setHorizontalHeaderLabels({QStringLiteral("时间"),
                                        QStringLiteral("级别"),
                                        QStringLiteral("设备"),
                                        QStringLiteral("点位"),
                                        QStringLiteral("数值"),
                                        QStringLiteral("状态"),
                                        QStringLiteral("描述")});
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->verticalHeader()->setVisible(false);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);

    m_table->setSortingEnabled(true);
    m_table->sortByColumn(ColumnTime, Qt::DescendingOrder); // 查历史先看最近发生的

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(filterRow);
    layout->addWidget(m_summary);
    layout->addWidget(m_table);

    connect(queryButton, &QPushButton::clicked, this, &AlarmHistoryPanel::onQuery);
    connect(exportButton, &QPushButton::clicked, this, &AlarmHistoryPanel::onExportCsv);
    connect(todayButton, &QPushButton::clicked, this, &AlarmHistoryPanel::setRangeToday);
    connect(weekButton, &QPushButton::clicked, this, &AlarmHistoryPanel::setRangeLastWeek);

    // 筛选条件一改就重筛，不必再点一次"查询"（重筛只走内存，不碰数据库）
    connect(m_deviceBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &AlarmHistoryPanel::applyFilter);
    connect(m_levelBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &AlarmHistoryPanel::applyFilter);
}

void AlarmHistoryPanel::reloadDevices()
{
    const QString current = m_deviceBox->currentData().toString();

    m_deviceBox->blockSignals(true);
    m_deviceBox->clear();
    m_deviceBox->addItem(QStringLiteral("全部设备"), QString());
    for (const DeviceInfo &info : m_manager->devices())
        m_deviceBox->addItem(info.name.isEmpty() ? info.id : info.name, info.id);
    m_deviceBox->blockSignals(false);

    const int index = m_deviceBox->findData(current);
    m_deviceBox->setCurrentIndex(index >= 0 ? index : 0);
}

void AlarmHistoryPanel::setRangeToday()
{
    const QDateTime now = QDateTime::currentDateTime();
    m_toEdit->setDateTime(now);
    m_fromEdit->setDateTime(QDateTime(now.date(), QTime(0, 0)));
}

void AlarmHistoryPanel::setRangeLastWeek()
{
    const QDateTime now = QDateTime::currentDateTime();
    m_toEdit->setDateTime(now);
    m_fromEdit->setDateTime(now.addDays(-7));
}

void AlarmHistoryPanel::onQuery()
{
    if (!m_storage || !m_storage->isOpen()) {
        m_records.clear();
        m_visible.clear();
        m_table->setRowCount(0);
        m_summary->setText(QStringLiteral("本地数据库不可用，无法查询告警历史"));
        return;
    }

    const QDateTime from = m_fromEdit->dateTime();
    const QDateTime to = m_toEdit->dateTime();
    if (from > to) {
        m_summary->setText(QStringLiteral("起始时间晚于结束时间，请重新选择"));
        return;
    }

    m_records = m_storage->queryAlarms(from, to, kQueryLimit);
    appendMissingDevices(m_records);
    applyFilter();

    QString text = QStringLiteral("命中 %1 条 ｜ 当前筛选 %2 条 ｜ %3 ~ %4")
                       .arg(m_records.size())
                       .arg(m_visible.size())
                       .arg(from.toString(QStringLiteral("yyyy-MM-dd HH:mm")),
                            to.toString(QStringLiteral("yyyy-MM-dd HH:mm")));

    // 被上限截断时必须说出来，否则看起来就像"这段时间只有这么多"
    if (m_records.size() >= kQueryLimit)
        text += QStringLiteral(" ｜ 已达 %1 条上限，请缩小时间范围").arg(kQueryLimit);

    m_summary->setText(text);

    Log::info(QStringLiteral("告警历史查询：命中 %1 条").arg(m_records.size()));
}

void AlarmHistoryPanel::applyFilter()
{
    const int level = m_levelBox->currentData().toInt();
    const QString deviceId = m_deviceBox->currentData().toString();

    m_visible.clear();
    for (const AlarmRecord &record : m_records) {
        if (level >= 0 && static_cast<int>(record.level) != level)
            continue;
        if (!deviceId.isEmpty() && record.deviceId != deviceId)
            continue;
        m_visible.append(record);
    }

    // 排序开着的时候插行会被反复重排，先关掉，填完再打开
    m_table->setSortingEnabled(false);
    m_table->setRowCount(0);

    for (const AlarmRecord &record : m_visible) {
        const int row = m_table->rowCount();
        m_table->insertRow(row);

        auto *timeItem = new QTableWidgetItem(
            record.time.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));

        auto *levelItem = new NumericItem(alarmLevelName(record.level),
                                          static_cast<int>(record.level));
        levelItem->setForeground(levelForeground(record.level));

        auto *deviceItem = new QTableWidgetItem(deviceName(record.deviceId));
        auto *tagItem = new QTableWidgetItem(record.tagId);

        auto *valueItem = new NumericItem(QString::number(record.value, 'f', 2), record.value);

        auto *stateItem = new QTableWidgetItem(stateText(record));
        auto *messageItem = new QTableWidgetItem(record.message);

        const QList<QTableWidgetItem *> items = {timeItem, levelItem, deviceItem, tagItem,
                                                 valueItem, stateItem, messageItem};

        // 还在活动且没确认的，整行上色，跟告警页保持一致的视觉语言
        if (record.active && !record.acknowledged) {
            const QColor background = levelBackground(record.level);
            const QColor foreground = levelForeground(record.level);
            for (QTableWidgetItem *item : items) {
                item->setBackground(background);
                item->setForeground(foreground);
            }
        } else if (!record.active) {
            timeItem->setForeground(QColor(150, 150, 150)); // 已恢复的淡显，别和活动告警抢视线
        }

        for (int column = 0; column < items.size(); ++column)
            m_table->setItem(row, column, items.at(column));
    }

    // 重新打开排序：会按表头当前的排序指示器自动重排，用户选过的列不会被重置
    m_table->setSortingEnabled(true);
}

void AlarmHistoryPanel::appendMissingDevices(const QList<AlarmRecord> &records)
{
    for (const AlarmRecord &record : records) {
        if (m_deviceBox->findData(record.deviceId) < 0) {
            m_deviceBox->addItem(QStringLiteral("%1（已移除）").arg(record.deviceId),
                                 record.deviceId);
        }
    }
}

QString AlarmHistoryPanel::deviceName(const QString &deviceId) const
{
    if (m_manager) {
        const DeviceInfo info = m_manager->device(deviceId);
        if (!info.name.isEmpty())
            return info.name;
    }
    return deviceId;
}

void AlarmHistoryPanel::onExportCsv()
{
    if (m_visible.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("导出"),
                                 QStringLiteral("当前筛选条件下没有记录可导出。"));
        return;
    }

    const QString suggested =
        QStringLiteral("alarm_history_%1.csv")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));

    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("导出告警历史"),
                                                      suggested, QStringLiteral("CSV 文件 (*.csv)"));
    if (path.isEmpty())
        return;

    if (exportCsv(path)) {
        QMessageBox::information(this, QStringLiteral("导出成功"),
                                 QStringLiteral("已导出 %1 条记录到：\n%2")
                                     .arg(m_visible.size())
                                     .arg(path));
    } else {
        QMessageBox::warning(this, QStringLiteral("导出失败"), m_exportError);
    }
}

bool AlarmHistoryPanel::exportCsv(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        m_exportError = file.errorString();
        return false;
    }

    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);
    out << QChar(0xFEFF); // BOM：让 Excel 正确识别 UTF-8 中文

    out << QStringLiteral("时间,级别,设备,点位,数值,状态,描述\n");
    for (const AlarmRecord &record : m_visible) {
        out << record.time.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) << ','
            << alarmLevelName(record.level) << ','
            << csvField(deviceName(record.deviceId)) << ','
            << csvField(record.tagId) << ','
            << QString::number(record.value, 'f', 2) << ','
            << stateText(record) << ','
            << csvField(record.message) << '\n';
    }

    file.close();
    Log::info(QStringLiteral("已导出 %1 条告警历史到 %2").arg(m_visible.size()).arg(path));
    return true;
}
