#include "comm/protocolregistry.h"

#include "comm/deviceconnection.h"
#include "utils/logger.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFileInfoList>
#include <QPluginLoader>

#include <algorithm>

namespace {

/// 老版本枚举编号 → 协议 id。
///
/// **这张表是冻结的**：它只负责把旧数据读起来，新协议一律用字符串 id，
/// 不要再往里加条目 —— 每加一条就意味着多一个"历史包袱协议"。
struct LegacyMapping
{
    int legacy;
    const char *id;
};

const LegacyMapping kLegacyMap[] = {
    {0, "mock"},
    {1, "modbus_tcp"},
    {2, "mqtt"},
};

} // namespace

ProtocolRegistry &ProtocolRegistry::instance()
{
    // 函数内静态：线程安全的懒初始化（C++11 起保证），进程内唯一。
    // 不做显式析构 —— 卸载顺序不可控，而插件对象本来就要活到进程结束。
    static ProtocolRegistry registry;
    return registry;
}

QString ProtocolRegistry::defaultProtocolId()
{
    return QStringLiteral("mock");
}

QString ProtocolRegistry::idFromLegacyInt(int legacyProtocol)
{
    for (const LegacyMapping &mapping : kLegacyMap) {
        if (mapping.legacy == legacyProtocol)
            return QString::fromLatin1(mapping.id);
    }
    // 认不出来就落回默认协议：宁可让设备按模拟源跑起来，
    // 也不要留一个"协议未知、永远连不上"的设备在那儿让人猜。
    return defaultProtocolId();
}

int ProtocolRegistry::legacyIntFromId(const QString &id)
{
    for (const LegacyMapping &mapping : kLegacyMap) {
        if (id == QLatin1String(mapping.id))
            return mapping.legacy;
    }
    return -1;
}

bool ProtocolRegistry::registerPlugin(IProtocolPlugin *plugin, bool takeOwnership)
{
    if (!plugin)
        return false;

    const QString id = plugin->id().trimmed();
    if (id.isEmpty()) {
        Log::warn(QStringLiteral("忽略一个没有 id 的协议插件：%1").arg(plugin->displayName()));
        if (takeOwnership)
            delete plugin;
        return false;
    }

    if (m_plugins.contains(id)) {
        Log::warn(QStringLiteral("协议 id 重复，已忽略后来的那个：%1（%2）")
                      .arg(id, plugin->displayName()));
        if (takeOwnership)
            delete plugin;
        return false;
    }

    m_plugins.insert(id, plugin);
    if (takeOwnership)
        m_owned.append(plugin);
    return true;
}

int ProtocolRegistry::loadPlugins(const QString &directory)
{
    const QDir dir(directory);
    if (!dir.exists())
        return 0; // 没装插件是很正常的状态，不是错误

    int loaded = 0;
    const QFileInfoList entries = dir.entryInfoList(
        {QStringLiteral("*.dll"), QStringLiteral("*.so"), QStringLiteral("*.dylib")},
        QDir::Files);

    for (const QFileInfo &entry : entries) {
        auto *loader = new QPluginLoader(entry.absoluteFilePath());

        QObject *root = loader->instance();
        if (!root) {
            // 插件目录里混着别的动态库很常见（比如顺手拷进来的依赖），
            // 所以只记一条信息级日志，不当成错误打扰用户。
            Log::info(QStringLiteral("跳过 %1：不是本项目的协议插件（%2）")
                          .arg(entry.fileName(), loader->errorString()));
            delete loader;
            continue;
        }

        auto *plugin = qobject_cast<IProtocolPlugin *>(root);
        if (!plugin) {
            loader->unload();
            delete loader;
            continue;
        }

        const QString id = plugin->id();
        if (!registerPlugin(plugin, false)) {
            loader->unload(); // 注册失败就把它卸掉，别留一个不可用的实例在内存里
            delete loader;
            continue;
        }

        // loader 必须保活：插件的实例归它所有，loader 一析构对象就没了。
        m_loaders.append(loader);
        m_externalIds.append(id);
        Log::info(QStringLiteral("已加载协议插件 %1（%2），来自 %3")
                      .arg(id, plugin->displayName(), entry.fileName()));
        ++loaded;
    }

    return loaded;
}

int ProtocolRegistry::loadPluginsFromStandardLocations()
{
    QStringList candidates;

    const QString appDir = QCoreApplication::applicationDirPath();
    if (!appDir.isEmpty()) {
        // 构建树里 exe 和插件同层；安装包里 exe 在 bin/、插件在 ../plugins/。
        // 两个都试，免得为了"从哪跑"再写一套判断。
        candidates << appDir + QStringLiteral("/plugins/protocols");
        candidates << appDir + QStringLiteral("/../plugins/protocols");
    }

    const QByteArray extra = qgetenv("MONITOR_PROTOCOL_PATH");
    if (!extra.isEmpty())
        candidates << QString::fromLocal8Bit(extra);

    int loaded = 0;
    QStringList visited;
    for (const QString &candidate : candidates) {
        const QString absolute = QDir::cleanPath(QDir(candidate).absolutePath());
        if (visited.contains(absolute)) // 两个候选路径可能落到同一个目录
            continue;
        visited.append(absolute);
        loaded += loadPlugins(absolute);
    }
    return loaded;
}

QStringList ProtocolRegistry::ids() const
{
    QStringList result = m_plugins.keys();
    result.sort();
    return result;
}

QList<IProtocolPlugin *> ProtocolRegistry::plugins() const
{
    QList<IProtocolPlugin *> result = m_plugins.values();
    std::sort(result.begin(), result.end(),
              [](const IProtocolPlugin *left, const IProtocolPlugin *right) {
                  return left->displayName().localeAwareCompare(right->displayName()) < 0;
              });
    return result;
}

IProtocolPlugin *ProtocolRegistry::plugin(const QString &id) const
{
    return m_plugins.value(id, nullptr);
}

DeviceConnection *ProtocolRegistry::create(const QString &id) const
{
    // 空 id 当默认协议处理：老数据、手写的配置文件都可能漏掉这个字段，
    // 落回默认总比"设备永远起不来"要好。
    const QString wanted = id.trimmed().isEmpty() ? defaultProtocolId() : id;

    IProtocolPlugin *found = m_plugins.value(wanted, nullptr);
    if (!found) {
        // 把可用协议一起打出来 —— 否则排查时只知道"失败了"，
        // 还得回头翻代码才知道到底有哪些协议可用。
        Log::error(QStringLiteral("未知协议 %1，当前可用：%2")
                       .arg(wanted, ids().join(QStringLiteral(", "))));
        return nullptr;
    }

    DeviceConnection *connection = found->create();
    if (!connection)
        Log::error(QStringLiteral("协议 %1 创建连接失败").arg(wanted));
    return connection;
}

ProtocolTraits ProtocolRegistry::traits(const QString &id) const
{
    if (IProtocolPlugin *found = m_plugins.value(id, nullptr))
        return found->traits();
    return ProtocolTraits();
}

QString ProtocolRegistry::displayName(const QString &id) const
{
    if (IProtocolPlugin *found = m_plugins.value(id, nullptr))
        return found->displayName();
    return id;
}

quint16 ProtocolRegistry::defaultPort(const QString &id) const
{
    if (IProtocolPlugin *found = m_plugins.value(id, nullptr))
        return found->defaultPort();
    return 0;
}

QString ProtocolRegistry::pluginDirectory()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    if (appDir.isEmpty())
        return QString();
    return appDir + QStringLiteral("/plugins/protocols");
}
