#!/usr/bin/env bash
set -euo pipefail

# ============================================================================
# video-agent/run.sh — 全自动视频制作入口
#
# 用法:
#   ~/video-agent/run.sh \
#     --script    ./口播稿.md \
#     --visual    ./素材指南.md \
#     --repo      /path/to/project-repo \
#     --out-dir   ~/my-video              # 可选，默认 ~/teaching-video-$(date)
#     --model     opus                    # 可选，默认 opus
#     --voice     zh-CN-YunxiNeural       # 可选，默认 zh-CN-YunxiNeural
#     --aspect    16:9                    # 可选，默认 16:9（支持 9:16 竖屏）
#     --source-files "src/core/Foo.h,src/Bar.cpp"  # 可选，展示的代码文件
#     --resume                            # 可选，断点续跑
#     --dry-run                           # 可选，只初始化不执行
# ============================================================================

AGENT_DIR="$(cd "$(dirname "$0")" && pwd)"

# ── 默认值 ──
SCRIPT_FILE=""
VISUAL_FILE=""
REPO_DIR=""
OUT_DIR=""
MODEL="opus"
VOICE="zh-CN-YunxiNeural"
ASPECT="16:9"
SOURCE_FILES=""
RESUME=false
DRY_RUN=false
MAX_TURNS=200

# ── 参数解析 ──
while [[ $# -gt 0 ]]; do
  case "$1" in
    --script)       SCRIPT_FILE="$(realpath "$2")"; shift 2 ;;
    --visual)       VISUAL_FILE="$(realpath "$2")"; shift 2 ;;
    --repo)         REPO_DIR="$(realpath "$2")"; shift 2 ;;
    --out-dir)      OUT_DIR="$2"; shift 2 ;;
    --model)        MODEL="$2"; shift 2 ;;
    --voice)        VOICE="$2"; shift 2 ;;
    --aspect)       ASPECT="$2"; shift 2 ;;
    --source-files) SOURCE_FILES="$2"; shift 2 ;;
    --max-turns)    MAX_TURNS="$2"; shift 2 ;;
    --resume)       RESUME=true; shift ;;
    --dry-run)      DRY_RUN=true; shift ;;
    -h|--help)
      cat <<'USAGE'
全自动视频制作工具

必选参数:
  --script FILE       口播稿 Markdown 文件（用 --- 分隔段落）
  --visual FILE       视觉素材指南 Markdown 文件
  --repo   DIR        项目 Git 仓库路径（用于代码展示素材）

可选参数:
  --out-dir DIR       输出项目目录（默认: ~/teaching-video-YYYYMMDD-HHMMSS）
  --model MODEL       Claude 模型: opus / sonnet（默认: opus）
  --voice VOICE       TTS 声音（默认: zh-CN-YunxiNeural）
  --aspect RATIO      画面比例: 16:9 / 9:16 / 1:1（默认: 16:9）
  --source-files LIST 要展示的代码文件，逗号分隔的相对路径（相对于 repo）
                      省略则自动选择 repo 中最有代表性的文件
  --max-turns N       Claude 最大工具调用轮数（默认: 200）
  --resume            断点续跑（检测已有 pipeline-state.json）
  --dry-run           只初始化项目目录，不启动 Claude
USAGE
      exit 0 ;;
    *) echo "未知参数: $1"; exit 1 ;;
  esac
done

# ── 参数校验 ──
err() { echo "ERROR: $1" >&2; exit 1; }

[[ -n "$SCRIPT_FILE" ]] || err "缺少 --script 参数"
[[ -n "$VISUAL_FILE" ]] || err "缺少 --visual 参数"
[[ -n "$REPO_DIR" ]]    || err "缺少 --repo 参数"
[[ -f "$SCRIPT_FILE" ]] || err "口播稿文件不存在: $SCRIPT_FILE"
[[ -f "$VISUAL_FILE" ]] || err "素材指南文件不存在: $VISUAL_FILE"
[[ -d "$REPO_DIR" ]]    || err "仓库目录不存在: $REPO_DIR"

# ── 查找 claude CLI（延迟到实际需要时才报错） ──
find_claude() {
  # 1. 环境变量指定
  if [[ -n "${CLAUDE_BIN:-}" ]] && [[ -x "$CLAUDE_BIN" ]]; then return 0; fi
  # 2. PATH 中
  if command -v claude &>/dev/null; then CLAUDE_BIN="claude"; return 0; fi
  # 3. 常见安装路径
  for p in ~/.local/bin/claude ~/.npm-global/bin/claude /usr/local/bin/claude; do
    if [[ -x "$p" ]]; then CLAUDE_BIN="$p"; return 0; fi
  done
  # 4. ccd-cli（Claude Code remote/desktop 安装方式）
  local latest_ccd=$(ls -t ~/.claude/remote/ccd-cli/* 2>/dev/null | head -1)
  if [[ -n "$latest_ccd" ]] && [[ -x "$latest_ccd" ]]; then CLAUDE_BIN="$latest_ccd"; return 0; fi
  # 5. npx 回退
  if command -v npx &>/dev/null && npx claude --version &>/dev/null 2>&1; then CLAUDE_BIN="npx claude"; return 0; fi
  return 1
}

# ── 解析画面尺寸 ──
case "$ASPECT" in
  16:9)  WIDTH=1920; HEIGHT=1080 ;;
  9:16)  WIDTH=1080; HEIGHT=1920 ;;
  1:1)   WIDTH=1080; HEIGHT=1080 ;;
  *)     err "不支持的画面比例: $ASPECT（支持 16:9, 9:16, 1:1）" ;;
esac

# ── 创建项目目录 ──
if [[ -z "$OUT_DIR" ]]; then
  OUT_DIR="$HOME/teaching-video-$(date +%Y%m%d-%H%M%S)"
fi

if [[ "$RESUME" == true ]]; then
  if [[ ! -d "$OUT_DIR" ]]; then
    err "断点续跑模式但项目目录不存在: $OUT_DIR"
  fi
  if [[ ! -f "$OUT_DIR/pipeline-state.json" ]]; then
    echo "WARN: 项目目录存在但无 pipeline-state.json，将作为新项目初始化"
    RESUME=false
  fi
fi

mkdir -p "$OUT_DIR"/{src/data/source-samples,public/audio,output,logs,scripts}

# ── 复制输入文件 ──
cp "$SCRIPT_FILE" "$OUT_DIR/src/data/script.md"
cp "$VISUAL_FILE" "$OUT_DIR/src/data/visual-guide.md"

# ── 自动检测或复制源码文件 ──
if [[ -n "$SOURCE_FILES" ]]; then
  # 用户指定的文件
  IFS=',' read -ra FILES <<< "$SOURCE_FILES"
  for f in "${FILES[@]}"; do
    f="$(echo "$f" | xargs)"  # trim
    if [[ -f "$REPO_DIR/$f" ]]; then
      cp "$REPO_DIR/$f" "$OUT_DIR/src/data/source-samples/"
    else
      echo "WARN: 源码文件不存在: $REPO_DIR/$f"
    fi
  done
else
  # 自动选择：找 repo 中最有代表性的源码文件（按大小和类型筛选）
  echo "自动检测代码文件..."
  find "$REPO_DIR/src" "$REPO_DIR/lib" "$REPO_DIR/core" "$REPO_DIR/app" \
    -maxdepth 3 \
    -type f \( -name '*.h' -o -name '*.hpp' -o -name '*.cpp' -o -name '*.py' \
               -o -name '*.ts' -o -name '*.js' -o -name '*.go' -o -name '*.rs' \
               -o -name '*.java' -o -name '*.cs' \) \
    -size +500c -size -50k \
    2>/dev/null | head -10 | while read -r f; do
      cp "$f" "$OUT_DIR/src/data/source-samples/"
  done || true

  # 如果上面没找到，搜索整个 repo
  if [[ -z "$(ls -A "$OUT_DIR/src/data/source-samples/" 2>/dev/null)" ]]; then
    find "$REPO_DIR" \
      -maxdepth 4 \
      -type f \( -name '*.h' -o -name '*.hpp' -o -name '*.cpp' -o -name '*.py' \
                 -o -name '*.ts' -o -name '*.js' -o -name '*.go' -o -name '*.rs' \) \
      -not -path '*/node_modules/*' -not -path '*/.git/*' -not -path '*/vendor/*' \
      -size +500c -size -50k \
      2>/dev/null | head -6 | while read -r f; do
        cp "$f" "$OUT_DIR/src/data/source-samples/"
    done || true
  fi
fi

SOURCE_SAMPLE_LIST=$(ls "$OUT_DIR/src/data/source-samples/" 2>/dev/null | tr '\n' ',' | sed 's/,$//' | tr -d '\n')

# ── 提取视频标题 ──
VIDEO_TITLE=$(head -5 "$SCRIPT_FILE" | grep '^# ' | head -1 | sed 's/^# //' | tr -d '\n')
if [[ -z "$VIDEO_TITLE" ]]; then
  VIDEO_TITLE="教学视频"
fi

# ── 写入配置文件 ──
jq -n \
  --arg title "$VIDEO_TITLE" \
  --arg repoDir "$REPO_DIR" \
  --arg projectDir "$OUT_DIR" \
  --arg agentDir "$AGENT_DIR" \
  --arg scriptFile "src/data/script.md" \
  --arg visualGuideFile "src/data/visual-guide.md" \
  --arg sourceCodeSamples "src/data/source-samples/" \
  --arg sourceFileList "$SOURCE_SAMPLE_LIST" \
  --arg voice "$VOICE" \
  --arg model "$MODEL" \
  --argjson width "$WIDTH" \
  --argjson height "$HEIGHT" \
  --argjson fps 30 \
  --arg aspect "$ASPECT" \
  '{title: $title, repoDir: $repoDir, projectDir: $projectDir, agentDir: $agentDir,
    scriptFile: $scriptFile, visualGuideFile: $visualGuideFile,
    sourceCodeSamples: $sourceCodeSamples, sourceFileList: $sourceFileList,
    voice: $voice, model: $model, width: $width, height: $height, fps: $fps, aspect: $aspect}' \
  > "$OUT_DIR/video-agent-config.json"

# ── 复制工作流文档和脚本 ──
cp "$AGENT_DIR/VIDEO_WORKFLOW.md" "$OUT_DIR/VIDEO_WORKFLOW.md"
cp "$AGENT_DIR/scripts/"*.sh "$OUT_DIR/scripts/"
chmod +x "$OUT_DIR/scripts/"*.sh

# ── 生成项目 CLAUDE.md ──
sed \
  -e "s|{{PROJECT_DIR}}|$OUT_DIR|g" \
  -e "s|{{REPO_DIR}}|$REPO_DIR|g" \
  -e "s|{{VOICE}}|$VOICE|g" \
  -e "s|{{WIDTH}}|$WIDTH|g" \
  -e "s|{{HEIGHT}}|$HEIGHT|g" \
  -e "s|{{VIDEO_TITLE}}|$VIDEO_TITLE|g" \
  "$AGENT_DIR/CLAUDE.md" > "$OUT_DIR/CLAUDE.md"

# ── 磁盘空间预检 ──
AVAIL_KB=$(df --output=avail "$OUT_DIR" | tail -1)
AVAIL_GB=$((AVAIL_KB / 1024 / 1024))
if [[ $AVAIL_GB -lt 3 ]]; then
  echo "WARNING: 可用磁盘空间仅 ${AVAIL_GB}GB，视频渲染需要至少 3GB"
  echo "         建议在空间充足的分区运行"
fi

# ── 打印摘要 ──
cat << SUMMARY

╔══════════════════════════════════════════════╗
║         视频制作 Agent 项目已初始化           ║
╚══════════════════════════════════════════════╝

  标题:     $VIDEO_TITLE
  项目目录: $OUT_DIR
  口播稿:   $SCRIPT_FILE
  素材指南: $VISUAL_FILE
  源码仓库: $REPO_DIR
  代码样本: ${SOURCE_SAMPLE_LIST:-（无）}
  TTS 声音: $VOICE
  画面尺寸: ${WIDTH}x${HEIGHT} ($ASPECT)
  模型:     $MODEL
  断点续跑: $RESUME
  磁盘空间: ${AVAIL_GB}GB 可用

SUMMARY

if [[ "$DRY_RUN" == true ]]; then
  echo "  [dry-run] 项目已初始化，未启动 Claude。"
  echo ""
  echo "  手动启动:"
  echo "    cd $OUT_DIR && claude -p --model $MODEL --dangerously-skip-permissions --max-turns $MAX_TURNS \"读取 CLAUDE.md 和 video-agent-config.json，执行全流程\""
  echo ""
  echo "  断点续跑:"
  echo "    $0 --resume --out-dir $OUT_DIR --script $SCRIPT_FILE --visual $VISUAL_FILE --repo $REPO_DIR"
  exit 0
fi

# ── 查找 Claude CLI（仅在非 dry-run 时需要） ──
if ! find_claude; then
  err "找不到 claude CLI。请先安装: npm install -g @anthropic-ai/claude-code
  或设置环境变量: export CLAUDE_BIN=/path/to/claude"
fi
echo "使用 Claude CLI: $CLAUDE_BIN"

# ── 启动 Claude Agent ──
echo "启动 Claude Agent..."
echo "日志: $OUT_DIR/logs/agent.log"
echo "────────────────────────────────────────────"

PROMPT="你是全自动视频制作 Agent。

请执行以下操作：
1. 读取 video-agent-config.json 了解项目配置
2. 读取 VIDEO_WORKFLOW.md 了解完整工作流
3. $(if [[ "$RESUME" == true ]]; then echo "检测到断点续跑模式：读取 pipeline-state.json，跳过已完成的任务，从第一个未完成任务继续"; else echo "从头开始执行全流程（Stage 0 → Stage 6）"; fi)

执行过程中：
- 每完成一个任务立即用 scripts/update-task.sh 更新状态
- 遇到错误最多重试 3 次
- Stage 2（TTS）和 Stage 3（素材生成）必须并行执行
- 所有日志写入 logs/ 目录

开始执行。"

cd "$OUT_DIR"
"$CLAUDE_BIN" -p \
  --model "$MODEL" \
  --dangerously-skip-permissions \
  --max-turns "$MAX_TURNS" \
  "$PROMPT" \
  2>&1 | tee "$OUT_DIR/logs/agent.log"

EXIT_CODE=${PIPESTATUS[0]}

echo ""
echo "────────────────────────────────────────────"
if [[ $EXIT_CODE -eq 0 ]]; then
  echo "Agent 执行完毕。"
  if [[ -f "$OUT_DIR/output/final_normalized.mp4" ]]; then
    echo "最终视频: $OUT_DIR/output/final_normalized.mp4"
    ls -lh "$OUT_DIR/output/final_normalized.mp4"
  elif [[ -f "$OUT_DIR/output/final.mp4" ]]; then
    echo "视频（未标准化）: $OUT_DIR/output/final.mp4"
  else
    echo "WARNING: 未找到输出视频文件，请检查日志: $OUT_DIR/logs/agent.log"
  fi
else
  echo "Agent 异常退出 (code=$EXIT_CODE)。"
  echo "检查日志: $OUT_DIR/logs/agent.log"
  echo "断点续跑: $0 --resume --out-dir $OUT_DIR --script $SCRIPT_FILE --visual $VISUAL_FILE --repo $REPO_DIR"
fi

# 打印进度
if [[ -f "$OUT_DIR/pipeline-state.json" ]]; then
  echo ""
  bash "$OUT_DIR/scripts/progress.sh" "$OUT_DIR/pipeline-state.json"
fi

exit $EXIT_CODE
