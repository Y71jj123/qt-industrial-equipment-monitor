#pragma once

#include <QString>

/// 崩溃转储。
///
/// 现场是无人值守的：程序崩了如果没留下现场，事后基本没法查 ——
/// 而且很多时候连"崩在哪一步"都说不清。这里把一个未捕获异常落成转储文件，
/// 事后可以直接用调试器打开看调用栈。
///
/// 平台差异：
///   - **Windows**：`SetUnhandledExceptionFilter` + `MiniDumpWriteDump`，产出 `.dmp`；
///   - **其他平台**：装 SIGSEGV / SIGABRT / SIGFPE / SIGILL 处理器，
///     产出带调用栈的 `.txt`（有 `execinfo.h` 时采集栈帧）。
namespace CrashHandler {

/// 安装崩溃处理器（可重复调用，只生效一次）。
/// @param dumpDir 转储目录；为空则不做任何事
void install(const QString &dumpDir);

/// 已配置的转储目录；未安装时返回空串。界面/日志可以把它显示给运维。
QString dumpDirectory();

} // namespace CrashHandler
