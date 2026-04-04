# 《我用内核驱动做了个限速工具，AI 帮了大忙，也坑了我三次》
## 教学视频剧本·专业版（面向开发者）

**预估时长：约 15 分钟**

---

> **[口播]** = 主讲人台词  
> **[动画]** = 动态示意图  
> **[代码]** = 代码高亮录屏  

---

## 开场：从「够用」到「做对」

**[动画：分屏——左边「用户态 WFP」来回箭头，右边「内核 Callout」就地决策]**

**口播：**

"最初的方案：用户态 WFP，ETW 抓事件，令牌桶超额就 `FwpmFilterAdd0` 加 Block 规则，100ms 后删掉。每次限速决策要在用户态绕一圈再回内核，对高速流量，100ms 周期会被这个往返打碎。

改法：写 WFP Callout 驱动，在 `classifyFn` 里直接查内核令牌桶，就地放行或丢包。没有 IPC，没有上下文切换。"

---

## 第一幕：架构对话——让 AI 先把坑说出来

**[画面：Copilot 对话，第一条消息「我想在驱动层实现对进程的网络限速，你规划一下怎么实现吧。」]**

**口播：**

"第一步是规划，不是写代码——在规划阶段把陷阱逼出来。

AI 的方案基本对：ALE flow-established 层关联 flow→PID，传输层 classify 用 flowContext 取 PID，SpinLock 保护的内核令牌桶表，IOCTL 下发规则。

但漏了一个细节：`FwpsFlowAssociateContext0` 要求 callout 必须注册 `flowDeleteFn`，否则返回 `STATUS_INVALID_PARAMETER`。Review 时我发现了，补上。工具链约束属于 AI 的盲区。"

**[动画：WFP 分层注册示意图——ALE_FLOW_ESTABLISHED 层绑定 flow context，TRANSPORT 层 classify 使用 flowContext]**

---

## 第二幕：TDD 让内核外的代码先稳下来

**[代码：TokenBucket 的测试文件，逐行扫过]**

**口播：**

"内核驱动难以单元测试，但令牌桶是纯算法，完全可测。AI 用 TDD 方式实现，并主动加了时间注入接口：

```cpp
void setTimeForTest(TimePoint tp);
void advanceTimeForTest(std::chrono::milliseconds ms);
```

生产代码用真实时钟，测试注入虚拟时钟，10 个测试整套跑不到 1ms，任何时间相关的退化即刻可见。`TrafficTracker` 滑动窗口同样处理，9 个测试全覆盖。**先稳住用户态逻辑，再进内核，调试成本低一个数量级。**"

---

## 第三幕：AI 在内核里的三个坑

**[动画：三个「地雷」图标依次弹出，每个对应一个 bug]**

**口播：**

"用户态代码顺利，进内核之后 AI 踩了三个坑，每一个都值得记录。"

**[代码：对比两段 struct 定义]**

**口播：**

"**坑一：C 和 C++ 的 struct 语义差异。**

`DriverProtocol.h` 用了 C++ 风格的 struct，驱动是 `.c` 文件，C 编译器不认无 typedef 的类型名，报一堆「未声明标识符」。以为是 include 路径问题，其实一行 `typedef struct` 搞定。跨语言共享头文件的约束 AI 没感知到。"

**[代码：展示 `NDIS630=1` 宏的位置]**

**口播：**

"**坑二：`NDIS630` 宏缺失。**

`ndis.h` 和 `fwpsk.h` 里大量类型定义被 `#if NDIS_SUPPORT_NDIS630` 包裹。不加 `NDIS630=1`，报几十个「未声明标识符」，完全看不出和 NDIS 版本有关。AI 的编译参数里没有这行，加上全通。"

**[代码：高亮那几行被删除的「快速放行」逻辑]**

**口播：**

"**坑三：入站包错误旁路，最隐蔽。**

```c
if (layerId == FWPS_LAYER_INBOUND_TRANSPORT_V4 &&
    FWPS_IS_METADATA_FIELD_PRESENT(InMetaValues,
        FWPS_METADATA_FIELD_ALE_CLASSIFY_REQUIRED)) {
    ClassifyOut->actionType = FWP_ACTION_PERMIT;
    return;
}
```

AI 的意图是跳过 ALE 重分类的边缘包。问题：`ALE_CLASSIFY_REQUIRED` 在 TCP 正常数据流里持续出现，导致下行限速完全失效——IOCTL 正确、令牌桶更新、classify 被调用，流量就是不受控。

定位：在各 return 分支加计数日志，发现几乎所有入站包都走了这条 PERMIT 路径。删掉，解决。AI 没有在 Windows 真实流量里跑过，所以它不知道这个假设是错的。"

---

## 第四幕：驱动签名的「最后一公里」

**[动画：驱动加载流程：.sys → 签名验证 → 成功加载 / 错误 577]**

**口播：**

"编译通过，`sc start` 给你错误 577——签名不合规。测试期解法：

```powershell
bcdedit /set testsigning on   # 然后重启
New-SelfSignedCertificate ... # 生成自签名代码签名证书
Import-Certificate ...        # 导入 Root 和 TrustedPublisher
signtool sign /fd SHA256 ...  # 签名 .sys
```

AI 打包成了 `sign-driver.ps1`，一键执行。漏提了一件事：testsigning 重启后才生效，重启前跑还是 577。"

**[画面：sc query 输出 STATE: 4 RUNNING，界面启动]**

**口播：**

"驱动加载成功后，用户态 App 用 `CreateFileW("\\\\.\\NetMonitorDrv")` 打开设备，`DeviceIoControl` 下发规则。界面里选中一个进程，设置限速，IOCTL 链路把 `{pid, direction, bytesPerSecond}` 传进内核，classify 开始按令牌桶决策每个数据包的命运。"

---

## 第五幕：效果与反思

**[画面：curl.exe 下载，限速前 15MB/s，限速后稳定在目标速率附近]**

**口播：**

"最终效果：限速后数秒收敛，稳定在目标带宽附近，约 ±10% 抖动，限后台进程够用。

复盘：架构层 AI 可信——ETW + WFP callout 方案正确，令牌桶逻辑无误，节省约 70% 编码时间。三个 bug 属于同一类：「在这个精确的 Windows 内核上下文里，假设不成立」。

最有价值的工作流不是「AI 写我改」，而是**「AI 写框架，人做系统性 review，专找假设不成立的地方」**。发现 `ALE_CLASSIFY_REQUIRED` 旁路不是从报错里推出来的，是从「如果我是 AI，我会在哪里偷懒」这个视角逆推出来的。"

---

**[最终画面：项目文件树 + 29 个测试全绿 + 驱动 STATE: RUNNING 三格并列]**

**口播：**

"ETW 监控、令牌桶 TDD、WFP 用户态、内核 callout 驱动、IOCTL 通信、测试签名部署——整套下来两周，和 AI 协作全程。源码在描述里，感谢收看。"

---

*全文约 1500 字。*
