#include <QApplication>
#include <QDir>
#include <QSettings>
#include <QStandardPaths>

#include "comm/protocolregistry.h"
#include "core/alarmengine.h"
#include "core/devicemanager.h"
#include "storage/datastorage.h"
#include "ui/logindialog.h"
#include "ui/mainwindow.h"
#include "ui/theme.h"
#include "utils/crashhandler.h"
#include "utils/logger.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    QApplication::setApplicationName(QStringLiteral(APP_NAME));
    QApplication::setApplicationVersion(QStringLiteral(APP_VERSION));
    QApplication::setOrganizationName(QStringLiteral("Y71jj123"));

    // 全局样式：沿用用户上次选择的主题（样式表与调色板集中维护在 ui/theme.cpp）
    applyTheme(loadSavedTheme());

    // 日志与数据库都放在系统标准的应用数据目录，不跟着工作目录跑
    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    Log::instance().init(QDir(dataDir).filePath(QStringLiteral("logs/app.log")));
    Log::info(QStringLiteral("应用启动，版本 %1").arg(QStringLiteral(APP_VERSION)));

    // 崩溃转储：现场无人值守，一次崩溃没留下现场就基本没法查。
    // 放在日志之后安装，这样崩溃前最后几条日志一定已经在盘上了。
    CrashHandler::install(QDir(dataDir).filePath(QStringLiteral("dumps")));
    Log::info(QStringLiteral("崩溃转储目录：%1").arg(CrashHandler::dumpDirectory()));

    // 协议：先注册编译进主程序的内置协议（mock / modbus_tcp / mqtt / modbus_rtu），
    // 再扫描外部插件目录。
    // 必须赶在任何设备开始采集之前完成 —— 采集调度器建连接时只会问注册表，
    // 这一步漏了，设备会因为"协议不可用"起不来。
    registerBuiltinProtocols();
    const int externalPlugins = ProtocolRegistry::instance().loadPluginsFromStandardLocations();
    Log::info(QStringLiteral("协议就绪：%1 个（其中外部插件 %2 个）｜ %3")
                  .arg(ProtocolRegistry::instance().ids().size())
                  .arg(externalPlugins)
                  .arg(ProtocolRegistry::instance().ids().join(QStringLiteral(", "))));

    // 登录（演示用本地账号，生产环境应换成服务端鉴权）
    LoginDialog login;
    if (login.exec() != QDialog::Accepted)
        return 0;

    Log::info(QStringLiteral("用户 %1（%2）已登录")
                  .arg(login.userName(), userRoleName(login.role())));

    // 数据保留策略：运行时配置（存 QSettings），默认值保留 30 天、按 1 小时桶降采样。
    // 不依赖界面：改配置无需重新编译，纯 SQLite + 定时器后台跑，不影响采集与界面。
    DataStorage storage;
    {
        QSettings settings(QStringLiteral("Y71jj123"), QStringLiteral(APP_NAME));
        storage.setRetentionPolicy(
            settings.value(QStringLiteral("storage/retentionDays"), 30).toInt(),
            settings.value(QStringLiteral("storage/downsampleEnabled"), true).toBool(),
            settings.value(QStringLiteral("storage/downsampleBucketHours"), 1).toInt());
    }
    if (!storage.open()) {
        Log::warn(QStringLiteral("本地数据库打开失败，历史数据功能将不可用"));
    }

    // 采样是攒批落库的，靠 200ms 定时器驱动；事件循环一停定时器就不再触发，
    // 所以退出前必须手动刷一次，否则最后一批数据会丢在内存里。
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &storage, &DataStorage::flush);

    // 核心层
    DeviceManager deviceManager;
    AlarmEngine alarmEngine;

    // 界面层
    MainWindow window(&deviceManager, &alarmEngine, &storage,
                      login.userName(), login.role());
    window.show();

    const int code = app.exec();

    Log::info(QStringLiteral("应用退出，返回码 %1").arg(code));
    return code;
}
