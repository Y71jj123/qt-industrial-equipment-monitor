#include "virtualdevicewindow.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("QtDeviceSimulator"));
    QApplication::setApplicationVersion(QStringLiteral("1.0.0"));
    QApplication::setOrganizationName(QStringLiteral("Y71jj123"));

    // 命令行参数存在的意义：让这个工具在**无人值守**环境里也能用 ——
    // 自动化验证（脚本抓包断言报文）与截图夹具都需要"起进程即开始监听"，
    // 而不是靠人去点按钮。
    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Modbus TCP 虚拟设备模拟器 —— 供监控端联调与演示"));
    parser.addHelpOption();
    parser.addVersionOption();

    const QCommandLineOption portOption(
        {QStringLiteral("p"), QStringLiteral("port")},
        QStringLiteral("监听端口（默认 1502）"),
        QStringLiteral("port"),
        QStringLiteral("1502"));
    const QCommandLineOption autoStartOption(
        QStringLiteral("auto-start"),
        QStringLiteral("启动后立即开始监听（便于自动化验证与截图）"));

    parser.addOption(portOption);
    parser.addOption(autoStartOption);
    parser.process(app);

    const quint16 port = static_cast<quint16>(parser.value(portOption).toUShort());

    VirtualDeviceWindow window(parser.isSet(autoStartOption) ? port : quint16(0));
    window.show();

    return QApplication::exec();
}
