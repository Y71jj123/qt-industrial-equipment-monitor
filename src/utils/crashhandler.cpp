#include "utils/crashhandler.h"

#include <QDir>
#include <QString>

#ifdef Q_OS_WIN
#  include <windows.h>
#  include <dbghelp.h>
#else
#  include <csignal>
#  include <cstdio>
#  include <cstring>
#  if defined(__has_include)
#    if __has_include(<execinfo.h>)
#      define MONITOR_HAVE_EXECINFO 1
#      include <execinfo.h>
#    endif
#  endif
#endif

namespace {

/// 转储目录的路径缓冲长度。512 个宽字符足够放下 Windows 的应用数据目录。
constexpr int kMaxDirChars = 512;

#ifdef Q_OS_WIN

wchar_t g_dumpDir[kMaxDirChars] = {0};
bool g_installed = false;

/// 未捕获异常的过滤器：把现场写成 minidump。
///
/// ⚠️ **这里一行 Qt 都不能调**。崩溃时堆和栈的状态都不可信，
/// 任何分配内存的操作都可能二次崩溃，结果是连转储都留不下来。
/// 所以只允许用 Win32 API，文件名也只用栈上的宽字符缓冲拼。
LONG WINAPI unhandledExceptionFilter(EXCEPTION_POINTERS *exceptionInfo)
{
    if (g_dumpDir[0] != L'\0') {
        SYSTEMTIME now;
        GetLocalTime(&now);

        wchar_t path[kMaxDirChars + 64];
        // 用 _snwprintf 而不是 wsprintf/swprintf：后两者在 MSVC 与 MinGW 上的
        // 重载与原型不一致，而"某编译器编不过"是崩溃处理器最不该有的问题。
        _snwprintf(path, kMaxDirChars + 64,
                   L"%ls\\crash_%04d%02d%02d_%02d%02d%02d.dmp",
                   g_dumpDir,
                   int(now.wYear), int(now.wMonth), int(now.wDay),
                   int(now.wHour), int(now.wMinute), int(now.wSecond));

        HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION info;
            info.ThreadId = GetCurrentThreadId();
            info.ExceptionPointers = exceptionInfo;
            info.ClientPointers = FALSE;

            MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                              MiniDumpNormal, &info, nullptr, nullptr);
            CloseHandle(file);
        }
    }

    // 交回系统默认处理，**不要**吞掉异常：静默退出会让上层完全看不出发生过崩溃，
    // 退出码也会变成"正常"，那比崩溃本身更难查。
    return EXCEPTION_CONTINUE_SEARCH;
}

#else // !Q_OS_WIN

char g_dumpDir[kMaxDirChars] = {0};
bool g_installed = false;

void writeBacktrace(const char *reason)
{
    if (g_dumpDir[0] == '\0')
        return;

    char path[kMaxDirChars + 64];
    std::snprintf(path, sizeof(path), "%s/crash_%s.txt", g_dumpDir, reason);

    FILE *file = std::fopen(path, "w");
    if (!file)
        return;

    std::fprintf(file, "崩溃信号: %s\n", reason);
#ifdef MONITOR_HAVE_EXECINFO
    void *frames[64];
    const int count = backtrace(frames, 64);
    backtrace_symbols_fd(frames, count, fileno(file));
#else
    std::fprintf(file, "（本平台无 execinfo.h，未采集调用栈）\n");
#endif
    std::fclose(file);
}

void signalHandler(int signalNumber)
{
    const char *reason = "UNKNOWN";
    switch (signalNumber) {
    case SIGSEGV:
        reason = "SIGSEGV";
        break;
    case SIGABRT:
        reason = "SIGABRT";
        break;
    case SIGFPE:
        reason = "SIGFPE";
        break;
    case SIGILL:
        reason = "SIGILL";
        break;
    default:
        break;
    }

    writeBacktrace(reason);

    // 记录完再恢复默认行为并重新触发：这样退出码仍然是"被信号杀死"，
    // 父进程/脚本能正确判断这是崩溃而不是正常退出。
    std::signal(signalNumber, SIG_DFL);
    std::raise(signalNumber);
}

#endif // Q_OS_WIN

} // namespace

void CrashHandler::install(const QString &dumpDir)
{
    if (dumpDir.isEmpty())
        return;

    QDir().mkpath(dumpDir);

#ifdef Q_OS_WIN
    const QString native = QDir::toNativeSeparators(dumpDir);
    const int length = int(qMin<qsizetype>(native.size(), kMaxDirChars - 1));
    native.left(length).toWCharArray(g_dumpDir);
    g_dumpDir[length] = L'\0';

    if (!g_installed) {
        SetUnhandledExceptionFilter(unhandledExceptionFilter);
        g_installed = true;
    }
#else
    const QByteArray narrow = dumpDir.toUtf8();
    const int length = int(qMin<qsizetype>(narrow.size(), kMaxDirChars - 1));
    std::memcpy(g_dumpDir, narrow.constData(), size_t(length));
    g_dumpDir[length] = '\0';

    if (!g_installed) {
        std::signal(SIGSEGV, signalHandler);
        std::signal(SIGABRT, signalHandler);
        std::signal(SIGFPE, signalHandler);
        std::signal(SIGILL, signalHandler);
        g_installed = true;
    }
#endif
}

QString CrashHandler::dumpDirectory()
{
#ifdef Q_OS_WIN
    return QString::fromWCharArray(g_dumpDir);
#else
    return QString::fromUtf8(g_dumpDir);
#endif
}
