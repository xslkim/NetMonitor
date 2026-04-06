# 视频制作 Agent 状态追踪文档

> **本文档是 Agent 的断点续跑控制器。Agent 启动时必须先读取 `pipeline-state.json`，跳过已完成的任务，从第一个未完成的任务继续执行。**

---

## 1. 状态文件

路径：`~/teaching-video/pipeline-state.json`

Agent 每完成一个任务后**立即**写入状态文件。状态文件是整个流水线的单一事实来源。

### 1.1 状态文件 Schema

```json
{
  "version": 2,
  "startedAt": "2026-04-06T10:00:00+08:00",
  "updatedAt": "2026-04-06T10:35:22+08:00",
  "config": {
    "repoDir": "/path/to/NetMonitor",
    "projectDir": "/home/user/teaching-video",
    "voice": "zh-CN-YunxiNeural"
  },
  "tasks": {
    "T00_sudo_check":        { "status": "completed", "ts": "...", "note": "" },
    "T01_apt_install":       { "status": "completed", "ts": "...", "note": "" },
    "T02_nodejs_install":    { "status": "skipped",   "ts": "...", "note": "already installed v20.18.0" },
    "T03_python_venv":       { "status": "completed", "ts": "...", "note": "" },
    "T04_remotion_init":     { "status": "completed", "ts": "...", "note": "" },
    "T05_copy_sources":      { "status": "completed", "ts": "...", "note": "" },
    "T06_env_verify":        { "status": "completed", "ts": "...", "note": "" },
    "T10_parse_script":      { "status": "completed", "ts": "...", "note": "" },
    "T20_tts_S01":           { "status": "completed", "ts": "...", "note": "28.5s" },
    "T20_tts_S02":           { "status": "completed", "ts": "...", "note": "22.1s" },
    "T20_tts_S03":           { "status": "error",     "ts": "...", "note": "edge-tts timeout, retry 1/3", "retries": 1 },
    "T20_tts_S04":           { "status": "pending",   "ts": "",    "note": "" },
    "...": "..."
  }
}
```

### 1.2 任务状态定义

| 状态 | 含义 | Agent 行为 |
|------|------|-----------|
| `pending` | 未开始 | 检查前置任务是否完成，满足则执行 |
| `running` | 正在执行 | 如果 Agent 重启时发现此状态，视为上次被中断，重新执行 |
| `completed` | 成功完成 | 跳过，不重复执行 |
| `error` | 执行失败 | 检查 `retries` 次数，未超限则重试，超限则报错停止 |
| `skipped` | 主动跳过 | 前置已满足或环境已预装，跳过 |

### 1.3 Agent 启动逻辑（伪代码）

```
1. 读取 ~/teaching-video/pipeline-state.json
   - 如果不存在：初始化全部任务为 pending，写入文件
   - 如果存在：加载现有状态

2. 按任务 ID 排序，遍历每个任务：
   a. status == completed 或 skipped → 跳过
   b. status == running → 视为中断，设为 pending 后重新执行
   c. status == error → 检查 retries < 3，是则重试，否则停止并报告
   d. status == pending → 检查 depends_on 中所有前置任务是否 completed/skipped
      - 是：执行任务
      - 否：跳过（等待并行分支完成后再回来）

3. 任务执行前：设 status = running，写入状态文件
4. 任务执行后：设 status = completed（或 error），写入状态文件
5. 全部任务完成后：输出总结报告
```

---

## 2. 完整任务清单

共 **42 个任务**，按阶段和依赖关系排列。

### STAGE 0：环境搭建（T00 ~ T06）

| 任务 ID | 名称 | 依赖 | 验证方式 | 产出物 |
|---------|------|------|---------|--------|
| `T00_sudo_check` | 检测免密 sudo | 无 | `sudo -n true` 返回 0 | 无 |
| `T01_apt_install` | 安装系统包 | T00 | `which ffmpeg && which chromium-browser && fc-list :lang=zh` | ffmpeg, chromium, 字体 |
| `T02_nodejs_install` | 安装 Node.js 20 | T00 | `node -v` 输出 v20.x | node, npm |
| `T03_python_venv` | 创建 Python 虚拟环境 + edge-tts | T00 | `source ~/video-agent-venv/bin/activate && edge-tts --version` | ~/video-agent-venv |
| `T04_remotion_init` | 初始化 Remotion 项目 + 安装依赖 | T02 | `cd ~/teaching-video && npx remotion --version` | ~/teaching-video |
| `T05_copy_sources` | 复制源仓库文件到项目 | T04 | `test -f src/data/TEACHING_SCRIPT_STORY.md && test -f src/data/source-samples/TokenBucket.h` | src/data/*.md, source-samples/ |
| `T06_env_verify` | 环境验证检查清单 | T01, T02, T03, T04, T05 | 全部 7 项检查通过 | 无 |

### STAGE 1：脚本解析（T10 ~ T11）

| 任务 ID | 名称 | 依赖 | 验证方式 | 产出物 |
|---------|------|------|---------|--------|
| `T10_parse_script` | 创建并运行 parse-script.ts | T06 | `jq '.segments | length' src/data/segments.json` 输出 8 | segments.json, narration_S01~S08.txt |
| `T11_parse_verify` | 验证解析结果 | T10 | 8 段、17 素材全覆盖、总字数 1800-2200 | 无 |

### STAGE 2：语音合成（T20 ~ T25）—— 与 STAGE 3 可并行

| 任务 ID | 名称 | 依赖 | 验证方式 | 产出物 |
|---------|------|------|---------|--------|
| `T20_tts_S01` | 生成 S01 音频 | T11 | `test -f public/audio/S01.mp3 && test -s public/audio/S01.mp3` | S01.mp3, S01.vtt |
| `T20_tts_S02` | 生成 S02 音频 | T11 | 同上 | S02.mp3, S02.vtt |
| `T20_tts_S03` | 生成 S03 音频 | T11 | 同上 | S03.mp3, S03.vtt |
| `T20_tts_S04` | 生成 S04 音频 | T11 | 同上 | S04.mp3, S04.vtt |
| `T20_tts_S05` | 生成 S05 音频 | T11 | 同上 | S05.mp3, S05.vtt |
| `T20_tts_S06` | 生成 S06 音频 | T11 | 同上 | S06.mp3, S06.vtt |
| `T20_tts_S07` | 生成 S07 音频 | T11 | 同上 | S07.mp3, S07.vtt |
| `T20_tts_S08` | 生成 S08 音频 | T11 | 同上 | S08.mp3, S08.vtt |
| `T21_silence` | 生成静音文件 | T11 | `test -f public/audio/silence.mp3` | silence.mp3 |
| `T22_audio_manifest` | 生成 audio-manifest.json | T20_tts_S01 ~ S08 | `jq '.segments | length' src/data/audio-manifest.json` 输出 8 | audio-manifest.json |
| `T23_vtt_to_json` | VTT 字幕预解析为 JSON | T20_tts_S01 ~ S08 | `test -f src/data/subtitles_S01.json`（全 8 个） | subtitles_S01~S08.json |

### STAGE 3：视觉素材生成（T30 ~ T3F）—— 与 STAGE 2 可并行

| 任务 ID | 名称 | 依赖 | 验证方式 | 产出物 |
|---------|------|------|---------|--------|
| `T30_theme` | 创建 theme.ts 全局样式 | T06 | `test -f src/styles/theme.ts` | theme.ts |
| `T30_code_highlight` | 预生成代码高亮 JSON | T05 | `test -f src/data/highlighted-code.json` | highlighted-code.json |
| `T31_TextCard` | 素材 4/12/16/17 — 通用文字卡 | T30_theme | `test -f src/components/TextCard.tsx` | TextCard.tsx |
| `T31_BillingMock` | 素材 1 — 云账单模拟 | T30_theme | `test -f src/components/BillingMock.tsx` | BillingMock.tsx |
| `T31_TaskManagerMock` | 素材 2 — 任务管理器动画 | T30_theme | `test -f src/components/TaskManagerMock.tsx` | TaskManagerMock.tsx |
| `T31_PricingComparison` | 素材 3 — 工具定价对比 | T30_theme | `test -f src/components/PricingComparison.tsx` | PricingComparison.tsx |
| `T31_ChatSimulation` | 素材 5 — AI 对话模拟 | T30_theme | `test -f src/components/ChatSimulation.tsx` | ChatSimulation.tsx |
| `T31_ArchitectureDiagram` | 素材 6 — 三层架构动画 | T30_theme | `test -f src/components/ArchitectureDiagram.tsx` | ArchitectureDiagram.tsx |
| `T31_TokenBucket` | 素材 7 — 令牌桶动画 | T30_theme | `test -f src/components/TokenBucketAnimation.tsx` | TokenBucketAnimation.tsx |
| `T31_CodeGeneration` | 素材 8 — 代码生成快进 | T30_theme, T30_code_highlight | `test -f src/components/CodeGeneration.tsx` | CodeGeneration.tsx |
| `T31_TestResults` | 素材 9 — 测试结果终端 | T30_theme | `test -f src/components/TestResults.tsx` | TestResults.tsx |
| `T31_ModuleTimeline` | 素材 10 — 模块时间线 | T30_theme | `test -f src/components/ModuleTimeline.tsx` | ModuleTimeline.tsx |
| `T31_BugComparison` | 素材 11 — 踩坑分屏对比 | T30_theme | `test -f src/components/BugComparison.tsx` | BugComparison.tsx |
| `T31_NetMonitorDemo` | 素材 13 — 操作演示模拟 | T30_theme | `test -f src/components/NetMonitorDemo.tsx` | NetMonitorDemo.tsx |
| `T31_TimeComparison` | 素材 14 — 时间对比 ×50 | T30_theme | `test -f src/components/TimeComparison.tsx` | TimeComparison.tsx |
| `T31_AIInternAnalogy` | 素材 15 — AI 实习生比喻 | T30_theme | `test -f src/components/AIInternAnalogy.tsx` | AIInternAnalogy.tsx |
| `T31_SubtitleOverlay` | 字幕覆盖层 | T30_theme | `test -f src/components/SubtitleOverlay.tsx` | SubtitleOverlay.tsx |

### STAGE 4：Remotion 工程编排（T40 ~ T43）

| 任务 ID | 名称 | 依赖 | 验证方式 | 产出物 |
|---------|------|------|---------|--------|
| `T40_timing` | 创建 timing.ts（时序分配 + 组件映射） | T31_* 全部完成 | `test -f src/timing.ts` | timing.ts |
| `T41_segment_visuals` | 创建 SegmentVisuals.tsx | T40 | `test -f src/components/SegmentVisuals.tsx` | SegmentVisuals.tsx |
| `T42_video_root` | 创建 Root.tsx + Video.tsx + index.ts | T41, T22, T23 | `test -f src/Root.tsx && test -f src/Video.tsx` | Root.tsx, Video.tsx, index.ts |
| `T43_compile_check` | TypeScript 编译验证 + 单帧渲染测试 | T42 | `npx remotion still MainVideo --frame=0 --output=test_frame.png` 成功 | test_frame.png |

### STAGE 5：渲染输出（T50 ~ T51）

| 任务 ID | 名称 | 依赖 | 验证方式 | 产出物 |
|---------|------|------|---------|--------|
| `T50_preview_frames` | 渲染 3 个关键帧验证 | T43 | 3 张 PNG 非纯黑、中文正常 | test_frame_*.png |
| `T51_full_render` | 完整视频渲染 | T50 | `test -f output/final.mp4 && test $(stat -c%s output/final.mp4) -gt 10000000` | output/final.mp4 |

### STAGE 6：后处理与校验（T60 ~ T62）

| 任务 ID | 名称 | 依赖 | 验证方式 | 产出物 |
|---------|------|------|---------|--------|
| `T60_normalize` | 音频标准化 | T51 | `test -f output/final_normalized.mp4` | final_normalized.mp4 |
| `T61_quality_check` | 质量校验（分辨率、时长、非黑帧） | T60 | ffprobe 输出 1920x1080, 600-780s | 校验报告 |
| `T62_summary` | 生成完成报告 | T61 | 报告文件存在 | pipeline-report.txt |

---

## 3. 任务依赖关系图

```
T00 ─┬─ T01 ──────────────┐
     ├─ T02 ── T04 ── T05 ├── T06 ── T10 ── T11 ─┬── T20_tts_S01~S08 ── T22 ─┐
     └─ T03 ──────────────┘                        │   T21                      │
                                                    │   T20_* ──────── T23 ──── ├── T42 ── T43 ── T50 ── T51 ── T60 ── T61 ── T62
                                                    │                           │
                                                    └── T30_theme ── T31_* ─── T40 ── T41 ┘
                                                        T30_code_highlight ─┘
```

**关键并行点**：
- T01, T02, T03 可并行（都只依赖 T00）
- **STAGE 2 (T20~T23) 和 STAGE 3 (T30~T31) 可完全并行**
- T20_tts_S01 ~ S08 八段 TTS 可并行
- T31 的 14 个组件可并行创建

---

## 4. 状态文件操作脚本

Agent 在执行过程中使用以下函数操作状态文件：

### 4.1 初始化状态文件

```bash
#!/bin/bash
# init-pipeline-state.sh
# Agent 首次启动时运行，生成包含全部任务的初始状态文件

STATE_FILE=~/teaching-video/pipeline-state.json
REPO_DIR="${1:?Usage: $0 <NetMonitor-repo-path>}"

cat > "$STATE_FILE" << 'TEMPLATE'
{
  "version": 2,
  "startedAt": "TIMESTAMP",
  "updatedAt": "TIMESTAMP",
  "config": {
    "repoDir": "REPO_DIR_PLACEHOLDER",
    "projectDir": "HOME_PLACEHOLDER/teaching-video",
    "voice": "zh-CN-YunxiNeural"
  },
  "tasks": {}
}
TEMPLATE

# 填充实际值
sed -i "s|TIMESTAMP|$(date -Iseconds)|g" "$STATE_FILE"
sed -i "s|REPO_DIR_PLACEHOLDER|$REPO_DIR|g" "$STATE_FILE"
sed -i "s|HOME_PLACEHOLDER|$HOME|g" "$STATE_FILE"

# 任务列表（ID:依赖）
TASKS=(
  "T00_sudo_check:"
  "T01_apt_install:T00_sudo_check"
  "T02_nodejs_install:T00_sudo_check"
  "T03_python_venv:T00_sudo_check"
  "T04_remotion_init:T02_nodejs_install"
  "T05_copy_sources:T04_remotion_init"
  "T06_env_verify:T01_apt_install,T02_nodejs_install,T03_python_venv,T04_remotion_init,T05_copy_sources"
  "T10_parse_script:T06_env_verify"
  "T11_parse_verify:T10_parse_script"
  "T20_tts_S01:T11_parse_verify"
  "T20_tts_S02:T11_parse_verify"
  "T20_tts_S03:T11_parse_verify"
  "T20_tts_S04:T11_parse_verify"
  "T20_tts_S05:T11_parse_verify"
  "T20_tts_S06:T11_parse_verify"
  "T20_tts_S07:T11_parse_verify"
  "T20_tts_S08:T11_parse_verify"
  "T21_silence:T11_parse_verify"
  "T22_audio_manifest:T20_tts_S01,T20_tts_S02,T20_tts_S03,T20_tts_S04,T20_tts_S05,T20_tts_S06,T20_tts_S07,T20_tts_S08"
  "T23_vtt_to_json:T20_tts_S01,T20_tts_S02,T20_tts_S03,T20_tts_S04,T20_tts_S05,T20_tts_S06,T20_tts_S07,T20_tts_S08"
  "T30_theme:T06_env_verify"
  "T30_code_highlight:T05_copy_sources"
  "T31_TextCard:T30_theme"
  "T31_BillingMock:T30_theme"
  "T31_TaskManagerMock:T30_theme"
  "T31_PricingComparison:T30_theme"
  "T31_ChatSimulation:T30_theme"
  "T31_ArchitectureDiagram:T30_theme"
  "T31_TokenBucket:T30_theme"
  "T31_CodeGeneration:T30_theme,T30_code_highlight"
  "T31_TestResults:T30_theme"
  "T31_ModuleTimeline:T30_theme"
  "T31_BugComparison:T30_theme"
  "T31_NetMonitorDemo:T30_theme"
  "T31_TimeComparison:T30_theme"
  "T31_AIInternAnalogy:T30_theme"
  "T31_SubtitleOverlay:T30_theme"
  "T40_timing:T31_TextCard,T31_BillingMock,T31_TaskManagerMock,T31_PricingComparison,T31_ChatSimulation,T31_ArchitectureDiagram,T31_TokenBucket,T31_CodeGeneration,T31_TestResults,T31_ModuleTimeline,T31_BugComparison,T31_NetMonitorDemo,T31_TimeComparison,T31_AIInternAnalogy,T31_SubtitleOverlay"
  "T41_segment_visuals:T40_timing"
  "T42_video_root:T41_segment_visuals,T22_audio_manifest,T23_vtt_to_json"
  "T43_compile_check:T42_video_root"
  "T50_preview_frames:T43_compile_check"
  "T51_full_render:T50_preview_frames"
  "T60_normalize:T51_full_render"
  "T61_quality_check:T60_normalize"
  "T62_summary:T61_quality_check"
)

# 用 node 构建 JSON（避免 bash JSON 转义问题）
node -e "
const fs = require('fs');
const state = JSON.parse(fs.readFileSync('$STATE_FILE', 'utf-8'));
const tasks = $(printf '%s\n' "${TASKS[@]}" | jq -R -s 'split("\n") | map(select(. != ""))');
const taskObj = {};
for (const entry of tasks) {
  const [id, deps] = entry.split(':');
  taskObj[id] = {
    status: 'pending',
    ts: '',
    note: '',
    retries: 0,
    depends_on: deps ? deps.split(',') : []
  };
}
state.tasks = taskObj;
fs.writeFileSync('$STATE_FILE', JSON.stringify(state, null, 2));
console.log('Initialized', Object.keys(taskObj).length, 'tasks');
"
```

### 4.2 更新任务状态

```bash
#!/bin/bash
# update-task.sh <task_id> <status> [note]
# 用法：./update-task.sh T20_tts_S03 completed "28.5s"
#       ./update-task.sh T20_tts_S04 error "edge-tts timeout"

STATE_FILE=~/teaching-video/pipeline-state.json
TASK_ID="$1"
STATUS="$2"
NOTE="${3:-}"

node -e "
const fs = require('fs');
const state = JSON.parse(fs.readFileSync('$STATE_FILE', 'utf-8'));
if (!state.tasks['$TASK_ID']) {
  console.error('Unknown task: $TASK_ID');
  process.exit(1);
}
const task = state.tasks['$TASK_ID'];
task.status = '$STATUS';
task.ts = new Date().toISOString();
task.note = '$NOTE';
if ('$STATUS' === 'error') task.retries = (task.retries || 0) + 1;
state.updatedAt = new Date().toISOString();
fs.writeFileSync('$STATE_FILE', JSON.stringify(state, null, 2));
console.log('$TASK_ID → $STATUS');
"
```

### 4.3 查询可执行任务

```bash
#!/bin/bash
# next-tasks.sh — 输出当前所有可以执行的任务（前置已完成且自身为 pending）

STATE_FILE=~/teaching-video/pipeline-state.json

node -e "
const fs = require('fs');
const state = JSON.parse(fs.readFileSync('$STATE_FILE', 'utf-8'));
const ready = [];
for (const [id, task] of Object.entries(state.tasks)) {
  if (task.status !== 'pending' && task.status !== 'running') continue;
  const deps = task.depends_on || [];
  const allMet = deps.every(d => {
    const dep = state.tasks[d];
    return dep && (dep.status === 'completed' || dep.status === 'skipped');
  });
  if (allMet) ready.push(id);
}
if (ready.length === 0) {
  const errors = Object.entries(state.tasks).filter(([,t]) => t.status === 'error');
  if (errors.length > 0) {
    console.log('BLOCKED by errors:', errors.map(([id, t]) => id + '(' + t.note + ')').join(', '));
  } else {
    console.log('ALL DONE');
  }
} else {
  console.log('Ready tasks:', ready.join(', '));
}
"
```

### 4.4 生成进度摘要

```bash
#!/bin/bash
# progress.sh — 输出当前进度

STATE_FILE=~/teaching-video/pipeline-state.json

node -e "
const fs = require('fs');
const state = JSON.parse(fs.readFileSync('$STATE_FILE', 'utf-8'));
const counts = { pending: 0, running: 0, completed: 0, error: 0, skipped: 0 };
for (const task of Object.values(state.tasks)) counts[task.status]++;
const total = Object.keys(state.tasks).length;
const done = counts.completed + counts.skipped;
console.log('Progress: ' + done + '/' + total + ' (' + Math.round(done/total*100) + '%)');
console.log('  completed:', counts.completed);
console.log('  skipped:  ', counts.skipped);
console.log('  running:  ', counts.running);
console.log('  error:    ', counts.error);
console.log('  pending:  ', counts.pending);
if (counts.error > 0) {
  console.log('\nErrors:');
  for (const [id, t] of Object.entries(state.tasks)) {
    if (t.status === 'error') console.log('  ' + id + ': ' + t.note + ' (retries: ' + t.retries + ')');
  }
}
"
```

---

## 5. Agent 执行规则

### 5.1 写状态的时机

```
每个任务的执行流程：
  1. 设 status = "running"，写入状态文件
  2. 执行任务
  3. 运行验证命令
  4. 验证通过 → 设 status = "completed"，写入状态文件
     验证失败 → 设 status = "error" + 错误信息，写入状态文件
```

### 5.2 重试策略

- 最大重试次数：3
- `retries` 字段记录已重试次数
- 超过 3 次 → Agent 停止该分支，输出错误报告，继续其他不依赖此任务的分支
- TTS 任务重试间隔：5 秒
- apt/npm 任务重试间隔：10 秒

### 5.3 跳过策略

Agent 在执行前可以先检查产出物是否已存在：
- 如果产出物已存在且通过验证 → 直接设 `skipped`，不重复执行
- 例如：`T02_nodejs_install`，如果 `node -v` 已经是 v20.x → 跳过

### 5.4 并行执行

当 `next-tasks.sh` 返回多个任务时，Agent 应尽量并行执行：
- T20_tts_S01 ~ S08 可全部并行
- T31_* 组件创建可全部并行
- **STAGE 2 整体和 STAGE 3 整体可并行**

### 5.5 致命错误处理

以下情况 Agent 应停止整个流水线：
- `T00_sudo_check` 失败（无法安装任何东西）
- `T06_env_verify` 重试 3 次后仍失败（环境不可用）
- `T43_compile_check` 重试 3 次后仍失败（代码有结构性错误）

---

## 6. 状态文件示例（中途被中断后的样子）

```json
{
  "version": 2,
  "startedAt": "2026-04-06T10:00:00+08:00",
  "updatedAt": "2026-04-06T10:42:15+08:00",
  "config": {
    "repoDir": "/home/user/NetMonitor",
    "projectDir": "/home/user/teaching-video",
    "voice": "zh-CN-YunxiNeural"
  },
  "tasks": {
    "T00_sudo_check":        { "status": "completed", "ts": "2026-04-06T10:00:05+08:00", "note": "", "retries": 0, "depends_on": [] },
    "T01_apt_install":       { "status": "completed", "ts": "2026-04-06T10:02:30+08:00", "note": "", "retries": 0, "depends_on": ["T00_sudo_check"] },
    "T02_nodejs_install":    { "status": "skipped",   "ts": "2026-04-06T10:00:06+08:00", "note": "v20.18.0 already installed", "retries": 0, "depends_on": ["T00_sudo_check"] },
    "T03_python_venv":       { "status": "completed", "ts": "2026-04-06T10:01:15+08:00", "note": "", "retries": 0, "depends_on": ["T00_sudo_check"] },
    "T04_remotion_init":     { "status": "completed", "ts": "2026-04-06T10:03:45+08:00", "note": "", "retries": 0, "depends_on": ["T02_nodejs_install"] },
    "T05_copy_sources":      { "status": "completed", "ts": "2026-04-06T10:03:50+08:00", "note": "", "retries": 0, "depends_on": ["T04_remotion_init"] },
    "T06_env_verify":        { "status": "completed", "ts": "2026-04-06T10:04:00+08:00", "note": "7/7 checks passed", "retries": 0, "depends_on": ["T01_apt_install","T02_nodejs_install","T03_python_venv","T04_remotion_init","T05_copy_sources"] },
    "T10_parse_script":      { "status": "completed", "ts": "2026-04-06T10:04:30+08:00", "note": "8 segments, 1987 chars", "retries": 0, "depends_on": ["T06_env_verify"] },
    "T11_parse_verify":      { "status": "completed", "ts": "2026-04-06T10:04:32+08:00", "note": "", "retries": 0, "depends_on": ["T10_parse_script"] },
    "T20_tts_S01":           { "status": "completed", "ts": "2026-04-06T10:05:10+08:00", "note": "28.5s", "retries": 0, "depends_on": ["T11_parse_verify"] },
    "T20_tts_S02":           { "status": "completed", "ts": "2026-04-06T10:05:08+08:00", "note": "22.1s", "retries": 0, "depends_on": ["T11_parse_verify"] },
    "T20_tts_S03":           { "status": "completed", "ts": "2026-04-06T10:05:15+08:00", "note": "45.3s", "retries": 0, "depends_on": ["T11_parse_verify"] },
    "T20_tts_S04":           { "status": "completed", "ts": "2026-04-06T10:05:20+08:00", "note": "38.7s", "retries": 0, "depends_on": ["T11_parse_verify"] },
    "T20_tts_S05":           { "status": "running",   "ts": "2026-04-06T10:05:22+08:00", "note": "", "retries": 0, "depends_on": ["T11_parse_verify"] },
    "T20_tts_S06":           { "status": "pending",   "ts": "", "note": "", "retries": 0, "depends_on": ["T11_parse_verify"] },
    "T20_tts_S07":           { "status": "pending",   "ts": "", "note": "", "retries": 0, "depends_on": ["T11_parse_verify"] },
    "T20_tts_S08":           { "status": "pending",   "ts": "", "note": "", "retries": 0, "depends_on": ["T11_parse_verify"] },

    "T30_theme":             { "status": "completed", "ts": "2026-04-06T10:05:00+08:00", "note": "", "retries": 0, "depends_on": ["T06_env_verify"] },
    "T30_code_highlight":    { "status": "completed", "ts": "2026-04-06T10:05:05+08:00", "note": "42 lines", "retries": 0, "depends_on": ["T05_copy_sources"] },
    "T31_TextCard":          { "status": "completed", "ts": "2026-04-06T10:08:00+08:00", "note": "", "retries": 0, "depends_on": ["T30_theme"] },
    "T31_BillingMock":       { "status": "completed", "ts": "2026-04-06T10:10:00+08:00", "note": "", "retries": 0, "depends_on": ["T30_theme"] },
    "T31_TaskManagerMock":   { "status": "completed", "ts": "2026-04-06T10:12:00+08:00", "note": "", "retries": 0, "depends_on": ["T30_theme"] },
    "T31_PricingComparison": { "status": "completed", "ts": "2026-04-06T10:14:00+08:00", "note": "", "retries": 0, "depends_on": ["T30_theme"] },
    "T31_ChatSimulation":    { "status": "completed", "ts": "2026-04-06T10:18:00+08:00", "note": "", "retries": 0, "depends_on": ["T30_theme"] },
    "T31_ArchitectureDiagram": { "status": "completed", "ts": "2026-04-06T10:22:00+08:00", "note": "", "retries": 0, "depends_on": ["T30_theme"] },
    "T31_TokenBucket":       { "status": "running",   "ts": "2026-04-06T10:25:00+08:00", "note": "", "retries": 0, "depends_on": ["T30_theme"] },
    "T31_CodeGeneration":    { "status": "pending",   "ts": "", "note": "", "retries": 0, "depends_on": ["T30_theme","T30_code_highlight"] },
    "T31_TestResults":       { "status": "pending",   "ts": "", "note": "", "retries": 0, "depends_on": ["T30_theme"] },
    "T31_ModuleTimeline":    { "status": "pending",   "ts": "", "note": "", "retries": 0, "depends_on": ["T30_theme"] },
    "T31_BugComparison":     { "status": "pending",   "ts": "", "note": "", "retries": 0, "depends_on": ["T30_theme"] },
    "T31_NetMonitorDemo":    { "status": "pending",   "ts": "", "note": "", "retries": 0, "depends_on": ["T30_theme"] },
    "T31_TimeComparison":    { "status": "pending",   "ts": "", "note": "", "retries": 0, "depends_on": ["T30_theme"] },
    "T31_AIInternAnalogy":   { "status": "pending",   "ts": "", "note": "", "retries": 0, "depends_on": ["T30_theme"] },
    "T31_SubtitleOverlay":   { "status": "pending",   "ts": "", "note": "", "retries": 0, "depends_on": ["T30_theme"] },

    "T21_silence":           { "status": "pending",   "ts": "", "note": "", "retries": 0, "depends_on": ["T11_parse_verify"] },
    "T22_audio_manifest":    { "status": "pending",   "ts": "", "note": "", "retries": 0, "depends_on": ["T20_tts_S01","T20_tts_S02","T20_tts_S03","T20_tts_S04","T20_tts_S05","T20_tts_S06","T20_tts_S07","T20_tts_S08"] },
    "T23_vtt_to_json":       { "status": "pending",   "ts": "", "note": "", "retries": 0, "depends_on": ["T20_tts_S01","T20_tts_S02","T20_tts_S03","T20_tts_S04","T20_tts_S05","T20_tts_S06","T20_tts_S07","T20_tts_S08"] },

    "T40_timing":            { "status": "pending",   "ts": "", "note": "", "retries": 0, "depends_on": ["T31_TextCard","T31_BillingMock","T31_TaskManagerMock","T31_PricingComparison","T31_ChatSimulation","T31_ArchitectureDiagram","T31_TokenBucket","T31_CodeGeneration","T31_TestResults","T31_ModuleTimeline","T31_BugComparison","T31_NetMonitorDemo","T31_TimeComparison","T31_AIInternAnalogy","T31_SubtitleOverlay"] },
    "T41_segment_visuals":   { "status": "pending",   "ts": "", "note": "", "retries": 0, "depends_on": ["T40_timing"] },
    "T42_video_root":        { "status": "pending",   "ts": "", "note": "", "retries": 0, "depends_on": ["T41_segment_visuals","T22_audio_manifest","T23_vtt_to_json"] },
    "T43_compile_check":     { "status": "pending",   "ts": "", "note": "", "retries": 0, "depends_on": ["T42_video_root"] },
    "T50_preview_frames":    { "status": "pending",   "ts": "", "note": "", "retries": 0, "depends_on": ["T43_compile_check"] },
    "T51_full_render":       { "status": "pending",   "ts": "", "note": "", "retries": 0, "depends_on": ["T50_preview_frames"] },
    "T60_normalize":         { "status": "pending",   "ts": "", "note": "", "retries": 0, "depends_on": ["T51_full_render"] },
    "T61_quality_check":     { "status": "pending",   "ts": "", "note": "", "retries": 0, "depends_on": ["T60_normalize"] },
    "T62_summary":           { "status": "pending",   "ts": "", "note": "", "retries": 0, "depends_on": ["T61_quality_check"] }
  }
}
```

**解读**：Agent 在 STAGE 2 的 S05 和 STAGE 3 的 TokenBucket 组件处被中断。重启后：
- `T20_tts_S05` 和 `T31_TokenBucket` 状态为 `running` → 重置为 `pending` 后重新执行
- `T20_tts_S06~S08` 等 `pending` 任务在前置满足后继续执行
- 所有 `completed` 的任务不会重复执行
