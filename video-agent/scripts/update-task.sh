#!/usr/bin/env bash
# update-task.sh <state-file> <task-id> <status> [note]
#
# 用法:
#   bash scripts/update-task.sh pipeline-state.json T20_tts_S01 completed "28.5s"
#   bash scripts/update-task.sh pipeline-state.json T20_tts_S01 error "timeout"
set -euo pipefail

STATE_FILE="${1:?用法: $0 <state-file> <task-id> <status> [note]}"
TASK_ID="${2:?缺少 task-id}"
STATUS="${3:?缺少 status (pending|running|completed|error|skipped)}"
NOTE="${4:-}"
LOCK_FILE="${STATE_FILE}.lock"

(
flock -x 9
node - "$STATE_FILE" "$TASK_ID" "$STATUS" "$NOTE" <<'NODE'
const fs = require('fs');
const [, , stateFile, taskId, status, note = ''] = process.argv;

if (!fs.existsSync(stateFile)) {
  console.error('State file not found:', stateFile);
  process.exit(1);
}

const state = JSON.parse(fs.readFileSync(stateFile, 'utf-8'));
if (!state.tasks[taskId]) {
  // 动态添加新任务（支持 Stage 1 后动态扩展任务列表）
  state.tasks[taskId] = { status: 'pending', ts: '', note: '', retries: 0, depends_on: [] };
}

const task = state.tasks[taskId];
task.status = status;
task.ts = new Date().toISOString();
if (note) task.note = note;
if (status === 'error') {
  task.retries = (task.retries || 0) + 1;
}

state.updatedAt = new Date().toISOString();
fs.writeFileSync(stateFile, JSON.stringify(state, null, 2));
console.log(`${taskId} → ${status}${note ? ' (' + note + ')' : ''}`);
NODE
) 9>"$LOCK_FILE"
