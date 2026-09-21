# ROADMAP — 迭代计划

> 当前版本 **v0.8**。已完成的能力见 [README.md](./README.md#核心功能)，
> 编码约定与已知坑见 [CLAUDE.md](./CLAUDE.md)。

## 现状基线

| 指标 | 值 |
| --- | --- |
| 代码规模 | 14,888 行（34 个 `.cpp` + 29 个 `.h`，含插件 / 测试 / 基准夹具） |
| 分层行数 | ui 7,641 / comm 1,678 / core 1,666 / storage 1,230 / utils 1,019 / 插件 373 / 测试 846 / 基准 358 |
| 编译标准 | `-Wall -Wextra` **零错误零警告**；`ctest` **5/5 套件全绿** |
| 协议实现 | Modbus TCP、Modbus RTU（QSerialPort，需 Qt6SerialPort）、MQTT 3.1.1（均为手写，零第三方协议库）；**协议可插拔**（外部插件零重编译） |
| 无硬件联调 | `tools/` 下零依赖 Python 模拟器（Modbus 从站 / MQTT 发布 / HTTP JSON 桩） |
| 实测性能 | 100 台×10 点位 = **1000 点/秒、单核 9.5 %**；500 台 = **4999 点/秒、单核 53.6 %**，零丢失 |

**已经跨过的坎**（后面别退回去）：采集线程化（每设备一条 `QThread`）、采样攒批事务落库、
CONNACK 返回码解析、分组设备树、双主题、配置导入导出、总览仪表盘、告警工单闭环与 MTTR、
溢出队列与补传、协议插件化、单元测试 + CI、日志滚动与崩溃转储。

---

## 迭代原则

1. **每一项都必须能在无硬件环境下验证** —— 靠 `tools/` 的模拟器与协议桩，
   不接受"接了真机才能测"的改动。
2. **优先补"工业现场真需要、但演示版普遍缺"的能力**，而不是堆界面。
3. **一项完成 = 代码 + 实测验证 + README 同步 + 提交**，四件套缺一不可。

---

## P0 — 补齐业务闭环（最影响"像不像真工业软件"）

### P0-1 告警 → 工单闭环 ✅ 已完成

- **做什么**：告警确认不再是"点一下就完了"，而是必须录入处理结论（处理人 / 处理动作 / 备注），
  生成处理记录并落库；据此统计 **MTTR（平均修复时长）**、按设备 / 按班次的告警与处理统计。
- **为什么**：现场考核的核心指标就是 MTTR。只有告警条数、没有处理闭环，告警模块就只是个高亮列表。
- **落点**：`core/alarmengine.*`（`AlarmDisposition` + `handleAlarm` + MTTR）、
  `storage/datastorage.*`（`alarms` 补 `disposition/handled_by/handled_at/note` 四列）、
  `ui/alarmpanel.*`（处理对话框）、`ui/overviewpanel.*`（MTTR 卡）、`ui/reportpanel.*`（MTTR 列）。
- **验收**：✅ 能对活动告警录入处理结论；报表页能出 MTTR 与处理明细；已恢复的记录拒绝再处理。

### P0-2 Modbus 连续寄存器块读 + 串口 RTU ✅ 已完成

- **做什么**：功能码 03/04 支持一次读多个**连续**寄存器（把点位按地址排序后合并成尽量少的块）；
  新增 Modbus RTU（串口）作为第二个现场接入方式。
- **为什么**：原来是逐点位读，20 个点就是 20 次网络往返 —— 现场 PLC 绝不会这么用，
  这是"演示版"和"能上线"之间最明显的一条线。
- **落点**：
  - 新增 `comm/modbuscommon.h` —— Modbus 公共逻辑（PDU 构造 / CRC16 / 响应解析 /
    `buildBlocks` 连续块合并 / `runPoll` 按寄存器类型分组块读），**TCP 与 RTU 共用同一份**，
    保证块读行为跨传输层一致。
  - `comm/modbusconnection.*` —— 改用 `modbus::runPoll` 块读（原逐点位 `transact` 已删）；
    `transactPdu` 只负责 MBAP 组帧。
  - 新增 `comm/modbusrtuconnection.*`（`QSerialPort`）—— 复用 `modbus::runPoll` + CRC16 帧；
    **整段用 CMake 定义的 `HAVE_QT_SERIALPORT` 宏护卫**，没装 Qt6SerialPort 时自动不编译，构建照常。
  - `DeviceInfo` 加串口参数（baudRate / dataBits / parity / stopBits，刻意用 int 不碰 QSerialPort 类型，
    让配置 / 存储 / 对话框等通用层在 Windows 也能编译）；`configio` / `datastorage` 收发串口字段
    （`ensureColumn` 补 4 列）；`devicedialog` 在 `usesSerial` 时显示串口参数控件。
  - `protocolplugin.h` 加 `usesSerial` 特性；`builtinprotocols.cpp` 在 `HAVE_QT_SERIALPORT` 宏下
    注册 `ModbusRtuProtocolPlugin`（displayName "Modbus RTU"，默认端口 0）；
    `AcquisitionScheduler::createConnection()` **无 switch 改动**（插件化已把分支消掉）。
  - `tools/modbus_rtu_sim.py` —— 零依赖（仅 pyserial）的 RTU 从站模拟器，配 `socat` 虚拟串口做无硬件验证。
- **验收**：✅ `tst_modbus` 用 echo 传输层断言：20 个连续保持寄存器 → **只发 1 个请求**；
  保持 + 输入寄存器混合 → 2 个请求；CRC16 标准向量 `0x4B37` 通过；`buildBlocks` 连续合并 /
  125 上限 / 地址缺口断块均正确。块读在 Windows 上用现有 TCP 模拟器可现场复现。
  RTU 串口的真实验证需在装有 Qt6SerialPort 的 Linux 构建里用 socat 虚拟串口完成（CI 即 Ubuntu）。

### P0-3 采样溢出队列（原「断线缓存与补传」）✅ 已完成

- **做什么**：落库链路不可用时（数据库没打开 / 事务提交失败），采样**不丢弃**，
  改为追加写入磁盘队列文件（JSON Lines，`<数据库名>.pending.jsonl`，超 64 MB 停止接收）；
  下次启动 `open()` 成功后**按时间升序补传**，并且**只有确认提交成功才删除队列文件**。
- **为什么**：工业现场"数据一条不能少"是硬要求。原来 `flush()` 在写库失败时直接丢弃整批，
  而且 `insertSample()` 在库未打开时逐条拒绝 —— 数据库一旦启动失败，丢掉的是**全部**采样。
- **语义澄清**：本项原本写的是"设备离线期间"，但本项目是**主动轮询**模型，
  设备断开时根本采不到值，谈不上缓存。真正会丢数据的场景是**落库链路不可用**，
  所以按这个语义实现（这也正是现场网关"断链缓存"的实际所指）。
- **落点**：`storage/datastorage.*`（`spillBatch` / `replaySpillFile` / `spillBacklogCount`，
  `flush()` 两条失败路径改为溢出，`insertSample()` 不再因库未打开而拒绝）。
- **验收**：✅ 库不可用时 5 条采样全部落盘；重启后 5 条完整补传且不重复；
  按 `rowid` 校验物理写入顺序 = 时间升序；队列含坏行时跳过坏行仍能补传好行。

---

## P1 — 技术深度（体现架构能力，面试主要问这里）

### P1-1 协议插件化 ✅ 已完成

- **做什么**：把 `createConnection()` 的硬编码 `switch` 换成协议注册表 + `QPluginLoader` 动态加载。
  每个协议编成独立动态库，通过元数据声明"协议 id / 显示名 / 默认端口 / 需要哪些配置字段"。
  新增协议 = 丢一个 dll 进去，主程序零改动、零重编译。
- **为什么**：`switch` 的问题不只是"改一行"，而是**连带**——加一种协议要同时动调度器、
  设备对话框（`protocol == Mqtt` 之类的硬判断）、存储列、配置导入导出四处。
  真正卡住扩展的是这些分散的硬编码，不是那个 switch 本身。
- **落点**：`comm/protocolplugin.h`（`IProtocolPlugin` + `ProtocolTraits`）、
  `comm/protocolregistry.*`（注册表 + 插件加载 + 老整型映射）、`comm/builtinprotocols.cpp`
  （内置协议改成插件形式注册）、`plugins/http_json/`（外部插件样板）、
  `core/devicemanager.h`（`protocol` 枚举 → 字符串 `protocolId`）、
  `storage`（`devices.protocol_id` 列 + `ensureColumn` 迁移）、
  `ui/devicedialog.*`（协议列表与字段可见性全由 traits 驱动）、`main.cpp`（启动时注册 + 扫描插件）。
- **验收**：✅ 见下面的落地记录。

### P1-2 单元测试 + CI ✅ 已完成

- **做什么**：`QTest` 覆盖纯逻辑：告警引擎状态机（正常→越限→恢复→再越限）、
  工单闭环与 MTTR、`TagPoint` 工程量换算、`ConfigIo` 配置往返、
  采样溢出队列与补传顺序；GitHub Actions 在每次 push / PR 上构建 + 零警告门槛 + 跑测试。
- **为什么**：前面几项改动都在"数据正确性"上做文章（NULL 语义、补传顺序、MTTR 换算），
  这些东西靠手点界面验证一次就忘了；只有测试能防止以后被改坏。
- **落点**：`src/CMakeLists.txt`（拆出 `monitor_core` 静态库 + 仅含 main 的可执行文件）、
  `tests/`（`tst_alarmengine` / `tst_datastorage` / `tst_configio`）、
  `CMakeLists.txt`（`include(CTest)` + `add_subdirectory(tests)`）、`.github/workflows/build.yml`。
- **验收**：✅ `ctest --test-dir build --output-on-failure` → **5/5 套件全绿**。
- **踩坑记录**：Windows 上 ctest 直接跑测试会以 `0xc0000135`（找不到 DLL）整体失败，
  而人肉双击又是好的 —— 极易误判成"测试写错了"。解法是在 `tests/CMakeLists.txt` 里用
  `ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:<Qt bin>"` 把 Qt 的 bin 目录塞进测试进程
  （**不能手拼 PATH**：Windows 的 PATH 分隔符是 `;`，手拼会被 CMake 当列表分隔符切碎）。

### P1-3 性能基线报告 ✅ 已完成

- **做什么**：`tools/bench/` 压测夹具（默认不编，`-DMONITOR_BUILD_BENCH=ON` 打开）。
  两段测量：**A** 端到端吞吐（N 台模拟设备 × P 点位 × 周期，测采样速率 / 进程 CPU /
  内存 / 落库行数），**B** 写库微基准（攒批一个事务 vs 逐条各提交一次事务）。
- **为什么**："攒批把 N 次 fsync 压成 1 次"这种话，没有数字就只是自我感觉良好。
  尤其是**批量写入最容易出的问题是"悄悄少写几条"**，必须同时数采集次数与数据库行数来对账。
- **落点**：`tools/bench/bench_main.cpp` + `tools/bench/CMakeLists.txt`；
  顶层 `MONITOR_BUILD_BENCH` 选项；README 新增「性能基线」一节。
- **验收**：✅ 数字已写进 README，且夹具留在仓库里**可复现**。
- **坑**：夹具忘了调 `registerBuiltinProtocols()` → 注册表是空的 → 一台设备都起不来。
  好在插件化之后这条路径是**明确报错**（日志写"未知协议 mock，当前可用："，可用列表为空），
  一眼就能定位 —— 如果当初设计成"兜底用默认协议"，这里会变成"跑完 20 秒一个点都没有"。

### 数据保留策略（已在 P2-4 补齐）

每段 209 字节、1000 点/秒约 17.6 GB/天的问题，原来是个"待补缺口"；现由 P2-4 的
"保留最近 N 天原始数据 + 更早数据按小时桶降采样"解决，数据库不再无限增长。实现与验收见上。

---

## P2 — 产品化包装（让项目"看起来完整"）

- **P2-1** `cmake --install` + CPack 生成安装包（NSIS / ZIP），附开始菜单快捷方式。 ✅ 已完成（ZIP）
- **P2-2** 运行截图 / 动图 + 架构图补全（README 现在只有 mermaid 流程图，缺真实界面）。 ✅ 已完成
- **P2-3** 日志滚动（按大小切分 + 保留 N 个）+ 崩溃转储，避免长期运行把磁盘写满。 ✅ 已完成
- **P2-4** 数据保留策略（保留最近 N 天原始数据 + 更早数据按小时桶降采样）。 ✅ 已完成

### P2-4 数据保留策略 ✅ 已完成

- **做什么**：采样表 `samples` 只保留最近 N 天（默认 30 天）的原始数据；更早的数据一旦"整桶过期"，
  就按时间桶（默认 1 小时一个桶）聚合进 `samples_downsampled` 趋势表（avg / min / max / 计数），
  然后删除原始旧行。原始表 + 降采样表加在一起也不会无限增长。
- **为什么**：性能基线算过，1000 点/秒连续跑一天约 17.6 GB，没有清理机制数据库迟早撑爆磁盘。
  但工业现场"历史趋势"又不能丢 —— 降采样把"每秒 1 点"压成"每小时 1 点"，既保住长期趋势又控住体积。
- **落点**：`storage/datastorage.*`（`samples_downsampled` 表 + `setRetentionPolicy` / `applyRetentionPolicy` /
  `downsampledRowCount`，聚合用 `INSERT OR REPLACE` + 唯一键保证幂等）；`main.cpp` 启动时读 QSettings 配置、
  立即跑一次、之后每日定时器触发。
- **关键设计**：过期判定**对齐到桶边界**——只处理"整个桶都已过期"的数据，避免把一个桶拆成两半
  （一半进降采样、一半留原始表）造成半桶丢失或重复计数。
- **验收**：`tst_datStorage::dataRetentionPurgeAndDownsample` 断言：过期原始数据被删、降采样行数值/计数正确、
  运行期插入的更老数据也能被聚合、关闭降采样时只删不聚合。`ctest` 5/5 含此套件。

---

## 里程碑

| 版本 | 目标 | 状态 |
| --- | --- | --- |
| v0.5 | 双主题 / 告警通知体系 / 采集线程化 / 曲线交互 / 配置导入导出 | ✅ 已完成 |
| v0.6 | 总览仪表盘 / MQTT 接入鉴权 | ✅ 已完成 |
| v0.7 | P0 三项（工单闭环 / 块读+RTU / 断线补传） | ✅ 全部完成（P0-2 已由 Ubuntu 虚拟机 + socat 虚拟串口验证路径打通） |
| v0.8 | P1 三项（插件化 / 测试+CI / 性能基线） | ✅ 全部完成 |
| v1.0 | P2 完成，可交付现场试用 | ✅ 全部完成（含数据保留策略 P2-4） |

### 已完成项的落地记录

| 项 | 落地内容 | 验证方式 |
| --- | --- | --- |
| P0-1 工单闭环 | `AlarmDisposition` + `AlarmRecord` 处理字段；`AlarmEngine::handleAlarm/averageHandleDurationMs`；`alarms` 表补 4 列（`ensureColumn` 迁移）；告警面板「处理选中…」对话框（强制填说明）；总览页 KPI 墙扩到 2×5 加「已处理告警 / MTTR」；报表页加「已处理 / MTTR」列与 Excel 处理字段；处理动作写操作留痕 | 临时控制台测试 **32 项断言全通过**（引擎状态机 + 数据库往返 + MTTR SQL 实测 120005ms ≈ 期望 120000ms） |
| P0-3 采样溢出队列 | 落库失败**不再丢弃**，改为 JSON Lines 追加写盘（`<db>.pending.jsonl`，64MB 闸门）；启动时按时间升序补传，**提交成功后才删队列文件**；`insertSample` 不再因"库未打开"而拒绝采样 | 临时控制台测试 **13 项断言全通过**（含用独立连接按 `rowid` 校验补传的物理写入顺序 = 时间升序；坏行容错） |
| P0-2 块读 + Modbus RTU | 新增 `comm/modbuscommon.h`（PDU / CRC16 / `buildBlocks` / `runPoll` 块读，TCP 与 RTU 共用）；`modbusconnection.*` 改走块读；新增 `comm/modbusrtuconnection.*`（`HAVE_QT_SERIALPORT` 宏护卫）；`DeviceInfo` 增串口参数（int 不碰 QSerialPort）、`configio` / `datastorage` / `devicedialog` 全链路贯穿；`protocolplugin.h` 增 `usesSerial`，`builtinprotocols.cpp` 注册 `modbus_rtu`；`tools/modbus_rtu_sim.py` + socat 虚拟串口验证 | `tst_modbus` **13 项断言全 PASS**：20 连续点→1 请求、保持+输入→2 请求、CRC16 标准向量 `0x4B37`、块合并 / 125 上限 / 缺口断块、寄存器与线圈解析；块读在 Windows 用现有 TCP 模拟器可复现 |
| P1-2 测试 + CI | 源码拆出 `monitor_core` 静态库（测试链接的就是产品那份代码）；`tests/` 五个 QTest 套件（新增 `tst_modbus`：CRC16 / 块合并 / 解析 / 块读分组）；顶层 CMake 接 `include(CTest)` + `BUILD_TESTING`；`.github/workflows/build.yml` 自动构建 + **零警告门槛** + ctest | `ctest` **5/5 套件全绿**；CI 配置就绪 |
| P1-1 协议插件化 | `IProtocolPlugin`（id / 显示名 / 默认端口 / `ProtocolTraits` / 连接工厂）+ `ProtocolRegistry`（注册、`QPluginLoader` 加载、按 id 建连接、老整型映射）；内置协议改写成插件形式注册（**含 Modbus RTU**，受 `HAVE_QT_SERIALPORT` 宏护卫），`createConnection()` 里**再无 switch**；`DeviceInfo::protocol` 枚举 → 字符串 `protocolId`，`devices.protocol_id` 列 + `ensureColumn` 迁移；设备对话框的协议列表与字段可见性全由 traits 驱动；外部插件样板 `plugins/http_json/` | 临时验证程序 **40 项断言全 PASS**：注册表规则 / 老整型映射 / 未知协议明确失败 / 外部插件真实取数（连本地 HTTP 桩，**两轮采集只发 2 次 GET**） / 写回 / 老 schema 数据库读出来自动变成字符串 id / 对话框出现第 4 个协议且字段可见性随 traits 变化；**真实程序与安装包都认到 5 个协议**（含 Modbus RTU，装 Qt6SerialPort 时）；`ctest` 5/5 |
| P2-3 日志滚动 + 崩溃转储 | `Log::init(path, maxBytes, maxFiles)` 按大小滚动（默认 2MB × 5 份，超限丢最老）；新增 `utils/crashhandler.*`：Windows 走 `SetUnhandledExceptionFilter` + `MiniDumpWriteDump` 产 `.dmp`，其他平台走信号处理器产带栈的 `.txt`；`main.cpp` 启动即安装 | 临时验证程序 **13 项断言全 PASS**；**真的触发一次空指针崩溃**，产出 67,927 字节的 minidump |
| P2-4 数据保留策略 | `samples_downsampled` 表（桶聚合趋势行）+ `applyRetentionPolicy` 每日清理过期原始数据（对齐桶边界避免半桶丢失）；`main.cpp` 启动时读 QSettings 配置并立即执行一次 | `tst_datastorage::dataRetentionPurgeAndDownsample` **多断言 PASS**：过期原始数据被删、降采样行数值/计数正确、运行期插入的更老数据也能聚合、关闭降采样时只删不聚合；`ctest` 5/5 含此套件 |
| P2-1 安装包 | `install(TARGETS)` + `qt_generate_deploy_app_script(NO_TRANSLATIONS)` 自动带上 Qt 运行时；顶层接 CPack（默认 ZIP，可切 NSIS）；README 补打包说明 | **解压 ZIP 到全新目录、PATH 里不含 Qt 直接运行成功**（EXITCODE=124 = 活满 6 秒）；包内 10 项关键文件齐全；33 MB |
| P2-2 运行截图 | 写了一个截图夹具（真实构造主窗口 + 3 台 Mock 设备跑满 60 秒窗口），抓下 10 个页签存进 `docs/screenshots/`，README 新增「界面预览」 | 10 张 PNG（共 1.6 MB）；顺带**发现并修掉一个真 bug**（见下） |
| P1-3 性能基线 | `tools/bench/`（默认不编，`-DMONITOR_BUILD_BENCH=ON`）：A 段 100/500 台模拟设备的端到端吞吐与 CPU / 内存 / 落库对账；B 段写库微基准（同一张表、同一个库文件） | 100 台×10 点位 → **1000 点/秒、单核 9.5 %**；500 台 → **4999 点/秒、单核 53.6 %**，两次**采集数 = 落库行数（零丢失）**；写库**攒批 22000 条/秒 vs 逐条 203 条/秒 → 约 110 倍**；顺带暴露"无数据保留策略"缺口（见上） |

### P2-2 顺带修掉的真 bug：信号屏蔽作用域盖住了选中还原

`MainWindow::rebuildDeviceTree()` 里用 `QSignalBlocker` 屏蔽重建期间的信号，
但 blocker 是 RAII、**作用域一直延伸到函数末尾**，把末尾还原选中时的
`m_deviceTree->setCurrentItem(target)` 也一起屏蔽了 —— 而那一行的注释还写着
"信号已解除屏蔽，这里会正常同步右侧面板"。

**后果**：每次重建设备树（分组变化、导入配置等）之后，树上显示着选中某台设备，
但 `onDeviceTreeSelectionChanged` 从未触发 → **趋势曲线与设备详情面板收不到通知**。
最直观的症状就是：**树上明明选中着设备，趋势曲线却是空的**（首次启动时尤其明显，
因为构造期的 rebuildDeviceTree 就已经把自动选中那一次同步吃掉了）。

**修法**：把 `QSignalBlocker` 收进一个只包住"清空 + 重建"的内层块，
让它在还原选中之前析构。

**验证（前后对比，不是"看着对"）**：截图夹具里**不做任何选中操作**，直接读图表内部状态 ——
修复前 `series=0`（空图），修复后 `series=5`、`axisX` 有正常的 60 秒窗口、单条曲线 120 个点。

---

## 明确不做（保持项目边界清晰）

- **不做 Web 端 / 移动端** —— 桌面客户端是本项目的定位，B/S 另立仓库。
- **不做服务端账号体系** —— 单机演示定位，角色鉴权留在本地（README 已注明生产环境需换服务端鉴权）。
- **不引入第三方协议库**（libmodbus / paho-mqtt）—— 手写协议栈是刻意为之：
  它能证明"报文级"的理解，也是排查现场问题时的底气。

---

## 每项怎么验证（无硬件、无人值守）

GUI 程序在自动化环境里会**卡在登录框**，`timeout` 跑一下只能证明"没崩"，等于没验。
可行的做法见技能 `qt-gui-smoke-test`：

1. 临时加一个 smoke 目标，用**独立临时数据库**直接构造 `MainWindow` 并 `show()`；
2. 协议类改动配一个纯标准库的桩服务器**抓包**，直接断言报文字节；
3. 验完**撤掉临时目标**，`--clean-first` 全量重建确认干净。
