#include "ui/alarmpanel.h"

#include "core/devicemanager.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QColor>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
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

} // namespace

AlarmPanel::AlarmPanel(AlarmEngine *engine, DeviceManager *manager, QWidget *parent)
    : QWidget(parent)
    , m_engine(engine)
    , m_manager(manager)
{
    setupUi();
    refresh();
}

void AlarmPanel::setupUi()
{
    m_summary = new QLabel(this);

    m_activeOnly = new QCheckBox(QStringLiteral("仅显示活动告警"), this);
    connect(m_activeOnly, &QCheckBox::toggled, this, &AlarmPanel::refresh);

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
    toolbar->addWidget(ackButton);
    toolbar->addWidget(ackAllButton);
    toolbar->addWidget(clearButton);

    m_table = new QTableWidget(0, 7, this);
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

    for (const AlarmRecord &record : records) {
        if (record.active) {
            ++activeCount;
            if (!record.acknowledged)
                ++unacknowledged;
        }

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

        const QString stateText = record.active
                                      ? (record.acknowledged ? QStringLiteral("已确认")
                                                             : QStringLiteral("活动"))
                                      : QStringLiteral("已恢复");

        const QStringList texts = {
            record.time.toString(QStringLiteral("MM-dd HH:mm:ss")),
            alarmLevelName(record.level),
            deviceName,
            record.tagId,
            QString::number(record.value, 'f', 2),
            stateText,
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

        m_rowKeys.append(qMakePair(record.deviceId, record.tagId));
    }

    m_summary->setText(QStringLiteral("活动告警 %1 条（未确认 %2） ｜ 历史共 %3 条")
                           .arg(activeCount)
                           .arg(unacknowledged)
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
