# NetMonitor 教学视频剧本
## 《从零构建 Windows 进程级网络限速工具》

**总时长预估：约 40 分钟**  
**适合人群：有 C++ 基础、想深入了解 Windows 底层编程的开发者**  
**技术栈：C++17 / ETW / WFP / Win32 GUI / CMake / GoogleTest / WDK**

---

## 制作说明

> **口播**：主讲人语音内容（需配合代码/图示）  
> **[动画]**：需制作的动画或屏幕录制内容  
> **[画面]**：截图或静态示意图  
> **[代码]**：代码高亮展示，需屏幕录制  

---

# 第 0 章：片头引子（约 1.5 分钟）

---

**[动画：全屏展示一个正在下载大文件的 Windows 任务管理器，网络列显示"占用 95%"]**

**口播：**

"你有没有遇到这种情况——一个后台进程突然狂吃带宽，直接把你的游戏或者视频通话打进了地狱？任务管理器能告诉你是哪个进程干的，但它没法帮你限制它只能用多少网速。"

**[动画：圈出某个高占用进程，旁边打出大字：「任务管理器 = 只能看不能管」]**

**口播：**

"Windows 有第三方工具能做这件事，比如 NetLimiter、GlassWire，但它们要么收费，要么你完全不知道它底层在干什么。今天，我们从零开始，用 C++ 和 Windows 底层 API，自己造一个进程级网络监控与限速工具——NetMonitor。"

**[动画：快速展示最终成品界面：ListView 展示各进程实时网速，右键菜单设置限速，状态栏显示驱动连接]**

**口播：**

"这不是一个业余脚本，而是一个工程级项目：  
- 用 ETW 在内核层捕获每一个 TCP/UDP 数据包；  
- 用令牌桶算法精确控制流量配额；  
- 用 WFP Windows 过滤平台实现真正的网络阻断；  
- 采用 TDD 测试驱动开发，29 个单元测试全部通过；  
- 最后还写了一个 Windows 内核驱动，实现驱动层级的限速。"

**[动画：以上几条逐条弹出，配合相关技术图标]**

**口播：**

"全程使用 AI 协助编程——我会告诉你哪些地方 AI 帮了大忙，哪些地方 AI 犯了错、我是怎么纠正的。好，我们开始。"

---

# 第 1 章：需求分析与架构设计（约 3 分钟）

---

**[画面：打开 DESIGN.md 文件，滚动浏览]**

**口播：**

"好的项目永远从文档开始。我在 DESIGN.md 里写下了这个项目的全部需求和架构设计，这是对自己也是对 AI 的一份'合同'。"

**[动画：展示功能需求表格，逐行高亮]**

**口播：**

"功能需求分八条。高优先级的四条是核心：  
第一，实时监控所有产生网络流量的进程；  
第二，每秒刷新上行、下行速率和累计流量；  
第三、第四条，能对单个进程分别设置上行和下行限速，单位是 KB/s。

另外两条也很重要：可以设置报警策略，如果某进程在指定时间窗口内流量超过阈值，自动触发限速；还有一个简洁的 Win32 图形界面。"

**[动画：展示架构图，从上到下逐层亮起：GUI层 → 核心协调层 → 底层组件]**

**口播：**

"架构上我分了三层。最顶层是 GUI，用 Win32 原生 API 构建 ListView 列表和工具栏。中间是核心协调层，MainWindow 持有并协调所有组件。最底层是三个独立的专职模块：

**EtwMonitor**，负责监听 Windows 内核的网络事件，它就是我们的'眼睛'；  
**AlertManager**，负责评估报警策略；  
**WfpLimiter**，负责实际的网络阻断，配合 **TokenBucket** 令牌桶实现限速。"

**[动画：展示数据流向图，数据从网卡驱动流向 ETW，再到 EtwMonitor，再到 TrafficTracker，箭头逐步动态出现]**

**口播：**

"数据流是这样的：网卡收发数据 → 触发内核 ETW 事件 → EtwMonitor 回调拿到 PID 和字节数 → TrafficTracker 聚合统计 → AlertManager 检查是否超阈值 → 超了则通过 TokenBucket + WfpLimiter 阻断流量。"

**[画面：展示文件结构树]**

**口播：**

"文件结构很清晰：`src/core/` 放所有核心逻辑，`src/gui/` 放界面，`tests/` 放单元测试。这种分层结构让核心逻辑完全可测，不依赖任何界面代码。"

---

# 第 2 章：开发环境搭建（约 2 分钟）

---

**[代码：打开 CMakeLists.txt，高亮展示]**

**口播：**

"构建系统用 CMake，编译器是 MSVC。先来看 CMakeLists.txt 的关键部分。"

```cmake
cmake_minimum_required(VERSION 3.20)
project(NetMonitor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
```

**口播：**

"C++17，这是硬性要求，后面会用到 `std::string_view`、结构化绑定、`if constexpr` 等特性。"

**[代码：高亮 FetchContent 部分]**

```cmake
include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)
FetchContent_MakeAvailable(googletest)
```

**口播：**

"GoogleTest 用 FetchContent 自动拉取，不需要手动管理第三方库。第一次构建时 CMake 会自动下载，之后就缓存了。这是现代 C++ 项目管理依赖的标准做法。"

**[代码：展示三个构建目标：NetMonitorCore、NetMonitor、NetMonitorTests]**

**口播：**

"注意我把所有核心逻辑抽成了一个静态库 `NetMonitorCore`，然后主程序 `NetMonitor` 和测试程序 `NetMonitorTests` 都链接这个库。这样测试就不需要启动 GUI，直接测纯逻辑。"

**[动画：展示构建命令]**

```powershell
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

**口播：**

"构建命令就这两行。如果你用的是 Visual Studio 2022，也可以直接在 VS 里打开 CMakeLists.txt，它会自动识别。"

---

# 第 3 章：ETW 内核监控——用「官方窃听器」抓流量（约 4 分钟）

---

**[动画：绘制一张示意图：应用层 → 传输层 → 网卡驱动，中间有一个「ETW 探针」接在传输层旁边]**

**口播：**

"ETW，全称 Event Tracing for Windows，是 Windows 内置的高性能内核事件追踪机制。很多人知道用 Wireshark 抓包，但 Wireshark 工作在应用层，做进程级统计需要额外对应。而 ETW 直接在内核传输层埋点，每次 TCP 或 UDP 收发数据，内核都会触发一个事件，里面包含 PID 和字节数。这正是我们需要的数据来源。"

**[画面：展示 EtwMonitor.h 接口]**

**口播：**

"EtwMonitor 的接口很简洁：`start()` 开始监控，`stop()` 停止，还有一个回调 `setCallback`，每次抓到流量事件就调这个回调，把 PID、字节数、方向传出去。"

**[代码：展示 EtwMonitor.cpp 的核心逻辑片段——EVENT_TRACE_PROPERTIES 配置]**

**口播：**

"使用 ETW 需要三步。第一步，启动一个追踪会话，叫 NT Kernel Logger，这是 Windows 专门用来记录内核事件的全局会话。配置里有一个关键的标志位："

```cpp
properties->EnableFlags = EVENT_TRACE_FLAG_NETWORK_TCPIP;
```

**口播：**

"这行告诉内核：我只关心 TCP/IP 网络事件。事件量会很大，我们只拦截自己需要的。"

**[动画：展示事件分类表：TcpIpGuid 的 Opcode 10/11/26/27，UdpIpGuid 的 10/11]**

**口播：**

"第二步，识别事件。ETW 的每个事件有 Provider GUID 和 Opcode。Opcode 10 表示发送，11 表示接收。TCP 有 IPv4 和 IPv6 两个版本，所以有四个 Opcode，UDP 有两个。"

**[代码：展示事件数据结构解析]**

```cpp
struct TcpIpEventData {
    ULONG PID;   // 进程 ID —— 这就是我们要的！
    ULONG size;  // 本次传输字节数
    // ... 后面是 IP 地址、端口等
};
```

**口播：**

"第三步，把事件数据解析出来。事件 payload 的前 8 字节，前 4 字节是 PID，后 4 字节是本次传输的字节数。就这么简单粗暴。我们读出 PID 和 size，然后调用回调。"

**[代码：展示 processThread_ 独立线程的设计]**

**口播：**

"有一个坑需要注意：`ProcessTrace` 这个 Windows API 会一直阻塞，直到追踪会话被停止。所以 EtwMonitor 在一个独立的 `std::thread` 里运行它。当我们调用 `stop()` 的时候，先 `CloseTrace`，这会让 `ProcessTrace` 解除阻塞，然后 join 这个线程。这是经典的生产者-消费者模式在内核事件里的应用。"

---

# 第 4 章：令牌桶算法 TDD——最有趣的一章（约 5 分钟）

---

**[动画：绘制令牌桶示意图：一个桶，水龙头以固定速率滴水（令牌），出口放水（消费令牌），桶满则溢出]**

**口播：**

"令牌桶算法是流量控制领域的经典模型。你可以把它想象成一个水桶：水龙头以固定速率往桶里滴水，每滴水代表一个'令牌'，代表允许通过的字节数。网络数据包进来时要消耗令牌，消耗成功就放行，令牌不足就拒绝。桶有容量上限，多余的令牌会溢出——这就是 burst（允许的瞬时峰值）。"

**[动画：动态演示：速率 1000 byte/s，1秒后有 1000 令牌，突然来一个 800 字节的包，消耗 800 令牌，剩 200；再来一个 500 字节的包，令牌不足，被拒绝]**

**口播：**

"这个算法的优雅之处在于：它不需要真的'等待'令牌积累。我们记录上次检查的时间和令牌数，下次检查时根据时间差计算应该补充多少令牌。计算量极小，适合在内核事件回调这种高频路径里使用。"

**[代码：展示 TokenBucket.h 的接口]**

```cpp
class TokenBucket {
public:
    TokenBucket(uint64_t ratePerSec, uint64_t burstSize);
    bool    tryConsume(uint64_t tokens);
    uint64_t available() const;
    void    setRate(uint64_t rate, uint64_t burst);
    void    reset();

    // 测试支持：时间注入
    void setTimeForTest(TimePoint tp);
    void advanceTimeForTest(std::chrono::milliseconds ms);
};
```

**口播：**

"接口设计时我加了两个测试专用方法：`setTimeForTest` 和 `advanceTimeForTest`。为什么要这样做？因为令牌桶依赖时间，如果测试里真的 sleep 500 毫秒，一个测试套件跑下来就要几十秒，这是不可接受的。通过时间注入，我们可以在测试里'模拟'时间流逝，测试在 1 毫秒内完成。"

**[当前屏幕：VSCode 打开 test_token_bucket.cpp，分屏显示测试文件]**

**口播：**

"TDD 的流程是：先写测试，运行看到失败（红），再写实现，运行看到通过（绿），最后重构。我们来看这 10 个测试。"

**[代码：逐个展示并讲解关键测试]**

**口播：**

"第一个测试最基本——初始令牌数等于 burstSize："

```cpp
TEST_F(TokenBucketTest, InitialTokensEqualBurst) {
    EXPECT_EQ(bucket->available(), 1000);
}
```

**口播：**

"第三个测试很关键——不能消费超过可用量的令牌："

```cpp
TEST_F(TokenBucketTest, CannotConsumeMoreThanAvailable) {
    EXPECT_FALSE(bucket->tryConsume(1500));
    EXPECT_EQ(bucket->available(), 1000); // 原来多少还是多少
}
```

**口播：**

"这里要注意：`tryConsume` 失败时必须保持令牌数不变，不能扣了一部分。这是原子性的要求。"

**[代码：高亮时间相关测试]**

```cpp
TEST_F(TokenBucketTest, TokensRefillOverTime) {
    EXPECT_TRUE(bucket->tryConsume(1000)); // 排空
    
    bucket->advanceTimeForTest(500ms);     // 模拟 0.5 秒
    EXPECT_EQ(bucket->available(), 500);   // 1000/s × 0.5s = 500
}

TEST_F(TokenBucketTest, TokensDoNotExceedBurst) {
    bucket->advanceTimeForTest(5000ms);    // 模拟 5 秒
    EXPECT_EQ(bucket->available(), 1000);  // 被 burst 截断，不超过 1000
}
```

**口播：**

"这两个测试分别验证补充令牌的数量和上限截断。注意第二个——即使过了 5 秒理论上该补 5000 个令牌，实际上桶容量是 1000，多余的全部溢出。"

**[代码：展示 TokenBucket.cpp 核心实现]**

```cpp
void TokenBucket::refill() {
    auto current = now();
    double elapsed = std::chrono::duration<double>(current - lastRefill_).count();
    if (elapsed > 0) {
        tokens_ = std::min(static_cast<double>(burst_),
                           tokens_ + elapsed * static_cast<double>(rate_));
        lastRefill_ = current;
    }
}
```

**口播：**

"实现非常简洁：计算距上次 refill 的时间差，乘以速率得到应补充的令牌数，加上去后用 `std::min` 钳位到 burst 上限。`now()` 方法在生产代码里用实际时钟，在测试里用注入的时间——这是依赖注入的精髓。"

**[动画：展示终端运行测试的输出，10/10 绿色通过]**

**口播：**

"所有 10 个测试通过。重要的是，整个测试套件的运行时间不到 1 毫秒，这就是时间注入的威力。"

---

# 第 5 章：流量聚合模块 TDD（约 4 分钟）

---

**[动画：绘制 TrafficTracker 工作示意图：多个 ETW 事件（PID=1234, 500B）汇入一个表格，表格每秒生成一个快照]**

**口播：**

"ETW 给我们的是原始事件流——每次网络收发都有一个事件。但界面需要的是'这一秒 Chrome 下载了多少字节'。TrafficTracker 就做这个聚合工作。"

**[代码：展示 TrafficTracker 的关键数据结构]**

**口播：**

"内部有两个核心数据结构：一个 `map<uint32_t, ProcessData>` 按 PID 聚合实时数据；另一个是历史快照队列，每次 `update()` 调用时把当前累计值拍一个快照存进去。速率计算就是用当前快照减去上一秒快照，除以时间差。"

**[代码：展示 RateCalculation 测试]**

```cpp
TEST_F(TrafficTrackerTest, RateCalculation) {
    tracker->addTraffic(100, 1000, Direction::Download);
    tracker->update();  // 第一个快照

    tracker->advanceTimeForTest(1000ms);
    tracker->addTraffic(100, 2000, Direction::Download);
    tracker->update();  // 第二个快照

    auto snap = tracker->getSnapshot();
    EXPECT_NEAR(snap[100].recvRate, 2000.0, 1.0);  // 2000 字节 / 1.0 秒
}
```

**口播：**

"注意 `EXPECT_NEAR` 而不是 `EXPECT_EQ`——浮点数比较永远要给容差，这里允许误差 ±1.0 bytes/s。"

**[代码：展示滑动窗口查询测试]**

```cpp
TEST_F(TrafficTrackerTest, TrafficInWindow) {
    for (int i = 0; i < 10; i++) {
        tracker->addTraffic(100, 100, Direction::Download);
        tracker->update();
        tracker->advanceTimeForTest(1000ms);
    }

    uint64_t traffic = tracker->getTrafficInWindow(100, 5, Direction::Download);
    EXPECT_GE(traffic, 400u);
    EXPECT_LE(traffic, 600u);  // 最近 5 秒大约 500 字节
}
```

**口播：**

"滑动窗口查询是报警策略的基础。它在历史快照里找到 5 秒前的那个快照作为基准，用当前累计减去基准值，就得到这 5 秒内的流量。注意测试用的是 `GE` 和 `LE`，因为历史快照的时间点可能不是精确的整数秒，所以给了个合理的范围。"

**[动画：展示滑动窗口的可视化——时间轴上有多个快照点，标记「当前点」和「5秒前的最近快照」，两者累计值之差即为窗口流量]**

**[动画：展示 pruneInactive 的用途图示——30秒无活动的进程从表格里消失]**

**口播：**

"TrafficTracker 还有一个 `pruneInactive` 方法，超过 30 秒没有流量的进程会被清除。否则打开电脑一段时间后，列表里会堆满早已退出的僵尸进程记录。"

---

# 第 6 章：报警策略引擎（约 3 分钟）

---

**[动画：展示一个场景：Chrome 10分钟内下载了 600MB，超过了「500MB/10分钟」的报警策略，自动触发「限速到 100KB/s」]**

**口播：**

"报警策略解决的问题是：我不想每天盯着界面，但我想在某个进程吃太多流量时自动限制它。AlertManager 管理若干条策略，每条策略描述'哪个进程、在什么时间窗口内、用了多少流量就触发、触发后限速到多少'。"

**[代码：展示 AlertPolicy 结构体]**

```cpp
struct AlertPolicy {
    uint32_t pid;
    uint64_t thresholdBytes;  // 500 * 1024 * 1024 = 500MB
    int      windowSeconds;   // 600 = 10分钟
    Direction direction;
    uint64_t limitBytesPerSec; // 触发后限速 100KB/s
    bool     triggered;       // 防止重复触发
};
```

**口播：**

"`triggered` 字段很关键。策略触发一次后就标记为已触发，不会再重复触发直到用户手动 `resetPolicy`。这防止了每秒都在重复触发同一个报警。"

**[代码：展示 AlertManager::evaluate 的设计亮点]**

```cpp
std::vector<AlertAction> evaluate(TrafficQueryFn queryFn);
```

**口播：**

"注意 `evaluate` 的参数是一个函数对象 `TrafficQueryFn`，而不是直接持有 `TrafficTracker` 的引用。这是依赖倒置的体现——AlertManager 不关心流量数据从哪来，只要给它一个'查询函数'就行。在测试里，我们直接传入一个 lambda 返回假数据，完全不需要启动 ETW。"

**[动画：展示 AlertManager 和 TrafficTracker 之间的解耦关系图]**

---

# 第 7 章：WFP 用户态限速——「Windows 防火墙的内部」（约 4 分钟）

---

**[动画：展示 Windows 网络栈分层图，从应用层到网卡驱动，中间有一层「WFP（Windows Filtering Platform）」的过滤引擎，箭头从数据包指向 WFP，WFP 决定「放行或阻断」]**

**口播：**

"WFP，Windows Filtering Platform，是 Windows 防火墙、IDS/IPS 软件的底层基础设施。每个进出网络的数据包都要经过 WFP 的过滤引擎，由各个 filter（规则）决定放行还是拒绝。我们的 WfpLimiter 就是给目标进程动态添加和移除「阻断」规则来实现限速的。"

**[动画：展示令牌桶 + WFP 的联动逻辑]**

```
ETW 收到数据包事件
  ↓
TokenBucket::tryConsume(packetBytes)
  ├─ 成功（令牌充足） → 什么都不做，正常放行
  └─ 失败（令牌耗尽） → WfpLimiter::blockProcess(pid, dir)
                            添加 WFP 阻断规则

每 100ms 定时器触发
  ↓
对所有受限进程，令牌桶已补充了一些
TokenBucket::available() 较充足
  → WfpLimiter::unblockProcess(pid, dir)
      移除 WFP 阻断规则
```

**口播：**

"这是一个「开关式」限速：令牌不够就阻断，攒够了令牌就放开。不是精确的平滑限速，但对于限制后台下载这种场景完全够用。100ms 的调节周期意味着最坏情况下限速误差 100ms 内可以收敛。"

**[代码：展示 WFP 添加过滤规则的关键代码片段]**

**口播：**

"WFP 用户态 API 的使用分几步：首先 `FwpmEngineOpen0` 打开过滤引擎；然后构造一个 `FWPM_FILTER0` 结构，指定过滤层是上行还是下行传输层：

- 上行：`FWPM_LAYER_OUTBOUND_TRANSPORT_V4`
- 下行：`FWPM_LAYER_INBOUND_TRANSPORT_V4`

匹配条件设为 `FWPM_CONDITION_ALE_APP_ID`，也就是可执行文件的路径。这样这条规则只精确匹配这个进程，不影响其他进程。最后 action 设为 `FWP_ACTION_BLOCK`，加进规则引擎。"

**[动画：展示「加规则 → 数据包到达 → 被 WFP 匹配 → 丢弃 → 应用层得到 WSAECONNRESET 或超时」的流程]**

**口播：**

"移除规则时调用 `FwpmFilterDeleteById0` 删掉这条规则，数据包就重新能通过了。整个添加/移除过程大约在微秒级别，对系统性能影响极小。"

---

# 第 8 章：Win32 GUI 构建（约 3 分钟）

---

**[画面：展示最终的界面截图，标注各个控件]**

**口播：**

"GUI 用 Win32 原生 API 构建，不依赖 MFC、Qt 或其他框架。这听起来很古老，但在 Windows 做系统工具，直接用 Win32 是最轻量的选择，编译出来的 EXE 不需要任何运行时依赖。"

**[代码：展示 ListView 初始化关键代码]**

**口播：**

"主界面是一个 `ListView` 控件，配置了 9 列：PID、进程名、下行速率、上行速率、总下行、总上行、下行限速、上行限速、状态。通过 `ListView_InsertColumn` 逐列添加，然后每秒 `ListView_SetItemText` 刷新每个单元格的内容。"

**[动画：展示两个定时器的运作示意图]**

**口播：**

"MainWindow 里有两个定时器：

`IDT_REFRESH`，1 秒触发一次，做三件事：调 `tracker.update()` 计算速率快照；评估报警策略；刷新 ListView 界面。

原始设计里还有一个 `IDT_LIMITER`，100ms 触发，负责检查令牌桶是否可以解除 WFP 阻断。"

**[代码：展示右键菜单的实现]**

**口播：**

"右键菜单通过 `CreatePopupMenu` + `AppendMenuW` 动态构建，显示当前限速值之后跟「设置下行/上行限速」「添加报警策略」「清除限速」等选项。动态构建而不是预定义菜单，是因为需要在菜单里显示当前已设置的限速值，让用户一眼看到现状。"

**[代码：展示限速对话框的实现，输入框 + KB/s 单位]**

**口播：**

"限速对话框让用户输入 KB/s 值。填 0 或留空代表取消限制。代码在 `Dialogs.cpp` 里，用 `CreateWindowW` 手工构建模态对话框，没有用资源文件 .dlg，这样可以动态调整布局。"

---

# 第 9 章：内核驱动进阶——「让限速真正在驱动层实现」（约 5 分钟）

---

**[动画：展示用户态 WFP 和内核态 WFP callout driver 的对比图：用户态方案要进出用户/内核两个上下文；内核驱动方案完全在内核里决策，延迟更低、更可靠]**

**口播：**

"用户态 WFP 方案有一个根本限制：判断是否限速的逻辑在用户态运行，数据包必须先到达用户态，决策后再通知内核。这有上下文切换开销，而且在高流量场景下可能跟不上数据包速率。

进阶方案是写一个 WFP Callout 内核驱动——决策逻辑直接在内核里运行，数据包在内核里就被判断，根本不需要上下文切换。"

**[动画：展示内核驱动架构图：用户态 App 通过 IOCTL 下发限速规则 → 内核驱动保存规则到 per-PID 表 → WFP callout 在每个数据包到达时查表 → 判断放行或阻断]**

**口播：**

"架构是这样的：用户态程序通过 IOCTL 向驱动下发限速规则；驱动在内核里维护一张每 PID 每方向的速率表；WFP callout 函数在每个数据包到达时查这张表，用内核版的令牌桶判断是否放行。"

**[代码：展示 DriverProtocol.h 里的 IOCTL 定义]**

```c
#define IOCTL_NETMONITOR_SET_RATE_LIMIT   \
    CTL_CODE(FILE_DEVICE_NETMONITOR, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_NETMONITOR_REMOVE_RATE_LIMIT \
    CTL_CODE(FILE_DEVICE_NETMONITOR, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_NETMONITOR_QUERY_LIMIT      \
    CTL_CODE(FILE_DEVICE_NETMONITOR, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)
```

**口播：**

"用户态和内核态之间的通信协议用一组 IOCTL 定义，放在共享头文件 `DriverProtocol.h` 里，用户态和内核都 include 这一份——这是很重要的工程实践，避免双方定义不一致。"

**[代码：展示 WFP callout classify 函数的核心逻辑]**

```c
// flowContext 里存着这条流对应的 PID
HANDLE processId = NULL;
if (FlowContext != 0) {
    NETMONITOR_FLOW_CONTEXT* ctx = (NETMONITOR_FLOW_CONTEXT*)(UINT_PTR)FlowContext;
    processId = ctx->ProcessId;
} else if (FWPS_IS_METADATA_FIELD_PRESENT(InMetaValues, FWPS_METADATA_FIELD_PROCESS_ID)) {
    processId = (HANDLE)(UINT_PTR)InMetaValues->processId;
}

if (NetMonitorRateLimitConsume(processId, direction, packetBytes)) {
    ClassifyOut->actionType = FWP_ACTION_PERMIT;
} else {
    ClassifyOut->actionType = FWP_ACTION_BLOCK;
}
```

**口播：**

"classify 函数是驱动的心脏。它试图从 flowContext 拿到 PID——flowContext 是在 ALE flow-established 层提前关联好的，这是最可靠的 PID 来源；如果没有，就从元数据里读。

拿到 PID 后调用 `NetMonitorRateLimitConsume`，这是内核版令牌桶，用 `KeQueryPerformanceCounter` 替代用户态的 `steady_clock`，其余逻辑和用户态版本完全一样。"

**[动画：展示 ALE flow-established 层如何提前「打标签」：TCP 连接建立时，在内核记录 flowHandle → PID 的映射，后续每个属于这条 flow 的数据包都能通过 flowContext 找到对应的 PID]**

**口播：**

"为什么要在 ALE flow-established 层提前关联 PID？因为在传输层，`processId` 元数据不总是有效的——尤其是入站数据包，内核不一定知道这个包属于哪个进程。而 ALE flow-established 层在 TCP 三次握手完成时触发，此时 PID 是确定的，我们用 `FwpsFlowAssociateContext0` 把 PID 绑到 flowId 上，后续所有属于这条连接的包都能查到正确的 PID。"

**[画面：展示内核驱动的编译命令——手工用 VS 2022 cl + WDK 头文件/库，而不是 Visual Studio WDK 项目]**

**口播：**

"编译内核驱动需要 WDK（Windows Driver Kit）。正常途径是用 Visual Studio 的内核驱动项目类型，但工具链版本不匹配时经常报奇怪的错误。我的解决方案是绕过项目系统，直接调用 MSVC 的 `cl.exe` 和 `link.exe`，手工指定 WDK 的 include 路径和库路径。这多了一些手工维护成本，但行为完全透明可控。"

**[代码：展示内核驱动编译时的关键宏定义]**

```c
/D_AMD64_=1 /DAMD64=1 /DNDIS630=1
```

**口播：**

"内核模式的编译有几个必须的宏：`_AMD64_` 和 `AMD64` 告诉 ntddk.h 我们是 64 位目标；`NDIS630=1` 是一个血泪教训——不加这个，NDIS 和 WFP 头文件里的某些类型定义不会展开，你会得到一堆'未声明的标识符'错误，找半天找不到原因。"

---

# 第 10 章：构建、测试与部署（约 3 分钟）

---

**[动画：展示完整的构建流程图]**

**口播：**

"整个项目的构建分两条线：用户态应用用 CMake，驱动用手工脚本 `build-driver-manual.ps1`。"

**[代码：展示 build-driver-manual.ps1 的关键部分]**

**口播：**

"驱动构建脚本做三件事：调用 vcvars64.bat 初始化 VS 2022 编译环境，用 cl.exe 编译 .c 文件，用 link.exe 链接成 .sys 文件。输出目录是 `driver/build/manual/`。"

**[动画：展示测试运行结果：29 测试全绿，运行时间 < 50ms]**

**口播：**

"用户态测试直接运行 `NetMonitorTests.exe`，29 个测试全都在 50ms 内通过。这就是 TDD 的收益——你随时可以运行这 29 个测试，任何一个核心逻辑的退化都会立刻被发现。"

**[动画：展示驱动签名流程动画：生成自签名证书 → 导入到根证书和受信任发布者 → signtool 签名 .sys]**

**口播：**

"内核驱动的部署有一个额外要求：Windows 内核默认只加载有受信任证书签名的驱动。正式商业驱动要花钱向微软买 EV 代码签名证书。开发测试阶段，我们用 `bcdedit /set testsigning on` 开启测试签名模式，然后用 PowerShell `New-SelfSignedCertificate` 生成一个自签名证书，把它导入系统的信任链，再用 `signtool.exe` 给 .sys 文件签名。这一套在 `sign-driver.ps1` 里全自动完成。"

**[动画：展示 sc.exe 命令：sc create NetMonitorDrv type= kernel / sc start NetMonitorDrv / SERVICE_NAME: NetMonitorDrv STATE: 4 RUNNING]**

**口播：**

"加载驱动用 `sc.exe create` 注册为内核服务，`sc.exe start` 启动。SERVICE_NAME 显示 STATE: 4 RUNNING，说明驱动已经在内核里运行了。用户态 App 通过 `CreateFileW("\\\\.\\NetMonitorDrv")` 打开设备句柄，然后就可以发 IOCTL 了。"

---

# 第 11 章：完整演示（约 2 分钟）

---

**[屏幕录制：以管理员身份启动 NetMonitor.exe，状态栏显示「Driver: 已连接 | ETW: 运行中」]**

**口播：**

"启动应用，状态栏下方显示'Driver: 已连接'——说明应用成功打开了驱动设备；'ETW: 运行中'说明内核事件监控已经启动。片刻后，ListView 里开始出现各个活跃进程的实时流量数据。"

**[屏幕录制：在另一个终端启动大文件下载，ListView 里出现 curl.exe，下行速率显示 10+ MB/s]**

**口播：**

"我在另一个终端下载一个 100MB 的测试文件，可以看到 curl.exe 出现在列表里，下行速率大约每秒 10 多 MB。"

**[屏幕录制：在 NetMonitor 里选中 curl.exe，点击「设置下行限速」，输入 100，点确定]**

**口播：**

"现在，我对 curl.exe 设置 100 KB/s 的下行限速……"

**[屏幕录制：观察 curl.exe 的下行速率在几秒内下降到约 100 KB/s，「下行限速」列显示「100 KB/s」，状态列显示「↓限速」]**

**口播：**

"……几秒内，速率明显降下来了，稳定在 100 KB/s 附近。下行限速列和状态列都更新了。"

**[屏幕录制：点「清除限速」，速率恢复]**

**口播：**

"清除限速后，速率立刻恢复。整个交互链路是：界面 → DriverClient.setRateLimit IOCTL → 内核驱动更新令牌桶 → WFP callout 开始限流。"

---

# 结尾：总结与延伸（约 1 分钟）

---

**[动画：展示整个项目的技术栈全景图，所有技术名词标出来，连线表示依赖关系]**

**口播：**

"这个项目用到了哪些核心技术？

- **ETW**：Windows 内核事件追踪，这是监控所有进程网络流量的基础，和 WinPcap/Npcap 走的是完全不同的路径；
- **WFP**：Windows 过滤平台，防火墙、杀毒软件、VPN 都用这套；
- **令牌桶算法**：流量控制的经典算法，路由器里也用这个；
- **TDD**：29 个单元测试保证了核心逻辑的正确性，时间注入技术让测试毫无等待；
- **WDK 内核驱动**：真正的内核编程，callout 机制、IRQL、SpinLock 这些都涉及到了；
- **IOCTL 用户/内核通信**：这是 Windows 驱动与应用交互的标准模式。"

**[动画：打出三个「延伸方向」]**

**口播：**

"如果你想继续深入，这里有三个延伸方向：
1. 把「开关式」阻断改为真正的流量整形（Traffic Shaping）——用 `FwpsInjectTransportSendAsync0` 把数据包重新注入，实现平滑的带宽控制；
2. 实现对所有进程的全局带宽配额策略，而不只是单进程；
3. 把监控数据写入时序数据库，结合 Grafana 做可视化面板。

所有源代码都在项目目录里。感谢收看。"

**[片尾：展示项目文件结构全览，淡出]**

---

## 附录：AI 辅助编程要点回顾

> 本视频是在 AI（GitHub Copilot）辅助下完成的，以下是几个值得分享的实战经验。

**AI 明显帮到的地方：**
- EtwMonitor 中 `EVENT_TRACE_PROPERTIES` 结构体的内存布局（有隐藏的 session name 字符串内存在结构体末尾，AI 提醒了这个细节）
- WFP `FWPM_FILTER0` 的初始化字段，特别是 `FWP_EMPTY` 这个 weight 设置
- CMakeLists.txt 的 FetchContent 配置
- TokenBucket 时间注入的测试辅助接口设计

**AI 犯错、需要人工纠正的地方：**
1. `DriverProtocol.h` 里初始使用了 C++ 风格的 `struct` 定义没有 `typedef`，导致 C 代码的内核驱动编译失败——内核驱动源文件是 .c 而非 .cpp
2. 内核驱动首次遗漏了 `NDIS630=1` 宏定义，导致 WFP/NDIS 头文件里的类型定义不展开，报大量莫名其妙的「未定义标识符」
3. 在 WFP callout 里对带有 `ALE_CLASSIFY_REQUIRED` 标志的入站包无条件放行——这个旁路条件太宽泛，导致下行限速实际失效
4. `FwpsFlowAssociateContext0` 首次没有处理 `STATUS_OBJECT_NAME_EXISTS` 的错误码，导致连接重用时 flow context 没有更新

**结论：AI 是高效的代码生成器，但不能替代对底层机制的理解。遇到 Windows 内核编程这类细节繁多的领域，AI 提供了正确的框架，但细节上的坑仍然需要开发者自己踩过才能识别。**

---

*本剧本全文约 5800 字。视频制作时请按章节分别录制，每段录制前先预跑一遍对应代码，确保画面与口播同步。*
