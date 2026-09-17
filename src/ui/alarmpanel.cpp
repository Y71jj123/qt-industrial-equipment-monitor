#include "ui/alarmpanel.h"

#include "core/devicemanager.h"
#include "storage/datastorage.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {

/// 未确认告警的行背景色（浅色，保证深色文字可读）。
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

/// 表格里「状态」列的文案。顺序即优先级：处理过 > 已确认 > 活动 > 已恢复。
QString stateText(const AlarmRecord &record)
{
    if (record.handled())
        return QStringLiteral("已处理");
    if (record.active)
        return record.acknowledged ? QStringLiteral("已确认") : QStringLiteral("活动");
    return QStringLiteral("已恢复");
}

/// 表格里「处理结论」列的文案。
QString handlingText(const AlarmRecord &record)
{
    if (!record.handled())
        return QStringLiteral("—");

    QString text = alarmDispositionName(record.disposition);
    if (!record.handledBy.isEmpty())
        text += QStringLiteral(" · %1").arg(record.handledBy);
    return text;
}

} // namespace

AlarmPanel::AlarmPanel(AlarmEngine *engine,
                       DeviceManager *manager,
                       DataStorage *storage,
                       const QString &currentUser,
                       QWidget *parent)
    : QWidget(parent)
    , m_engine(engine)
    , m_manager(manager)
    , m_storage(storage)
    , m_currentUser(currentUser)
{
    setupUi();
    refresh();
}

void AlarmPanel::setupUi()
{
    m_summary = new QLabel(this);

    m_activeOnly = new QCheckBox(QStringLiteral("仅显示活动告警"), this);
    connect(m_activeOnly, &QCheckBox::toggled, this, &AlarmPanel::refresh);

    auto *handleButton = new QPushButton(QStringLiteral("处理选中…"), this);
    handleButton->setToolTip(QStringLiteral("录入处理结论（处理人 / 结论 / 说明），并计入 MTTR 统计"));
    connect(handleButton, &QPushButton::clicked, this, &AlarmPanel::handleCurrent);

    auto *ackButton = new QPushButton(QStringLiteral("确认选中"), this);
    connect(ackButton, &QPushButton::clicked, this, &AlarmPanel::acknowledgeCurrent);

    auto *ackAllButton = new QPushButton(QStringLiteral("确认全部"), this);
    connect(ackAllButton, &QPushButton::clicked, m_engine, &AlarmEngine::acknowledgeAll);

    auto *clearButton = new QPushButton(QStringLiteral("清空记录"), this);
    connect(clearButton, &QPushButton::clicked, m_engine, &AlarmEngine::clearHistory);

    auto *toolbar = new QHBoxLayout;
    toolbar->addWidget(m_summary);
    toolbar->addStretch();
    toolbar->addWidget(m_activeOnly);
    toolbar->addWidget(handleButton);
    toolbar->addWidget(ackButton);
    toolbar->addWidget(ackAllButton);
    toolbar->addWidget(clearButton);

    // 「处理结论」单列一列而不是塞进描述里：现场扫表时看的是这一列，
    // 塞进描述会让每行长出一大截，反而不便于横向比较。
    m_table = new QTableWidget(0, 8, this);
    m_table->setHorizontalHeaderLabels({QStringLiteral("时间"),
                                        QStringLiteral("级别"),
                                        QStringLiteral("设备"),
                                        QStringLiteral("点位"),
                                        QStringLiteral("数值"),
                                        QStringLiteral("状态"),
                                        QStringLiteral("处理结论"),
                                        QStringLiteral("描述")});
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->verticalHeader()->setVisible(false);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(toolbar);
    layout->addWidget(m_table);
}

void AlarmPanel::refresh()
{
    const bool activeOnly = m_activeOnly->isChecked();
    const QList<AlarmRecord> records = m_engine->history();

    m_table->setRowCount(0);
    m_rowKeys.clear();

    int activeCount = 0;
    int unacknowledged = 0;
    int handled = 0;

    for (const AlarmRecord &record : records) {
        if (record.active) {
            ++activeCount;
            if (!record.acknowledged)
                ++unacknowledged;
        }
        if (record.handled())
            ++handled;

        if (activeOnly && !record.active)
            continue;

        const int row = m_table->rowCount();
        m_table->insertRow(row);

        QString deviceName = record.deviceId;
        if (m_manager) {
            const DeviceInfo info = m_manager->device(record.deviceId);
            if (!info.name.isEmpty())
                deviceName = info.name;
        }

        const QStringList texts = {
            record.time.toString(QStringLiteral("MM-dd HH:mm:ss")),
            alarmLevelName(record.level),
            deviceName,
            record.tagId,
            QString::number(record.value, 'f', 2),
            stateText(record),
            handlingText(record),
            record.message,
        };

        for (int column = 0; column < texts.size(); ++column) {
            auto *item = new QTableWidgetItem(texts.at(column));

            if (record.active && !record.acknowledged) {
                item->setBackground(levelBackground(record.level));
                item->setForeground(levelForeground(record.level));
            } else if (!record.active) {
                // 已恢复的记录淡显，避免和活动告警抢注意力
                item->setForeground(QColor(150, 150, 150));
            }

            m_table->setItem(row, column, item);
        }

        // 处理说明用 tooltip 挂在「处理结论」列上：不占列宽，悬停又能看全
        if (record.handled() && !record.handlingNote.isEmpty()) {
            if (QTableWidgetItem *item = m_table->item(row, 6))
                item->setToolTip(QStringLiteral("%1\n处理时间：%2")
                                     .arg(record.handlingNote,
                                          record.handledAt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))));
        }

        m_rowKeys.append(qMakePair(record.deviceId, record.tagId));
    }

    m_summary->setText(QStringLiteral("活动告警 %1 条（未确认 %2） ｜ 已处理 %3 ｜ MTTR %4 ｜ 历史共 %5 条")
                           .arg(activeCount)
                           .arg(unacknowledged)
                           .arg(handled)
                           .arg(formatHandleDuration(m_engine->averageHandleDurationMs()))
                           .arg(records.size()));
}

void AlarmPanel::acknowledgeCurrent()
{
    const int row = m_table->currentRow();
    if (row < 0 || row >= m_rowKeys.size())
        return;

    const QPair<QString, QString> key = m_rowKeys.at(row);
    m_engine->acknowledge(key.first, key.second);
}

void AlarmPanel::handleCurrent()
{
    const int row = m_table->currentRow();
    if (row < 0 || row >= m_rowKeys.size()) {
        QMessageBox::information(this, QStringLiteral("还没有选中"),
                                 QStringLiteral("请先在列表里选一条告警。"));
        return;
    }

    const QPair<QString, QString> key = m_rowKeys.at(row);

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("录入处理结论"));
    dialog.resize(460, 300);

    auto *dispositionBox = new QComboBox(&dialog);
    // data 里存枚举值：这样"界面顺序"和"枚举定义顺序"互不依赖，
    // 以后往枚举里插新项也不会把选项串位。
    const QList<AlarmDisposition> dispositions = {AlarmDisposition::Resolved,
                                                  AlarmDisposition::Maintained,
                                                  AlarmDisposition::FalseAlarm,
                                                  AlarmDisposition::Ignored};
    for (const AlarmDisposition disposition : dispositions)
        dispositionBox->addItem(alarmDispositionName(disposition), int(disposition));

    auto *handlerEdit = new QLineEdit(m_currentUser, &dialog);
    handlerEdit->setPlaceholderText(QStringLiteral("默认取当前登录用户"));

    auto *noteEdit = new QPlainTextEdit(&dialog);
    noteEdit->setPlaceholderText(
        QStringLiteral("例如：更换 3 号轴承后温度回落至 42℃，观察 30 分钟无复现"));

    auto *form = new QFormLayout;
    form->addRow(QStringLiteral("处理结论"), dispositionBox);
    form->addRow(QStringLiteral("处理人"), handlerEdit);
    form->addRow(QStringLiteral("处理说明"), noteEdit);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    auto *layout = new QVBoxLayout(&dialog);
    layout->addLayout(form);
    layout->addWidget(buttons);

    // 必须填处理说明：只有结论没有说明，"闭环"就退化成了一次点击，
    // 三个月后没人知道当时到底做了什么。
    while (true) {
        if (dialog.exec() != QDialog::Accepted)
            return;
        if (!noteEdit->toPlainText().trimmed().isEmpty())
            break;

        QMessageBox::warning(this, QStringLiteral("还差一步"),
                             QStringLiteral("请填写「处理说明」—— 没有说明的处理记录，事后等于没有。"));
    }

    const auto disposition = static_cast<AlarmDisposition>(dispositionBox->currentData().toInt());
    const QString handledBy = handlerEdit->text().trimmed();
    const QString note = noteEdit->toPlainText().trimmed();

    if (!m_engine->handleAlarm(key.first, key.second, disposition, handledBy, note)) {
        QMessageBox::information(this, QStringLiteral("无法处理"),
                                 QStringLiteral("该告警已经恢复，无需再录入处理结论。\n"
                                                "（已恢复的记录属于事后补录，不在本功能范围内。）"));
        return;
    }

    if (!m_storage)
        return;

    m_storage->markAlarmHandled(key.first, key.second, disposition, handledBy, note);

    // 处理告警本身也是一次操作，写进操作留痕 —— 审计要查"谁在什么时候动了什么"。
    DataStorage::OperationEntry entry;
    entry.time = QDateTime::currentDateTime();
    entry.user = handledBy;
    entry.deviceId = key.first;
    entry.tagId = key.second;
    entry.action = QStringLiteral("处理告警");
    entry.detail = note.isEmpty()
                       ? alarmDispositionName(disposition)
                       : QStringLiteral("%1：%2").arg(alarmDispositionName(disposition), note);
    entry.success = true;
    m_storage->insertOperation(entry);
}
