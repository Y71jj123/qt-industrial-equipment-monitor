#include <QApplication>

#include "core/alarmengine.h"
#include "core/devicemanager.h"
#include "storage/datastorage.h"
#include "ui/mainwindow.h"
#include "utils/logger.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    QApplication::setApplicationName(QStringLiteral(APP_NAME));
    QApplication::setApplicationVersion(QStringLiteral(APP_VERSION));
    QApplication::setOrganizationName(QStringLiteral("Y71jj123"));

    Log::instance().init(QStringLiteral("logs/app.log"));
    Log::info(QStringLiteral("应用启动，版本 %1").arg(QStringLiteral(APP_VERSION)));

    // 数据层：本地历史数据（失败不致命，仅影响历史查询功能）
    DataStorage storage;
    if (!storage.open()) {
        Log::warn(QStringLiteral("本地数据库打开失败，历史数据功能将不可用"));
    }

    // 核心层
    DeviceManager deviceManager;
    AlarmEngine alarmEngine;

    // 界面层
    MainWindow window(&deviceManager, &alarmEngine, &storage);
    window.show();

    const int code = app.exec();

    Log::info(QStringLiteral("应用退出，返回码 %1").arg(code));
    return code;
}
