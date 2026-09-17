#include "ui/rulepanel.h"

#include "core/alarmengine.h"
#include "core/devicemanager.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

RulePanel::RulePanel(AlarmEngine *engine, DeviceManager *manager, QWidget *parent)
    : QWidget(parent)
    , m_engine(engine)
    , m_manager(manager)
{
    setupUi();
    reload();
}

void RulePanel::setupUi()
{
    // ---------------- 编辑区 ----------------
    auto *editor = new QGroupBox(QStringLiteral("规则编辑"), this);

    m_deviceBox = new QComboBox(editor);
    m_deviceBox->setMinimumWidth(160);
    m_tagBox = new QComboBox(editor);
    m_tagBox->setMinimumWidth(130);

    m_kindBox = new QComboBox(editor);
    m_kindBox->addItem(alarmKindName(AlarmKind::Range), int(AlarmKind::Range));
    m_kindBox->addItem(alarmKindName(AlarmKind::RateChange), int(AlarmKind::RateChange));

    m_lowSpin = new QDoubleSpinBox(editor);
    m_lowSpin->setRange(-1000000.0, 1000000.0);
    m_lowSpin->setDecimals(2);
    m_lowSpin->setValue(0.0);

    m_highSpin = new QDoubleSpinBox(editor);
    m_highSpin->setRange(-1000000.0, 1000000.0);
    m_highSpin->setDecimals(2);
    m_highSpin->setValue(100.0);

    m_rateSpin = new QDoubleSpinBox(editor);
    m_rateSpin->setRange(0.0, 1000000.0);
    m_rateSpin->setDecimals(2);
    m_rateSpin->setValue(5.0);
    m_rateSpin->setSuffix(QStringLiteral(" /s"));

    m_enabledBox = new QCheckBox(QStringLiteral("启用"), editor);
    m_enabledBox->setChecked(true);

    m_lowLabel = new QLabel(QStringLiteral("下限"), editor);
    m_highLabel = new QLabel(QStringLiteral("上限"), editor);
    m_rateLabel = new QLabel(QStringLiteral("变化率"), editor);

    auto *applyButton = new QPushButton(QStringLiteral("添加 / 更新规则"), editor);
    auto *removeButton = new QPushButton(QStringLiteral("删除选中"), editor);
    removeButton->setProperty("variant", "danger");

    auto *grid = new QGridLayout(editor);
    grid->addWidget(new QLabel(QStringLiteral("设备"), editor), 0, 0);
    grid->addWidget(m_deviceBox, 0, 1);
    grid->addWidget(new QLabel(QStringLiteral("点位"), editor), 0, 2);
    grid->addWidget(m_tagBox, 0, 3);
    grid->addWidget(new QLabel(QStringLiteral("类型"), editor), 0, 4);
    grid->addWidget(m_kindBox, 0, 5);
    grid->addWidget(m_lowLabel, 1, 0);
    grid->addWidget(m_lowSpin, 1, 1);
    grid->addWidget(m_highLabel, 1, 2);
    grid->addWidget(m_highSpin, 1, 3);
    grid->addWidget(m_rateLabel, 1, 4);
    grid->addWidget(m_rateSpin, 1, 5);
    grid->addWidget(m_enabledBox, 2, 0);
    grid->addWidget(applyButton, 2, 4);
    grid->addWidget(removeButton, 2, 5);

    // ---------------- 规则表 ----------------
    m_table = new QTableWidget(0, 7, this);
    m_table->setHorizontalHeaderLabels({QStringLiteral("设备"),
                                        QStringLiteral("点位"),
                                        QStringLiteral("类型"),
                                        QStringLiteral("下限"),
                                        QStringLiteral("上限"),
                                        QStringLiteral("变化率(/s)"),
                                        QStringLiteral("状态")});
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_table->verticalHeader()->setVisible(false);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(editor);
    layout->addWidget(m_table);

    connect(m_deviceBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &RulePanel::onDeviceChanged);
    connect(m_kindBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &RulePanel::onKindChanged);
    connect(applyButton, &QPushButton::clicked, this, &RulePanel::onApply);
    connect(removeButton, &QPushButton::clicked, this, &RulePanel::onRemoveSelected);
    connect(m_table, &QTableWidget::itemSelectionChanged,
            this, &RulePanel::onTableSelectionChanged);
}

void RulePanel::reload()
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
    refreshTable();
    onKindChanged();
}

void RulePanel::populatePoints(const QString &deviceId)
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

void RulePanel::onDeviceChanged()
{
    populatePoints(m_deviceBox->currentData().toString());
}

void RulePanel::onKindChanged()
{
    const auto kind = static_cast<AlarmKind>(m_kindBox->currentData().toInt());
    const bool isRange = (kind == AlarmKind::Range);

    m_lowLabel->setVisible(isRange);
    m_lowSpin->setVisible(isRange);
    m_highLabel->setVisible(isRange);
    m_highSpin->setVisible(isRange);
    m_rateLabel->setVisible(!isRange);
    m_rateSpin->setVisible(!isRange);
}

void RulePanel::onApply()
{
    const QString deviceId = m_deviceBox->currentData().toString();
    const QString tagId = m_tagBox->currentData().toString();
    if (deviceId.isEmpty() || tagId.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("规则"), QStringLiteral("请先选择设备与点位。"));
        return;
    }

    AlarmRule rule;
    rule.deviceId = deviceId;
    rule.tagId = tagId;
    rule.kind = static_cast<AlarmKind>(m_kindBox->currentData().toInt());
    rule.lowLimit = m_lowSpin->value();
    rule.highLimit = m_highSpin->value();
    rule.rateLimit = m_rateSpin->value();
    rule.enabled = m_enabledBox->isChecked();

    if (rule.kind == AlarmKind::Range && rule.lowLimit >= rule.highLimit) {
        QMessageBox::warning(this, QStringLiteral("规则无效"), QStringLiteral("下限必须小于上限。"));
        return;
    }
    if (rule.kind == AlarmKind::RateChange && rule.rateLimit <= 0.0) {
        QMessageBox::warning(this, QStringLiteral("规则无效"),
                             QStringLiteral("变化率阈值必须大于 0。"));
        return;
    }

    m_engine->addRule(rule);
    refreshTable();
}

void RulePanel::onRemoveSelected()
{
    const int row = m_table->currentRow();
    if (row < 0)
        return;

    const QString deviceId = m_table->item(row, 0)->data(Qt::UserRole).toString();
    const QString tagId = m_table->item(row, 1)->data(Qt::UserRole).toString();

    const auto answer = QMessageBox::question(
        this, QStringLiteral("删除规则"),
        QStringLiteral("确定删除 %1 / %2 的告警规则吗？").arg(deviceId.left(8), tagId),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;

    m_engine->removeRule(deviceId, tagId);
    refreshTable();
}

void RulePanel::onTableSelectionChanged()
{
    const int row = m_table->currentRow();
    if (row < 0)
        return;

    // 把选中行的规则回填到编辑区，方便改完再"添加 / 更新"
    const QList<AlarmRule> rules = m_engine->rules();
    if (row >= rules.size())
        return;

    const AlarmRule &rule = rules.at(row);
    const int deviceIndex = m_deviceBox->findData(rule.deviceId);
    if (deviceIndex >= 0) {
        m_deviceBox->setCurrentIndex(deviceIndex);
        const int tagIndex = m_tagBox->findData(rule.tagId);
        if (tagIndex >= 0)
            m_tagBox->setCurrentIndex(tagIndex);
    }

    const int kindIndex = m_kindBox->findData(int(rule.kind));
    if (kindIndex >= 0)
        m_kindBox->setCurrentIndex(kindIndex);

    m_lowSpin->setValue(rule.lowLimit);
    m_highSpin->setValue(rule.highLimit);
    m_rateSpin->setValue(rule.rateLimit);
    m_enabledBox->setChecked(rule.enabled);
}

void RulePanel::refreshTable()
{
    const QList<AlarmRule> rules = m_engine->rules();

    m_table->setRowCount(0);
    for (int i = 0; i < rules.size(); ++i) {
        const AlarmRule &rule = rules.at(i);
        const int row = m_table->rowCount();
        m_table->insertRow(row);

        const DeviceInfo info = m_manager->device(rule.deviceId);
        const QString deviceName = info.name.isEmpty() ? rule.deviceId : info.name;

        auto *deviceItem = new QTableWidgetItem(deviceName);
        deviceItem->setData(Qt::UserRole, rule.deviceId);
        m_table->setItem(row, 0, deviceItem);

        auto *tagItem = new QTableWidgetItem(rule.tagId);
        tagItem->setData(Qt::UserRole, rule.tagId);
        m_table->setItem(row, 1, tagItem);

        m_table->setItem(row, 2, new QTableWidgetItem(alarmKindName(rule.kind)));
        m_table->setItem(row, 3, new QTableWidgetItem(
                                     rule.kind == AlarmKind::Range
                                         ? QString::number(rule.lowLimit, 'f', 2)
                                         : QStringLiteral("—")));
        m_table->setItem(row, 4, new QTableWidgetItem(
                                     rule.kind == AlarmKind::Range
                                         ? QString::number(rule.highLimit, 'f', 2)
                                         : QStringLiteral("—")));
        m_table->setItem(row, 5, new QTableWidgetItem(
                                     rule.kind == AlarmKind::RateChange
                                         ? QString::number(rule.rateLimit, 'f', 2)
                                         : QStringLiteral("—")));
        m_table->setItem(row, 6, new QTableWidgetItem(
                                     rule.enabled ? QStringLiteral("已启用")
                                                  : QStringLiteral("已停用")));
    }
}
