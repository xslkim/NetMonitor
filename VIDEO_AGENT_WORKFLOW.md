# 自动化视频制作 Agent 工作流文档
## 《我用 AI 两小时造了一个 Windows 进程限速工具》

> **本文档是 AI Agent 的执行指南。Agent 应严格按照本文档的阶段顺序执行，每阶段完成后写状态文件，遇到错误按文档中的恢复策略处理。**

---

## 全局概览

```
输入：
  ├── TEACHING_SCRIPT_STORY.md   （口播稿，~2000字中文）
  ├── TEACHING_VISUAL_GUIDE.md   （素材指南，17项素材描述）
  └── NetMonitor 仓库源码        （用于素材 8 代码展示等）

输出：
  └── output/final.mp4           （1920×1080，30fps，h264+aac，约10-12分钟）

运行环境：Ubuntu 22.04（需预装依赖或配置免密 sudo，见 STAGE 0）
视频框架：Remotion (React)

源仓库路径（Agent 需在 Stage 0 配置）：
  NETMONITOR_REPO_DIR=<NetMonitor 仓库的绝对路径>
```

### 阶段依赖关系

```
STAGE 0: 环境搭建
    │
STAGE 1: 脚本解析 ──→ segments.json
    │
    ├──→ STAGE 2: 语音合成（可并行）──→ audio/*.mp3 + audio-manifest.json
    │
    ├──→ STAGE 3: 素材生成（可并行）──→ 17项视觉素材（React组件）
    │
    ▼
STAGE 4: Remotion 工程搭建 + 合成编排
    │
STAGE 5: 渲染输出
    │
STAGE 6: 后处理 + 校验
```

**Stage 2 和 Stage 3 无依赖关系，必须并行执行以节省时间。Stage 4 依赖两者都完成。**

---

## STAGE 0：环境搭建

### 0.0 前置条件：免密 sudo

本工作流的 Stage 0 需要执行多条 `sudo` 命令安装系统依赖。为了实现全自动无人值守执行，**运行 Agent 的用户必须已配置免密 sudo**。

配置方法见 `SUDO_NOPASSWD_SETUP.md`。

如果无法配置免密 sudo（例如共享服务器），则需要：
1. **手动预装所有依赖**后再启动 Agent，或
2. 使用已预装所有依赖的 Docker 镜像。

Agent 应在 Stage 0 开始时检测 `sudo -n true` 是否成功，若失败则提示用户手动完成系统依赖安装后再继续。

### 0.1 系统依赖

```bash
# 检测免密 sudo 是否可用
if ! sudo -n true 2>/dev/null; then
  echo "ERROR: 免密 sudo 不可用。请先配置免密 sudo（见 SUDO_NOPASSWD_SETUP.md）或手动安装依赖。"
  exit 1
fi

# 更新源
sudo apt update && sudo apt upgrade -y

# 基础工具
sudo apt install -y curl wget git build-essential

# ffmpeg（Remotion 渲染依赖）
sudo apt install -y ffmpeg

# Chromium（Remotion 用 headless Chrome 渲染 React 组件）
sudo apt install -y chromium-browser
# 如果 chromium-browser 不可用，尝试：
# sudo snap install chromium

# 中文字体（极其关键 —— 没有这个，所有中文在渲染时显示为方框）
sudo apt install -y fonts-noto-cjk fonts-noto-cjk-extra

# 刷新字体缓存
fc-cache -fv

# Python3（用于 edge-tts 语音合成）
sudo apt install -y python3 python3-pip python3-venv
```

### 0.2 Node.js 安装

```bash
# 使用 NodeSource 安装 Node.js 20 LTS
curl -fsSL https://deb.nodesource.com/setup_20.x | sudo -E bash -
sudo apt install -y nodejs

# 验证
node -v   # 应显示 v20.x.x
npm -v    # 应显示 10.x.x
```

### 0.3 Python 依赖

```bash
# 创建虚拟环境
python3 -m venv ~/video-agent-venv
source ~/video-agent-venv/bin/activate

# 安装 edge-tts
pip install edge-tts

# 验证
edge-tts --list-voices | grep zh-CN
# 应输出多个中文声音，包括 zh-CN-YunxiNeural
```

### 0.4 初始化 Remotion 项目

```bash
mkdir -p ~/teaching-video && cd ~/teaching-video

# 创建 Remotion 项目
npx create-video@latest . --template blank-ts --no-git

# 安装额外依赖
npm install shiki gray-matter
npm install -D @types/node
```

### 0.4.1 配置源仓库路径

Agent 需要将 NetMonitor 仓库中的输入文件链接到 Remotion 项目中：

```bash
# 设置源仓库路径（Agent 根据实际位置填写）
export NETMONITOR_REPO_DIR="/path/to/NetMonitor"

# 复制输入文件到 Remotion 项目
mkdir -p ~/teaching-video/src/data
cp "$NETMONITOR_REPO_DIR/TEACHING_SCRIPT_STORY.md" ~/teaching-video/src/data/
cp "$NETMONITOR_REPO_DIR/TEACHING_VISUAL_GUIDE.md" ~/teaching-video/src/data/

# 复制需要展示的源码文件（用于素材 8 代码生成动画）
mkdir -p ~/teaching-video/src/data/source-samples
cp "$NETMONITOR_REPO_DIR/src/core/TokenBucket.h" ~/teaching-video/src/data/source-samples/
cp "$NETMONITOR_REPO_DIR/src/core/DivertLimiter.h" ~/teaching-video/src/data/source-samples/

# 将仓库路径写入配置文件，供后续阶段引用
echo "{\"repoDir\": \"$NETMONITOR_REPO_DIR\"}" > ~/teaching-video/src/data/repo-config.json
```

### 0.5 环境验证检查清单

Agent 必须逐项验证，全部通过才可进入下一阶段：

| 检查项 | 命令 | 预期 |
|--------|------|------|
| Node.js | `node -v` | v20.x |
| npm | `npm -v` | 10.x |
| ffmpeg | `ffmpeg -version` | 成功输出版本号 |
| Chromium | `which chromium-browser \|\| which chromium` | 路径存在 |
| 中文字体 | `fc-list :lang=zh \| head -5` | 至少输出一行 Noto Sans CJK |
| edge-tts | `edge-tts --version` | 成功输出版本号 |
| Remotion | `cd ~/teaching-video && npx remotion --version` | 成功输出版本号 |

### 0.6 错误处理

| 错误 | 恢复策略 |
|------|----------|
| apt 安装失败 | `sudo apt --fix-broken install`，然后重试，最多 3 次 |
| npm install 超时 | `npm cache clean --force`，换淘宝镜像 `npm config set registry https://registry.npmmirror.com`，重试 |
| chromium-browser 不存在 | 尝试 `sudo snap install chromium`，或下载 Chrome：`wget https://dl.google.com/linux/direct/google-chrome-stable_current_amd64.deb && sudo dpkg -i google-chrome-stable_current_amd64.deb` |
| 中文字体验证失败 | `sudo apt install -y fonts-wqy-zenhei fonts-wqy-microhei` 作为备选方案 |
| Node.js 版本过低 | 卸载后通过 nvm 安装：`curl -o- https://raw.githubusercontent.com/nvm-sh/nvm/v0.39.7/install.sh \| bash && nvm install 20` |

### 0.7 写状态文件

```bash
echo '{"stage": 0, "status": "success", "timestamp": "'$(date -Iseconds)'"}' > ~/teaching-video/stage-0-status.json
```

---

## STAGE 1：脚本解析

### 1.1 目标

将 `TEACHING_SCRIPT_STORY.md` 和 `TEACHING_VISUAL_GUIDE.md` 解析为结构化 JSON，作为后续所有阶段的数据源。

### 1.2 解析规则

口播稿以 `---` 分隔符划分为 8 个段落，对应关系如下：

| 段落 ID | 标题 | 对应素材编号 |
|---------|------|-------------|
| S01 | 开场：流量费用的烦恼 | 素材 1（账单截图）、素材 2（任务管理器动画） |
| S02 | 现成工具太贵了 | 素材 3（定价截图）、素材 4（文字卡：黑盒金句） |
| S03 | AI 给出技术方案 | 素材 5（AI 对话录屏）、素材 6（三层架构动画）、素材 7（令牌桶动画） |
| S04 | AI 飞速写代码 | 素材 8（代码生成录屏）、素材 9（测试结果截图）、素材 10（模块时间线动画） |
| S05 | 踩坑与修复 | 素材 11（踩坑对比动画）、素材 12（文字卡：犯错金句） |
| S06 | 最终跑通 | 素材 13（操作演示模拟录屏） |
| S07 | 回顾与总结 | 素材 14（时间对比动画）、素材 15（AI 实习生比喻动画）、素材 16（文字卡：结尾金句） |
| S08 | 片尾 | 素材 17（文字卡：片尾信息） |

> **注意**：口播稿中的技术描述已与仓库实际实现对齐。限速核心是 **WinDivert + DivertLimiter**（不是 WFP）。架构三层为 ETW 监控 → 令牌桶配额 → WinDivert 阻断。

### 1.3 实现

创建 `~/teaching-video/src/parse-script.ts`：

```typescript
// 输入：两个 markdown 文件路径
// 输出：segments.json 写入 src/data/segments.json

interface AssetSpec {
  id: number;           // 1-17
  type: 'screenshot' | 'animation' | 'recording' | 'textcard';
  description: string;  // 从视觉指南中提取
}

interface Segment {
  id: string;           // "S01" .. "S08"
  title: string;
  narrationText: string;       // 纯文本，去掉 markdown 标记
  charCount: number;           // 中文字符数（用于估算时长）
  assets: AssetSpec[];
}

interface ParsedScript {
  segments: Segment[];
  totalChars: number;
}
```

### 1.4 解析步骤

1. 读取 `src/data/TEACHING_SCRIPT_STORY.md`（Stage 0.4.1 已复制到此路径）
2. 去掉顶部标题和元信息（第一个 `---` 之前的所有内容）
3. 以 `---` 分割正文为段落
4. 对每个段落：去掉空行和 markdown 格式标记，提取纯中文文本
5. 按上表硬编码段落-素材映射关系（因为映射是固定的）
6. 写入 `src/data/segments.json`
7. **额外输出**：为每个段落生成独立的纯文本文件 `src/data/narration_S01.txt` ~ `src/data/narration_S08.txt`，供 Stage 2 TTS 直接读取

### 1.5 验证

- `segments.json` 中有且仅有 8 个段落
- 每个段落的 `narrationText` 非空
- 所有 17 个素材 ID（1-17）都被引用
- 总字符数在 1800-2200 之间

### 1.6 错误处理

| 错误 | 恢复策略 |
|------|----------|
| 段落数不是 8 | 尝试用空行 + 连续三个以上短横线的正则分割；如仍失败，用段落内的主题关键词（「流量费」「工具」「AI」「代码」「坑」「跑通」「回顾」「感谢」）做模糊匹配分割 |
| 某段落 narrationText 为空 | 检查是否误把分隔符吃掉了内容，回退用更宽松的分割策略 |
| 文件不存在 | 报错退出，提示用户检查文件路径 |

---

## STAGE 2：语音合成（TTS）

### 2.1 工具选择

**主选：edge-tts**
- 免费，无需 API Key，无速率限制
- 中文声音质量高（与 Microsoft Edge 朗读功能相同）
- 输出 MP3 + VTT 字幕文件（含字级别时间戳）

**声音选择：`zh-CN-YunxiNeural`**（男声，自然，适合技术内容讲解）

**备选方案（仅在 edge-tts 完全不可用时）：**
1. `zh-CN-XiaoyiNeural`（女声）
2. `pyttsx3`（离线 TTS，质量差，最后手段）

### 2.2 执行流程

对 `segments.json` 中每个段落执行：

```bash
# 创建输出目录
mkdir -p ~/teaching-video/public/audio

# 对每个段落生成音频（读取 Stage 1 输出的纯文本文件）
edge-tts \
  --voice "zh-CN-YunxiNeural" \
  --rate "+0%" \
  --text "$(cat src/data/narration_S01.txt)" \
  --write-media "public/audio/S01.mp3" \
  --write-subtitles "public/audio/S01.vtt"
```

### 2.3 音频后处理

```bash
# 获取每段音频的精确时长
ffprobe -v quiet -show_entries format=duration -of csv=p=0 public/audio/S01.mp3
```

### 2.4 生成音频清单

写入 `src/data/audio-manifest.json`：

```json
{
  "voice": "zh-CN-YunxiNeural",
  "segments": [
    {
      "id": "S01",
      "audioFile": "audio/S01.mp3",
      "subtitleFile": "audio/S01.vtt",
      "durationSeconds": 28.5
    }
  ],
  "totalDurationSeconds": 620.0
}
```

### 2.5 段落间的静音间隔

在每两个段落之间插入 1.5 秒静音，作为过渡：

```bash
# 生成 1.5 秒静音
ffmpeg -f lavfi -i anullsrc=r=44100:cl=mono -t 1.5 -q:a 9 -acodec libmp3lame public/audio/silence.mp3
```

最终合并为完整音轨时使用：

```bash
# 生成 concat 列表
for seg in S01 S02 S03 S04 S05 S06 S07 S08; do
  echo "file 'audio/${seg}.mp3'" >> concat.txt
  echo "file 'audio/silence.mp3'" >> concat.txt
done

ffmpeg -f concat -safe 0 -i concat.txt -c copy public/audio/full_narration.mp3
```

### 2.6 错误处理

| 错误 | 恢复策略 |
|------|----------|
| edge-tts 网络超时 | 重试 3 次，每次间隔 5 秒；如果是代理问题，设置 `HTTPS_PROXY` 环境变量 |
| edge-tts 输出空文件（<1KB） | 检查输入文本是否含有特殊字符，去掉 emoji 和特殊标点后重试 |
| 单段音频超过 90 秒 | 在句号/问号处切分文本为两段，分别生成后用 ffmpeg 拼接 |
| VTT 文件为空 | 不影响视频生成，跳过字幕功能，使用固定字幕组件替代 |
| edge-tts 完全不可用 | 安装 `pip install pyttsx3`，使用离线合成作为降级方案 |
| ffprobe 获取时长失败 | 使用 Python `mutagen` 库读取 MP3 时长作为备选 |

---

## STAGE 3：视觉素材生成

### 3.0 总体策略

所有 17 项素材均作为 Remotion React 组件实现。不需要外部图片资源——一切用代码绘制。这确保了：
- 风格统一
- 分辨率无损
- 可参数化调整
- 不依赖外部服务

### 3.1 目录结构

```
~/teaching-video/src/components/
  ├── TextCard.tsx                # 通用文字卡（素材 4, 12, 16, 17）
  ├── BillingMock.tsx             # 云账单模拟（素材 1）
  ├── TaskManagerMock.tsx         # 任务管理器动画（素材 2）
  ├── PricingComparison.tsx       # 工具定价对比（素材 3）
  ├── ChatSimulation.tsx          # AI 对话模拟（素材 5）
  ├── ArchitectureDiagram.tsx     # 三层架构动画（素材 6）
  ├── TokenBucketAnimation.tsx    # 令牌桶动画（素材 7）
  ├── CodeGeneration.tsx          # 代码生成快进（素材 8）
  ├── TestResults.tsx             # 测试结果终端（素材 9）
  ├── ModuleTimeline.tsx          # 模块产出时间线（素材 10）
  ├── BugComparison.tsx           # 踩坑分屏对比（素材 11）
  ├── NetMonitorDemo.tsx          # 操作演示模拟（素材 13）
  ├── TimeComparison.tsx          # 时间对比 ×50（素材 14）
  ├── AIInternAnalogy.tsx         # AI 实习生比喻（素材 15）
  └── SubtitleOverlay.tsx         # 字幕层
```

### 3.2 全局视觉规范

Agent 在创建组件时必须遵循以下设计规范：

```typescript
// src/styles/theme.ts
export const THEME = {
  // 颜色
  bgDark: '#0a0a0f',           // 主背景（深色）
  bgCard: '#1a1a2e',           // 卡片背景
  textPrimary: '#e0e0e0',      // 主文字
  textAccent: '#4fc3f7',       // 强调色（技术关键词）
  textQuote: '#ffd54f',        // 金句高亮
  success: '#66bb6a',          // 成功/通过
  error: '#ef5350',            // 错误/失败
  codeGreen: '#a5d6a7',        // 正确代码
  codeRed: '#ef9a9a',          // 错误代码

  // 字体
  fontChinese: "'Noto Sans CJK SC', 'Microsoft YaHei', sans-serif",
  fontCode: "'JetBrains Mono', 'Fira Code', 'Consolas', monospace",

  // 尺寸
  width: 1920,
  height: 1080,
  fps: 30,
};
```

### 3.3 各素材组件详细规格

#### 素材 1 — 云账单模拟（BillingMock.tsx）

**类型**：截图模拟
**内容**：模拟云服务商控制台的费用页面
**实现要点**：
- 深色面板，顶部标题「流量费用明细 — 2025年3月」
- 表格显示：出站流量 580GB / ¥0.80·GB⁻¹ / 合计 ¥464
- 「合计: ¥580」用红色大字高亮
- 静态画面 + 缓慢放大（Ken Burns）效果

```tsx
// 关键动画：缓慢放大
const scale = interpolate(frame, [0, durationInFrames], [1, 1.05], {
  extrapolateRight: 'clamp',
});
```

#### 素材 2 — 任务管理器动画（TaskManagerMock.tsx）

**类型**：动画
**内容**：模拟 Windows 任务管理器网络标签页
**实现要点**：
- 仿 Windows 11 风格的深色 UI
- 表格列：进程名 | PID | 网络使用
- 3 行数据动态更新：
  - Windows Update：数字从 0 跳到 20 MB/s
  - LogSync.exe：5 MB/s
  - crawler.py：8 MB/s
- 每行逐个高亮（黄色边框），配合标注文字浮出
- 最后底部滑入：「月底流量费: ¥580」

```tsx
// 每行出现时间
const row1Appear = 30;   // 第1秒
const row2Appear = 60;   // 第2秒
const row3Appear = 90;   // 第3秒
const totalAppear = 120; // 第4秒显示总费用
```

#### 素材 3 — 工具定价对比（PricingComparison.tsx）

**类型**：截图模拟
**内容**：三列展示 NetLimiter / GlassWire / SoftPerfect 价格
**实现要点**：
- 三个等宽卡片横向排列
- 每个卡片：工具名 + 价格 + 红色边框
- 价格用大号字体，红色
- 卡片依次从左到右淡入

#### 素材 4 — 文字卡：黑盒金句（TextCard.tsx）

**类型**：文字卡
**内容**：「花钱买一个黑盒来省钱，这逻辑说不通。」
**实现要点**：
- 深色背景，文字居中
- 文字淡入，停留 2.5 秒，淡出
- 字体大小 60px，颜色 `textQuote`

#### 素材 5 — AI 对话模拟（ChatSimulation.tsx）

**类型**：动画
**内容**：模拟与 AI 编程助手的对话界面
**实现要点**：
- 类似聊天 APP 的界面：左侧用户消息气泡，右侧 AI 回复气泡
- 用户消息：完整显示需求文字（截取口播稿中的描述，约 50 字）
- AI 回复：打字机效果，逐字显现
  - 使用 `useCurrentFrame()` 控制显示的字符数
  - `const visibleChars = Math.floor(frame * charsPerFrame)`
  - 每帧显示 2 个字符（约 1 秒显示 60 字）
- AI 回复内容：「方案分三层：ETW 监控 → 令牌桶配额 → WFP 阻断」
- 回复完成后底部浮出一个简化架构图

#### 素材 6 — 三层架构动画（ArchitectureDiagram.tsx）

**类型**：动画
**内容**：三层架构从下往上逐层浮现
**实现要点**：
- 三个层级用圆角矩形表示，垂直堆叠
- 每层包含：图标 + 名称 + 一行说明
  - 底层：「ETW 事件追踪（流量监控）」
  - 中层：「WinDivert 网络拦截（抓包/放行）」
  - 顶层：「令牌桶算法（配额管理）」
- 动画：每层从下方 translateY(50px) + opacity(0) → 正位 + opacity(1)
- 每层间隔 1 秒出现，总时长约 4 秒
- 层之间用箭头连接，箭头在层出现后 0.3 秒绘制

```tsx
const layerOpacity = (layerIndex: number) =>
  interpolate(frame, [layerIndex * 30, layerIndex * 30 + 15], [0, 1], {
    extrapolateLeft: 'clamp',
    extrapolateRight: 'clamp',
  });
```

#### 素材 7 — 令牌桶动画（TokenBucketAnimation.tsx）

**类型**：动画（最复杂的单个素材）
**内容**：令牌桶限流原理的可视化
**实现要点**：
- 画面元素（全部用 CSS/SVG 绘制）：
  - 中央：一个透明桶（SVG 矩形 + 圆底），内部有蓝色水位
  - 顶部：水龙头图标，以固定间隔滴下水滴（每 0.5 秒一滴）
  - 右侧：数据包队列（蓝色方块，标注字节数）从右侧进入
  - 底部出口：通过/阻断的判定区域
- 动画阶段（时间轴）：
  - 0-2s：桶是满的，水位高；3 个数据包依次到达，消耗水位，绿色箭头 → 「放行」
  - 2-4s：水位降到 0；新数据包到达，红色 × → 「阻断」
  - 4-6s：水龙头持续滴水，水位缓慢上升
  - 6-8s：水位够了，被阻断的数据包重新通过 → 「放行」
- 数据包移动用 `spring()` 或 `interpolate()` 实现
- 水位高度用 `interpolate()` 对应令牌数量

```tsx
// 水位计算示例
const waterLevel = interpolate(
  frame,
  [0, 60, 90, 120, 180, 240],       // 关键帧
  [100, 100, 20, 0, 60, 80],         // 水位百分比
  { extrapolateRight: 'clamp' }
);
```

#### 素材 8 — 代码生成快进（CodeGeneration.tsx）

**类型**：动画
**内容**：模拟 AI 快速生成代码的过程
**实现要点**：
- 使用 `shiki` 对实际的 TokenBucket.h 源代码做语法高亮
- 代码行逐行从上到下出现，每帧出现 2-3 行（模拟 3-5 倍速打字）
- 行末有闪烁光标
- 背景是深色代码编辑器风格（VS Code Dark+ 主题）
- 代码源：从 `src/data/source-samples/TokenBucket.h` 读取（Stage 0.4.1 已复制）

```typescript
// 从项目读取源代码
import { getHighlighter } from 'shiki';

const highlighter = await getHighlighter({
  themes: ['dark-plus'],
  langs: ['cpp'],
});

const html = highlighter.codeToHtml(sourceCode, {
  lang: 'cpp',
  theme: 'dark-plus',
});
```

**如果 shiki 在 Remotion 中运行有问题**：降级为手工着色——关键词用蓝色，字符串用橙色，注释用灰色，其余白色。用正则匹配即可。

#### 素材 9 — 测试结果终端（TestResults.tsx）

**类型**：截图模拟
**内容**：模拟终端运行 29 个测试全部通过的输出
**实现要点**：
- 黑色背景，等宽字体
- 逐行打字效果显示：
  ```
  [==========] Running 44 tests from 5 test suites.
  [----------] 10 tests from TokenBucketTest
  [  PASSED  ] 10 tests
  [----------] 9 tests from TrafficTrackerTest
  [  PASSED  ] 9 tests
  [----------] 10 tests from AlertManagerTest
  [  PASSED  ] 10 tests
  [----------] 11 tests from PacketSchedulerTest
  [  PASSED  ] 11 tests
  [----------] 4 tests from FlowTrackerTest
  [  PASSED  ] 4 tests
  [==========] 44 tests ran. (1 ms total)
  [  PASSED  ] 44 tests.
  ```
- `PASSED` 用绿色高亮
- 最后一行放大闪烁

#### 素材 10 — 模块产出时间线（ModuleTimeline.tsx）

**类型**：动画
**内容**：横向时间轴，0min → 120min，逐个模块打勾
**实现要点**：
- 横向进度条，左侧「0 min」，右侧「120 min」
- 7 个里程碑点依次亮起：
  1. 令牌桶（10 测试）✓
  2. TrafficTracker（9 测试）✓
  3. AlertManager（10 测试）✓
  4. PacketScheduler（11 测试）✓
  5. FlowTracker（4 测试）✓
  6. EtwMonitor + DivertLimiter ✓
  7. Win32 GUI ✓
- 每个里程碑：圆点变绿 + 勾号动画 + 标签上浮
- 进度条同步从左向右填充

#### 素材 11 — 踩坑分屏对比（BugComparison.tsx）

**类型**：动画
**内容**：左右分屏展示两个坑
**实现要点**：
- 画面左右对分，中间有细线
- 第一阶段（坑 1 — ETW 线程）：
  - 左：红色标题「✗ ProcessTrace 阻塞主线程」+ 灰色卡住的窗口示意
  - 右：绿色标题「✓ 独立线程运行」+ 正常窗口示意
- 1.5 秒后切换到第二阶段（坑 2 — WinDivert 流匹配）：
  - 左：红色代码 `按 PID 过滤（不可靠）`，大 × 标记
  - 右：绿色代码 `FlowTracker 五元组→PID 映射`，大 ✓ 标记
- 代码部分用等宽字体 + 语法着色

#### 素材 12 — 文字卡：犯错金句（TextCard.tsx 复用）

**内容**：「AI 犯错的速度很快，但改错的速度也很快。」
**参数**：`style="quote"`, `duration=2.5s`

#### 素材 13 — 操作演示模拟（NetMonitorDemo.tsx）

**类型**：动画（模拟演示，非实录）
**内容**：用 React 组件模拟 NetMonitor 界面的完整操作流程
**重要说明**：由于本流水线运行在 Ubuntu 上，无法实际运行 Windows 程序。因此用 React 组件模拟 NetMonitor 的界面和交互。视觉指南中标注的"实录"在此改为模拟演示。

**实现要点**：
- 仿 Win32 ListView 样式的表格，列：PID | 进程名 | 下行速率 | 上行速率 | 下行限速 | 状态
- 动画时间轴：
  - 0-2s：界面出现，状态栏显示「ETW: 运行中」
  - 2-5s：进程数据逐行填充（chrome.exe, svchost.exe, curl.exe 等）
  - 5-7s：curl.exe 的下行速率数字快速跳动，稳定在 15.2 MB/s
  - 7-8s：模拟右键菜单弹出，选中「设置下行限速」
  - 8-9s：模拟对话框输入 200，点确定
  - 9-12s：curl.exe 速率从 15.2 MB/s 逐渐降到 ~200 KB/s（数字平滑过渡）
  - 12-13s：下行限速列显示「200 KB/s」，状态列变为「↓限速」

```tsx
// 速率下降动画
const downloadRate = interpolate(
  frame,
  [9 * 30, 12 * 30],  // 第9秒到第12秒
  [15.2 * 1024, 200],  // KB/s
  { extrapolateRight: 'clamp', easing: Easing.out(Easing.cubic) }
);
```

#### 素材 14 — 时间对比 ×50（TimeComparison.tsx）

**类型**：动画
**内容**：手工开发 vs AI 辅助的时间对比
**实现要点**：
- 两个横向条形图，上下排列
- 上方：灰色长条，标注「手工开发：1-2 周」
- 下方：亮色短条（约为上方的 1/50），标注「AI 辅助：2 小时」
- 两个条从左到右生长动画
- 中央大字弹出：「× 50 倍效率提升」，使用 `spring()` 弹性动画

#### 素材 15 — AI 实习生比喻（AIInternAnalogy.tsx）

**类型**：动画
**内容**：实习生 + 工程师 = 最佳搭档
**实现要点**：
- 左侧：用 emoji 或简笔 SVG 画一个年轻人形象
  - 下方标签：「知识面广、手速极快」
- 右侧：用 emoji 或简笔 SVG 画一个戴眼镜的人形象
  - 下方标签：「有判断力、能发现问题」
- 中间：连接线动画绘制，线上标注「最佳搭档」
- 人物和标签依次淡入，连接线最后出现

#### 素材 16 — 文字卡：结尾金句（TextCard.tsx 复用）

**内容**：两行：
```
不是你不需要懂技术，
而是你可以用两个小时，走完以前两周的路。
```
**参数**：`style="closing"`, `duration=4s`, 加大字号

#### 素材 17 — 片尾信息（TextCard.tsx 复用）

**内容**：
```
NetMonitor
C++17 / ETW / WinDivert / Win32 / GoogleTest / CMake
感谢收看
```
**参数**：`style="credits"`, `duration=4s`, 黑底白字

### 3.4 字幕覆盖层（SubtitleOverlay.tsx）

独立组件，读取 VTT 文件，根据当前帧时间显示对应字幕文字。

```tsx
// 字幕样式
const subtitleStyle: React.CSSProperties = {
  position: 'absolute',
  bottom: 80,
  width: '100%',
  textAlign: 'center',
  fontSize: 36,
  color: '#ffffff',
  textShadow: '2px 2px 4px rgba(0,0,0,0.8)',
  fontFamily: THEME.fontChinese,
};
```

### 3.5 错误处理

| 错误 | 恢复策略 |
|------|----------|
| 组件渲染报错（React 错误） | 隔离测试：用 `npx remotion still` 逐个渲染每个组件的第一帧，定位出错组件 |
| 中文显示为方框 | 检查 `fc-list :lang=zh`，确认字体已安装；在组件 CSS 中显式指定 `fontFamily` |
| shiki 在 Remotion bundle 中不工作 | 降级方案：预先在 Node 脚本中用 shiki 生成 HTML 字符串，保存为 JSON，组件中用 `dangerouslySetInnerHTML` 渲染 |
| SVG 动画性能差 | 减少同屏 SVG 元素数量；复杂动画改用 CSS transform 而非 SVG 属性动画 |
| 某个组件开发失败 | 用通用 TextCard 替代，显示该素材的文字描述。视频可以不完美但必须能渲染完成 |

---

## STAGE 4：Remotion 工程搭建与合成编排

### 4.1 项目入口文件

```typescript
// src/index.ts — 注册所有 Composition
import { registerRoot } from 'remotion';
import { RemotionRoot } from './Root';
registerRoot(RemotionRoot);
```

```tsx
// src/Root.tsx — 定义主视频 Composition
import { Composition } from 'remotion';
import { MainVideo } from './Video';
import audioManifest from './data/audio-manifest.json';
import { THEME } from './styles/theme';

export const RemotionRoot: React.FC = () => {
  const audioFrames = Math.ceil(audioManifest.totalDurationSeconds * THEME.fps);
  const segmentCount = audioManifest.segments.length;
  const silenceGapFrames = Math.round(1.5 * THEME.fps); // 段落间 1.5 秒静音
  const totalGapFrames = (segmentCount - 1) * silenceGapFrames; // 7 个间隔
  const introOutroFrames = 2 * THEME.fps + 2 * THEME.fps; // 开头 2 秒淡入 + 结尾 2 秒淡出
  const totalDuration = audioFrames + totalGapFrames + introOutroFrames;

  return (
    <Composition
      id="MainVideo"
      component={MainVideo}
      durationInFrames={totalDuration}
      fps={THEME.fps}
      width={THEME.width}
      height={THEME.height}
    />
  );
};
```

> **关键**：`totalDuration` 必须包含音频总时长 + 段落间静音间隔（7 × 1.5s = 10.5s）+ 开头淡入（2s）+ 结尾淡出（2s）。如果只按 `totalDurationSeconds` 计算，最后一段或片尾会被 Composition 截断。

### 4.2 主视频合成逻辑

```tsx
// src/Video.tsx
import { AbsoluteFill, Audio, Sequence, staticFile, interpolate, useCurrentFrame } from 'remotion';
import segments from './data/segments.json';
import audioManifest from './data/audio-manifest.json';

export const MainVideo: React.FC = () => {
  const frame = useCurrentFrame();
  const fps = 30;
  const introFrames = 2 * fps;  // 开头 2 秒淡入
  const outroFrames = 2 * fps;  // 结尾 2 秒淡出
  const silenceGap = Math.round(1.5 * fps); // 段落间 1.5 秒静音
  let frameOffset = introFrames; // 从淡入结束后开始排列段落

  // 开头淡入
  const introOpacity = interpolate(frame, [0, introFrames], [0, 1], { extrapolateRight: 'clamp' });

  // 计算总内容结束帧（用于结尾淡出）
  const allSegmentFrames = audioManifest.segments.reduce(
    (sum, seg) => sum + Math.ceil(seg.durationSeconds * fps), 0
  );
  const totalContentEnd = introFrames + allSegmentFrames + (audioManifest.segments.length - 1) * silenceGap;
  const outroOpacity = interpolate(frame, [totalContentEnd, totalContentEnd + outroFrames], [1, 0], {
    extrapolateLeft: 'clamp', extrapolateRight: 'clamp',
  });

  return (
    <AbsoluteFill style={{ backgroundColor: '#0a0a0f', opacity: Math.min(introOpacity, outroOpacity) }}>
      {/* 背景音乐（可选，音量极低） */}
      {/* <Audio src={staticFile("bgm.mp3")} volume={0.06} /> */}

      {audioManifest.segments.map((audioSeg, index) => {
        const durationInFrames = Math.ceil(audioSeg.durationSeconds * fps);
        const segment = segments.segments[index];

        const result = (
          <Sequence from={frameOffset} durationInFrames={durationInFrames} key={audioSeg.id}>
            {/* 段落音频 */}
            <Audio src={staticFile(audioSeg.audioFile)} />

            {/* 视觉素材层 —— 根据 segment.assets 决定显示哪些组件 */}
            <SegmentVisuals segment={segment} durationInFrames={durationInFrames} />

            {/* 字幕覆盖层 */}
            <SubtitleOverlay vttFile={audioSeg.subtitleFile} />
          </Sequence>
        );

        frameOffset += durationInFrames + silenceGap;
        return result;
      })}
    </AbsoluteFill>
  );
};
```

### 4.3 段落内素材分配时序

```typescript
// src/timing.ts
// 每个段落内有多个素材，需要将段落总时长分配给各个素材

interface AssetTiming {
  assetId: number;
  startFrame: number;    // 相对于段落起点
  durationFrames: number;
  component: string;     // 组件名
}

function computeAssetTimings(segment: Segment, totalFrames: number): AssetTiming[] {
  const assets = segment.assets;
  if (assets.length === 0) return [];
  if (assets.length === 1) {
    return [{ assetId: assets[0].id, startFrame: 0, durationFrames: totalFrames, component: getComponentName(assets[0]) }];
  }

  // 多素材：按素材描述文字长度成比例分配时间
  // 最少每个素材 2 秒（60 帧）
  const minFrames = 60;
  const transitionFrames = 15; // 0.5 秒交叉淡入淡出

  const weights = assets.map(a => Math.max(a.description.length, 20));
  const totalWeight = weights.reduce((a, b) => a + b, 0);

  let remaining = totalFrames;
  const timings: AssetTiming[] = [];
  let cursor = 0;

  assets.forEach((asset, i) => {
    let frames = Math.max(minFrames, Math.round((weights[i] / totalWeight) * totalFrames));
    if (i === assets.length - 1) frames = remaining; // 最后一个取剩余
    remaining -= frames;

    timings.push({
      assetId: asset.id,
      startFrame: cursor,
      durationFrames: frames,
      component: getComponentName(asset),
    });
    cursor += frames;
  });

  return timings;
}
```

### 4.4 SegmentVisuals 组件

```tsx
// src/components/SegmentVisuals.tsx
// 根据 segment 的 assets 列表，用 <Sequence> 按计算好的时序渲染各素材组件

const SegmentVisuals: React.FC<{ segment: Segment; durationInFrames: number }> = ({ segment, durationInFrames }) => {
  const timings = computeAssetTimings(segment, durationInFrames);

  return (
    <AbsoluteFill>
      {timings.map((t) => (
        <Sequence from={t.startFrame} durationInFrames={t.durationFrames} key={t.assetId}>
          <AssetComponent id={t.assetId} durationInFrames={t.durationFrames} />
        </Sequence>
      ))}
    </AbsoluteFill>
  );
};
```

### 4.5 转场效果

段落内素材之间：0.5 秒交叉淡入淡出（通过 `interpolate` 控制 opacity）

段落之间：1.5 秒黑屏静音间隔

开头：2 秒从黑屏淡入（`introFrames = 2 * fps`，在第一个 Sequence 之前）

结尾：2 秒淡出到黑屏（`outroFrames = 2 * fps`，在最后一个 Sequence 之后）

> **重要**：这些额外帧已计入 Root.tsx 的 `totalDuration`。Video.tsx 中的 `frameOffset` 应从 `introFrames` 开始，确保所有 Sequence 不超出 Composition 总时长。

---

## STAGE 5：渲染输出

### 5.1 预览验证（可选但推荐）

```bash
cd ~/teaching-video

# 启动 Remotion Studio 预览（如果有桌面环境）
npx remotion studio

# 或者渲染单帧静态图来验证
npx remotion still MainVideo --frame=0 --output=test_frame_0.png
npx remotion still MainVideo --frame=900 --output=test_frame_30s.png
npx remotion still MainVideo --frame=5400 --output=test_frame_3min.png
```

验证检查：
- 图片不是全黑
- 中文正确显示（不是方框）
- 布局正常（不是元素堆叠溢出）

### 5.2 完整渲染

```bash
mkdir -p ~/teaching-video/output

npx remotion render MainVideo \
  --output output/final.mp4 \
  --codec h264 \
  --audio-codec aac \
  --concurrency 50% \
  --timeout 120000 \
  --log verbose \
  2>&1 | tee output/render.log
```

**参数说明**：
- `--concurrency 50%`：使用 50% CPU 核心并行渲染帧，防止 OOM
- `--timeout 120000`：每帧最多 120 秒超时（复杂动画可能需要较长时间）
- `--log verbose`：详细日志，便于排查问题

### 5.3 预计渲染时间

- 10 分钟视频 = 18000 帧（30fps）
- 4 核 CPU：约 30-60 分钟
- 8 核 CPU：约 15-30 分钟
- 16 核 CPU：约 8-15 分钟

### 5.4 错误处理

| 错误 | 恢复策略 |
|------|----------|
| Chromium 启动失败 | 设置环境变量 `PUPPETEER_EXECUTABLE_PATH=$(which chromium-browser)`，或添加 `--disable-gpu --no-sandbox` |
| OOM (Out of Memory) | 降低 `--concurrency` 为 `25%` 或 `1`；如果仍然 OOM，分段渲染（见 5.5） |
| 单帧超时 | 检查该帧对应的组件是否有无限循环或过大的 DOM；增加 `--timeout` 到 180000 |
| 渲染中途中断 | 使用 `--frames` 从断点续渲（需要知道已完成的帧数），最后用 ffmpeg 拼接 |
| 编译错误 | 查看 `render.log` 定位 TypeScript/React 报错，修复后重新渲染 |

### 5.5 分段渲染备选方案

如果完整渲染失败（OOM 或超时），改用分段策略：

```bash
# 计算总帧数
TOTAL=$(node -e "console.log(Math.ceil(require('./src/data/audio-manifest.json').totalDurationSeconds * 30))")

# 每段 3000 帧（约 100 秒）
CHUNK=3000
for ((i=0; i<TOTAL; i+=CHUNK)); do
  END=$((i + CHUNK - 1))
  if [ $END -ge $TOTAL ]; then END=$((TOTAL - 1)); fi
  npx remotion render MainVideo \
    --output "output/chunk_${i}.mp4" \
    --frames="${i}-${END}" \
    --codec h264 --audio-codec aac \
    --concurrency 1
done

# 生成 concat 列表
ls output/chunk_*.mp4 | sort -V | sed 's/^/file /' > output/chunks.txt

# 拼接
ffmpeg -f concat -safe 0 -i output/chunks.txt -c copy output/final.mp4
```

---

## STAGE 6：后处理与校验

### 6.1 音频标准化

```bash
ffmpeg -i output/final.mp4 \
  -af loudnorm=I=-16:TP=-1.5:LRA=11 \
  -c:v copy \
  output/final_normalized.mp4
```

### 6.2 质量校验检查清单

Agent 必须逐项执行验证：

```bash
# 1. 文件存在且大小合理（>10MB）
ls -lh output/final_normalized.mp4

# 2. 获取视频信息
ffprobe -v quiet -print_format json -show_format -show_streams output/final_normalized.mp4

# 验证项：
#   - 视频流：codec=h264, width=1920, height=1080, fps≈30
#   - 音频流：codec=aac, channels=2 或 1
#   - 时长：600-780 秒（10-13 分钟）

# 3. 在 5 个均匀间隔的时间点截图，确认画面不是纯黑
DURATION=$(ffprobe -v quiet -show_entries format=duration -of csv=p=0 output/final_normalized.mp4)
for i in 1 2 3 4 5; do
  TIMESTAMP=$(echo "$DURATION $i" | awk '{printf "%.0f", $1 * $2 / 6}')
  ffmpeg -ss $TIMESTAMP -i output/final_normalized.mp4 -frames:v 1 -q:v 2 "output/check_${i}.jpg" -y
done

# 4. 检查截图文件大小（>5KB 说明不是纯黑帧）
for f in output/check_*.jpg; do
  SIZE=$(stat -c%s "$f")
  if [ $SIZE -lt 5000 ]; then
    echo "WARNING: $f may be a blank frame (${SIZE} bytes)"
  fi
done
```

### 6.3 校验失败的处理

| 问题 | 处理 |
|------|------|
| 时长偏差 >15% | 回到 Stage 4 检查 timing 计算逻辑，确认 audio-manifest.json 中的时长数据 |
| 截图全黑 | 回到 Stage 3 检查对应时间点的组件，用 `npx remotion still` 单独渲染该帧 |
| 无音频流 | 检查 public/audio/ 目录下的 MP3 文件是否存在且格式正确 |
| 文件过小（<10MB） | 渲染可能中途失败，检查 render.log 末尾错误信息 |

### 6.4 最终输出

```
~/teaching-video/output/
  ├── final.mp4                 # 原始渲染
  ├── final_normalized.mp4      # 音频标准化后（最终交付物）
  ├── render.log                # 渲染日志
  ├── check_1.jpg ~ check_5.jpg # 质量抽帧截图
  └── stage-6-status.json       # 最终状态
```

---

## 附录 A：完整文件清单

```
~/teaching-video/
  ├── remotion.config.ts
  ├── tsconfig.json
  ├── package.json
  ├── src/
  │   ├── index.ts
  │   ├── Root.tsx
  │   ├── Video.tsx
  │   ├── parse-script.ts
  │   ├── timing.ts
  │   ├── styles/
  │   │   └── theme.ts
  │   ├── components/
  │   │   ├── TextCard.tsx
  │   │   ├── BillingMock.tsx
  │   │   ├── TaskManagerMock.tsx
  │   │   ├── PricingComparison.tsx
  │   │   ├── ChatSimulation.tsx
  │   │   ├── ArchitectureDiagram.tsx
  │   │   ├── TokenBucketAnimation.tsx
  │   │   ├── CodeGeneration.tsx
  │   │   ├── TestResults.tsx
  │   │   ├── ModuleTimeline.tsx
  │   │   ├── BugComparison.tsx
  │   │   ├── NetMonitorDemo.tsx
  │   │   ├── TimeComparison.tsx
  │   │   ├── AIInternAnalogy.tsx
  │   │   ├── SegmentVisuals.tsx
  │   │   └── SubtitleOverlay.tsx
  │   └── data/
  │       ├── TEACHING_SCRIPT_STORY.md   # Stage 0.4.1 从源仓库复制
  │       ├── TEACHING_VISUAL_GUIDE.md   # Stage 0.4.1 从源仓库复制
  │       ├── repo-config.json           # 源仓库路径配置
  │       ├── source-samples/            # Stage 0.4.1 从源仓库复制
  │       │   ├── TokenBucket.h
  │       │   └── DivertLimiter.h
  │       ├── segments.json              # Stage 1 输出
  │       ├── narration_S01.txt ~ S08.txt # Stage 1 输出（供 TTS 读取）
  │       └── audio-manifest.json        # Stage 2 输出
  ├── public/
  │   └── audio/
  │       ├── S01.mp3 ~ S08.mp3          # Stage 2 输出
  │       ├── S01.vtt ~ S08.vtt          # Stage 2 输出
  │       └── silence.mp3
  └── output/
      ├── final.mp4
      ├── final_normalized.mp4
      └── render.log
```

---

## 附录 B：Agent 执行命令摘要

Agent 可按以下顺序依次执行，每步检查状态后再进入下一步：

```bash
# ===== STAGE 0 =====
# 前置：确认免密 sudo 可用（见 SUDO_NOPASSWD_SETUP.md）
sudo -n true || { echo "需要免密 sudo"; exit 1; }
sudo apt update && sudo apt install -y curl wget git build-essential ffmpeg chromium-browser fonts-noto-cjk python3 python3-pip python3-venv
curl -fsSL https://deb.nodesource.com/setup_20.x | sudo -E bash - && sudo apt install -y nodejs
python3 -m venv ~/video-agent-venv && source ~/video-agent-venv/bin/activate && pip install edge-tts
npx create-video@latest ~/teaching-video --template blank-ts --no-git
cd ~/teaching-video && npm install shiki gray-matter
# 复制源仓库文件到 Remotion 项目（见 Stage 0.4.1）
export NETMONITOR_REPO_DIR="/path/to/NetMonitor"  # Agent 根据实际路径填写
mkdir -p src/data/source-samples
cp "$NETMONITOR_REPO_DIR/TEACHING_SCRIPT_STORY.md" src/data/
cp "$NETMONITOR_REPO_DIR/TEACHING_VISUAL_GUIDE.md" src/data/
cp "$NETMONITOR_REPO_DIR/src/core/TokenBucket.h" src/data/source-samples/
cp "$NETMONITOR_REPO_DIR/src/core/DivertLimiter.h" src/data/source-samples/

# ===== STAGE 1 =====
# 运行解析脚本（Agent 需先创建 parse-script.ts 并执行）
# 输出 segments.json + narration_S01.txt ~ narration_S08.txt
npx ts-node src/parse-script.ts

# ===== STAGE 2 =====
# 对每个段落运行 edge-tts（读取 Stage 1 输出的文本文件）
source ~/video-agent-venv/bin/activate
for seg in S01 S02 S03 S04 S05 S06 S07 S08; do
  edge-tts --voice "zh-CN-YunxiNeural" --text "$(cat src/data/narration_${seg}.txt)" \
    --write-media "public/audio/${seg}.mp3" --write-subtitles "public/audio/${seg}.vtt"
done
# 生成 audio-manifest.json（用 ffprobe 获取每段时长）

# ===== STAGE 3 =====
# 创建所有组件文件（Agent 需编写每个 .tsx 文件）

# ===== STAGE 4 =====
# 创建 index.ts, Root.tsx, Video.tsx, timing.ts
# 验证编译通过
npx remotion studio  # 或 npx remotion still MainVideo --frame=0

# ===== STAGE 5 =====
npx remotion render MainVideo --output output/final.mp4 --codec h264 --audio-codec aac --concurrency 50%

# ===== STAGE 6 =====
ffmpeg -i output/final.mp4 -af loudnorm -c:v copy output/final_normalized.mp4
# 运行校验检查
```

---

## 附录 C：常见环境问题速查

| 症状 | 原因 | 解决 |
|------|------|------|
| `error while loading shared libraries: libnss3.so` | Chromium 缺少系统库 | `sudo apt install -y libnss3 libatk-bridge2.0-0 libdrm2 libxcomposite1 libxdamage1 libxrandr2 libgbm1 libpango-1.0-0 libcairo2 libasound2` |
| `FATAL:zygote_host_impl_linux.cc` | Chromium 沙箱问题 | 在 `remotion.config.ts` 中添加 `Config.setChromiumOpenGlRenderer("swangle")` 或设置环境变量 `PUPPETEER_CHROMIUM_REVISION=latest` |
| 渲染输出无声音 | MP3 文件路径错误 | 确认 `staticFile()` 的路径相对于 `public/` 目录 |
| 中文字幕乱码 | VTT 文件编码问题 | 确保 VTT 文件是 UTF-8 编码：`file public/audio/S01.vtt` |
| `JavaScript heap out of memory` | Node.js 内存不足 | `export NODE_OPTIONS="--max-old-space-size=8192"` |
| Remotion 版本冲突 | 依赖版本不一致 | `npx remotion upgrade` 统一所有 remotion 包版本 |

---

*本文档版本：v1.0 | 适用于 Ubuntu 22.04 + Remotion 4.x + edge-tts*
