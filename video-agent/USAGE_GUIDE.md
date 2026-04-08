# video-agent 使用指南

> 全自动将口播稿 + 素材描述 + 代码仓库 → 成品视频（MP4）

---

## 快速开始

```bash
# 1. 准备两个输入文件（格式见 INPUT_SPEC.md）
#    - 口播稿：script.md
#    - 素材指南：visual-guide.md

# 2. 一行命令，全自动出片
~/video-agent/run.sh \
  --script ./script.md \
  --visual ./visual-guide.md \
  --repo /path/to/my-project

# 3. 输出
#    ~/teaching-video-20260407-143000/output/final_normalized.mp4
```

---

## 前置要求

### 系统环境

- **操作系统**：Ubuntu 22.04+（WSL2 亦可）
- **CPU**：4 核以上（影响渲染速度）
- **内存**：4GB+
- **磁盘**：3GB+ 可用空间
- **网络**：需要访问 edge-tts 服务（微软 TTS）

### 软件依赖

以下软件 run.sh 会自动检测并安装（需要免密 sudo），也可以手动预装：

| 软件 | 用途 | 安装命令 |
|------|------|---------|
| Node.js 20+ | Remotion 视频框架 | `curl -fsSL https://deb.nodesource.com/setup_20.x \| sudo -E bash - && sudo apt install nodejs` |
| ffmpeg | 音视频处理 | `sudo apt install ffmpeg` |
| Chromium | Remotion 渲染引擎 | `sudo apt install chromium-browser` |
| Python 3 + venv | edge-tts 运行环境 | `sudo apt install python3 python3-venv` |
| edge-tts | 中文语音合成 | `pip install edge-tts`（自动在虚拟环境中安装） |
| 中文字体 | 字幕和文字卡渲染 | `sudo apt install fonts-noto-cjk` |
| jq | JSON 处理 | `sudo apt install jq` |
| **Claude Code** | AI Agent 核心 | `npm install -g @anthropic-ai/claude-code` |

### Claude Code 配置

Claude Code 需要已登录且有可用额度：

```bash
# 确认已安装
claude --version

# 首次使用需要登录
claude
```

---

## 命令参考

```
~/video-agent/run.sh [选项]
```

### 必选参数

| 参数 | 说明 |
|------|------|
| `--script FILE` | 口播稿 Markdown 文件 |
| `--visual FILE` | 视觉素材指南 Markdown 文件 |
| `--repo DIR` | 项目 Git 仓库路径 |

### 可选参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `--out-dir DIR` | `~/teaching-video-时间戳` | 输出项目目录 |
| `--model MODEL` | `opus` | Claude 模型（`opus` 最聪明，`sonnet` 更快更便宜） |
| `--voice VOICE` | `zh-CN-YunxiNeural` | TTS 声音（见下方声音列表） |
| `--aspect RATIO` | `16:9` | 画面比例：`16:9`、`9:16`、`1:1` |
| `--source-files LIST` | 自动检测 | 要展示的代码文件，逗号分隔（相对于 repo） |
| `--max-turns N` | `200` | Claude 最大工具调用轮数 |
| `--resume` | - | 断点续跑（从上次中断处继续） |
| `--dry-run` | - | 只初始化项目，不启动 Claude |

### 常用 TTS 声音

```bash
# 查看所有中文声音
source ~/video-agent-venv/bin/activate && edge-tts --list-voices | grep zh-CN
```

| 声音 ID | 性别 | 风格 |
|---------|------|------|
| `zh-CN-YunxiNeural` | 男 | 自然，适合技术讲解 |
| `zh-CN-YunjianNeural` | 男 | 沉稳，适合叙事 |
| `zh-CN-XiaoxiaoNeural` | 女 | 亲和，适合教程 |
| `zh-CN-XiaoyiNeural` | 女 | 活泼，适合短视频 |

### 模型选择建议

| 场景 | 推荐模型 | 原因 |
|------|---------|------|
| 复杂动画素材多 | `opus` | 生成 React 动画组件需要强推理 |
| 简单文字卡为主 | `sonnet` | 更快，费用低 ~5x |
| 首次跑通验证 | `sonnet` | 先验证流程再用 opus 精修 |

---

## 工作流程

```
run.sh
  │
  ├── 1. 解析参数、创建项目目录
  ├── 2. 复制输入文件、检测代码样本
  ├── 3. 生成 video-agent-config.json + CLAUDE.md
  ├── 4. 启动 Claude Agent
  │       │
  │       ├── Stage 0: 环境检测（跳过已安装的）
  │       ├── Stage 1: 解析口播稿 → segments.json
  │       ├── Stage 2: TTS 语音合成（并行）  ─┐
  │       ├── Stage 3: 生成 React 组件（并行）─┤
  │       ├── Stage 4: Remotion 工程编排       ←┘
  │       ├── Stage 5: 渲染视频 → final.mp4
  │       └── Stage 6: 音频标准化 + 质量校验
  │
  └── 5. 输出结果
        output/final_normalized.mp4
```

**典型耗时**（8 段落、17 个素材、4 核 CPU）：

| 阶段 | 耗时 | 说明 |
|------|------|------|
| Stage 0 | 1-5 分钟 | 首次需要安装依赖 |
| Stage 1 | ~30 秒 | 文本解析 |
| Stage 2 | 1-2 分钟 | TTS 并行，取决于网络 |
| Stage 3 | 5-15 分钟 | **AI 生成组件，最耗时** |
| Stage 4 | 1-2 分钟 | 编排 + 编译检查 |
| Stage 5 | 15-60 分钟 | 渲染，取决于 CPU 核数 |
| Stage 6 | 1-2 分钟 | 后处理 |
| **总计** | **30-90 分钟** | |

---

## 输入文件准备

**详细格式规范见 `INPUT_SPEC.md`，这里给出快速检查要点。**

### 口播稿检查要点

```markdown
# 标题必须在第一行        ← ✓ 有标题

---

每行字幕不超过 20 个字。   ← ✓ 16:9 不超过 20 字
九比十六不超过 14 个字。   ← ✓ 9:16 不超过 14 字

>>> 素材切换标记            ← ✓ 可选，控制何时切换画面

这里是下一个素材的口播文字。

---                        ← ✓ 段落之间用 --- 分隔
```

### 素材指南检查要点

```markdown
---

## 第一段：段落标题        ← ✓ 与口播稿段落对应

**[截图]** 素材标题         ← ✓ 类型标记 + 标题
具体描述画面内容...        ← ✓ 描述越具体越好
要显示的数据/文案...       ← ✓ 写明具体数字和文字

**[文字卡]** 金句
> 「精确文案用引号标出」    ← ✓ 文字卡必须标明精确文案

---
```

### 常见问题

**Q: 不写 `>>>` 标记会怎样？**
A: 段落内的素材按描述长度成比例分配时间。大多数情况够用。

**Q: 每段口播文字应该多长？**
A: 中文约 200 字/分钟。一段建议 100-400 字（30秒-2分钟）。

**Q: 可以只有文字卡没有动画吗？**
A: 可以。全部用文字卡是最简单的方案，但视觉效果一般。

**Q: 代码展示必须来自 repo 吗？**
A: 不必。素材指南里可以直接写代码块，Agent 会用那段代码。

---

## 监控进度

### 查看实时日志

```bash
# 另开终端
tail -f ~/teaching-video-*/logs/agent.log
```

### 查看任务进度

```bash
bash ~/teaching-video-*/scripts/progress.sh ~/teaching-video-*/pipeline-state.json
```

输出示例：
```
进度: [████████████░░░░░░░░] 28/46 (61%)
  ✅ completed: 26
  ⏭  skipped:   2
  🔄 running:   1
  ❌ error:     0
  ⏳ pending:   17
```

### 查看可执行任务

```bash
bash ~/teaching-video-*/scripts/next-tasks.sh ~/teaching-video-*/pipeline-state.json
```

---

## 断点续跑

如果 Agent 中途中断（网络断开、手动停止、Claude 额度用完）：

```bash
# 从上次中断处继续
~/video-agent/run.sh \
  --resume \
  --out-dir ~/teaching-video-20260407-143000 \
  --script ./script.md \
  --visual ./visual-guide.md \
  --repo /path/to/repo
```

原理：
- 已完成的任务（`completed`/`skipped`）不会重新执行
- 中断时正在运行的任务（`running`）自动重置为 `pending` 并重新执行
- 失败的任务（`error`）如果重试次数 < 3 则自动重试

### 手动修复后续跑

如果某个任务反复失败，可以手动修复后标记为完成：

```bash
cd ~/teaching-video-20260407-143000

# 手动编辑/修复文件后，标记任务完成
bash scripts/update-task.sh pipeline-state.json T31_TokenBucket completed "手动修复"

# 然后续跑
~/video-agent/run.sh --resume --out-dir . ...
```

---

## 输出文件说明

```
~/teaching-video-*/
  ├── output/
  │   ├── final.mp4               # 原始渲染
  │   ├── final_normalized.mp4    # 最终成品（音频标准化后）
  │   ├── render.log              # 渲染日志
  │   ├── check_1.jpg ~ check_5.jpg  # 质量抽帧
  │   └── pipeline-report.txt     # 完成报告
  ├── pipeline-state.json         # 任务状态（可用于断点续跑）
  ├── logs/agent.log              # Agent 执行日志
  ├── public/audio/S*.mp3         # 各段语音（可单独使用）
  └── src/components/*.tsx        # 生成的 React 组件（可手动调整后重新渲染）
```

---

## 手动调整与重新渲染

如果对某个画面不满意，可以手动修改组件后只重新渲染：

```bash
cd ~/teaching-video-20260407-143000

# 1. 编辑不满意的组件
nano src/components/Asset_7.tsx

# 2. 预览单帧
npx remotion still MainVideo --frame=3000 --output=preview.png

# 3. 满意后重新渲染
npx remotion render MainVideo \
  --output output/final.mp4 \
  --codec h264 --audio-codec aac --concurrency 50%

# 4. 音频标准化
ffmpeg -i output/final.mp4 -af loudnorm=I=-16:TP=-1.5:LRA=11 -c:v copy output/final_normalized.mp4
```

---

## 故障排除

### Agent 执行相关

| 症状 | 解决 |
|------|------|
| `找不到 claude CLI` | 安装：`npm install -g @anthropic-ai/claude-code`，或设置 `export CLAUDE_BIN=/path/to/claude` |
| Agent 跑到一半停了 | 用 `--resume` 断点续跑 |
| Agent 反复重试同一个任务 | 查看 `pipeline-state.json` 中该任务的 `note` 字段了解错误原因，手动修复后标记完成 |
| 渲染成品是全黑视频 | 检查 `output/check_*.jpg` 抽帧，找到全黑的时间段，对应检查那个时间段的组件 |

### TTS 相关

| 症状 | 解决 |
|------|------|
| edge-tts 超时 | 检查网络；设置代理：`export HTTPS_PROXY=http://...` |
| 生成的音频没有声音 | 检查输入文本是否为空，或包含 edge-tts 无法处理的特殊字符 |
| 语速太快/太慢 | 修改 `video-agent-config.json` 的 `voice` 字段换个声音 |

### 渲染相关

| 症状 | 解决 |
|------|------|
| Chromium 启动失败 | `sudo apt install libnss3 libatk-bridge2.0-0 libdrm2 libxcomposite1 libxdamage1 libxrandr2 libgbm1 libpango-1.0-0 libcairo2 libasound2` |
| 内存不足 (OOM) | 设置 `export NODE_OPTIONS="--max-old-space-size=8192"`；降低并发：在 render 命令中用 `--concurrency 1` |
| 中文显示为方框 | `sudo apt install fonts-noto-cjk fonts-noto-cjk-extra && fc-cache -fv` |
| npm install 超时 | `npm config set registry https://registry.npmmirror.com` |

---

## 进阶用法

### 竖屏短视频

```bash
~/video-agent/run.sh \
  --script ./short-script.md \
  --visual ./short-visual.md \
  --repo /path/to/repo \
  --aspect 9:16 \
  --voice zh-CN-XiaoyiNeural
```

注意：竖屏模式下每行字幕上限 14 个字，写口播稿时需要调整。

### 只跑部分阶段

```bash
# 1. 先 dry-run 初始化
~/video-agent/run.sh --dry-run --out-dir ~/my-video ...

# 2. 手动标记不需要的阶段为 skipped
cd ~/my-video
bash scripts/update-task.sh pipeline-state.json T30_theme skipped "手动跳过"

# 3. 然后 resume 跑剩余部分
~/video-agent/run.sh --resume --out-dir ~/my-video ...
```

### 更换模型省钱

```bash
# 第一遍用 sonnet 跑通流程，确认结构正确
~/video-agent/run.sh --model sonnet --out-dir ~/my-video ...

# 如果某些组件质量不够，删除那个组件文件，用 opus 重新生成
rm ~/my-video/src/components/Asset_7.tsx
bash ~/my-video/scripts/update-task.sh ~/my-video/pipeline-state.json T31_Asset_7 pending "用opus重新生成"
~/video-agent/run.sh --resume --model opus --out-dir ~/my-video ...
```

### 自定义 Claude CLI 路径

```bash
export CLAUDE_BIN=/path/to/my/claude
~/video-agent/run.sh ...
```
