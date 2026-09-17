# ROADMAP — 迭代计划

> 当前版本 **v0.6**。已完成的能力见 [README.md](./README.md#核心功能)，
> 编码约定与已知坑见 [CLAUDE.md](./CLAUDE.md)。

## 现状基线

| 指标 | 值 |
| --- | --- |
| 代码规模 | 11,717 行（25 个 `.cpp` + 25 个 `.h`） |
| 分层行数 | ui 7,340 / core 1,510 / comm 1,197 / storage 916 / utils 694 |
| 编译标准 | `-Wall -Wextra` **零错误零警告** |
| 协议实现 | Modbus TCP、MQTT 3.1.1（均为手写，零第三方协议库） |
| 无硬件联调 | `tools/` 下两个零依赖 Python 模拟器 |

**已经跨过的坎**（后面别退回去）：采集线程化（每设备一条 `QThread`）、采样攒批事务落库、
CONNACK 返回码解析、分组设备树、双主题、配置导入导出、总览仪表盘。

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

### P0-2 Modbus 连续寄存器块读 + 串口 RTU

- **做什么**：功能码 03/04 支持一次读多个**连续**寄存器（把点位按地址排序后合并成尽量少的块）；
  新增 Modbus RTU（串口）作为第二个现场接入方式。
- **为什么**：现在是逐点位读，20 个点就是 20 次网络往返 —— 现场 PLC 绝不会这么用，
  这是"演示版"和"能上线"之间最明显的一条线。
- **落点**：
  - `comm/modbusconnection.*` —— 新增块读路径与地址合并算法（注意：不连续地址要正确切块，
    且单次读数量有规范上限，保持寄存器一次最多 125 个）
  - 新增 `comm/modbusrtuconnection.*`（`QSerialPort`）或让现有类支持传输层抽象
  - `AcquisitionScheduler::createConnection()` 的 switch 增一个分支
  - `tools/modbus_slave_sim.py` 支持块读与串口参数
- **验收**：20 个点位的配置实测发出请求数 ≤ 3 块；模拟器抓包可见合并后的块读。

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

### P1-1 协议插件化

把 `createConnection()` 的硬编码 `switch` 换成 `QPluginLoader` 动态加载的协议插件，
每个协议编译成独立动态库，通过元数据声明"支持哪个协议 / 默认端口"。
新增协议 = 丢一个 so/dll 进去，主程序零改动、零重编译。
**验收**：新增一个示例协议插件，不重新编译主程序即可在"添加设备"里选到。

### P1-2 单元测试 + CI ✅ 已完成

- **做什么**：`QTest` 覆盖纯逻辑：告警引擎状态机（正常→越限→恢复→再越限）、
  工单闭环与 MTTR、`TagPoint` 工程量换算、`ConfigIo` 配置往返、
  采样溢出队列与补传顺序；GitHub Actions 在每次 push / PR 上构建 + 零警告门槛 + 跑测试。
- **为什么**：前面几项改动都在"数据正确性"上做文章（NULL 语义、补传顺序、MTTR 换算），
  这些东西靠手点界面验证一次就忘了；只有测试能防止以后被改坏。
- **落点**：`src/CMakeLists.txt`（拆出 `monitor_core` 静态库 + 仅含 main 的可执行文件）、
  `tests/`（`tst_alarmengine` / `tst_datastorage` / `tst_configio`）、
  `CMakeLists.txt`（`include(CTest)` + `add_subdirectory(tests)`）、`.github/workflows/build.yml`。
- **验收**：✅ `ctest --test-dir build --output-on-failure` → **3/3 套件全绿**。
- **踩坑记录**：Windows 上 ctest 直接跑测试会以 `0xc0000135`（找不到 DLL）整体失败，
  而人肉双击又是好的 —— 极易误判成"测试写错了"。解法是在 `tests/CMakeLists.txt` 里用
  `ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:<Qt bin>"` 把 Qt 的 bin 目录塞进测试进程
  （**不能手拼 PATH**：Windows 的 PATH 分隔符是 `;`，手拼会被 CMake 当列表分隔符切碎）。

### P1-3 性能基线报告

写一个压测夹具：100 台模拟设备 × 10 点位 × 1s 周期，跑 10 分钟，
记录 CPU / 内存 / 落库延迟 / 界面帧率，把数字写进 README。
**价值**："攒批落库把 N 次 fsync 压成 1 次"这种说法，有数字才算证据。

---

## P2 — 产品化包装（让项目"看起来完整"）

- **P2-1** `cmake --install` + CPack 生成安装包（NSIS / ZIP），附开始菜单快捷方式。 ✅ 已完成（ZIP）
- **P2-2** 运行截图 / 动图 + 架构图补全（README 现在只有 mermaid 流程图，缺真实界面）。
- **P2-3** 日志滚动（按大小切分 + 保留 N 个）+ 崩溃转储，避免长期运行把磁盘写满。 ✅ 已完成

---

## 里程碑

| 版本 | 目标 | 状态 |
| --- | --- | --- |
| v0.5 | 双主题 / 告警通知体系 / 采集线程化 / 曲线交互 / 配置导入导出 | ✅ 已完成 |
| v0.6 | 总览仪表盘 / MQTT 接入鉴权 | ✅ 已完成 |
| v0.7 | P0 三项（工单闭环 / 块读+RTU / 断线补传） | 🚧 进行中：**P0-1、P0-3 已完成**；P0-2 待定（缺 Qt SerialPort） |
| v0.8 | P1 三项（插件化 / 测试+CI / 性能基线） | 🚧 进行中：**P1-2 已完成** |
| v1.0 | P2 完成，可交付现场试用 | 待开始 |

### 已完成项的落地记录

| 项 | 落地内容 | 验证方式 |
| --- | --- | --- |
| P0-1 工单闭环 | `AlarmDisposition` + `AlarmRecord` 处理字段；`AlarmEngine::handleAlarm/averageHandleDurationMs`；`alarms` 表补 4 列（`ensureColumn` 迁移）；告警面板「处理选中…」对话框（强制填说明）；总览页 KPI 墙扩到 2×5 加「已处理告警 / MTTR」；报表页加「已处理 / MTTR」列与 Excel 处理字段；处理动作写操作留痕 | 临时控制台测试 **32 项断言全通过**（引擎状态机 + 数据库往返 + MTTR SQL 实测 120005ms ≈ 期望 120000ms） |
| P0-3 采样溢出队列 | 落库失败**不再丢弃**，改为 JSON Lines 追加写盘（`<db>.pending.jsonl`，64MB 闸门）；启动时按时间升序补传，**提交成功后才删队列文件**；`insertSample` 不再因"库未打开"而拒绝采样 | 临时控制台测试 **13 项断言全通过**（含用独立连接按 `rowid` 校验补传的物理写入顺序 = 时间升序；坏行容错） |
| P1-2 测试 + CI | 源码拆出 `monitor_core` 静态库（测试链接的就是产品那份代码）；`tests/` 三个 QTest 套件共 22 个用例；顶层 CMake 接 `include(CTest)` + `BUILD_TESTING`；`.github/workflows/build.yml` 自动构建 + **零警告门槛** + ctest | `ctest` **3/3 套件全绿**；CI 配置就绪 |
| P2-3 日志滚动 + 崩溃转储 | `Log::init(path, maxBytes, maxFiles)` 按大小滚动（默认 2MB × 5 份，超限丢最老）；新增 `utils/crashhandler.*`：Windows 走 `SetUnhandledExceptionFilter` + `MiniDumpWriteDump` 产 `.dmp`，其他平台走信号处理器产带栈的 `.txt`；`main.cpp` 启动即安装 | 临时验证程序 **13 项断言全 PASS**；**真的触发一次空指针崩溃**，产出 67,927 字节的 minidump |
| P2-1 安装包 | `install(TARGETS)` + `qt_generate_deploy_app_script(NO_TRANSLATIONS)` 自动带上 Qt 运行时；顶层接 CPack（默认 ZIP，可切 NSIS）；README 补打包说明 | **解压 ZIP 到全新目录、PATH 里不含 Qt 直接运行成功**（EXITCODE=124 = 活满 6 秒）；包内 10 项关键文件齐全；33 MB |

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
