# 通用视频制作 Agent 工作流

> **本文档是 Agent 的执行参考。配合项目目录下的 `CLAUDE.md` 和 `video-agent-config.json` 使用。**
>
> **所有路径均相对于项目目录（`video-agent-config.json` 中的 `projectDir`）。**

---

## 全局概览

```
输入：
  ├── src/data/script.md           （口播稿，用 --- 分隔段落）
  ├── src/data/visual-guide.md     （素材指南，按段落描述视觉素材）
  └── src/data/source-samples/     （代码样本文件，用于代码展示）

输出：
  └── output/final_normalized.mp4  （最终视频）

运行环境：Ubuntu（需预装依赖或配置免密 sudo）
视频框架：Remotion (React + TypeScript)
```

### 阶段依赖关系

```
STAGE 0: 环境搭建
    │
STAGE 1: 脚本解析 ──→ segments.json + 动态任务清单
    │
    ├──→ STAGE 2: 语音合成（可并行）──→ audio/*.mp3 + audio-manifest.json
    │
    ├──→ STAGE 3: 素材生成（可并行）──→ React 组件
    │
    ▼
STAGE 4: Remotion 工程编排
    │
STAGE 5: 渲染输出
    │
STAGE 6: 后处理 + 校验
```

---

## STAGE 0：环境搭建

### 0.1 免密 sudo 检测

```bash
if ! sudo -n true 2>/dev/null; then
  echo "WARN: 免密 sudo 不可用，跳过系统包安装（假设已预装）"
  # 不阻塞，继续检测各工具是否可用
fi
```

### 0.2 系统依赖检测与安装

逐项检测，只安装缺失的：

```bash
MISSING_PKGS=()
for cmd_pkg in "curl:curl" "git:git" "jq:jq" "ffmpeg:ffmpeg" "python3:python3" "pip3:python3-pip"; do
  CMD="${cmd_pkg%%:*}"; PKG="${cmd_pkg##*:}"
  command -v "$CMD" &>/dev/null || MISSING_PKGS+=("$PKG")
done

# Chromium
command -v chromium-browser &>/dev/null || command -v chromium &>/dev/null || command -v google-chrome &>/dev/null || MISSING_PKGS+=("chromium-browser")

# 中文字体
fc-list :lang=zh 2>/dev/null | grep -qi "noto\|wqy" || MISSING_PKGS+=("fonts-noto-cjk" "fonts-noto-cjk-extra")

# python3-venv
dpkg -s python3-venv &>/dev/null 2>&1 || MISSING_PKGS+=("python3-venv")

if [ ${#MISSING_PKGS[@]} -gt 0 ] && sudo -n true 2>/dev/null; then
  sudo apt update && sudo apt install -y "${MISSING_PKGS[@]}"
  fc-cache -fv
elif [ ${#MISSING_PKGS[@]} -gt 0 ]; then
  echo "ERROR: 缺少系统包: ${MISSING_PKGS[*]}，但无 sudo 权限"
  echo "请手动安装后重试"
fi
```

### 0.3 Node.js

```bash
if ! node -v 2>/dev/null | grep -q '^v2[0-9]\.'; then
  curl -fsSL https://deb.nodesource.com/setup_20.x | sudo -E bash -
  sudo apt install -y nodejs
fi
```

### 0.4 Python + edge-tts

```bash
VENV_DIR=~/video-agent-venv
if [ ! -f "$VENV_DIR/bin/activate" ]; then
  python3 -m venv "$VENV_DIR"
fi
source "$VENV_DIR/bin/activate"
command -v edge-tts &>/dev/null || pip install edge-tts
```

### 0.5 初始化 Remotion 项目

**重要**：先读取 `video-agent-config.json` 获取 `projectDir`。Remotion 项目就在 projectDir 根目录。

```bash
cd "$PROJECT_DIR"

if [ ! -f package.json ] || ! npx remotion --version &>/dev/null 2>&1; then
  npx create-video@latest . --template blank-ts --no-git
fi

npm install shiki gray-matter 2>/dev/null || npm install gray-matter
npm install -D @types/node ts-node typescript
```

### 0.6 环境验证检查清单

| 检查项 | 命令 | 预期 |
|--------|------|------|
| Node.js | `node -v` | v20.x+ |
| ffmpeg | `ffmpeg -version` | 有输出 |
| Chromium | `which chromium-browser \|\| which chromium \|\| which google-chrome` | 路径存在 |
| 中文字体 | `fc-list :lang=zh \| head -3` | 至少一行 |
| edge-tts | `source ~/video-agent-venv/bin/activate && edge-tts --version` | 有输出 |
| Remotion | `npx remotion --version` | 有输出 |

全部通过后才进入 Stage 1。

---

## STAGE 1：脚本解析

> **输入格式详见 `INPUT_SPEC.md`。** 本节描述 Agent 的解析逻辑。

### 1.1 目标

将 `src/data/script.md` 和 `src/data/visual-guide.md` 解析为结构化 JSON，作为后续所有阶段的数据源。

### 1.2 口播稿解析步骤

创建 `src/parse-script.ts`（或直接用 Node.js 脚本），执行以下步骤：

```
1. 读取 script.md
2. 提取标题：第一行 # 开头的文字
3. 去掉标题区（第一个 --- 之前的内容）
4. 去掉尾注（最后一个 --- 之后仅含斜体 *...* 的段落）
5. 以 --- 分割正文为段落，过滤空段落
6. 对每个段落，解析以下标记：
   a. >>> 行 → 素材切换标记（提取标记文字，不纳入口播文本）
   b. （停顿）/（pause）→ 额外 1 秒静音标记
   c. （长停顿）→ 额外 2 秒静音标记
   d. **加粗** → 字幕高亮词
   e. 空行 → 字幕块分割点
   f. 其余行 → 口播纯文本（去 markdown 格式标记）
7. 生成字幕块列表（subtitle blocks）：
   - 每个空行分隔一个字幕块
   - 块内硬换行保留为字幕内行分割
   - 每行长度校验（从 config 的 aspect 读取上限）
   - 每块最多 2 行，超过的自动拆分
```

### 1.3 素材映射

两阶段映射：

**阶段 A — 解析素材指南：**
```
1. 以 --- 分割素材指南为段落（与口播稿段落一一对应）
2. 每个段落内，识别 **[类型]** 开头的块为一个素材
3. 提取：类型（动画/截图/录屏/文字卡/代码）、标题、完整描述、原始 markdown
4. 为素材编号（全局递增 id，从 1 开始）
```

**阶段 B — 口播稿 `>>>` 标记绑定素材：**
```
1. 如果段落内有 >>> 标记：
   a. 每个 >>> 的标记文字与该段落的素材标题做匹配
   b. 匹配规则：精确匹配 > 关键词模糊匹配 > "素材 N" 编号匹配 > 顺序位置
   c. >>> 标记到下一个 >>> 或段落结束之间的文字 = 该素材的展示时段
2. 如果段落内无 >>> 标记：
   a. 素材按描述文字长度成比例分配时间
```

### 1.4 输出

`src/data/segments.json`：

```typescript
interface SubtitleBlock {
  text: string;         // 显示文字
  lines: string[];      // 行分割后的数组
  highlights: string[]; // 高亮词列表
}

interface AssetSpec {
  id: number;          // 从 1 开始全局编号
  type: 'screenshot' | 'animation' | 'recording' | 'textcard' | 'code';
  title: string;       // 素材标题
  description: string; // 素材描述（纯文本摘要）
  rawMarkdown: string; // 原始 markdown（供组件生成参考）
  codeFile?: string;   // 如有指定的代码文件名
}

interface AssetBinding {
  assetId: number;
  narrationText: string;     // 该素材展示期间的口播文字
  subtitleBlocks: SubtitleBlock[];
}

interface Segment {
  id: string;              // "S01", "S02", ...
  title: string;           // 段落主题
  narrationText: string;   // 完整口播纯文本（供 TTS）
  charCount: number;       // 中文字符数
  assets: AssetSpec[];     // 该段落的所有素材
  bindings: AssetBinding[];// >>> 绑定关系（无 >>> 则为空，由 timing 阶段按比例分配）
  pauses: { afterBlock: number; durationSec: number }[]; // 停顿标记位置
}

interface ParsedScript {
  title: string;
  segments: Segment[];
  totalChars: number;
  totalAssets: number;
  subtitleMaxCharsPerLine: number; // 从 config.aspect 派生
}
```

额外输出：
- `src/data/narration_S01.txt` ~ `src/data/narration_SNN.txt`（每段纯文本，供 TTS 读取。包含停顿标记处的额外空行让 TTS 自然停顿，但不包含 `>>>` 标记行）

### 1.5 字幕长度校验

解析时自动校验每行字幕长度：

```typescript
const MAX_CHARS: Record<string, number> = {
  '16:9': 20,
  '9:16': 14,
  '1:1': 16,
};

// 计算一行的等效中文字符数
function countChars(line: string): number {
  let count = 0;
  for (const ch of line) {
    // CJK 字符、全角标点算 1；ASCII 字母/数字算 0.5；其他算 1
    if (/[\u4e00-\u9fff\u3000-\u303f\uff00-\uffef]/.test(ch)) count += 1;
    else if (/[a-zA-Z0-9 ]/.test(ch)) count += 0.5;
    else count += 1;
  }
  return Math.ceil(count);
}

// 超长行处理：在最近的标点处断行
function wrapLine(line: string, maxChars: number): string[] { ... }
```

超长行：
1. 先尝试在标点（，、。！？；：）处断行
2. 标点处断不了则在 maxChars 位置强制断行
3. 输出 WARNING 提示用户优化文案

### 1.6 动态生成任务清单

**关键步骤**：解析完成后，根据实际的段落数和素材数，动态生成 `pipeline-state.json`。

任务模板（N = 段落数，M = 素材数）：

```
STAGE 0: T00_sudo_check, T01_apt_install, T02_nodejs, T03_python, T04_remotion, T05_copy_sources, T06_env_verify
STAGE 1: T10_parse_script, T11_parse_verify
STAGE 2: T20_tts_S01..S{NN}（每段一个）, T21_silence, T22_audio_manifest, T23_vtt_to_json
STAGE 3: T30_theme, T30_code_highlight, T31_Asset_{1..M}（每个素材一个）, T31_SubtitleOverlay
STAGE 4: T40_timing, T41_segment_visuals, T42_video_root, T43_compile_check
STAGE 5: T50_preview_frames, T51_full_render
STAGE 6: T60_normalize, T61_quality_check, T62_summary
```

使用 `scripts/update-task.sh` 写入状态。

### 1.7 验证

- segments.json 中的段落数 ≥ 2
- 每个段落的 narrationText 非空
- 口播稿和素材指南的段落数一致
- 所有素材都被分配到某个段落
- 字幕超长行数量输出为 WARNING（不阻塞）
- 如果有 `>>>` 标记，每个标记都成功绑定了一个素材
- 总字符数 > 500

---

## STAGE 2：语音合成（TTS）

### 2.1 工具

**主选：edge-tts**（免费，质量高）

读取 `video-agent-config.json` 中的 `voice` 字段选择声音。

### 2.2 执行

```bash
source ~/video-agent-venv/bin/activate
mkdir -p public/audio

# 对每个段落并行执行
for seg in $(jq -r '.segments[].id' src/data/segments.json); do
  edge-tts \
    --voice "$VOICE" \
    --rate "+0%" \
    --text "$(cat src/data/narration_${seg}.txt)" \
    --write-media "public/audio/${seg}.mp3" \
    --write-subtitles "public/audio/${seg}.vtt" &
done
wait
```

### 2.3 生成静音文件

```bash
ffmpeg -f lavfi -i anullsrc=r=44100:cl=mono -t 1.5 -q:a 9 -acodec libmp3lame public/audio/silence.mp3
```

### 2.4 生成音频清单

用 ffprobe 获取每段时长，写入 `src/data/audio-manifest.json`：

```json
{
  "voice": "zh-CN-YunxiNeural",
  "segments": [
    {"id": "S01", "audioFile": "audio/S01.mp3", "subtitleFile": "audio/S01.vtt", "durationSeconds": 28.5}
  ],
  "totalDurationSeconds": 620.0
}
```

### 2.5 VTT → JSON 预解析

Remotion 组件无法直接读取 VTT 文件，需预解析为 JSON：

```javascript
// 将每个 S{xx}.vtt 解析为 src/data/subtitles_S{xx}.json
// 格式: [{start: 秒数, end: 秒数, text: "..."}]
```

### 2.6 错误处理

| 错误 | 恢复策略 |
|------|----------|
| edge-tts 超时 | 重试 3 次，间隔 5 秒 |
| 输出空文件 | 去掉特殊字符后重试 |
| 单段 >90 秒 | 在句号处切分，分别生成后 ffmpeg 拼接 |
| edge-tts 不可用 | 降级到 pyttsx3 |

---

## STAGE 3：视觉素材生成

### 3.0 核心原则

**所有素材均为 Remotion React 组件，用代码绘制，不依赖外部图片。**

Agent 根据 `segments.json` 中每个 asset 的 `description` 和 `rawMarkdown` 动态生成组件。

### 3.1 目录结构

```
src/
  ├── styles/theme.ts        # 全局视觉规范
  └── components/
      ├── TextCard.tsx        # 通用文字卡（可复用于多个素材）
      ├── SubtitleOverlay.tsx # 字幕覆盖层
      └── Asset_{N}.tsx       # 每个素材一个组件（N = 素材编号）
```

### 3.2 全局视觉规范

```typescript
// src/styles/theme.ts
// 读取 video-agent-config.json 中的 width/height/fps

export const THEME = {
  // 颜色
  bgDark: '#0a0a0f',
  bgCard: '#1a1a2e',
  textPrimary: '#e0e0e0',
  textAccent: '#4fc3f7',
  textQuote: '#ffd54f',
  success: '#66bb6a',
  error: '#ef5350',
  codeGreen: '#a5d6a7',
  codeRed: '#ef9a9a',

  // 字体
  fontChinese: "'Noto Sans CJK SC', 'Microsoft YaHei', sans-serif",
  fontCode: "'JetBrains Mono', 'Fira Code', 'Consolas', monospace",

  // 尺寸（从 config 读取）
  width: 1920,   // 替换为实际值
  height: 1080,  // 替换为实际值
  fps: 30,
};
```

### 3.3 组件生成策略

根据素材 `type` 选择实现模式：

#### textcard（文字卡）

```tsx
// 通用 TextCard 组件
interface TextCardProps {
  durationInFrames: number;
  lines: string[];
  variant: 'quote' | 'closing' | 'credits';
}
// 深色背景 + 居中文字 + 淡入淡出
// 从 asset.description 中提取引号内的文字作为 lines
```

#### screenshot（截图模拟）

```tsx
// 用 React/CSS 模拟截图画面
// 通常是：深色面板 + 表格/数据 + 高亮标注
// 可选 Ken Burns 缓慢放大效果
const scale = interpolate(frame, [0, durationInFrames], [1, 1.05], {extrapolateRight: 'clamp'});
```

#### animation（动画）

```tsx
// 最灵活的类型，根据描述实现
// 常见模式：
//   - 逐层/逐项出现（用 Sequence 或 interpolate 控制时序）
//   - 数值变化动画（用 interpolate 或 spring）
//   - 分屏对比（左右两列）
//   - 时间线/进度条
//   - 流程图/架构图
// 全部用 CSS/SVG 绘制，不需要外部图片
```

#### recording（录屏模拟）

```tsx
// 用 React 组件模拟界面操作
// 类似 animation，但模仿特定软件的 UI 风格
// 如：终端、代码编辑器、对话界面、应用程序窗口
```

### 3.4 代码展示素材

如果素材描述涉及代码展示，使用 `src/data/source-samples/` 中的代码文件：

```bash
# 预生成代码高亮（shiki 是异步的，不能在 Remotion 组件里直接用）
npx ts-node -e "
import { getHighlighter } from 'shiki';
import * as fs from 'fs';
(async () => {
  const highlighter = await getHighlighter({ themes: ['dark-plus'], langs: ['cpp','python','typescript','javascript','go','rust','java'] });
  const files = fs.readdirSync('src/data/source-samples/');
  const result: Record<string, {html: string, lines: string[]}> = {};
  for (const file of files) {
    const code = fs.readFileSync('src/data/source-samples/' + file, 'utf-8');
    const lang = file.split('.').pop() || 'text';
    const langMap: Record<string,string> = {h:'cpp', hpp:'cpp', c:'cpp', py:'python', ts:'typescript', js:'javascript'};
    const html = highlighter.codeToHtml(code, { lang: langMap[lang] || lang, theme: 'dark-plus' });
    result[file] = { html, lines: code.split('\\n') };
  }
  fs.writeFileSync('src/data/highlighted-code.json', JSON.stringify(result, null, 2));
  console.log('Highlighted', Object.keys(result).length, 'files');
})();
"
```

**降级方案**：shiki 失败时，用正则手工着色（关键词蓝色、字符串橙色、注释灰色）。

### 3.5 字幕覆盖层

```tsx
// src/components/SubtitleOverlay.tsx
// 接收 cues: {start, end, text}[] + 当前帧
// 根据时间显示对应字幕
// 样式：底部居中，白色文字，黑色阴影
```

### 3.6 降级策略

如果某个素材组件实在难以生成，**用 TextCard 替代**：

```tsx
// 降级：显示素材的文字描述
<TextCard
  durationInFrames={frames}
  lines={[asset.description.slice(0, 80)]}
  variant="quote"
/>
```

**视频必须能完整渲染，降级优于崩溃。**

### 3.7 错误处理

| 错误 | 恢复策略 |
|------|----------|
| 组件渲染报错 | `npx remotion still` 逐个测试，定位出错组件 |
| 中文显示方框 | 检查字体安装 + CSS `fontFamily` |
| shiki 不工作 | 降级为手工正则着色 |
| 组件太复杂 | 简化设计或用 TextCard 降级 |

---

## STAGE 4：Remotion 工程编排

### 4.1 文件结构

```
src/
  ├── index.ts       # registerRoot
  ├── Root.tsx        # Composition 定义（总时长计算）
  ├── Video.tsx       # 主视频合成（Sequence 编排）
  └── timing.ts       # 段落内素材时序分配
```

### 4.2 总时长计算

```
总时长 = 音频总时长 + 段落间隔(N-1个 × 1.5s) + 开头淡入(2s) + 结尾淡出(2s)
```

### 4.3 主视频合成逻辑

```tsx
// Video.tsx 核心结构
<AbsoluteFill style={{ backgroundColor: THEME.bgDark, opacity }}>
  {audioManifest.segments.map((audioSeg, index) => (
    <Fragment key={audioSeg.id}>
      {/* 段落内容 */}
      <Sequence from={frameOffset} durationInFrames={segFrames}>
        <Audio src={staticFile(audioSeg.audioFile)} />
        <SegmentVisuals segment={segment} durationInFrames={segFrames} />
        <SubtitleOverlay cues={subtitleCues} />
      </Sequence>

      {/* 段落间 1.5s 静音 */}
      {index < last && (
        <Sequence from={frameOffset + segFrames} durationInFrames={silenceGap}>
          <Audio src={staticFile('audio/silence.mp3')} />
        </Sequence>
      )}
    </Fragment>
  ))}
</AbsoluteFill>
```

### 4.4 段落内素材时序分配

```typescript
// timing.ts
// 两种模式，取决于 segment.bindings 是否为空：

// 模式 A：有 >>> 绑定（精确控制）
// segment.bindings 中每个 binding 的 narrationText 字符数占总字符数的比例 → 分配时间
// 这样素材切换点与口播内容精确同步

// 模式 B：无 >>> 绑定（自动分配）
// 按 asset.description 文字长度成比例分配时间

// 两种模式的共同规则：
// - 每个素材最少 2 秒（60 帧）
// - 最后一个素材取剩余时间
// - 停顿标记（segment.pauses）的额外时间加到对应位置
```

### 4.5 SegmentVisuals 组件

```tsx
// 根据 segment.assets 列表，用 <Sequence> 按计算好的时序渲染各素材组件
// 动态 import 对应的 Asset_{N} 组件

// 素材间转场：0.5s 交叉淡入淡出
// 实现方式：相邻 Sequence 重叠 15 帧，前一个 opacity 渐出，后一个 opacity 渐入
```

### 4.6 字幕数据加载

Remotion 组件运行在 headless Chromium 中，无法读文件系统。字幕必须从预解析 JSON import：

```tsx
// 动态 import 方式：根据段落数量生成 import 语句
import subtitles_S01 from './data/subtitles_S01.json';
// ... 根据实际段落数量
```

### 4.7 字幕渲染逻辑

SubtitleOverlay 组件需要同时使用两个数据源：

1. **VTT 预解析 JSON**（来自 Stage 2.5）：提供精确的字级时间戳
2. **segments.json 的 subtitleBlocks**（来自 Stage 1）：提供行分割和高亮词信息

合并策略：
- 以 VTT 时间戳驱动字幕切换时机
- 以 subtitleBlocks 的 `lines` 控制换行显示
- 以 subtitleBlocks 的 `highlights` 标记高亮词

```tsx
// SubtitleOverlay 渲染伪代码
const activeCue = findActiveCue(currentTime, vttCues);
if (!activeCue) return null;

const block = findMatchingBlock(activeCue.text, subtitleBlocks);
return (
  <div style={subtitleContainer}>
    {block.lines.map(line => (
      <div key={line}>
        {renderWithHighlights(line, block.highlights)}
      </div>
    ))}
  </div>
);

function renderWithHighlights(text: string, highlights: string[]) {
  // 将 highlights 中的词用 <span style={{color: THEME.textAccent}}> 包裹
}
```

### 4.8 停顿处理

`segment.pauses` 中的停顿标记转换为 Remotion 时间轴上的额外帧：

```typescript
// 停顿不生成额外 Sequence，而是延长包含停顿位置的素材的 durationInFrames
// 停顿期间：该素材继续显示最后一帧画面，字幕隐藏
```

### 4.9 转场效果

- 段落内素材间：0.5s 交叉淡入淡出（15 帧重叠）
- 段落间：1.5s 黑屏静音
- 开头：2s 黑屏淡入
- 结尾：2s 淡出黑屏

---

## STAGE 5：渲染输出

### 5.1 预览验证

```bash
# 渲染 3 个关键帧验证
npx remotion still MainVideo --frame=75 --output=output/test_2.5s.png
npx remotion still MainVideo --frame=900 --output=output/test_30s.png
npx remotion still MainVideo --frame=5400 --output=output/test_3min.png
```

验证：非全黑、中文正确、布局正常。

### 5.2 完整渲染

```bash
mkdir -p output

npx remotion render MainVideo \
  --output output/final.mp4 \
  --codec h264 \
  --audio-codec aac \
  --concurrency 50% \
  --timeout 120000 \
  --log verbose \
  2>&1 | tee logs/render.log
```

### 5.3 分段渲染备选

OOM 或超时时改用分段渲染：

```bash
CHUNK=3000  # 每段约100秒
for ((i=0; i<TOTAL; i+=CHUNK)); do
  END=$((i + CHUNK - 1))
  [ $END -ge $TOTAL ] && END=$((TOTAL - 1))
  npx remotion render MainVideo \
    --output "output/chunk_${i}.mp4" \
    --frames="${i}-${END}" \
    --codec h264 --audio-codec aac --concurrency 1
done

# 拼接
ls output/chunk_*.mp4 | sort -V | sed 's/^/file /' > output/chunks.txt
ffmpeg -f concat -safe 0 -i output/chunks.txt -c copy output/final.mp4
```

### 5.4 错误处理

| 错误 | 恢复策略 |
|------|----------|
| Chromium 启动失败 | 设置 `PUPPETEER_EXECUTABLE_PATH`，添加 `--no-sandbox` |
| OOM | 降低 concurrency 到 25% 或 1 |
| 单帧超时 | 检查组件是否有无限循环，增加 timeout |
| 编译错误 | 查看 render.log 定位 TS/React 报错 |
| `JavaScript heap out of memory` | `export NODE_OPTIONS="--max-old-space-size=8192"` |

---

## STAGE 6：后处理与校验

### 6.1 音频标准化

```bash
ffmpeg -i output/final.mp4 \
  -af loudnorm=I=-16:TP=-1.5:LRA=11 \
  -c:v copy \
  output/final_normalized.mp4
```

### 6.2 质量校验

```bash
# 1. 文件大小 >10MB
test $(stat -c%s output/final_normalized.mp4) -gt 10000000

# 2. 视频信息
ffprobe -v quiet -print_format json -show_format -show_streams output/final_normalized.mp4
# 验证: h264, 正确分辨率, fps≈30, 音频 aac

# 3. 五点抽帧
DURATION=$(ffprobe -v quiet -show_entries format=duration -of csv=p=0 output/final_normalized.mp4)
for i in 1 2 3 4 5; do
  TS=$(echo "$DURATION $i" | awk '{printf "%.0f", $1 * $2 / 6}')
  ffmpeg -ss $TS -i output/final_normalized.mp4 -frames:v 1 -q:v 2 "output/check_${i}.jpg" -y
done

# 4. 检查截图非纯黑（>5KB）
for f in output/check_*.jpg; do
  SIZE=$(stat -c%s "$f")
  [ $SIZE -lt 5000 ] && echo "WARNING: $f 可能是纯黑帧 (${SIZE} bytes)"
done
```

### 6.3 生成完成报告

```bash
cat > output/pipeline-report.txt << EOF
视频制作完成报告
================
标题: $(jq -r .title video-agent-config.json)
时间: $(date)
输出: output/final_normalized.mp4
大小: $(ls -lh output/final_normalized.mp4 | awk '{print $5}')
时长: $(ffprobe -v quiet -show_entries format=duration -of csv=p=0 output/final_normalized.mp4) 秒
分辨率: $(jq -r '"\(.width)x\(.height)"' video-agent-config.json)
EOF
```

---

## 附录 A：常见环境问题速查

| 症状 | 原因 | 解决 |
|------|------|------|
| `libnss3.so` 缺失 | Chromium 缺库 | `sudo apt install -y libnss3 libatk-bridge2.0-0 libdrm2 libxcomposite1 libxdamage1 libxrandr2 libgbm1 libpango-1.0-0 libcairo2 libasound2` |
| `FATAL:zygote_host_impl_linux.cc` | Chromium 沙箱 | `remotion.config.ts` 中设置 `Config.setChromiumOpenGlRenderer("swangle")` |
| 无声音 | MP3 路径错误 | `staticFile()` 路径相对于 `public/` |
| 中文乱码 | VTT 编码 | 确保 UTF-8 |
| `JavaScript heap out of memory` | Node 内存不足 | `export NODE_OPTIONS="--max-old-space-size=8192"` |
| Remotion 版本冲突 | 依赖不一致 | `npx remotion upgrade` |
| npm 超时 | 网络问题 | `npm config set registry https://registry.npmmirror.com` |

---

*本文档版本: v2.0-generic | 适用于 Ubuntu 22.04+ / Remotion 4.x / edge-tts*
