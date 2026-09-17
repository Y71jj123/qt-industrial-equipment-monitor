#pragma once

#include <QString>

/// 应用整体样式（工业风浅色主题）。
///
/// 统一在这里维护配色与控件样式，避免散落在各个面板里。
/// 在 main.cpp 里 `qApp->setStyleSheet(applicationStyleSheet())` 全局生效。
QString applicationStyleSheet();

/// 状态灯颜色（在线绿 / 故障橙 / 离线灰）。
QString stateColorName(bool online, bool fault);
