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
- 新增源文件要同时加进 `src/CMakeLists.txt` 的 `PROJECT_SOURCES`。
- 若链接报 `Permission denied`，说明程序还开着：
  `MSYS_NO_PATHCONV=1 taskkill /IM "qt-industrial-equipment-monitor.exe" /F`
- **不要运行 GUI**（无人值守环境会卡住），编译通过即可。

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
4. **新增协议只改一处** —— `AcquisitionScheduler::createConnection()` 里的 switch。
   上层（界面 / 调度）不应感知协议差异。
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
