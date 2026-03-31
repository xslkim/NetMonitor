# NetMonitor 开发文档

## 一、需求分析

### 1.1 功能需求

| 编号 | 需求描述 | 优先级 |
|------|----------|--------|
| F-01 | 实时监控系统中所有产生网络流量的进程 | 高 |
| F-02 | 每 1 秒刷新一次流量数据（上行速率、下行速率、累计流量） | 高 |
| F-03 | 对单个进程设置上行流量限速（单位：KB/s） | 高 |
| F-04 | 对单个进程设置下行流量限速（单位：KB/s） | 高 |
| F-05 | 针对进程配置报警策略：在指定时间窗口内，若流量超过阈值则触发报警 | 高 |
| F-06 | 报警触发后，自动对该进程应用指定限速 | 高 |
| F-07 | 提供简洁的图形界面（ListView 进程列表 + 状态栏） | 中 |
| F-08 | 支持对进程移除限速和报警策略 | 中 |

### 1.2 非功能需求

| 编号 | 需求描述 |
|------|----------|
| NF-01 | 平台：Windows 10 及以上，需管理员权限（ETW + WFP 均要求） |
| NF-02 | 开发语言：C++17 |
| NF-03 | 采用 TDD 开发方式，核心逻辑须有单元测试覆盖 |
| NF-04 | 监控精度：流量捕获在内核层完成，粒度为单次 TCP/UDP 数据包 |
| NF-05 | 限速粒度：基于令牌桶算法，100ms 级别的调节周期 |

### 1.3 约束与边界

- 限速通过 WFP 动态添加/移除阻断规则实现，属于"开关式"限流，适合稳态流量场景
- ETW 内核日志会话（NT Kernel Logger）全局唯一，程序启动时会停止同名旧会话
- 报警策略触发后不会重复报警，需手动调用 `resetPolicy` 才能再次触发
- 历史流量数据默认保留最近 1 小时（3600 秒快照）

---

## 二、架构设计

### 2.1 整体架构

```
┌────────────────────────────────────────────────────┐
│                   GUI 层                           │
│   MainWindow (Win32 ListView + 状态栏)              │
│   Dialogs (限速设置 / 报警策略对话框)                │
└────────────────┬───────────────────────────────────┘
                 │ 每 1s 刷新 / 右键操作
┌────────────────▼───────────────────────────────────┐
│                  核心协调层                         │
│   MainWindow 持有并协调以下组件                      │
│   ┌──────────────────────────────────────────────┐ │
│   │  TrafficTracker   AlertManager   LimitState  │ │
│   └────────┬───────────────┬──────────────────┬──┘ │
└────────────┼───────────────┼──────────────────┼────┘
             │               │                  │
  addTraffic │      evaluate │      block/unblock│
             ▼               ▼                  ▼
┌────────────────┐  ┌────────────────┐  ┌─────────────────┐
│  EtwMonitor    │  │  AlertManager  │  │  WfpLimiter     │
│ (ETW 内核监控)  │  │ (策略评估引擎)  │  │ (WFP 阻断规则)   │
└────────────────┘  └────────────────┘  └─────────────────┘
                                               ▲
                                               │ tryConsume
                                        ┌──────┴───────┐
                                        │ TokenBucket  │
                                        │ (令牌桶算法)   │
                                        └──────────────┘
```

### 2.2 模块说明

#### TokenBucket — 令牌桶限流算法

**职责**：以指定速率向桶中填充令牌，消费者按需取用令牌。令牌不足时返回 false，供上层决策是否阻断。

**核心接口**：
```cpp
TokenBucket(uint64_t ratePerSec, uint64_t burstSize);
bool    tryConsume(uint64_t tokens);   // 消费令牌，不足时返回 false
uint64_t available() const;            // 查询当前可用令牌数
void    setRate(uint64_t rate, uint64_t burst); // 动态调整速率
void    reset();                       // 重置至满桶
```

**时钟注入**：提供 `setTimeForTest` / `advanceTimeForTest` 接口，支持在单元测试中精确控制时间，无需 `Sleep`。

---

#### TrafficTracker — 流量聚合统计

**职责**：接收来自 ETW 的原始流量事件（PID + 字节数 + 方向），按进程聚合并维护历史快照，供速率计算和滑动窗口查询使用。

**核心接口**：
```cpp
void addTraffic(uint32_t pid, uint32_t bytes, Direction dir); // ETW 回调调用（线程安全）
void update();                                                  // 每秒调用，计算速率并存档
uint64_t getTrafficInWindow(uint32_t pid, int windowSec, Direction dir) const;
std::map<uint32_t, ProcessTrafficInfo> getSnapshot() const;
void pruneInactive(int inactiveSeconds = 30);
```

**历史快照机制**：每次 `update()` 记录一条 `PerSecondSnapshot{timestamp, totalSent, totalRecv}`，保留最近 `maxHistorySeconds` 秒。速率 = (当前累计 - 上一快照累计) / 时间差。

**滑动窗口查询**：`getTrafficInWindow` 找到窗口起点最近的一条快照作为基准值，与当前累计相减得到窗口内流量。

---

#### AlertManager — 报警策略引擎

**职责**：管理多条 `AlertPolicy`，每次评估时调用外部提供的流量查询函数，超阈值则触发一次性报警。

**核心接口**：
```cpp
int addPolicy(AlertPolicy policy);          // 添加策略，返回唯一 ID
void removePolicy(int id);
std::vector<AlertAction> evaluate(TrafficQueryFn queryFn); // 返回本次新触发的动作
void setAlertCallback(AlertCallback cb);    // 设置触发回调
void resetPolicy(int id);                   // 重置触发状态（允许再次报警）
```

**AlertPolicy 关键字段**：
```cpp
struct AlertPolicy {
    uint32_t pid;
    uint64_t thresholdBytes;   // 阈值，如 500 * 1024 * 1024
    int      windowSeconds;    // 时间窗口，如 600（10分钟）
    Direction direction;       // Upload / Download
    uint64_t limitBytesPerSec; // 触发后限速值
    bool     triggered;        // 是否已触发（防重复）
};
```

---

#### EtwMonitor — ETW 内核网络监控

**职责**：启动 `NT Kernel Logger` ETW 会话，启用 `EVENT_TRACE_FLAG_NETWORK_TCPIP`，在独立线程中持续处理 TcpIp/UdpIp 内核事件，通过回调函数将 (PID, bytes, direction) 传递给 TrafficTracker。

**事件识别**：

| Provider GUID | Opcode | 含义 |
|---------------|--------|------|
| `TcpIpGuid` {9A280AC0...} | 10 | TCP 发送 (IPv4) |
| `TcpIpGuid` | 11 | TCP 接收 (IPv4) |
| `TcpIpGuid` | 26 | TCP 发送 (IPv6) |
| `TcpIpGuid` | 27 | TCP 接收 (IPv6) |
| `UdpIpGuid` {BF3A50C5...} | 10 | UDP 发送 |
| `UdpIpGuid` | 11 | UDP 接收 |

**事件数据布局（IPv4，前 8 字节）**：
```c
struct TcpIpEventData {
    ULONG PID;   // 进程 ID
    ULONG size;  // 本次传输字节数
    // ... 地址、端口等字段
};
```

**注意**：`ProcessTrace` 会阻塞调用线程，因此在单独的 `std::thread` 中运行。

---

#### WfpLimiter — WFP 带宽阻断

**职责**：使用 Windows Filtering Platform 用户模式 API，在传输层对指定进程的上行或下行流量添加/移除阻断过滤规则，配合令牌桶实现粗粒度限速。

**限速工作原理**：
```
每 100ms 检查一次令牌桶 (IDT_LIMITER 定时器)
  ├─ 令牌充足 → unblockProcess (移除 WFP Block 规则)
  └─ 令牌耗尽 → blockProcess (添加 WFP Block 规则)

ETW 回调收到流量事件时：
  → TokenBucket::tryConsume(bytes)
      ├─ 成功 → 放行
      └─ 失败 → WfpLimiter::blockProcess
```

**WFP 过滤层**：
- 上行：`FWPM_LAYER_OUTBOUND_TRANSPORT_V4`
- 下行：`FWPM_LAYER_INBOUND_TRANSPORT_V4`
- 匹配条件：`FWPM_CONDITION_ALE_APP_ID`（可执行文件路径）

---

#### MainWindow — 主界面

**定时器**：

| 定时器 ID | 周期 | 动作 |
|-----------|------|------|
| `IDT_REFRESH` (1001) | 1000ms | `tracker.update()` → 评估报警 → 刷新 ListView → 更新状态栏 |
| `IDT_LIMITER` (1002) | 100ms | `applyLimits()` 检查令牌桶，解除到期的 WFP 阻断 |

**ListView 列定义**：

| 列 | 内容 |
|----|------|
| PID | 进程 ID |
| 进程名 | 可执行文件名 |
| 下行速率 | 实时下行 bytes/s（格式化为 KB/s、MB/s） |
| 上行速率 | 实时上行 |
| 总下行 | 自监控开始的累计下行量 |
| 总上行 | 累计上行量 |
| 下行限速 | 当前下行限制（无则显示"无限制"） |
| 上行限速 | 当前上行限制 |
| 状态 | "正常" / "↓阻断" / "↑阻断" |

---

### 2.3 数据流向

```
网卡驱动
  │  内核 TCP/UDP 事件
  ▼
ETW 内核日志 (NT Kernel Logger)
  │  EVENT_RECORD {PID, size, opcode}
  ▼
EtwMonitor::processEvent()
  │  addTraffic(pid, bytes, dir)
  ▼
TrafficTracker                     ──────────────► TokenBucket::tryConsume(bytes)
  │  update() 每秒                                    │ 失败
  │  getSnapshot()                                    ▼
  │  getTrafficInWindow()          WfpLimiter::blockProcess(pid, dir)
  ▼
AlertManager::evaluate()
  │ 超阈值
  ▼
AlertAction → 设置 TokenBucket 速率 + 弹出报警对话框

MainWindow 每秒读取 getSnapshot() → 更新 ListView
```

---

### 2.4 文件结构

```
NetMonitor/
├── CMakeLists.txt              # 构建配置（MSVC C++17，FetchContent Google Test）
├── src/
│   ├── main.cpp                # wWinMain 入口，COM/WSA 初始化
│   ├── app.manifest            # 请求管理员权限
│   ├── resource.h / .rc        # 控件 ID、菜单 ID 定义
│   ├── core/
│   │   ├── Types.h             # 公共数据结构（ProcessTrafficInfo、AlertPolicy 等）
│   │   ├── TokenBucket.h/.cpp
│   │   ├── TrafficTracker.h/.cpp
│   │   ├── AlertManager.h/.cpp
│   │   ├── EtwMonitor.h/.cpp
│   │   └── WfpLimiter.h/.cpp
│   └── gui/
│       ├── MainWindow.h/.cpp
│       └── Dialogs.h/.cpp
└── tests/
    ├── test_main.cpp
    ├── test_token_bucket.cpp   # 10 个测试
    ├── test_traffic_tracker.cpp # 9 个测试
    └── test_alert_manager.cpp  # 10 个测试
```

---

## 三、测试用例

测试框架：**Google Test v1.14.0**（CMake FetchContent 自动下载）

运行命令：
```bash
build\Release\NetMonitorTests.exe
```

### 3.1 TokenBucket 测试（10 个）

| 测试名 | 测试意图 | 预期结果 |
|--------|----------|----------|
| `InitialTokensEqualBurst` | 构造后初始令牌数等于 burstSize | `available() == 1000` |
| `ConsumeReducesTokens` | 消费 300 个令牌后减少 | `available() == 700` |
| `CannotConsumeMoreThanAvailable` | 尝试消费超过可用量 | 返回 false，令牌数不变 |
| `TokensRefillOverTime` | 排空后等待 500ms | 补充 500 个令牌（1000 token/s × 0.5s） |
| `TokensDoNotExceedBurst` | 等待 5 秒后令牌数不超上限 | `available() == 1000`（被 burst 截断） |
| `PartialConsumptionAndRefill` | 消费 800 后等待 200ms | `available() == 400`（200 剩余 + 200 补充） |
| `SetRateChangesRefillSpeed` | 动态将速率改为 2000/s | 500ms 后补充 1000 个令牌 |
| `ResetFillsToBurst` | 消费后调用 reset() | `available() == 1000`（满桶） |
| `ZeroRateMeansNoRefill` | 速率设为 0 | 1s 后令牌仍为 0，不补充 |
| `ExactConsumption` | 精确消费全部令牌 | 再消费 1 个返回 false |

**TDD 要点**：所有时间通过 `setTimeForTest` / `advanceTimeForTest` 注入，测试无需实际等待，运行 < 1ms。

---

### 3.2 TrafficTracker 测试（9 个）

| 测试名 | 测试意图 | 预期结果 |
|--------|----------|----------|
| `AddTrafficIncrementsTotal` | 添加流量后累计值正确 | `totalRecv == 500, totalSent == 300` |
| `MultipleAddsAccumulate` | 同方向多次添加累加 | `totalRecv == 800` |
| `RateCalculation` | 两次 update() 间计算速率 | `recvRate ≈ 2000.0 bytes/s` |
| `SeparateProcesses` | 两个 PID 的流量互不影响 | 快照中有 2 个独立进程记录 |
| `TrafficInWindow` | 10 秒滑动窗口内流量统计 | 约 500 字节（误差 ±100） |
| `TrafficInWindowFullRange` | 全范围窗口返回两次快照间的增量 | 返回 2000（第二次添加量） |
| `UnknownProcessReturnsZero` | 查询不存在的 PID | 返回 0 |
| `NameResolverIsCalled` | 设置名称解析器后首次见到 PID 调用它 | `processName == "test.exe"` |
| `PruneInactiveRemovesOldProcesses` | 进程超过 30 秒无活动 | 快照为空 |

---

### 3.3 AlertManager 测试（10 个）

| 测试名 | 测试意图 | 预期结果 |
|--------|----------|----------|
| `AddPolicyReturnsUniqueId` | 添加两条策略 | 返回的 ID 互不相同 |
| `GetPoliciesReturnsAll` | 添加两条后获取全部 | 共 2 条 |
| `RemovePolicyWorks` | 添加后按 ID 移除 | 列表为空 |
| `EvaluateTriggersWhenThresholdExceeded` | 10 分钟下行 600MB > 500MB 阈值 | 返回 1 个 AlertAction，limitBytesPerSec == 10240 |
| `EvaluateDoesNotTriggerBelowThreshold` | 流量仅 100MB，未超 500MB | 返回空列表 |
| `TriggeredPolicyDoesNotTriggerAgain` | 第一次触发后再次 evaluate | 第二次返回空列表（防重复） |
| `ResetPolicyAllowsRetrigger` | 触发后 resetPolicy，再次评估 | 可再次触发 |
| `CallbackInvokedOnTrigger` | 设置回调函数后触发策略 | 回调被调用，pid 正确 |
| `MultiplePoliciesSameProcess` | 同进程的上行和下行策略均超阈值 | 返回 2 个 AlertAction |
| `ClearPoliciesRemovesAll` | clearPolicies() | 列表为空 |

---

### 3.4 测试运行结果

```
[==========] Running 29 tests from 3 test suites.
[----------] 10 tests from TokenBucketTest ............. (1 ms total)
[----------] 9 tests from TrafficTrackerTest ........... (0 ms total)
[----------] 10 tests from AlertManagerTest ............ (0 ms total)
[==========] 29 tests from 3 test suites ran. (1 ms total)
[  PASSED  ] 29 tests.
```

---

## 四、构建与运行

### 4.1 环境依赖

- Visual Studio 2022（MSVC 19.38+）
- CMake 3.20+
- Windows SDK 10.0.19041+
- 网络连接（首次构建时 FetchContent 下载 Google Test）

### 4.2 构建步骤

```bash
# 配置（x64，Visual Studio 2022）
cmake -B build -G "Visual Studio 17 2022" -A x64

# 构建全部（含测试）
cmake --build build --config Release

# 仅构建主程序
cmake --build build --config Release --target NetMonitor

# 仅构建并运行测试
cmake --build build --config Release --target NetMonitorTests
build\Release\NetMonitorTests.exe
```

### 4.3 运行要求

`NetMonitor.exe` 需要以**管理员身份**运行，原因：
- ETW `NT Kernel Logger` 会话需要 `SeSystemProfilePrivilege`
- WFP `FwpmEngineOpen` / `FwpmFilterAdd` 需要管理员权限

程序清单（`app.manifest`）已声明 `requireAdministrator`，双击或从管理员终端启动均可触发 UAC 提升。
