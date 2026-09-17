#include "ui/theme.h"

#include <QApplication>
#include <QHash>
#include <QLineF>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>
#include <QPointF>
#include <QPolygonF>
#include <QRectF>
#include <QRegularExpression>
#include <QSettings>

namespace {

// ---------------------------------------------------------------------------
// 调色板
// ---------------------------------------------------------------------------

/// 一套主题的全部颜色。
///
/// 两套主题共用同一份 QSS 模板，只把模板里的 @token 替换成这里的颜色 ——
/// 这样"加一条控件样式"只需要写一遍，不会出现两套 QSS 各自漂移的问题。
struct Palette {
    QColor windowBg;         // 窗口底色
    QColor surface;          // 卡片 / 面板底
    QColor surfaceAlt;       // 次级面：表头、斑马纹
    QColor surfaceHover;     // 悬停底色
    QColor fieldBg;          // 输入类控件底
    QColor disabledBg;       // 禁用控件底
    QColor border;           // 细边框
    QColor borderStrong;     // 输入框边框（比卡片边框略重）
    QColor text;             // 主文字
    QColor textMuted;        // 次要文字
    QColor textFaint;        // 弱化文字（提示、禁用）
    QColor titleText;        // 面板标题
    QColor accent;           // 强调色
    QColor accentHover;
    QColor accentPressed;
    QColor accentSoft;       // 强调色的浅底（选中行 / 选中项）
    QColor accentSoftHover;
    QColor accentText;       // 强调色的文字形态（深底上要更亮）
    QColor onAccent;         // 强调色之上的文字
    QColor danger;
    QColor dangerHover;
    QColor toolbarBg;
    QColor toolButtonBg;
    QColor toolButtonBorder;
    QColor logBg;            // 日志区
    QColor logText;
    QColor chipBg;           // 状态栏小胶囊
    QColor scrollHandle;
    QColor scrollHandleHover;
    QColor chartBg;
    QColor chartGrid;
    QColor chartText;
};

Palette lightPalette()
{
    Palette p;
    p.windowBg          = QColor(0xeef2f7);
    p.surface           = QColor(0xffffff);
    p.surfaceAlt        = QColor(0xf5f8fc);
    p.surfaceHover      = QColor(0xeef5fd);
    p.fieldBg           = QColor(0xffffff);
    p.disabledBg        = QColor(0xdfe6ee);
    p.border            = QColor(0xdde5ee);
    p.borderStrong      = QColor(0xcfdae6);
    p.text              = QColor(0x24313f);
    p.textMuted         = QColor(0x5a6b7d);
    p.textFaint         = QColor(0x93a3b5);
    p.titleText         = QColor(0x1b3a5c);
    p.accent            = QColor(0x2f7bd6);
    p.accentHover       = QColor(0x3b8ae4);
    p.accentPressed     = QColor(0x2a6cbd);
    p.accentSoft        = QColor(0xdbeafb);
    p.accentSoftHover   = QColor(0xe8f1fb);
    p.accentText        = QColor(0x1b6fd0);
    p.onAccent          = QColor(0xffffff);
    p.danger            = QColor(0xd94a4a);
    p.dangerHover       = QColor(0xe25c5c);
    p.toolbarBg         = QColor(0xffffff);
    p.toolButtonBg      = QColor(0xf7fafd);
    p.toolButtonBorder  = QColor(0xd6e0ea);
    p.logBg             = QColor(0x1e2733); // 浅色界面里日志仍用深色控制台，运行信息更醒目
    p.logText           = QColor(0xc9d7e6);
    p.chipBg            = QColor(0xf1f5fa);
    p.scrollHandle      = QColor(0xc2cedd);
    p.scrollHandleHover = QColor(0xa3b3c6);
    p.chartBg           = QColor(0xffffff);
    p.chartGrid         = QColor(0xe3eaf3);
    p.chartText         = QColor(0x5a6b7d);
    return p;
}

Palette darkPalette()
{
    Palette p;
    p.windowBg          = QColor(0x161c24);
    p.surface           = QColor(0x1f2732);
    p.surfaceAlt        = QColor(0x232c38);
    p.surfaceHover      = QColor(0x2a3542);
    p.fieldBg           = QColor(0x1b222c);
    p.disabledBg        = QColor(0x2b3542);
    p.border            = QColor(0x2e3948);
    p.borderStrong      = QColor(0x3b4858);
    p.text              = QColor(0xdbe4ee);
    p.textMuted         = QColor(0x9dabbd);
    p.textFaint         = QColor(0x6b7a8c);
    p.titleText         = QColor(0xd6e6f7);
    p.accent            = QColor(0x3d8ee0);
    p.accentHover       = QColor(0x4f9dea);
    p.accentPressed     = QColor(0x327ac7);
    p.accentSoft        = QColor(0x24405e);
    p.accentSoftHover   = QColor(0x2c4c6e);
    p.accentText        = QColor(0x6cb0f0);
    p.onAccent          = QColor(0xffffff);
    p.danger            = QColor(0xd15454);
    p.dangerHover       = QColor(0xe06666);
    p.toolbarBg         = QColor(0x1f2732);
    p.toolButtonBg      = QColor(0x262f3b);
    p.toolButtonBorder  = QColor(0x36414f);
    p.logBg             = QColor(0x11171e);
    p.logText           = QColor(0xb9c9da);
    p.chipBg            = QColor(0x262f3b);
    p.scrollHandle      = QColor(0x3d4a5a);
    p.scrollHandleHover = QColor(0x56657a);
    p.chartBg           = QColor(0x1f2732);
    p.chartGrid         = QColor(0x303b4a);
    p.chartText         = QColor(0x9dabbd);
    return p;
}

Palette paletteOf(Theme theme)
{
    return theme == Theme::Dark ? darkPalette() : lightPalette();
}

// ---------------------------------------------------------------------------
// QSS 模板渲染
// ---------------------------------------------------------------------------

/// 模板里 @token -> 颜色的映射。漏写的 token 会在渲染时原样留下，便于一眼发现。
QHash<QString, QString> paletteValues(const Palette &p)
{
    return {
        {QStringLiteral("windowBg"), p.windowBg.name()},
        {QStringLiteral("surface"), p.surface.name()},
        {QStringLiteral("surfaceAlt"), p.surfaceAlt.name()},
        {QStringLiteral("surfaceHover"), p.surfaceHover.name()},
        {QStringLiteral("fieldBg"), p.fieldBg.name()},
        {QStringLiteral("disabledBg"), p.disabledBg.name()},
        {QStringLiteral("border"), p.border.name()},
        {QStringLiteral("borderStrong"), p.borderStrong.name()},
        {QStringLiteral("text"), p.text.name()},
        {QStringLiteral("textMuted"), p.textMuted.name()},
        {QStringLiteral("textFaint"), p.textFaint.name()},
        {QStringLiteral("titleText"), p.titleText.name()},
        {QStringLiteral("accent"), p.accent.name()},
        {QStringLiteral("accentHover"), p.accentHover.name()},
        {QStringLiteral("accentPressed"), p.accentPressed.name()},
        {QStringLiteral("accentSoft"), p.accentSoft.name()},
        {QStringLiteral("accentSoftHover"), p.accentSoftHover.name()},
        {QStringLiteral("accentText"), p.accentText.name()},
        {QStringLiteral("onAccent"), p.onAccent.name()},
        {QStringLiteral("danger"), p.danger.name()},
        {QStringLiteral("dangerHover"), p.dangerHover.name()},
        {QStringLiteral("toolbarBg"), p.toolbarBg.name()},
        {QStringLiteral("toolButtonBg"), p.toolButtonBg.name()},
        {QStringLiteral("toolButtonBorder"), p.toolButtonBorder.name()},
        {QStringLiteral("logBg"), p.logBg.name()},
        {QStringLiteral("logText"), p.logText.name()},
        {QStringLiteral("chipBg"), p.chipBg.name()},
        {QStringLiteral("scrollHandle"), p.scrollHandle.name()},
        {QStringLiteral("scrollHandleHover"), p.scrollHandleHover.name()},
    };
}

/// 样式表模板：所有颜色都写成 @token，由 renderStyle() 按主题替换。
const char *kStyleTemplate = R"QSS(
/* ==================== 全局 ==================== */
* { font-family: "Microsoft YaHei UI", "Microsoft YaHei", "Segoe UI", sans-serif; }

QWidget { font-size: 13px; color: @text; }
QMainWindow, QDialog { background: @windowBg; }

QToolTip {
    background: @surfaceAlt;
    color: @text;
    border: 1px solid @borderStrong;
    border-radius: 4px;
    padding: 5px 8px;
}

/* ==================== 工具栏 ==================== */
QToolBar {
    background: @toolbarBg;
    border: none;
    border-bottom: 1px solid @border;
    padding: 8px 10px;
    spacing: 6px;
}
QToolBar QToolButton {
    padding: 7px 14px;
    border: 1px solid @toolButtonBorder;
    border-radius: 6px;
    background: @toolButtonBg;
    color: @text;
    font-weight: 500;
}
QToolBar QToolButton:hover {
    background: @accentSoftHover;
    border-color: @accent;
    color: @accentText;
}
QToolBar QToolButton:pressed { background: @accentSoft; }
/* 可勾选的动作（如"声音报警"）没有原生按下态，靠这条把开 / 关画出来 */
QToolBar QToolButton:checked {
    background: @accentSoft;
    border-color: @accent;
    color: @accentText;
}
QToolBar QToolButton:disabled {
    background: @disabledBg;
    border-color: @border;
    color: @textFaint;
}
QToolBar::separator { width: 1px; background: @border; margin: 6px 8px; }

/* ==================== 标签页 ==================== */
QTabWidget::pane {
    border: 1px solid @border;
    border-radius: 8px;
    background: @surface;
    top: -1px;
}
QTabBar::tab {
    padding: 7px 20px;
    margin-right: 4px;
    background: @surfaceAlt;
    border: 1px solid @border;
    border-top: 2px solid transparent; /* 与选中态的顶栏同宽，避免切换时文字跳动 */
    border-bottom: none;
    border-top-left-radius: 6px;
    border-top-right-radius: 6px;
    color: @textMuted;
}
QTabBar::tab:hover:!selected { background: @surfaceHover; color: @text; }
QTabBar::tab:selected {
    background: @surface;
    color: @accentText;
    font-weight: 600;
    border-top: 2px solid @accent;
}
QTabBar::tab:disabled { color: @textFaint; background: @surfaceAlt; }

/* ==================== 列表 ==================== */
QListWidget {
    background: @surface;
    border: 1px solid @border;
    border-radius: 8px;
    padding: 6px;
    outline: none;
}
QListWidget::item {
    padding: 9px 10px;
    border: 1px solid transparent;
    border-radius: 6px;
    color: @text;
}
QListWidget::item:hover { background: @surfaceHover; }
/* 选中用"淡底 + 描边"而不是实心底：左侧状态灯的颜色要一直看得清 */
QListWidget::item:selected {
    background: @accentSoft;
    border-color: @accent;
    color: @text;
}

/* 设备树与上面的列表是一套观感：分组节点靠加粗字体区分，不额外上色 */
QTreeWidget {
    background: @surface;
    border: 1px solid @border;
    border-radius: 8px;
    padding: 6px;
    outline: none;
}
QTreeWidget::item {
    padding: 7px 6px;
    border: 1px solid transparent;
    border-radius: 6px;
    color: @text;
}
QTreeWidget::item:hover { background: @surfaceHover; }
QTreeWidget::item:selected {
    background: @accentSoft;
    border-color: @accent;
    color: @text;
}
QTreeWidget::branch { background: transparent; }

/* ==================== 表格 ==================== */
QTableWidget, QTableView {
    background: @surface;
    alternate-background-color: @surfaceAlt;
    border: 1px solid @border;
    border-radius: 8px;
    gridline-color: @border;
    selection-background-color: @accentSoft;
    selection-color: @text;
    outline: none;
}
/* 注意：这里刻意不给 ::item 设 background / padding —— 一旦设了，
   Qt 就不再画斑马纹（alternate-background-color），行高改由代码统一设置 */
QTableWidget::item:hover, QTableView::item:hover { background: @surfaceHover; }
QTableWidget::item:selected, QTableView::item:selected {
    background: @accentSoft;
    color: @text;
}
QHeaderView { background: transparent; border: none; }
QHeaderView::section {
    background: @surfaceAlt;
    color: @textMuted;
    padding: 8px 10px;
    border: none;
    border-right: 1px solid @border;
    border-bottom: 1px solid @borderStrong;
    font-weight: 600;
}
QHeaderView::section:hover { color: @accentText; }
QTableCornerButton::section { background: @surfaceAlt; border: none; }

/* ==================== 按钮 ==================== */
QPushButton {
    padding: 7px 18px;
    border: 1px solid @accent;
    border-radius: 6px;
    background: @accent;
    color: @onAccent;
    font-weight: 500;
}
QPushButton:hover { background: @accentHover; border-color: @accentHover; }
QPushButton:pressed { background: @accentPressed; border-color: @accentPressed; }
QPushButton:disabled { background: @disabledBg; border-color: @border; color: @textFaint; }
QPushButton[variant="secondary"] {
    background: @chipBg;
    color: @text;
    border: 1px solid @borderStrong;
}
QPushButton[variant="secondary"]:hover {
    background: @accentSoftHover;
    border-color: @accent;
    color: @accentText;
}
QPushButton[variant="secondary"]:disabled {
    background: @disabledBg;
    border-color: @border;
    color: @textFaint;
}
QPushButton[variant="danger"] { background: @danger; border-color: @danger; color: #ffffff; }
QPushButton[variant="danger"]:hover { background: @dangerHover; border-color: @dangerHover; }
QPushButton[variant="danger"]:disabled {
    background: @disabledBg;
    border-color: @border;
    color: @textFaint;
}

/* ==================== 输入类 ==================== */
QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox, QDateTimeEdit {
    padding: 6px 10px;
    border: 1px solid @borderStrong;
    border-radius: 6px;
    background: @fieldBg;
    color: @text;
    min-height: 18px;
    selection-background-color: @accentSoft;
    selection-color: @text;
}
QLineEdit:hover, QSpinBox:hover, QDoubleSpinBox:hover,
QComboBox:hover, QDateTimeEdit:hover { border-color: @accent; }
QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus,
QComboBox:focus, QDateTimeEdit:focus { border-color: @accent; }
QLineEdit:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled,
QComboBox:disabled, QDateTimeEdit:disabled {
    background: @disabledBg;
    color: @textFaint;
    border-color: @border;
}
QComboBox::drop-down { border: none; width: 22px; }
QComboBox QAbstractItemView {
    background: @surface;
    border: 1px solid @borderStrong;
    border-radius: 6px;
    padding: 4px;
    selection-background-color: @accentSoft;
    selection-color: @text;
    outline: none;
}

/* ==================== 分组卡片 ==================== */
QGroupBox {
    border: 1px solid @border;
    border-radius: 8px;
    background: @surface;
    margin-top: 16px;
    padding: 14px 12px 12px 12px;
}
QGroupBox::title {
    subcontrol-origin: margin;
    subcontrol-position: top left;
    left: 14px;
    padding: 0 6px;
    color: @accentText;
    font-weight: 600;
}

/* ==================== 日志区 ==================== */
/* 两套主题都保持等宽字体 + 深色控制台，日志的可读性优先于配色统一 */
QPlainTextEdit, QTextEdit {
    background: @fieldBg;
    color: @text;
    border: 1px solid @borderStrong;
    border-radius: 8px;
    padding: 6px;
    selection-background-color: @accentSoft;
    selection-color: @text;
}
QPlainTextEdit#logView {
    background: @logBg;
    color: @logText;
    border: 1px solid @border;
    border-radius: 8px;
    padding: 8px;
    font-family: Consolas, "Cascadia Mono", "Courier New", monospace;
    font-size: 12px;
}

/* ==================== 状态栏 ==================== */
QStatusBar {
    background: @surface;
    border-top: 1px solid @border;
    color: @textMuted;
    padding: 2px 10px;
}
QStatusBar::item { border: none; }
QLabel#statusInfo { color: @text; padding: 2px 4px; }
QLabel#statusSeparator { background: @border; }
/* 右侧两个信息胶囊：主题正常弱化，用户标签再弱一档 */
QLabel#statusBadge {
    color: @textMuted;
    background: @chipBg;
    border: 1px solid @border;
    border-radius: 9px;
    padding: 1px 10px;
}
QLabel#userBadge { color: @textFaint; padding: 2px 6px; }

/* ==================== 其它 ==================== */
QSplitter::handle { background: transparent; }
QSplitter::handle:horizontal { width: 6px; }
QSplitter::handle:vertical { height: 6px; }
QSplitter::handle:hover { background: @accentSoft; }

QCheckBox { spacing: 8px; color: @text; }
QCheckBox::indicator { width: 15px; height: 15px; }
QCheckBox:disabled { color: @textFaint; }

QLabel#panelTitle { font-size: 15px; font-weight: 600; color: @titleText; }
QLabel#panelHint { color: @textFaint; }
QLabel#stateLabel { font-weight: 600; }
QLabel#summaryLabel { color: @textMuted; }

/* ==================== 滚动条（细而现代，无箭头） ==================== */
QScrollBar:vertical {
    background: transparent;
    width: 10px;
    margin: 2px;
}
QScrollBar::handle:vertical {
    background: @scrollHandle;
    border-radius: 3px;
    min-height: 32px;
}
QScrollBar::handle:vertical:hover { background: @scrollHandleHover; }
QScrollBar:horizontal {
    background: transparent;
    height: 10px;
    margin: 2px;
}
QScrollBar::handle:horizontal {
    background: @scrollHandle;
    border-radius: 3px;
    min-width: 32px;
}
QScrollBar::handle:horizontal:hover { background: @scrollHandleHover; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; border: none; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
)QSS";

QString renderStyle(const Palette &palette)
{
    const QHash<QString, QString> values = paletteValues(palette);
    const QString source = QString::fromUtf8(kStyleTemplate);
    const QRegularExpression token(QStringLiteral("@([A-Za-z][A-Za-z0-9]*)"));

    QString out;
    out.reserve(source.size());
    int last = 0;
    QRegularExpressionMatchIterator it = token.globalMatch(source);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        out += source.mid(last, match.capturedStart() - last);
        // 找不到的 token 原样保留，方便一眼看出模板写错了
        out += values.value(match.captured(1), match.captured(0));
        last = match.capturedEnd();
    }
    out += source.mid(last);
    return out;
}

/// QPalette 兜底：QSS 覆盖不到的原生绘制（下拉箭头、勾选框、禁用文字）跟着主题走。
QPalette applicationPalette(const Palette &p)
{
    QPalette pal;
    pal.setColor(QPalette::Window, p.windowBg);
    pal.setColor(QPalette::WindowText, p.text);
    pal.setColor(QPalette::Base, p.fieldBg);
    pal.setColor(QPalette::AlternateBase, p.surfaceAlt);
    pal.setColor(QPalette::Text, p.text);
    pal.setColor(QPalette::Button, p.chipBg);
    pal.setColor(QPalette::ButtonText, p.text);
    pal.setColor(QPalette::BrightText, p.danger);
    pal.setColor(QPalette::Highlight, p.accent);
    pal.setColor(QPalette::HighlightedText, p.onAccent);
    pal.setColor(QPalette::ToolTipBase, p.surfaceAlt);
    pal.setColor(QPalette::ToolTipText, p.text);
    pal.setColor(QPalette::PlaceholderText, p.textFaint);
    pal.setColor(QPalette::Light, p.surface);
    pal.setColor(QPalette::Midlight, p.surfaceAlt);
    pal.setColor(QPalette::Mid, p.borderStrong);
    pal.setColor(QPalette::Dark, p.border);
    pal.setColor(QPalette::Shadow, p.windowBg);

    pal.setColor(QPalette::Disabled, QPalette::Text, p.textFaint);
    pal.setColor(QPalette::Disabled, QPalette::WindowText, p.textFaint);
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, p.textFaint);
    pal.setColor(QPalette::Disabled, QPalette::Highlight, p.disabledBg);
    pal.setColor(QPalette::Disabled, QPalette::HighlightedText, p.textFaint);
    return pal;
}

// ---------------------------------------------------------------------------
// 矢量图标
// ---------------------------------------------------------------------------

// 下面每个函数都在 24×24 的设计网格里作画，由 makeIcon() 统一缩放。
// 画笔（圆头圆角、宽度 2）与"无填充"已在 makeIcon() 里设好。

/// 机箱 + 加号
void drawAddDevice(QPainter &p, const QColor &)
{
    p.drawRoundedRect(QRectF(3.0, 4.5, 18.0, 15.0), 3.0, 3.0);
    p.drawLine(QPointF(12.0, 8.5), QPointF(12.0, 15.5));
    p.drawLine(QPointF(8.5, 12.0), QPointF(15.5, 12.0));
}

/// 铅笔：整体旋转 45°，笔杆 + 笔尖
void drawEditDevice(QPainter &p, const QColor &c)
{
    p.save();
    p.translate(12.5, 12.0);
    p.rotate(45.0);

    p.setBrush(c);
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(QRectF(-3.0, -9.5, 6.0, 11.0), 1.0, 1.0); // 笔杆

    const QPolygonF tip{{-3.0, 1.0}, {3.0, 1.0}, {0.0, 7.4}};    // 笔尖
    p.drawPolygon(tip);

    // 笔尖 / 笔杆 / 尾部橡皮之间的两道缝：不这么"擦"一下，
    // 整支笔就是一块斜着的实心色块，小尺寸下会看成分树叶。
    p.setCompositionMode(QPainter::CompositionMode_Clear);
    p.setPen(QPen(Qt::black, 1.1));
    p.drawLine(QPointF(-3.4, 1.2), QPointF(3.4, 1.2));
    p.setPen(QPen(Qt::black, 1.0));
    p.drawLine(QPointF(-3.2, -7.5), QPointF(3.2, -7.5));
    p.restore();
}

/// 垃圾桶
void drawRemoveDevice(QPainter &p, const QColor &)
{
    p.drawLine(QPointF(4.5, 6.6), QPointF(19.5, 6.6));                              // 桶盖
    p.drawPolyline(QPolygonF{{9.4, 6.6}, {9.9, 3.6}, {14.1, 3.6}, {14.6, 6.6}});    // 提手
    p.drawPolyline(QPolygonF{{6.4, 6.6}, {7.4, 20.4}, {16.6, 20.4}, {17.6, 6.6}});  // 桶身
    p.drawLine(QPointF(10.6, 9.8), QPointF(10.9, 17.4));
    p.drawLine(QPointF(13.4, 9.8), QPointF(13.1, 17.4));
}

/// 实心三角（播放）
void drawStartAcquisition(QPainter &p, const QColor &c)
{
    p.setBrush(c);
    p.setPen(Qt::NoPen);
    p.drawPolygon(QPolygonF{{7.2, 4.6}, {19.0, 12.0}, {7.2, 19.4}});
}

/// 实心圆角方块（停止）
void drawStopAcquisition(QPainter &p, const QColor &c)
{
    p.setBrush(c);
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(QRectF(6.4, 6.4, 11.2, 11.2), 2.2, 2.2);
}

/// 对勾
void drawAcknowledgeAll(QPainter &p, const QColor &c)
{
    p.setPen(QPen(c, 2.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawPolyline(QPolygonF{{4.8, 12.6}, {9.8, 17.6}, {19.2, 6.4}});
}

/// 铃铛轮廓（告警 / 消警两个图标共用）
void drawBell(QPainter &p)
{
    QPainterPath bell;
    bell.moveTo(5.6, 16.8);
    bell.lineTo(5.6, 11.3);
    bell.arcTo(QRectF(5.6, 4.9, 12.8, 12.8), 180.0, -180.0); // 圆顶：从左经顶到右
    bell.lineTo(18.4, 16.8);
    bell.closeSubpath();
    p.drawPath(bell);

    p.drawArc(QRectF(9.4, 15.9, 5.2, 5.2), 180 * 16, 180 * 16); // 铃口
}

/// 铃铛 + 斜杠（消警）
void drawClearAlarms(QPainter &p, const QColor &c)
{
    drawBell(p);

    // 斜杠不能和铃铛糊在一起：先按 Clear 擦出一条透明缝，再把斜线补上。
    // 图标是透明底，擦掉的地方会透出按钮背景，效果等同"剪纸"。
    const QLineF slash(QPointF(4.2, 20.2), QPointF(19.8, 3.8));
    p.save();
    p.setCompositionMode(QPainter::CompositionMode_Clear);
    p.setPen(QPen(Qt::black, 4.6, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(slash);
    p.restore();

    p.setPen(QPen(c, 1.8, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(slash);
}

/// 铃铛（告警）—— 与消警图标同一个铃铛轮廓，只是不加斜杠
void drawAlarmBell(QPainter &p, const QColor &)
{
    drawBell(p);
}

/// 喇叭（声音报警开 / 关共用）
void drawSpeaker(QPainter &p, const QColor &c)
{
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    p.drawRoundedRect(QRectF(3.0, 9.0, 4.2, 6.0), 0.9, 0.9);                     // 箱体
    p.drawPolygon(QPolygonF{{6.6, 9.0}, {10.8, 4.9}, {10.8, 19.1}, {6.6, 15.0}}); // 喇叭口
}

/// 喇叭 + 声波（声音报警开）
void drawSoundOn(QPainter &p, const QColor &c)
{
    drawSpeaker(p, c);

    // 两道同心圆弧，圆心取喇叭口 —— 折线在小尺寸下会看成一团噪点
    constexpr qreal mouthX = 10.8;
    constexpr qreal mouthY = 12.0;

    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(c, 1.8, Qt::SolidLine, Qt::RoundCap));
    p.drawArc(QRectF(mouthX - 3.4, mouthY - 4.2, 6.8, 8.4), -55 * 16, 110 * 16);   // 内圈
    p.drawArc(QRectF(mouthX - 6.4, mouthY - 7.6, 12.8, 15.2), -50 * 16, 100 * 16); // 外圈
}

/// 喇叭 + 叉（声音报警关）
void drawSoundOff(QPainter &p, const QColor &c)
{
    drawSpeaker(p, c);

    p.setPen(QPen(c, 2.0, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(13.4, 8.6), QPointF(20.0, 15.2));
    p.drawLine(QPointF(20.0, 8.6), QPointF(13.4, 15.2));
}

/// 文件夹（设备分组）
void drawDeviceGroup(QPainter &p, const QColor &)
{
    // 一个带标签的文件夹：标签比"两个错开的矩形"更容易一眼认出是分组
    p.drawPolyline(QPolygonF{{3.2, 19.4}, {3.2, 5.4}, {9.4, 5.4}, {11.6, 8.2}, {20.8, 8.2},
                              {20.8, 19.4}, {3.2, 19.4}});
}

/// 托盘 + 箭头（导入 / 导出共用）
/// @param down true 画向下箭头（导入），false 画向上箭头（导出）
void drawTrayWithArrow(QPainter &p, const QColor &c, bool down)
{
    p.drawPolyline(QPolygonF{{3.4, 14.2}, {3.4, 20.4}, {20.6, 20.4}, {20.6, 14.2}});

    p.setPen(QPen(c, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    if (down) {
        p.drawLine(QPointF(12.0, 3.6), QPointF(12.0, 14.6));
        p.drawPolyline(QPolygonF{{7.8, 10.4}, {12.0, 14.6}, {16.2, 10.4}});
    } else {
        p.drawLine(QPointF(12.0, 14.6), QPointF(12.0, 3.6));
        p.drawPolyline(QPolygonF{{7.8, 7.8}, {12.0, 3.6}, {16.2, 7.8}});
    }
}

/// 对比度圆（左半实心）—— 通用的"切换主题"符号
void drawToggleTheme(QPainter &p, const QColor &c)
{
    const QRectF outer(3.8, 3.8, 16.4, 16.4);
    p.drawEllipse(outer);

    QPainterPath half;
    half.moveTo(12.0, 3.8);
    half.arcTo(outer, 90.0, 180.0); // 自上而下走左半边
    half.closeSubpath();

    p.setBrush(c);
    p.setPen(Qt::NoPen);
    p.drawPath(half);
}

} // namespace

QString applicationStyleSheet(Theme theme)
{
    return renderStyle(paletteOf(theme));
}

void applyTheme(Theme theme)
{
    if (!qApp)
        return;

    const Palette palette = paletteOf(theme);
    qApp->setPalette(applicationPalette(palette));
    qApp->setStyleSheet(renderStyle(palette));
}

QIcon makeIcon(IconType type, const QColor &color, int size)
{
    // 以 24×24 为设计网格、按 4 倍超采样作画：小尺寸（16~20px）下也足够锐利，
    // 高 DPI 屏上由 QIcon 平滑缩放，不需要额外的多倍图。
    constexpr qreal kGridSize = 24.0;
    constexpr qreal kSupersample = 4.0;

    QPixmap pixmap(qRound(size * kSupersample), qRound(size * kSupersample));
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.scale(size * kSupersample / kGridSize, size * kSupersample / kGridSize);
    painter.setPen(QPen(color, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);

    switch (type) {
    case IconType::AddDevice:
        drawAddDevice(painter, color);
        break;
    case IconType::EditDevice:
        drawEditDevice(painter, color);
        break;
    case IconType::RemoveDevice:
        drawRemoveDevice(painter, color);
        break;
    case IconType::StartAcquisition:
        drawStartAcquisition(painter, color);
        break;
    case IconType::StopAcquisition:
        drawStopAcquisition(painter, color);
        break;
    case IconType::AcknowledgeAll:
        drawAcknowledgeAll(painter, color);
        break;
    case IconType::ClearAlarms:
        drawClearAlarms(painter, color);
        break;
    case IconType::ToggleTheme:
        drawToggleTheme(painter, color);
        break;
    case IconType::AlarmBell:
        drawAlarmBell(painter, color);
        break;
    case IconType::SoundOn:
        drawSoundOn(painter, color);
        break;
    case IconType::SoundOff:
        drawSoundOff(painter, color);
        break;
    case IconType::DeviceGroup:
        drawDeviceGroup(painter, color);
        break;
    case IconType::ExportConfig:
        drawTrayWithArrow(painter, color, false);
        break;
    case IconType::ImportConfig:
        drawTrayWithArrow(painter, color, true);
        break;
    }

    painter.end();
    return QIcon(pixmap);
}

QColor themeIconColor(Theme theme)
{
    // 工具栏图标画在工具栏底色上，浅色主题给深灰蓝、暗色主题给浅蓝灰
    return theme == Theme::Dark ? QColor(0xccdaea) : QColor(0x37485c);
}

ChartColors themeChartColors(Theme theme)
{
    const Palette p = paletteOf(theme);
    return ChartColors{p.chartBg, p.chartGrid, p.chartText};
}

Theme loadSavedTheme()
{
    QSettings settings(QStringLiteral("Y71jj123"),
                       QStringLiteral("qt-industrial-equipment-monitor"));
    const QString value = settings.value(QStringLiteral("ui/theme"),
                                         QStringLiteral("light")).toString();
    return value.compare(QLatin1String("dark"), Qt::CaseInsensitive) == 0 ? Theme::Dark
                                                                         : Theme::Light;
}

void saveTheme(Theme theme)
{
    QSettings settings(QStringLiteral("Y71jj123"),
                       QStringLiteral("qt-industrial-equipment-monitor"));
    settings.setValue(QStringLiteral("ui/theme"),
                      theme == Theme::Dark ? QStringLiteral("dark") : QStringLiteral("light"));
}

QString themeName(Theme theme)
{
    return theme == Theme::Dark ? QStringLiteral("暗色") : QStringLiteral("浅色");
}

QString stateColorName(bool online, bool fault)
{
    if (!online)
        return QStringLiteral("#95a3b2");
    return fault ? QStringLiteral("#e8912d") : QStringLiteral("#2fb46b");
}
