#pragma once

#include <QColor>
#include <QIcon>
#include <QString>

/// 应用主题。新增主题只要在 theme.cpp 里补一套调色板即可。
enum class Theme {
    Light, ///< 浅色工业风（默认）
    Dark,  ///< 暗色机房风
};

/// 工具栏等处的矢量图标类型。
///
/// 图标一律用 QPainter 现画、不引入图片资源：一是要跟着主题换色，
/// 二是免去维护 .qrc 与多套倍率图。
enum class IconType {
    AddDevice,        ///< 添加设备
    EditDevice,       ///< 编辑设备
    RemoveDevice,     ///< 移除设备
    StartAcquisition, ///< 开始采集
    StopAcquisition,  ///< 停止采集
    AcknowledgeAll,   ///< 确认全部告警
    ClearAlarms,      ///< 消警
    ToggleTheme,      ///< 切换主题
    AlarmBell,        ///< 铃铛（托盘图标 / 告警）
    SoundOn,          ///< 声音报警：开
    SoundOff,         ///< 声音报警：关
    DeviceGroup,      ///< 设备分组（文件夹）
    ExportConfig,     ///< 导出配置（箭头出托盘）
    ImportConfig,     ///< 导入配置（箭头进托盘）
};

/// 图表（Qt Charts）配色。
///
/// 图表背景 / 网格线 / 坐标轴不归 QSS 管，只能代码里设，所以单独给一份。
struct ChartColors {
    QColor background;
    QColor grid;
    QColor text;
};

/// 按 24×24 设计网格绘制一个矢量图标。
/// @param color 线条与填充色（通常取 themeIconColor()）
/// @param size  逻辑边长（像素）
QIcon makeIcon(IconType type, const QColor &color, int size = 18);

/// 当前主题下图标的前景色 —— 保证在两套背景上都够对比度。
QColor themeIconColor(Theme theme);

/// 当前主题下趋势图 / 历史图的配色。
ChartColors themeChartColors(Theme theme);

/// 应用整体样式表（按主题生成）。
QString applicationStyleSheet(Theme theme = Theme::Light);

/// 把主题应用到 qApp：同时设置 QPalette 与样式表。
///
/// QSS 管不到的原生绘制（下拉箭头、勾选框、禁用态文字）靠 QPalette 兜底，
/// 两者必须一起换，否则暗色下会出现"黑底黑箭头"。
void applyTheme(Theme theme);

/// 主题的读写：记住用户选择，下次启动沿用。
Theme loadSavedTheme();
void saveTheme(Theme theme);

/// 主题的中文名（状态栏展示用）。
QString themeName(Theme theme);

/// 状态灯颜色（在线绿 / 故障橙 / 离线灰）。
QString stateColorName(bool online, bool fault);
