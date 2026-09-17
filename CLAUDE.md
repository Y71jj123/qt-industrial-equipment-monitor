# CLAUDE.md — 项目上下文与约定

> 本文件供 AI 编码助手（Claude Code 等）阅读，说明项目结构、构建方式与代码规范。
> 人类读者请看 [README.md](./README.md)。

## 项目是什么

基于 **Qt 6 / C++17** 的**工业设备远程监控管理平台**桌面客户端。分层架构：

| 层 | 目录 | 职责 |
| --- | --- | --- |
| 界面 | `src/ui/` | 主窗口 + 各功能面板，只做展示与转发 |
| 通信 | `src/comm/` | `DeviceConnection` 抽象 + Mock / Modbus TCP / MQTT 实现 |
| 核心 | `src/core/` | 设备台账、采集调度（含自动重连）、告警引擎 |
| 存储 | `src/storage/` | SQLite 四表：`samples` / `alarms` / `operations` / `devices` |
| 工具 | `src/utils/` | 日志 |

## 构建与验证（**每次改完必须自己编译验证**）

```bash
export PATH="E:/Qt/Tools/mingw1310_64/bin;E:/Qt/6.11.2/mingw_64/bin;E:/Qt/Tools/Ninja:$PATH"
cd <项目根>
E:/Qt/Tools/CMake_64/bin/cmake.exe --preset qt-mingw-debug
E:/Qt/Tools/CMake_64/bin/cmake.exe --build build-vscode --parallel 8
```

- 必须做到**零错误、零警告**（编译带 `-Wall -Wextra`）。
- 新增源文件要同时加进 `src/CMakeLists.txt` 的 `PROJECT_SOURCES` ——
  它会自动进入 `monitor_core` 静态库，主程序和单元测试都能用到。
- 若链接报 `Permission denied`，说明程序还开着：
  `MSYS_NO_PATHCONV=1 taskkill /IM "qt-industrial-equipment-monitor.exe" /F`
- **不要运行 GUI**（无人值守环境会卡住），编译通过即可；逻辑正确性用下面的单元测试守。

**单元测试**（改动了 `core/` `storage/` `utils/` 的逻辑就应该跑一遍）：

```bash
E:/Qt/Tools/CMake_64/bin/ctest.exe --test-dir build-vscode --output-on-failure
```

- 四个套件：`tst_alarmengine`（状态机 / 工单闭环 / MTTR）、`tst_datastorage`
  （告警 NULL 语义 / 统计口径 / 溢出队列与补传顺序）、`tst_configio`（配置往返）、
  `tst_protocolregistry`（注册表规则 / 老整型映射 / 外部插件加载）。
- 测试**链接 `monitor_core`**，也就是产品实际运行的那份代码；不要另外编译一份源码来测。
- 新增测试文件后要在 `tests/CMakeLists.txt` 里加一行 `monitor_add_test(<名字>)`。
- 测试一律用 `QTEST_GUILESS_MAIN`（QCoreApplication）—— CI 没有显示器，
  `QTEST_MAIN` 会去要平台插件然后失败。
- ⚠️ Windows 上 ctest 跑测试若报 `0xc0000135`（找不到 DLL），不是测试写错了：
  `tests/CMakeLists.txt` 已经用 `ENVIRONMENT_MODIFICATION` 把 Qt 的 bin 目录加进测试进程 PATH，
  改这块时别手拼 `PATH=`（Windows 的 `;` 会被 CMake 当列表分隔符切碎）。
- 手工看测试明细时：**QTest 的输出在有些终端里会被吞掉**，用
  `./tst_xxx.exe -o - -o out.txt,txt` 把结果同时写到文件再看。
- 从命令行直接跑 `build-vscode/tests/*.exe` 记得把 Qt 的 bin 加进 PATH（例如
  `export PATH="/e/Qt/6.11.2/mingw_64/bin:$PATH"`）—— 测试产物目录里没有 Qt 的 DLL。

## 代码规范

- 命名：类 `BigCamel`，函数 `smallCamel`，成员变量 `m_` 前缀。
- 注释、日志、界面文案**一律用中文**；注释解释"为什么这么做"，不复述"做了什么"。
- 界面层不写业务逻辑；跨层只通过信号槽通信。
- 每个数据点的唯一标识是 `deviceId + "/" + tagId`。

## ⚠️ 已知的坑（务必避开）

1. **Qt Charts 的类型在全局命名空间** —— 本机 Qt 6.11.2 没有 `QtCharts` 这个 namespace。
   写 `QtCharts::QChart` 会报 `invalid use of incomplete type`。
   必须裸用 `QChart` / `QLineSeries` / `QChartView` / `QDateTimeAxis` / `QValueAxis`，
   include 用 `<QtCharts/QChart>` 这种路径。
2. **开关量不做工程量换算** —— `ModbusConnection::readPoint()` 里对 `TagPoint::boolean`
   直接取原始 0/1，不乘 `scale`。
3. **Qt Charts 是可选依赖** —— 图表相关代码要用 `#ifdef HAVE_QT_CHARTS` 包裹；
   但被多个面板共用的成员（如 `m_rightTabs`）**不能**放进 ifdef 里。
4. **新增协议 = 写一个插件，核心代码一行都不用改**（详见下面「协议插件」一节）。
   `AcquisitionScheduler::createConnection()` 里**已经没有 switch 了** ——
   谁要是往那儿加分支，等于把刚拆掉的耦合又装回去。
5. **改数据库结构**要写 `CREATE TABLE IF NOT EXISTS`，并**同时补一次 `ensureColumn()`**
   （老库不会因为 CREATE TABLE IF NOT EXISTS 而多出新列）。
   现有补列：`devices.grp`、`devices.username`、`devices.password`、
   `alarms.disposition`、`alarms.handled_by`、`alarms.handled_at`、`alarms.note`。
   - 在 `devices` 表加字段的完整清单：
     `devicemanager.h`(DeviceInfo) → `datastorage.cpp`(建表 + ensureColumn + saveDevice + loadDevices)
     → `configio.cpp`(deviceToJson/deviceFromJson) → `devicedialog.cpp`(控件 + setDevice + device())。
   - 在 `alarms` 表加字段的完整清单：
     `alarmengine.h`(AlarmRecord) → `datastorage.cpp`(建表 + ensureColumn + insertAlarm + queryAlarms)
     → `ui/alarmpanel.cpp`(表格列) → `ui/reportpanel.cpp`(表格列 + Excel 导出两处)。
   - **写入时必须区分"有值"和"NULL"**：未处理的告警要把 `disposition/handled_at` 写成 NULL，
     不能写 `disposition` 的默认值 0（那是"已处理恢复"）—— 否则报表会把未处理告警算成已处理。
     判断"是否处理过"**永远以 `handled_at` 是否有效为准**，不要看 disposition 的值。

## 协议插件（P1-1）

架构一句话：**协议是数据，不是代码分支**。协议 id → 插件的映射放在 `ProtocolRegistry`，
上层（采集调度 / 设备对话框 / 存储 / 配置）一律"问注册表"，不认识任何具体协议。

- 接口在 `comm/protocolplugin.h`：`IProtocolPlugin`（纯虚，**不继承 QObject**）
  + `ProtocolTraits`（声明需要哪些配置字段）。
- 内置协议（`mock` / `modbus_tcp` / `mqtt`）在 `comm/builtinprotocols.cpp` 里
  以插件形式注册，与外部插件**完全平权**。注册用显式调用
  `registerBuiltinProtocols()`（`main.cpp` 启动时调用一次），
  **不要改成"C++ 静态对象自动注册"** —— 静态库里的自注册对象会被链接器整个丢掉，
  而这种失败只在运行期才暴露（协议莫名其妙不存在）。
- 外部插件：编成 `MODULE` 动态库，产物落到 exe 旁的 `plugins/protocols/`，
  启动时 `loadPluginsFromStandardLocations()` 扫描（还会试 `<exe>/../plugins/protocols`，
  兼容安装包布局）。样板看 `plugins/http_json/`。
- **`DeviceInfo::protocol` 已从枚举改成字符串 `protocolId`**。别再引入按协议名的
  `if/else`：需要区分协议行为时，加 `ProtocolTraits` 字段或加接口方法。
- 老数据兼容：`devices.protocol`（整型）列**保留并继续写**（内置协议写对应编号、
  其它写 -1），老库/老导出文件靠 `ProtocolRegistry::idFromLegacyInt()` 这张
  **冻结映射表**读起来。写库与导出 JSON 都会同时留一份老整型，便于降级回旧版本。
- 界面：设备对话框的协议下拉来自 `registry.plugins()`，
  字段可见性来自 `registry.traits(id)`，连"端点字段叫什么"都由
  `traits.endpointLabel` 决定（MQTT 叫"订阅主题"、HTTP 插件叫"请求路径"，
  落在同一个 `DeviceInfo::mqttTopic` 字段上）。**界面文件里不该出现任何协议名。**
- 协议不可用时（插件缺失 / id 写错）必须**明确失败**并打日志列出可用协议，
  **不许兜底成默认协议** —— 那会让"配置错了"表现成"数据看着像对的"。

## 跨动态库的坑（协议插件踩出来的）

1. **不要跨模块用 `findChild<自己的类型*>` 找单例。** 协议插件是独立 DLL、静态链接了
   同一份 `monitor_core`，函数内静态局部变量于是"一个模块一份"。把单例挂到
   `QCoreApplication` 下用 `findChild<自己的类型*>` 查找**也不可靠** ——
   实测在插件里找不到主程序建的那个实例（同模块内一切正常），于是又新建一个，
   单例名存实亡（本例的症状是两个 `QFile` 句柄抢同一个日志文件、两套滚动计数互相打架）。
   可靠写法见 `utils/logger.cpp::Log::instance()`：**用 `findChild<QObject *>(名字)`
   在 QObject 层查找（QObject 的元对象在 Qt6Core 里、全进程唯一），再自己向下转型**。
2. 插件里可以正常用 `Log::info()` —— 上面的写法保证它落到**主程序那一个**日志文件里。
3. 插件的连接对象**不要设父对象**（`IProtocolPlugin::create()` 的约定）：
   采集调度器要把它 `moveToThread`，有父对象的 QObject 搬不了线程。

## 性能基线（`tools/bench/`）

- 夹具默认**不编译**，`-DMONITOR_BUILD_BENCH=ON` 打开（CI 不必为它花时间）。
- 改完任何影响采集 / 落库性能的代码，建议重跑一次并把新数字同步进 README 的「性能基线」。
- **夹具里别忘了 `registerBuiltinProtocols()`**：采集调度器只问协议注册表，
  没注册就一台设备都起不来（这条路径是明确报错的，日志里会写"未知协议 mock，当前可用："）。
- 报数时的两条纪律：
  1. **同时数采集回调次数与数据库行数并断言相等** —— 批量写入最容易"悄悄少写几条"，
     只看界面永远发现不了。
  2. **报告里必须写清构建类型与机器**。本机 VS Code 的预设是 **Debug**，
     用 Debug 跑出来的吞吐会明显低于 README 里那份 Release 数字，别混着比。

## ⚠️ 已知缺口：没有数据保留策略

每行采样约 209 字节（含索引），1000 点/秒连续跑一天约 **17.6 GB**。
数据库目前**只增不减**，没有任何清理 / 归档 / 降采样 —— 短时演示没问题，
但这是"接现场长期跑"必须补的一块（见 ROADMAP）。改动 `samples` 表相关逻辑时别忘了这件事。

## 长期运行相关（日志 / 崩溃）

- **日志文件的打开模式不能带 `QIODevice::Text`**：Text 模式在 Windows 上会把 `\n` 翻成 `\r\n`，
  于是"写入的字节数"和"磁盘上的字节数"每行差 1 字节。滚动阈值是按前者算的，
  结果文件会稳定超出上限（实测 4096 上限跑出 4224）。日志不需要 CRLF，保持纯 LF。
- **崩溃处理器里一行 Qt 都不能调**（`utils/crashhandler.cpp` 的过滤器/信号处理器）。
  崩溃时堆与栈状态都不可信，任何分配内存的操作都可能二次崩溃，结果是连转储都留不下来。
  只能用 Win32 API 与栈上的缓冲；文件名用 `_snwprintf`（`wsprintf`/`swprintf` 在
  MSVC 与 MinGW 上的原型不一致，而"某编译器编不过"是崩溃处理器最不该有的问题）。
- 崩溃处理器处理完要**交回系统默认行为**（`EXCEPTION_CONTINUE_SEARCH` / 恢复信号默认处置后重新 raise），
  不要吞掉异常 —— 静默退出会让退出码变成"正常"，比崩溃本身更难查。
6. **MQTT 的 CONNACK 只能在 `onReadyRead` 里判** —— 建连后的数据会先经 `readyRead`
   被 `onReadyRead` 收进 `m_buffer`；`open()` 里再 `socket->readAll()` 只会拿到空数组，
   于是"账号密码错误（返回码 4）"会被当成连接成功。
   现在由 `onReadyRead` 解析 CONNACK 并置位 `m_connAckReceived` / `m_connRejected`，
   `open()` 只等标志位。**改动这块别退回"在 open 里读 socket"的写法。**
7. **Windows 上 `SO_REUSEADDR` 允许两个进程绑同一端口** —— 本地起测试用 broker / 服务端桩时，
   旧进程没退干净会"看起来绑定成功"，但连接被旧进程接走。重启前务必确认旧进程已终止
   （用 `Get-CimInstance Win32_Process` 按命令行过滤，别一把 `taskkill /IM python.exe`）。

## 线程模型与性能约定

- **采集线程**：每台设备一条 `QThread`（`AcquisitionScheduler` 创建 / 销毁），
  `DeviceConnection` 用 `moveToThread` 搬进去。调度器、界面、存储全部住在 GUI 线程。
  - `configure()` 在 `moveToThread` **之前**调用（只写配置字段）；
    `open()` 必须**投递到工作线程**里执行 —— QTcpSocket / QTimer 必须在所属线程诞生。
  - 工作线程只做"收发 + 抛信号"，不碰调度器成员；信号跨线程自动走队列连接。
  - 停止必须 `quit()` + `wait()`（超时兜底 `terminate()`），否则 QThread 析构会崩。
  - 重连**复用同一条线程**，只换连接对象。
  - 对外接口与信号 (`start/stop/stopAll/isRunning/writeTag` + 6 个信号) 一律不变，
    界面层不知道线程的存在。`writeTag` 用 `BlockingQueuedConnection` 拿返回值。
  - `isRunning()` 读的是调度器里的状态快照，**不要**跨线程去读 `connection->isOpen()`。
- **批量写入**：`DataStorage::insertSample` 只入内存队列，每 200ms 或攒够 200 条
  用一个事务批量提交（`kSampleFlushIntervalMs` / `kSampleBatchSize`）。
  退出前 `aboutToQuit → flush()` 保底，别把这一步删了。
  **SQLite 连接不可跨线程**：提交由 GUI 线程的 QTimer 驱动，不要挪进工作线程。
- **数据不丢（溢出队列）**，两条铁律别改回去：
  1. `insertSample()` **不判断数据库是否打开**，一律入队 —— 以前是"库没打开就 return false"，
     结果数据库启动失败时丢掉的是**全部**采样。落库还是溢出，交给 `flush()` 决定。
  2. `flush()` 写库失败时**不丢弃**，把整批 `spillBatch()` 追加到 `<数据库名>.pending.jsonl`
     （JSON Lines：追加写不用读回整个文件，进程被强杀时已写完的行仍可解析）。
     `open()` 里 `replaySpillFile()` 按时间升序补传，**只有 commit 成功才 `QFile::remove`** ——
     先删后写，一崩就丢。回滚了必须保留文件。
  3. 判据：**"数据一条不少"优先于"别让文件变大"**。队列超 64 MB 时的选择是停止接收并报错，
     不是悄悄丢掉最老的一批。
- **界面节流**：`MainWindow` 把采样值攒 100ms 合并刷新一次表格 / 曲线 / 面板
  （`kUiRefreshIntervalMs`）。但**落库与告警判定不节流** —— 数据一条都不能少。

## 界面现状

- 全局样式集中在 `src/ui/theme.cpp` 的 `applicationStyleSheet()`，在 `main.cpp` 里 `setStyleSheet` 生效。
- 主窗口右侧 10 个 Tab：**总览** / 实时数据 / 趋势曲线 / 告警 / 告警历史 / 历史查询 / 远程控制 / 规则配置 / 报表统计 / 设备详情。
- 各面板都是独立的 `QWidget` 子类，构造时注入它依赖的核心对象（不 new 全局单例）。
- 「告警」= 当前活动告警（`AlarmEngine` 内存态），「告警历史」= 库里的历史（`DataStorage::queryAlarms`），两者别搞混。
- 告警提醒（非模态浮层 / 声音 / 系统托盘）统一在 `ui/alarmnotifier.h`，自己订阅 `AlarmEngine::alarmRaised`，主窗口只接它的 `alarmActivated` 信号。
- 「总览」= `ui/overviewpanel.*`：KPI 卡片墙 + 每设备状态卡 + 当前活动告警列表（最多 6 条），
  点设备卡发 `deviceActivated`，由主窗口选中设备树节点并切到「实时数据」。
  - 告警明细列表只是"扫一眼"，处理告警仍在「告警」页；`activeAlarms()` 出来是无序的，面板里按时间倒序排。
  - 配色靠 objectName 切换（`kpiCard` / `kpiCardOk` / `kpiCardWarn` / `kpiCardDanger` + 对应的 `kpiValue*`），
    语义色 token 是 `@success` / `@warning` / `@danger`；切换前先比 objectName，避免每 2 秒白跑一次 unpolish。
  - 数据库统计只在**页面可见时**才跑（`showEvent` 开 / `hideEvent` 关定时器）；
    实时值走 `tagUpdated`，攒 250ms 一批只更新对应卡片。
