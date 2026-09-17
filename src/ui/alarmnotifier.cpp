#include "ui/alarmnotifier.h"

#include "core/devicemanager.h"
#include "ui/theme.h"
#include "utils/logger.h"

#include <QAction>
#include <QApplication>
#include <QEnterEvent>
#include <QFontMetrics>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QList>
#include <QMainWindow>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QSettings>
#include <QStatusBar>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QVBoxLayout>

namespace {

constexpr int kToastWidth = 344;   ///< 浮层宽度（固定，便于堆叠对齐）
constexpr int kToastMargin = 14;   ///< 浮层距窗口右下角的留白
constexpr int kToastGap = 8;       ///< 浮层之间的间距
constexpr int kToastTextWidth = kToastWidth - 16 - 12 - 4; ///< 正文可用宽度（扣掉左右边距）
constexpr int kToastTitleWidth = 240; ///< 标题可用宽度（还要给级别徽标和关闭按钮留位置）
constexpr int kMaxToasts = 4;      ///< 同屏最多堆几条，再多就丢最旧的，避免刷屏
constexpr int kAutoCloseMs = 6000; ///< 非严重告警的自动淡出延时

/// 浮层淡出时长。
constexpr int kFadeMs = 400;

/// 告警级别的主色（左侧色条 + 级别文字）：两套主题共用，需要足够醒目。
QColor levelAccent(AlarmLevel level)
{
    switch (level) {
    case AlarmLevel::Critical:
        return QColor(0xd94a4a);
    case AlarmLevel::Warning:
        return QColor(0xe8912d);
    case AlarmLevel::Info:
        break;
    }
    return QColor(0x2f7bd6);
}

/// QSS 里要用 rgba() 才能带上透明度（QColor::name() 会把它丢掉），
/// 且 alpha 只认百分比写法。
QString cssColor(const QColor &color)
{
    return QStringLiteral("rgba(%1,%2,%3,%4%)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(qRound(color.alpha() * 100.0 / 255.0));
}

/// 按当前主题取一个"弱化文字色"——直接调透明度，比在 QSS 里再定义一套 token 简单。
QColor mutedText(const QWidget *widget)
{
    QColor color = widget->palette().color(QPalette::WindowText);
    color.setAlpha(170);
    return color;
}

QSettings appSettings()
{
    return QSettings(QStringLiteral("Y71jj123"),
                     QStringLiteral("qt-industrial-equipment-monitor"));
}

} // namespace

// ============================ AlarmToast ============================

AlarmToast::AlarmToast(const AlarmRecord &record, const QString &deviceName, QWidget *parent)
    : QWidget(parent)
    , m_record(record)
    , m_key(record.deviceId + QLatin1Char('/') + record.tagId)
    , m_accent(levelAccent(record.level))
{
    // 刻意不做成独立的顶层小窗：那样要自己跟随主窗口的移动 / 最小化 / 层级，
    // 作为主窗口的子控件贴角摆放，行为最可控。
    setFixedWidth(kToastWidth);
    setCursor(Qt::PointingHandCursor);
    setToolTip(QStringLiteral("%1 ｜ %2\n点位 %3 ｜ 数值 %4\n%5\n\n点击查看告警列表")
                   .arg(alarmLevelName(record.level),
                        deviceName,
                        record.tagId,
                        QString::number(record.value, 'f', 2),
                        record.message));

    ensurePolished(); // 先让样式表生效，下面取字体量宽度才准

    const QColor text = palette().color(QPalette::WindowText);
    const QColor muted = mutedText(this);

    auto *levelLabel = new QLabel(alarmLevelName(record.level), this);
    levelLabel->setStyleSheet(QStringLiteral("color:%1; font-weight:700;").arg(m_accent.name()));

    auto *titleLabel = new QLabel(this);
    titleLabel->setStyleSheet(QStringLiteral("color:%1; font-weight:600;").arg(text.name()));
    titleLabel->ensurePolished(); // 加粗后字更宽，量宽度之前必须先把样式表落实

    // 设备名可能很长，不限宽会把右边的关闭按钮挤出浮层外
    const QFontMetrics titleMetrics(titleLabel->font());
    titleLabel->setText(titleMetrics.elidedText(deviceName, Qt::ElideRight, kToastTitleWidth));

    auto *closeButton = new QPushButton(QStringLiteral("×"), this);
    closeButton->setFixedSize(20, 20);
    closeButton->setCursor(Qt::ArrowCursor);
    closeButton->setFocusPolicy(Qt::NoFocus);
    closeButton->setToolTip(QStringLiteral("关闭这条提醒"));
    closeButton->setStyleSheet(
        QStringLiteral("QPushButton { border:none; background:transparent; color:%1;"
                       " font-size:15px; padding:0; }"
                       "QPushButton:hover { color:%2; }")
            .arg(cssColor(muted), m_accent.name()));
    connect(closeButton, &QPushButton::clicked, this, &AlarmToast::fadeOut);

    auto *header = new QHBoxLayout;
    header->setSpacing(8);
    header->addWidget(levelLabel);
    header->addWidget(titleLabel);
    header->addStretch();
    header->addWidget(closeButton, 0, Qt::AlignTop);

    // 正文一律压成一行：浮层高度固定才好堆叠，完整内容看 tooltip 或告警列表。
    // 两个标签都带自己的样式表（字号不同），量宽度前先 ensurePolished 让字号生效。
    auto *infoLabel = new QLabel(this);
    infoLabel->setStyleSheet(QStringLiteral("color:%1; font-size:12px;").arg(cssColor(muted)));
    infoLabel->ensurePolished();

    const QFontMetrics infoMetrics(infoLabel->font());
    infoLabel->setText(infoMetrics.elidedText(
        QStringLiteral("%1 ｜ 数值 %2 ｜ %3")
            .arg(record.tagId,
                 QString::number(record.value, 'f', 2),
                 record.time.toString(QStringLiteral("MM-dd HH:mm:ss"))),
        Qt::ElideRight, kToastTextWidth));

    auto *messageLabel = new QLabel(this);
    messageLabel->setStyleSheet(QStringLiteral("color:%1; font-size:12px;").arg(text.name()));
    messageLabel->ensurePolished();

    const QFontMetrics messageMetrics(messageLabel->font());
    messageLabel->setText(messageMetrics.elidedText(record.message, Qt::ElideRight, kToastTextWidth));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 10, 12, 12); // 左边多留 4px 给那条级别色条
    layout->setSpacing(5);
    layout->addLayout(header);
    layout->addWidget(infoLabel);
    layout->addWidget(messageLabel);

    // 文字都不换行，高度是确定的，交给布局算即可（宽度已被 setFixedWidth 钉死）
    adjustSize();

    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, &AlarmToast::fadeOut);
}

void AlarmToast::startLifecycle()
{
    // 严重告警只能人工关：自动淡出会让人错过最该看到的那条
    if (critical())
        return;

    m_timer->start(kAutoCloseMs);
}

void AlarmToast::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);

    QPainterPath card;
    card.addRoundedRect(box, 8.0, 8.0);

    painter.fillPath(card, palette().color(QPalette::Base));
    painter.setPen(QPen(palette().color(QPalette::Mid), 1.0));
    painter.drawPath(card);

    // 左侧级别色条：一眼分清严重程度。裁到圆角卡片里，免得方角戳出来
    painter.setClipPath(card);
    painter.fillRect(QRectF(box.left(), box.top(), 5.0, box.height()), m_accent);
}

void AlarmToast::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }

    emit activated(m_record.deviceId, m_record.tagId);

    // 点开就算是看过了；严重告警例外，它必须由人按 × 明确关掉
    if (!critical())
        fadeOut();
}

void AlarmToast::enterEvent(QEnterEvent *event)
{
    QWidget::enterEvent(event);

    // 鼠标压上来时先别淡出 —— 正在看的时候消失最恼人
    if (m_timer->isActive())
        m_timer->stop();
}

void AlarmToast::leaveEvent(QEvent *event)
{
    QWidget::leaveEvent(event);

    if (!m_fading && !m_timer->isActive() && !critical())
        m_timer->start(kAutoCloseMs);
}

void AlarmToast::fadeOut()
{
    if (m_fading)
        return;
    m_fading = true;
    m_timer->stop();

    auto *effect = new QGraphicsOpacityEffect(this); // 控件接管所有权
    setGraphicsEffect(effect);

    auto *animation = new QPropertyAnimation(effect, "opacity", this);
    animation->setDuration(kFadeMs);
    animation->setStartValue(1.0);
    animation->setEndValue(0.0);
    animation->setEasingCurve(QEasingCurve::OutCubic);
    connect(animation, &QPropertyAnimation::finished, this, [this]() { emit dismissed(this); });
    animation->start(QAbstractAnimation::DeleteWhenStopped);
}

// ============================ AlarmNotifier ============================

AlarmNotifier::AlarmNotifier(QWidget *host, AlarmEngine *engine, DeviceManager *manager)
    : QObject(host)
    , m_host(host)
    , m_engine(engine)
    , m_manager(manager)
{
    if (m_host)
        m_host->installEventFilter(this); // 窗口一变形就要把浮层重新贴回右下角

    setupTray();

    if (m_engine) {
        connect(m_engine, &AlarmEngine::alarmRaised, this, &AlarmNotifier::notify);
        connect(m_engine, &AlarmEngine::activeAlarmsChanged, this, &AlarmNotifier::updateTrayIcon);
    }

    updateTrayIcon();
}

bool AlarmNotifier::soundEnabled()
{
    return appSettings().value(QStringLiteral("ui/alarmSound"), true).toBool();
}

void AlarmNotifier::setSoundEnabled(bool enabled)
{
    appSettings().setValue(QStringLiteral("ui/alarmSound"), enabled);
}

void AlarmNotifier::setupTray()
{
    // 部分精简版 Windows / 远程会话里根本没有通知区域，此时整体降级：
    // 浮层与声音照常，只是没有托盘图标，也不要因此崩掉。
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        Log::warn(QStringLiteral("系统托盘不可用，已降级为仅浮层 + 声音提醒"));
        return;
    }

    m_tray = new QSystemTrayIcon(this);
    connect(m_tray, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick)
                    showHost();
            });

    m_trayMenu = new QMenu(m_host);

    m_actionShow = m_trayMenu->addAction(QStringLiteral("显示主窗口"));
    connect(m_actionShow, &QAction::triggered, this, &AlarmNotifier::showHost);

    m_actionAckAll = m_trayMenu->addAction(QStringLiteral("确认全部告警"));
    m_actionAckAll->setEnabled(m_engine != nullptr);
    if (m_engine)
        connect(m_actionAckAll, &QAction::triggered, m_engine, &AlarmEngine::acknowledgeAll);

    m_trayMenu->addSeparator();
    m_actionQuit = m_trayMenu->addAction(QStringLiteral("退出"));
    connect(m_actionQuit, &QAction::triggered, this, &AlarmNotifier::quitApplication);

    m_tray->setContextMenu(m_trayMenu);
    m_tray->show();
}

void AlarmNotifier::notify(const AlarmRecord &record)
{
    QString deviceName = record.deviceId;
    if (m_manager) {
        const DeviceInfo info = m_manager->device(record.deviceId);
        if (!info.name.isEmpty())
            deviceName = info.name;
    }

    if (record.level == AlarmLevel::Critical) {
        // 只给严重告警出声：成批告警时全都响会变成噪音，反倒让人把声音关掉
        if (soundEnabled())
            QApplication::beep();

        // 窗口不在前台时闪一下任务栏，补上浮层看不见的情况
        if (m_host && !m_host->isActiveWindow())
            QApplication::alert(m_host, 4000);
    }

    showToast(record, deviceName);
    showTrayMessage(record, deviceName);
    updateTrayIcon();
}

void AlarmNotifier::showHost()
{
    if (!m_host)
        return;

    if (m_host->isMinimized())
        m_host->showNormal();
    else
        m_host->show();

    m_host->raise();
    m_host->activateWindow();
}

bool AlarmNotifier::handleCloseRequest()
{
    // "退出"是明确的退出意图，或者压根没有托盘 —— 这时候必须真关，
    // 否则窗口一藏，用户就再也找不回来了
    if (m_quitting || !m_tray)
        return false;

    m_host->hide();

    if (!m_closeHintShown) {
        m_closeHintShown = true;
        m_tray->showMessage(QStringLiteral("已最小化到托盘"),
                            QStringLiteral("采集与告警提醒仍在后台运行。\n"
                                           "双击托盘图标可重新打开，或从托盘菜单退出。"),
                            QSystemTrayIcon::Information, 4000);
    }
    return true;
}

bool AlarmNotifier::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_host && event->type() == QEvent::Resize)
        layoutToasts();

    return QObject::eventFilter(watched, event);
}

void AlarmNotifier::showToast(const AlarmRecord &record, const QString &deviceName)
{
    if (!m_host)
        return;

    const QString key = record.deviceId + QLatin1Char('/') + record.tagId;

    // 同一点位再次告警时先把旧浮层收掉，让新的落到最下面（离视线最近）
    const QList<AlarmToast *> existing = m_toasts;
    for (AlarmToast *toast : existing) {
        if (toast->key() == key) {
            removeToast(toast);
            break;
        }
    }

    while (m_toasts.size() >= kMaxToasts)
        removeToast(m_toasts.last());

    auto *toast = new AlarmToast(record, deviceName, m_host);
    connect(toast, &AlarmToast::dismissed, this, &AlarmNotifier::removeToast);
    connect(toast, &AlarmToast::activated, this, &AlarmNotifier::alarmActivated);

    m_toasts.prepend(toast);
    toast->show();
    toast->startLifecycle();
    layoutToasts();
}

void AlarmNotifier::removeToast(AlarmToast *toast)
{
    if (!toast || !m_toasts.removeOne(toast))
        return;

    toast->hide();
    toast->deleteLater();
    layoutToasts();
}

void AlarmNotifier::layoutToasts()
{
    if (!m_host)
        return;

    int bottom = m_host->height() - kToastMargin;
    if (auto *window = qobject_cast<QMainWindow *>(m_host)) {
        if (QStatusBar *bar = window->statusBar())
            bottom -= bar->height(); // 别盖住状态栏上的统计信息
    }

    for (AlarmToast *toast : m_toasts) {
        const int height = toast->height();
        const int x = m_host->width() - kToastMargin - toast->width();
        const int y = bottom - height;

        toast->setGeometry(x, y, toast->width(), height);
        toast->raise(); // 子控件默认压在分割器下面，不抬一下看不见
        bottom = y - kToastGap;
    }
}

void AlarmNotifier::updateTrayIcon()
{
    if (!m_tray)
        return;

    int unacknowledged = 0;
    if (m_engine) {
        const QList<AlarmRecord> alarms = m_engine->activeAlarms();
        for (const AlarmRecord &record : alarms) {
            if (!record.acknowledged)
                ++unacknowledged;
        }
    }

    // 托盘底色由系统决定、深浅都有可能，所以别用跟随主题的图标色：
    // 平时用中性蓝、有待确认告警时转红，两种底色上都看得清。
    m_tray->setIcon(makeIcon(IconType::AlarmBell,
                             unacknowledged > 0 ? QColor(0xe04a4a) : QColor(0x5a9be0),
                             32));
    m_tray->setToolTip(unacknowledged > 0
                           ? QStringLiteral("工业设备监控 ｜ %1 条告警待确认").arg(unacknowledged)
                           : QStringLiteral("工业设备监控 ｜ 运行正常"));

    if (m_actionAckAll)
        m_actionAckAll->setEnabled(m_engine != nullptr && unacknowledged > 0);
}

void AlarmNotifier::showTrayMessage(const AlarmRecord &record, const QString &deviceName)
{
    if (!m_tray || !m_tray->isVisible())
        return;

    QSystemTrayIcon::MessageIcon icon = QSystemTrayIcon::Information;
    int timeout = 5000;
    if (record.level == AlarmLevel::Critical) {
        icon = QSystemTrayIcon::Critical;
        timeout = 10000; // 严重的多留一会儿
    } else if (record.level == AlarmLevel::Warning) {
        icon = QSystemTrayIcon::Warning;
    }

    m_tray->showMessage(QStringLiteral("[%1] %2").arg(alarmLevelName(record.level), deviceName),
                        QStringLiteral("%1：%2\n%3")
                            .arg(record.tagId,
                                 QString::number(record.value, 'f', 2),
                                 record.message),
                        icon, timeout);
}

void AlarmNotifier::quitApplication()
{
    m_quitting = true;

    // 主动隐藏：否则托盘图标会一直赖在任务栏上，直到鼠标划过才消失
    if (m_tray)
        m_tray->hide();

    QApplication::quit();
}
