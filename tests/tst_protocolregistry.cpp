#include "comm/protocolregistry.h"

#include "comm/deviceconnection.h"
#include "core/devicemanager.h"

#include <QDir>
#include <QFileInfo>
#include <QSignalSpy>
#include <QtTest>

namespace {

/// 一个最小的协议插件实现：用来验证注册表的规则（重复 id、空 id 等），
/// 不涉及任何真实通信。**这正好也说明插件接口有多轻** ——
/// 十行就能造一个协议出来。
class StubPlugin : public IProtocolPlugin
{
public:
    explicit StubPlugin(QString id, QString name = QString())
        : m_id(std::move(id))
        , m_name(std::move(name))
    {
    }

    QString id() const override { return m_id; }
    QString displayName() const override { return m_name.isEmpty() ? m_id : m_name; }
    quint16 defaultPort() const override { return 12345; }
    ProtocolTraits traits() const override
    {
        ProtocolTraits traits;
        traits.usesEndpoint = true;
        return traits;
    }
    DeviceConnection *create() const override { return nullptr; }

private:
    QString m_id;
    QString m_name;
};

} // namespace

/// 协议注册表 + 协议插件化的基本规则。
///
/// 这块逻辑是"新增协议零重编译核心"的地基：只要注册表的行为稳定，
/// 上层（采集调度、界面、存储）就不需要认识任何具体协议。
class TestProtocolRegistry : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void registersBuiltins();
    void rejectsDuplicateId();
    void rejectsEmptyId();
    void legacyIntMappingIsStable();
    void unknownProtocolIsReported();
    void emptyIdFallsBackToDefault();
    void traitsComeFromPlugin();
    void loadsExternalPluginFromDirectory();

private:
    /// 构建树里协议插件的输出目录（测试进程在 build/tests/ 下）。
    static QString pluginDirectory();
};

void TestProtocolRegistry::initTestCase()
{
    registerBuiltinProtocols();
}

QString TestProtocolRegistry::pluginDirectory()
{
    return QDir::cleanPath(QCoreApplication::applicationDirPath()
                           + QStringLiteral("/../src/plugins/protocols"));
}

void TestProtocolRegistry::registersBuiltins()
{
    ProtocolRegistry &registry = ProtocolRegistry::instance();

    QVERIFY(registry.ids().contains(QStringLiteral("mock")));
    QVERIFY(registry.ids().contains(QStringLiteral("modbus_tcp")));
    QVERIFY(registry.ids().contains(QStringLiteral("mqtt")));

    // 内置协议必须**始终**在：它们是程序能跑起来的底线，
    // 不依赖任何外部文件是否存在。
    QVERIFY(registry.plugin(QStringLiteral("mock")) != nullptr);
    QVERIFY(registry.plugin(QStringLiteral("modbus_tcp")) != nullptr);
    QVERIFY(registry.plugin(QStringLiteral("mqtt")) != nullptr);
}

void TestProtocolRegistry::rejectsDuplicateId()
{
    ProtocolRegistry &registry = ProtocolRegistry::instance();

    // mock 已经被内置插件占了：后来的必须被拒绝，而不是悄悄覆盖 ——
    // 静默覆盖会让"到底跑的是哪个实现"在排查时变成无解问题。
    QVERIFY(!registry.registerPlugin(new StubPlugin(QStringLiteral("mock"))));

    // 没被占用的 id 则应该注册成功
    QVERIFY(registry.registerPlugin(new StubPlugin(QStringLiteral("tst-stub"))));
    QCOMPARE(registry.ids().count(QStringLiteral("tst-stub")), 1);
}

void TestProtocolRegistry::rejectsEmptyId()
{
    ProtocolRegistry &registry = ProtocolRegistry::instance();
    QVERIFY(!registry.registerPlugin(new StubPlugin(QString())));
}

void TestProtocolRegistry::legacyIntMappingIsStable()
{
    // 老版本的数据库 / 导出配置里协议是整型枚举。这张映射表**是冻结的** ——
    // 它只负责把旧数据读起来；一旦改动，老库里的设备就会变成另一种协议。
    QCOMPARE(ProtocolRegistry::idFromLegacyInt(0), QStringLiteral("mock"));
    QCOMPARE(ProtocolRegistry::idFromLegacyInt(1), QStringLiteral("modbus_tcp"));
    QCOMPARE(ProtocolRegistry::idFromLegacyInt(2), QStringLiteral("mqtt"));

    // 认不出来的一律落回默认协议：宁可让设备按模拟源跑起来，
    // 也不要留一台"协议未知、永远连不上"的设备让人猜。
    QCOMPARE(ProtocolRegistry::idFromLegacyInt(99), ProtocolRegistry::defaultProtocolId());

    // 反查：内置协议有编号（写库时顺手留一份，便于降级回旧版本），
    // 外部插件没有（旧版本本来就不认识它）。
    QCOMPARE(ProtocolRegistry::legacyIntFromId(QStringLiteral("modbus_tcp")), 1);
    QCOMPARE(ProtocolRegistry::legacyIntFromId(QStringLiteral("tst-stub")), -1);
}

void TestProtocolRegistry::unknownProtocolIsReported()
{
    ProtocolRegistry &registry = ProtocolRegistry::instance();

    // 未知协议必须**明确失败**：如果这里改成"兜底返回一个默认连接"，
    // 那么配置写错的表现就变成"数据看着像对的"，比连不上危险得多。
    QVERIFY(registry.create(QStringLiteral("definitely-not-a-protocol")) == nullptr);
    QVERIFY(registry.plugin(QStringLiteral("definitely-not-a-protocol")) == nullptr);
    QCOMPARE(registry.defaultPort(QStringLiteral("definitely-not-a-protocol")), quint16(0));
    // 显示名对未注册 id 原样返回，避免界面出现空白
    QCOMPARE(registry.displayName(QStringLiteral("weird-id")), QStringLiteral("weird-id"));
}

void TestProtocolRegistry::emptyIdFallsBackToDefault()
{
    ProtocolRegistry &registry = ProtocolRegistry::instance();

    // 空 id 当默认协议：老数据、手写的配置文件都可能漏掉这个字段
    DeviceConnection *connection = registry.create(QString());
    QVERIFY(connection != nullptr);
    delete connection;
}

void TestProtocolRegistry::traitsComeFromPlugin()
{
    ProtocolRegistry &registry = ProtocolRegistry::instance();

    // 界面靠 traits 决定显示哪些字段 —— 这是"新增协议不用改界面"的关键。
    const ProtocolTraits modbus = registry.traits(QStringLiteral("modbus_tcp"));
    QVERIFY(modbus.usesSlaveId);
    QVERIFY(modbus.usesNetwork);
    QVERIFY(!modbus.usesCredentials);
    QVERIFY(!modbus.usesEndpoint);

    const ProtocolTraits mqtt = registry.traits(QStringLiteral("mqtt"));
    QVERIFY(mqtt.usesEndpoint);
    QVERIFY(mqtt.usesCredentials);
    QCOMPARE(mqtt.endpointLabel, QStringLiteral("订阅主题"));

    // 模拟设备是纯本地数据源：连地址端口都不该显示
    const ProtocolTraits mock = registry.traits(QStringLiteral("mock"));
    QVERIFY(!mock.usesNetwork);

    // 未注册的协议：给一份**保守的默认** traits —— 地址/端口照常显示
    // （多显示一个用不上的字段，好过把该填的字段藏起来让人没处填），
    // 其余可选字段一律不显示。界面因此既不会崩，也不会凭空多出几行空控件。
    const ProtocolTraits unknown = registry.traits(QStringLiteral("nope"));
    QVERIFY(unknown.usesNetwork);
    QVERIFY(!unknown.usesSlaveId);
    QVERIFY(!unknown.usesEndpoint);
    QVERIFY(!unknown.usesCredentials);
}

void TestProtocolRegistry::loadsExternalPluginFromDirectory()
{
    const QString directory = pluginDirectory();
    const bool exists = QFileInfo::exists(directory + QStringLiteral("/monitor_protocol_http_json.dll"))
                        || QFileInfo::exists(directory + QStringLiteral("/monitor_protocol_http_json.so"));
    if (!exists)
        QSKIP("协议插件未构建（或平台后缀不同），跳过外部插件加载测试");

    ProtocolRegistry &registry = ProtocolRegistry::instance();
    QVERIFY(registry.loadPlugins(directory) == 1);

    // 外部插件注册进来之后，和内置协议**完全平权**：
    // 界面列它、调度器建它，都不需要知道它来自动态库。
    IProtocolPlugin *plugin = registry.plugin(QStringLiteral("http_json"));
    QVERIFY(plugin != nullptr);
    QCOMPARE(plugin->displayName(), QStringLiteral("HTTP / JSON 数据源"));
    QCOMPARE(plugin->defaultPort(), quint16(8080));
    QCOMPARE(plugin->traits().endpointLabel, QStringLiteral("请求路径"));

    // 再加载一次：id 重复会被拒绝，不会加载进第二份
    QCOMPARE(registry.loadPlugins(directory), 0);
}

QTEST_GUILESS_MAIN(TestProtocolRegistry)
#include "tst_protocolregistry.moc"
