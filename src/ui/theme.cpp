#include "ui/theme.h"

QString applicationStyleSheet()
{
    static const char *kStyle = R"QSS(
* { font-family: "Microsoft YaHei UI", "Microsoft YaHei", "Segoe UI", sans-serif; }

QWidget { font-size: 13px; color: #24313f; }
QMainWindow, QDialog { background: #eef2f7; }

QToolBar {
    background: #ffffff;
    border: none;
    border-bottom: 1px solid #dde5ee;
    padding: 6px 8px;
    spacing: 4px;
}
QToolBar QToolButton {
    padding: 6px 14px;
    border: 1px solid #d6e0ea;
    border-radius: 4px;
    background: #f7fafd;
    color: #24313f;
}
QToolBar QToolButton:hover { background: #e8f1fb; border-color: #b9d3ee; }
QToolBar QToolButton:pressed { background: #d8e8f8; }
QToolBar::separator { width: 1px; background: #e2e9f1; margin: 4px 8px; }

QTabWidget::pane {
    border: 1px solid #dde5ee;
    border-radius: 6px;
    background: #ffffff;
    top: -1px;
}
QTabBar::tab {
    padding: 7px 18px;
    margin-right: 3px;
    background: #e9eff6;
    border: 1px solid #dde5ee;
    border-bottom: none;
    border-top-left-radius: 6px;
    border-top-right-radius: 6px;
    color: #5a6b7d;
}
QTabBar::tab:selected { background: #ffffff; color: #1b6fd0; font-weight: 600; }
QTabBar::tab:hover:!selected { background: #e0e9f3; }

QListWidget {
    background: #ffffff;
    border: 1px solid #dde5ee;
    border-radius: 6px;
    padding: 4px;
    outline: none;
}
QListWidget::item { padding: 8px 10px; border-radius: 4px; color: #24313f; }
QListWidget::item:hover { background: #eef5fd; }
QListWidget::item:selected { background: #2f7bd6; color: #ffffff; }

QTableWidget, QTableView {
    background: #ffffff;
    border: 1px solid #dde5ee;
    border-radius: 6px;
    gridline-color: #edf2f7;
    selection-background-color: #dbeafb;
    selection-color: #12324f;
    outline: none;
}
QHeaderView::section {
    background: #f4f8fc;
    color: #4c5f73;
    padding: 7px 8px;
    border: none;
    border-right: 1px solid #e6edf5;
    border-bottom: 1px solid #dde5ee;
    font-weight: 600;
}
QTableCornerButton::section { background: #f4f8fc; border: none; }

QPushButton {
    padding: 6px 16px;
    border: none;
    border-radius: 4px;
    background: #2f7bd6;
    color: #ffffff;
}
QPushButton:hover { background: #3b8ae4; }
QPushButton:pressed { background: #2a6cbd; }
QPushButton:disabled { background: #c9d6e3; color: #f2f6fa; }
QPushButton[variant="secondary"] {
    background: #eef3f9;
    color: #2b3a4a;
    border: 1px solid #d6e0ea;
}
QPushButton[variant="secondary"]:hover { background: #e2ecf7; }
QPushButton[variant="danger"] { background: #d94a4a; }
QPushButton[variant="danger"]:hover { background: #e25c5c; }

QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox, QDateTimeEdit {
    padding: 5px 8px;
    border: 1px solid #cfdae6;
    border-radius: 4px;
    background: #ffffff;
    color: #24313f;
    min-height: 18px;
}
QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus,
QComboBox:focus, QDateTimeEdit:focus { border-color: #2f7bd6; }
QComboBox::drop-down { border: none; width: 20px; }
QComboBox QAbstractItemView {
    background: #ffffff;
    border: 1px solid #cfdae6;
    selection-background-color: #dbeafb;
    selection-color: #12324f;
}

QGroupBox {
    border: 1px solid #dde5ee;
    border-radius: 6px;
    margin-top: 14px;
    background: #ffffff;
    padding-top: 6px;
}
QGroupBox::title {
    subcontrol-origin: margin;
    left: 12px;
    padding: 0 6px;
    color: #1b6fd0;
    font-weight: 600;
}

QPlainTextEdit#logView {
    background: #1e2733;
    color: #c9d7e6;
    border: none;
    border-radius: 6px;
    font-family: Consolas, "Courier New", monospace;
    font-size: 12px;
    padding: 6px;
}

QStatusBar { background: #ffffff; border-top: 1px solid #dde5ee; color: #4c5f73; }
QStatusBar::item { border: none; }

QSplitter::handle { background: #e6edf5; }
QSplitter::handle:horizontal { width: 3px; }
QSplitter::handle:vertical { height: 3px; }

QCheckBox { spacing: 6px; }

QLabel#panelTitle { font-size: 15px; font-weight: 600; color: #1b3a5c; }
QLabel#stateLabel { font-weight: 600; }
QLabel#summaryLabel { color: #4c5f73; }
)QSS";

    return QString::fromUtf8(kStyle);
}

QString stateColorName(bool online, bool fault)
{
    if (!online)
        return QStringLiteral("#95a3b2");
    return fault ? QStringLiteral("#e8912d") : QStringLiteral("#2fb46b");
}
