#pragma once

#include "comm/protocolplugin.h"

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

class DeviceConnection;
class QPluginLoader;

/// 协议注册表：协议 id → 插件。
///
/// 这是"协议插件化"的落点。以前 `AcquisitionScheduler::createConnection()` 里是一个
/// 按枚举分支的 switch，界面上还有一堆 `protocol == Mqtt` 的硬判断 —— 新增一种协议
/// 要同时改请求层、界面、存储、配置四处。现在这些地方一律改成**问注册表**：
/// 注册表里有什么协议，界面就列什么、调度器就能建什么。
class ProtocolRegistry
{
public:
    static ProtocolRegistry &instance();

    /// 注册一个插件。
    /// @param takeOwnership 由注册表负责生命周期（内置协议传 true）；
    ///        外部插件的实例归 QPluginLoader 所有，传 false。
    ///
    /// id 重复会被**拒绝**而不是覆盖：静默覆盖会让排查时"到底跑的是哪个实现"
    /// 变得不可查，而这种问题在现场极难定位。
    bool registerPlugin(IProtocolPlugin *plugin, bool takeOwnership = true);

    /// 扫描目录下的动态库，加载其中实现了 IProtocolPlugin 的插件，返回成功数量。
    /// 目录不存在返回 0（不是错误：没装插件是很正常的状态）。
    int loadPlugins(const QString &directory);

    /// 扫描标准位置：可执行文件旁的 `plugins/protocols`、上一级的同名目录、
    /// 一级环境变量 `MONITOR_PROTOCOL_PATH` 指定的目录。返回加载成功总数。
    int loadPluginsFromStandardLocations();

    /// 已注册的协议 id（含外部插件），按 id 排序。
    QStringList ids() const;

    /// 全部插件，按显示名排序 —— 界面下拉框直接照着填。
    QList<IProtocolPlugin *> plugins() const;

    IProtocolPlugin *plugin(const QString &id) const;

    /// 按 id 建连接。id 未注册时返回 nullptr，并**把已注册的 id 一起打进日志** ——
    /// 否则排查时只看到一句"失败了"，还得去翻代码找有哪些可用协议。
    DeviceConnection *create(const QString &id) const;

    ProtocolTraits traits(const QString &id) const;

    /// 协议显示名；未注册的 id 原样返回（宁可显示一个陌生的 id，
    /// 也不要显示空白让人以为设备没配协议）。
    QString displayName(const QString &id) const;

    /// 协议默认端口；未注册时返回 0，调用方据此跳过"自动填端口"。
    quint16 defaultPort(const QString &id) const;

    /// 本项目的默认协议（新建设备的初始选择）。
    static QString defaultProtocolId();

    /// 老版本的数据库 / 配置文件里协议存的是整型枚举，这里做一次性映射。
    /// 映射表是**冻结的**：新协议一律用字符串 id，不再往这张表里加东西。
    static QString idFromLegacyInt(int legacyProtocol);

    /// 反查：id → 老的整型编号。非内置协议返回 -1。
    /// 写库时会顺手留一份，万一需要降级回旧版本还能认出来。
    static int legacyIntFromId(const QString &id);

    /// 可执行文件旁的插件目录（`<exe>/plugins/protocols`）。
    static QString pluginDirectory();

private:
    ProtocolRegistry() = default;

    QHash<QString, IProtocolPlugin *> m_plugins;
    QList<IProtocolPlugin *> m_owned;   ///< 内置协议，随进程存活，不做显式释放
    QList<QPluginLoader *> m_loaders;   ///< 保活：外部插件的实例归各自的 loader 所有
    QStringList m_externalIds;          ///< 来自动态库的协议 id（供日志区分来源）
};

/// 注册编译进主程序的三个内置协议（`mock` / `modbus_tcp` / `mqtt`）。
///
/// 必须在第一次创建连接之前调用一次。放在 main.cpp 里显式调用而不是靠
/// "静态对象自动注册"：静态库里的自注册对象很容易被链接器整个丢掉，
/// 那种失败是**运行时才发现**的（协议莫名其妙不存在），显式调用最省心。
void registerBuiltinProtocols();
