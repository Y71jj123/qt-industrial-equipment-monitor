#include "ui/overviewpanel.h"

#include "core/acquisitionscheduler.h"
#include "core/alarmengine.h"
#include "storage/datastorage.h"

#include <QAbstractItemView>
#include <QColor>
#include <QDate>
#include <QEvent>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHideEvent>
#include <QLabel>
#include <QLocale>
#include <QPainter>
#include <QPixmap>
#include <QShowEvent>
#include <QStringList>
#include <QStyle>
#include <QTableWidget>
#include <QTime>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace {

/// 设备卡的列数。3 列在 1360 宽的主窗口里既不挤也不空。
constexpr int kDeviceCardColumns = 3;

/// 上面一堵 KPI 墙的列数。
constexpr int kKpiColumns = 4;

/// 「当前活动告警」最多列几条。
/// 这里是"扫一眼有没有事"，不是处理告警的地方 —— 看明细去「告警」页。
constexpr int kMaxAlarmRows = 6;

/// 数据库统计的刷新间隔（毫秒）。
/// 仪表盘看的是"量级"而不是"实时"，2 秒足够；再快就是拿磁盘换没人在意的精度。
constexpr int kStatsRefreshMs = 2000;

/// 设备卡攒批刷新间隔（毫秒）。与主窗口 100ms 的表格节流相比，
/// 这里更钝一些：卡面本来就只是"看一眼"的概览，250ms 完全够。
constexpr int kCardRefreshMs = 250;

/// 状态灯圆点。
QPixmap dotPixmap(const QColor &color, int size)
{
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawEllipse(1, 1, size - 2, size - 2);

    return pixmap;
}

/// 三态灯色 —— 与左侧设备树保持同一套颜色，避免"两处绿灯深浅不一"。
QColor stateLightColor(DeviceState state)
{
    switch (state) {
    case DeviceState::Online:
        return QColor(0x2fb46b);
    case DeviceState::Fault:
        return QColor(0xe8912d);
    case DeviceState::Offline:
        break;
    }
    return QColor(0x95a3b2);
}

/// 大数字统一走千分位，位数一眼能数清（1234567 → 1,234,567）。
QString formatCount(qint64 value)
{
    return QLocale::system().toString(value);
}

/// 百分比：一位小数在"99.5%"这种临界值上比取整更有意义。
QString formatPercent(double percent)
{
    return QStringLiteral("%1%").arg(QString::number(percent, 'f', 1));
}

} // namespace

OverviewPanel::OverviewPanel(DeviceManager *deviceManager,
                             AlarmEngine *alarmEngine,
                             DataStorage *storage,
                             AcquisitionScheduler *scheduler,
                             QWidget *parent)
    : QWidget(parent)
    , m_deviceManager(deviceManager)
    , m_alarmEngine(alarmEngine)
    , m_storage(storage)
    , m_scheduler(scheduler)
{
    setupUi();

    // 采集数据 → 本页（只记值，界面更新交给攒批定时器）
    if (m_scheduler) {
        connect(m_scheduler, &AcquisitionScheduler::tagUpdated,
                this, &OverviewPanel::onTagUpdated);
    }

    // 设备与告警的状态变化 → 立刻反映到卡面
    if (m_deviceManager) {
        connect(m_deviceManager, &DeviceManager::deviceAdded, this,
                [this](const DeviceInfo &) { refresh(); });
        connect(m_deviceManager, &DeviceManager::deviceRemoved, this,
                [this](const QString &) { refresh(); });
        connect(m_deviceManager, &DeviceManager::deviceUpdated, this,
                [this](const DeviceInfo &) { refresh(); });
        connect(m_deviceManager, &DeviceManager::groupsChanged, this,
                [this]() { rebuildDeviceCards(); });
        connect(m_deviceManager, &DeviceManager::onlineChanged, this,
                [this](const QString &deviceId, bool) {
                    updateDeviceCard(deviceId);
                    updateKpiValues();
                });
        connect(m_deviceManager, &DeviceManager::faultChanged, this,
                [this](const QString &deviceId, bool) {
                    updateDeviceCard(deviceId);
                    updateKpiValues();
                });
    }

    if (m_alarmEngine) {
        connect(m_alarmEngine, &AlarmEngine::activeAlarmsChanged, this, [this]() {
            updateKpiValues();
            rebuildActiveAlarmTable();
            markAllDevicesDirty();
        });
    }

    refresh();
}

void OverviewPanel::setupUi()
{
    // KPI 数字墙
    m_kpiGrid = new QGridLayout;
    m_kpiGrid->setContentsMargins(0, 0, 0, 0);
    m_kpiGrid->setSpacing(10);

    m_cardDevices = createKpiCard(0, 0, QStringLiteral("设备总数"), QStringLiteral("台"));
    m_cardOnline = createKpiCard(0, 1, QStringLiteral("在线设备"), QStringLiteral("台"));
    m_cardOffline = createKpiCard(0, 2, QStringLiteral("离线 / 故障"), QStringLiteral("台"));
    m_cardOnlineRate = createKpiCard(0, 3, QStringLiteral("在线率"), QString());
    m_cardActiveAlarms = createKpiCard(1, 0, QStringLiteral("活动告警"), QStringLiteral("条"));
    m_cardUnackAlarms = createKpiCard(1, 1, QStringLiteral("未确认告警"), QStringLiteral("条"));
    m_cardTotalSamples = createKpiCard(1, 2, QStringLiteral("历史采样点"), QStringLiteral("条"));
    m_cardSessionSamples = createKpiCard(1, 3, QStringLiteral("本次运行采样"), QStringLiteral("条"));

    // 设备状态区
    m_deviceSectionTitle = new QLabel(QStringLiteral("设备状态"), this);
    m_deviceSectionTitle->setObjectName(QStringLiteral("sectionTitle"));

    m_emptyHint = new QLabel(QStringLiteral("还没有设备 —— 用工具栏的「添加设备」建一台，这里就会长出状态卡。"),
                             this);
    m_emptyHint->setObjectName(QStringLiteral("emptyHint"));
    m_emptyHint->setWordWrap(true);

    m_deviceGrid = new QGridLayout;
    m_deviceGrid->setContentsMargins(0, 0, 0, 0);
    m_deviceGrid->setSpacing(10);
    m_deviceGrid->setAlignment(Qt::AlignTop);

    // 活动告警区：有告警时才露表格，平时只有一行"当前没有活动告警"
    m_alarmSectionTitle = new QLabel(QStringLiteral("当前活动告警"), this);
    m_alarmSectionTitle->setObjectName(QStringLiteral("sectionTitle"));

    m_noAlarmHint = new QLabel(QStringLiteral("当前没有活动告警。"), this);
    m_noAlarmHint->setObjectName(QStringLiteral("emptyHint"));

    m_activeAlarmTable = new QTableWidget(0, 5, this);
    m_activeAlarmTable->setHorizontalHeaderLabels({QStringLiteral("时间"),
                                                   QStringLiteral("设备"),
                                                   QStringLiteral("点位"),
                                                   QStringLiteral("级别"),
                                                   QStringLiteral("描述")});
    m_activeAlarmTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_activeAlarmTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    m_activeAlarmTable->verticalHeader()->setVisible(false);
    m_activeAlarmTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_activeAlarmTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    // 高度按"最多 6 行"封顶：再多就该去「告警」页处理了，别把首页撑成列表页
    m_activeAlarmTable->setMaximumHeight(240);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(12);
    layout->addLayout(m_kpiGrid);
    layout->addWidget(m_deviceSectionTitle);
    layout->addWidget(m_emptyHint);
    layout->addLayout(m_deviceGrid);
    layout->addWidget(m_alarmSectionTitle);
    layout->addWidget(m_noAlarmHint);
    layout->addWidget(m_activeAlarmTable);
    layout->addStretch(1);

    // 数据库统计：低频 + 只在页面可见时转（见 showEvent / hideEvent）
    m_statsTimer = new QTimer(this);
    m_statsTimer->setInterval(kStatsRefreshMs);
    connect(m_statsTimer, &QTimer::timeout, this, &OverviewPanel::refreshDatabaseStats);

    // 设备卡攒批刷新：单次触发，有脏数据才启动
    m_cardTimer = new QTimer(this);
    m_cardTimer->setSingleShot(true);
    m_cardTimer->setInterval(kCardRefreshMs);
    connect(m_cardTimer, &QTimer::timeout, this, &OverviewPanel::flushDirtyDeviceCards);
}

OverviewPanel::KpiCard *OverviewPanel::createKpiCard(int row,
                                                     int column,
                                                     const QString &title,
                                                     const QString &unit)
{
    auto *card = new KpiCard;

    card->frame = new QFrame(this);
    card->frame->setObjectName(QStringLiteral("kpiCard"));
    card->frame->setFrameShape(QFrame::NoFrame);
    card->frame->setMinimumHeight(92);

    auto *titleLabel = new QLabel(title, card->frame);
    titleLabel->setObjectName(QStringLiteral("kpiTitle"));

    card->value = new QLabel(QStringLiteral("—"), card->frame);
    card->value->setObjectName(QStringLiteral("kpiValue"));

    auto *valueRow = new QHBoxLayout;
    valueRow->setContentsMargins(0, 0, 0, 0);
    valueRow->setSpacing(4);
    valueRow->addWidget(card->value, 0, Qt::AlignBottom);
    if (!unit.isEmpty()) {
        auto *unitLabel = new QLabel(unit, card->frame);
        unitLabel->setObjectName(QStringLiteral("kpiUnit"));
        valueRow->addWidget(unitLabel, 0, Qt::AlignBottom);
    }
    valueRow->addStretch(1);

    card->hint = new QLabel(QString(), card->frame);
    card->hint->setObjectName(QStringLiteral("kpiHint"));

    auto *cardLayout = new QVBoxLayout(card->frame);
    cardLayout->setContentsMargins(14, 12, 14, 12);
    cardLayout->setSpacing(3);
    cardLayout->addWidget(titleLabel);
    cardLayout->addLayout(valueRow);
    cardLayout->addWidget(card->hint);

    m_kpiGrid->addWidget(card->frame, row, column);
    return card;
}

void OverviewPanel::applyTone(KpiCard *card, const QString &tone)
{
    if (!card || !card->frame)
        return;

    const QString frameName = QStringLiteral("kpiCard") + tone;
    const QString valueName = QStringLiteral("kpiValue") + tone;

    // 名称没变就别动 —— objectName 赋值本身很轻，但紧跟的 unpolish/polish
    // 会触发整棵子树的样式重算，每 2 秒白跑一次不值得。
    if (card->frame->objectName() == frameName)
        return;

    card->frame->setObjectName(frameName);
    card->value->setObjectName(valueName);

    const QList<QWidget *> widgets = {card->frame, card->value};
    for (QWidget *widget : widgets) {
        widget->style()->unpolish(widget);
        widget->style()->polish(widget);
    }
}

void OverviewPanel::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);

    // 切到本页：立刻刷一次，别让用户看到上一轮的陈旧数字
    refresh();
    if (m_statsTimer)
        m_statsTimer->start();
}

void OverviewPanel::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);

    if (m_statsTimer)
        m_statsTimer->stop();
}

bool OverviewPanel::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonRelease) {
        if (auto *frame = qobject_cast<QFrame *>(watched)) {
            const QString deviceId = frame->property("deviceId").toString();
            if (!deviceId.isEmpty()) {
                emit deviceActivated(deviceId);
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void OverviewPanel::refresh()
{
    updateKpiValues();
    rebuildDeviceCards();
    rebuildActiveAlarmTable();
    refreshDatabaseStats();
}

QString OverviewPanel::valueKey(const QString &deviceId, const QString &tagId)
{
    return deviceId + QLatin1Char('/') + tagId;
}

void OverviewPanel::updateKpiValues()
{
    const QList<DeviceInfo> devices = m_deviceManager ? m_deviceManager->devices()
                                                      : QList<DeviceInfo>{};

    int online = 0;
    int fault = 0;
    int offline = 0;
    for (const DeviceInfo &device : devices) {
        switch (device.state()) {
        case DeviceState::Online:
            ++online;
            break;
        case DeviceState::Fault:
            ++fault;
            break;
        case DeviceState::Offline:
            ++offline;
            break;
        }
    }

    const int total = devices.size();
    const double onlineRate = total > 0 ? (100.0 * online / total) : 0.0;

    int activeAlarms = 0;
    int unackAlarms = 0;
    if (m_alarmEngine) {
        const QList<AlarmRecord> alarms = m_alarmEngine->activeAlarms();
        activeAlarms = alarms.size();
        for (const AlarmRecord &record : alarms) {
            if (!record.acknowledged)
                ++unackAlarms;
        }
    }

    // ---- 设备总数：中性，没有"好坏"可言 ----
    m_cardDevices->value->setText(QString::number(total));
    m_cardDevices->hint->setText(total > 0 ? QStringLiteral("共 %1 个分组").arg(m_deviceManager->groups().size())
                                           : QStringLiteral("尚未接入设备"));
    applyTone(m_cardDevices, QString());

    // ---- 在线设备 ----
    m_cardOnline->value->setText(QString::number(online));
    m_cardOnline->hint->setText(QStringLiteral("故障 %1 台").arg(fault));
    if (total == 0)
        applyTone(m_cardOnline, QString());
    else if (online == total)
        applyTone(m_cardOnline, QStringLiteral("Ok"));
    else if (online == 0)
        applyTone(m_cardOnline, QStringLiteral("Danger"));
    else
        applyTone(m_cardOnline, QStringLiteral("Warn"));

    // ---- 离线 / 故障 ----
    m_cardOffline->value->setText(QString::number(offline + fault));
    m_cardOffline->hint->setText(QStringLiteral("离线 %1 · 故障 %2").arg(offline).arg(fault));
    if (total == 0)
        applyTone(m_cardOffline, QString());
    else if (fault > 0)
        applyTone(m_cardOffline, QStringLiteral("Danger"));
    else if (offline > 0)
        applyTone(m_cardOffline, QStringLiteral("Warn"));
    else
        applyTone(m_cardOffline, QStringLiteral("Ok"));

    // ---- 在线率 ----
    if (total == 0) {
        m_cardOnlineRate->value->setText(QStringLiteral("—"));
        m_cardOnlineRate->hint->setText(QStringLiteral("无设备"));
        applyTone(m_cardOnlineRate, QString());
    } else {
        m_cardOnlineRate->value->setText(formatPercent(onlineRate));
        m_cardOnlineRate->hint->setText(QStringLiteral("%1 / %2 台在线").arg(online).arg(total));
        if (onlineRate >= 99.0)
            applyTone(m_cardOnlineRate, QStringLiteral("Ok"));
        else if (onlineRate >= 50.0)
            applyTone(m_cardOnlineRate, QStringLiteral("Warn"));
        else
            applyTone(m_cardOnlineRate, QStringLiteral("Danger"));
    }

    // ---- 活动告警 ----
    m_cardActiveAlarms->value->setText(QString::number(activeAlarms));
    m_cardActiveAlarms->hint->setText(activeAlarms == 0 ? QStringLiteral("当前无告警")
                                                        : QStringLiteral("需要处理"));
    applyTone(m_cardActiveAlarms,
              activeAlarms == 0 ? QStringLiteral("Ok") : QStringLiteral("Danger"));

    // ---- 未确认告警 ----
    m_cardUnackAlarms->value->setText(QString::number(unackAlarms));
    m_cardUnackAlarms->hint->setText(QStringLiteral("已确认 %1 条").arg(activeAlarms - unackAlarms));
    if (unackAlarms == 0)
        applyTone(m_cardUnackAlarms, QStringLiteral("Ok"));
    else if (activeAlarms > 0 && unackAlarms == activeAlarms)
        applyTone(m_cardUnackAlarms, QStringLiteral("Danger"));
    else
        applyTone(m_cardUnackAlarms, QStringLiteral("Warn"));

    // ---- 历史采样点（数据库）----
    m_cardTotalSamples->value->setText(formatCount(m_totalSamples));
    m_cardTotalSamples->hint->setText(QStringLiteral("今日 +%1").arg(formatCount(m_todaySamples)));
    applyTone(m_cardTotalSamples, QString());

    // ---- 本次运行采样（实时计数，不受批量落库延迟影响）----
    m_cardSessionSamples->value->setText(formatCount(m_sessionSamples));
    m_cardSessionSamples->hint->setText(QStringLiteral("自本次启动起累计"));
    applyTone(m_cardSessionSamples, QString());
}

void OverviewPanel::rebuildDeviceCards()
{
    // 清空旧卡：takeAt 会把 item 的所有权交出来，控件本身用 deleteLater 销毁
    // （立刻 delete 有可能撞上正在处理的事件，Qt 里这是常见坑）。
    while (QLayoutItem *item = m_deviceGrid->takeAt(0)) {
        if (QWidget *widget = item->widget())
            widget->deleteLater();
        delete item;
    }
    m_deviceCards.clear();

    const QList<DeviceInfo> devices = m_deviceManager ? m_deviceManager->devices()
                                                      : QList<DeviceInfo>{};

    int index = 0;
    for (const DeviceInfo &device : devices) {
        auto *frame = new QFrame(this);
        frame->setObjectName(QStringLiteral("deviceCard"));
        frame->setFrameShape(QFrame::NoFrame);
        frame->setMinimumHeight(88);
        // 卡片可点：把设备 id 挂在属性上，点击时由 eventFilter 取出来
        frame->setProperty("deviceId", device.id);
        frame->setCursor(Qt::PointingHandCursor);
        frame->setToolTip(QStringLiteral("点击查看 %1 的实时数据")
                              .arg(device.name.isEmpty() ? device.id : device.name));
        frame->installEventFilter(this);

        auto *light = new QLabel(frame);
        light->setFixedSize(12, 12);

        auto *name = new QLabel(device.name.isEmpty() ? device.id : device.name, frame);
        name->setObjectName(QStringLiteral("deviceCardName"));

        auto *headerRow = new QHBoxLayout;
        headerRow->setContentsMargins(0, 0, 0, 0);
        headerRow->setSpacing(7);
        headerRow->addWidget(light, 0, Qt::AlignVCenter);
        headerRow->addWidget(name, 1);

        auto *value = new QLabel(QStringLiteral("—"), frame);
        value->setObjectName(QStringLiteral("deviceCardValue"));

        auto *meta = new QLabel(frame);
        meta->setObjectName(QStringLiteral("deviceCardMeta"));
        meta->setWordWrap(true);

        auto *cardLayout = new QVBoxLayout(frame);
        cardLayout->setContentsMargins(12, 10, 12, 10);
        cardLayout->setSpacing(3);
        cardLayout->addLayout(headerRow);
        cardLayout->addWidget(value);
        cardLayout->addWidget(meta);

        DeviceCard card;
        card.frame = frame;
        card.light = light;
        card.name = name;
        card.value = value;
        card.meta = meta;

        // 卡面大数字挑"第一个非开关量点位"：开关量显示 0/1 没什么信息量，
        // 温度 / 压力这类连续量才是现场真正关心的。
        const QList<TagPoint> points = device.points.isEmpty() ? defaultTagPoints() : device.points;
        for (const TagPoint &point : points) {
            if (!point.boolean) {
                card.primaryTag = point.id;
                card.primaryUnit = point.unit;
                break;
            }
        }

        m_deviceCards.insert(device.id, card);
        m_deviceGrid->addWidget(frame, index / kDeviceCardColumns, index % kDeviceCardColumns);
        ++index;
    }

    m_emptyHint->setVisible(devices.isEmpty());
    m_deviceSectionTitle->setText(
        devices.isEmpty() ? QStringLiteral("设备状态")
                          : QStringLiteral("设备状态（%1 台）").arg(devices.size()));

    // 建完立刻填一遍内容，避免卡片先以"—"露脸再跳数
    for (const DeviceInfo &device : devices)
        updateDeviceCard(device.id);
}

void OverviewPanel::updateDeviceCard(const QString &deviceId)
{
    const auto it = m_deviceCards.find(deviceId);
    if (it == m_deviceCards.end() || !m_deviceManager)
        return;

    DeviceCard &card = it.value();
    const DeviceInfo device = m_deviceManager->device(deviceId);

    card.light->setPixmap(dotPixmap(stateLightColor(device.state()), 12));
    card.name->setText(device.name.isEmpty() ? device.id : device.name);

    // 主点位实时值
    if (!card.primaryTag.isEmpty() && m_lastValues.contains(valueKey(deviceId, card.primaryTag))) {
        const double value = m_lastValues.value(valueKey(deviceId, card.primaryTag)).toDouble();
        const QString number = QString::number(value, 'f', 1);
        card.value->setText(card.primaryUnit.isEmpty()
                                ? number
                                : QStringLiteral("%1 %2").arg(number, card.primaryUnit));
    } else {
        card.value->setText(QStringLiteral("—"));
    }

    // 该设备的活动告警数
    int activeAlarms = 0;
    if (m_alarmEngine) {
        for (const AlarmRecord &record : m_alarmEngine->activeAlarms()) {
            if (record.deviceId == deviceId)
                ++activeAlarms;
        }
    }

    QStringList parts;
    parts << device.groupName();
    parts << deviceStateName(device.state());
    if (activeAlarms > 0)
        parts << QStringLiteral("告警 %1").arg(activeAlarms);

    const QDateTime seen = m_lastSeen.value(deviceId);
    parts << (seen.isValid() ? QStringLiteral("更新 %1").arg(seen.toString(QStringLiteral("HH:mm:ss")))
                             : QStringLiteral("暂无数据"));

    card.meta->setText(parts.join(QStringLiteral(" · ")));
}

void OverviewPanel::markAllDevicesDirty()
{
    if (!m_deviceManager)
        return;

    const QList<DeviceInfo> devices = m_deviceManager->devices();
    for (const DeviceInfo &device : devices)
        m_dirtyDevices.insert(device.id);

    if (!m_dirtyDevices.isEmpty() && m_cardTimer && !m_cardTimer->isActive())
        m_cardTimer->start();
}

void OverviewPanel::onTagUpdated(const QString &deviceId, const QString &tagId, const QVariant &value)
{
    ++m_sessionSamples;
    m_lastValues.insert(valueKey(deviceId, tagId), value);
    m_lastSeen.insert(deviceId, QDateTime::currentDateTime());

    // 只更新"本次运行采样"这一个数字：它是纯计数，改一行文本几乎零成本，
    // 让用户在高频采集下也能看到数字在跳，比整体重算更有"活着"的感觉。
    if (m_cardSessionSamples)
        m_cardSessionSamples->value->setText(formatCount(m_sessionSamples));

    m_dirtyDevices.insert(deviceId);
    if (m_cardTimer && !m_cardTimer->isActive())
        m_cardTimer->start();
}

void OverviewPanel::flushDirtyDeviceCards()
{
    for (const QString &deviceId : m_dirtyDevices)
        updateDeviceCard(deviceId);

    m_dirtyDevices.clear();
}

void OverviewPanel::rebuildActiveAlarmTable()
{
    if (!m_activeAlarmTable)
        return;

    QList<AlarmRecord> alarms;
    if (m_alarmEngine)
        alarms = m_alarmEngine->activeAlarms();

    // AlarmEngine 的活动告警存在 QHash 里，拿出来是无序的 —— 这里按时间倒序，
    // "最新出的那条"永远在第一行。
    std::sort(alarms.begin(), alarms.end(),
              [](const AlarmRecord &left, const AlarmRecord &right) {
                  return left.time > right.time;
              });

    const int rows = qMin(int(alarms.size()), kMaxAlarmRows);
    m_activeAlarmTable->setRowCount(rows);

    for (int row = 0; row < rows; ++row) {
        const AlarmRecord &record = alarms.at(row);
        const QString deviceName = m_deviceManager
                                       ? m_deviceManager->device(record.deviceId).name
                                       : QString();
        const QString shownName = deviceName.isEmpty() ? record.deviceId : deviceName;

        const QStringList cells = {record.time.toString(QStringLiteral("MM-dd HH:mm:ss")),
                                   shownName,
                                   record.tagId,
                                   alarmLevelName(record.level),
                                   record.message};

        for (int column = 0; column < cells.size(); ++column) {
            auto *item = new QTableWidgetItem(cells.at(column));
            item->setToolTip(cells.at(column));
            m_activeAlarmTable->setItem(row, column, item);
        }

        // 级别上色：严重红、警告橙、提示不上色（弱化处理，免得整屏都在喊）
        QColor levelColor;
        if (record.level == AlarmLevel::Critical)
            levelColor = QColor(0xd94a4a);
        else if (record.level == AlarmLevel::Warning)
            levelColor = QColor(0xd4832a);
        if (levelColor.isValid()) {
            if (QTableWidgetItem *item = m_activeAlarmTable->item(row, 3)) {
                item->setForeground(levelColor);
                QFont font = item->font();
                font.setBold(true);
                item->setFont(font);
            }
        }
    }

    const bool empty = alarms.isEmpty();
    m_alarmSectionTitle->setText(
        empty ? QStringLiteral("当前活动告警")
              : QStringLiteral("当前活动告警（共 %1 条%2）")
                    .arg(alarms.size())
                    .arg(alarms.size() > kMaxAlarmRows
                             ? QStringLiteral("，仅显示最新 %1 条").arg(kMaxAlarmRows)
                             : QString()));
    m_noAlarmHint->setVisible(empty);
    m_activeAlarmTable->setVisible(!empty);
}

void OverviewPanel::refreshDatabaseStats()
{
    if (!m_storage || !m_storage->isOpen()) {
        m_totalSamples = 0;
        m_todaySamples = 0;
    } else {
        const QDateTime todayStart(QDate::currentDate(), QTime(0, 0));
        m_totalSamples = m_storage->countSamples();
        m_todaySamples = m_storage->countSamples(todayStart, QDateTime::currentDateTime());
    }

    if (!m_cardTotalSamples)
        return;

    m_cardTotalSamples->value->setText(formatCount(m_totalSamples));
    m_cardTotalSamples->hint->setText(QStringLiteral("今日 +%1").arg(formatCount(m_todaySamples)));
}
