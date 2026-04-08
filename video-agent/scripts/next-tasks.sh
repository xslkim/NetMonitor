#!/usr/bin/env bash
# next-tasks.sh <state-file>
# 归一化中断/可重试状态，输出当前所有可执行任务
set -euo pipefail

STATE_FILE="${1:?用法: $0 <state-file>}"
LOCK_FILE="${STATE_FILE}.lock"

(
flock -x 9
node - "$STATE_FILE" <<'NODE'
const fs = require('fs');
const [, , stateFile] = process.argv;

if (!fs.existsSync(stateFile)) {
  console.log('NO_STATE_FILE');
  process.exit(0);
}

const state = JSON.parse(fs.readFileSync(stateFile, 'utf-8'));
let mutated = false;

for (const [id, task] of Object.entries(state.tasks)) {
  // 中断恢复：running → pending
  if (task.status === 'running') {
    task.status = 'pending';
    task.note = (task.note || '') + ' | recovered';
    mutated = true;
  }
  // 可重试：error 且 retries < 3 → pending
  if (task.status === 'error' && (task.retries || 0) < 3) {
    task.status = 'pending';
    mutated = true;
  }
}

if (mutated) {
  state.updatedAt = new Date().toISOString();
  fs.writeFileSync(stateFile, JSON.stringify(state, null, 2));
}

const ready = [];
for (const [id, task] of Object.entries(state.tasks)) {
  if (task.status !== 'pending') continue;
  const deps = task.depends_on || [];
  const allMet = deps.every(d => {
    const dep = state.tasks[d];
    return dep && (dep.status === 'completed' || dep.status === 'skipped');
  });
  if (allMet) ready.push(id);
}

if (ready.length === 0) {
  const errors = Object.entries(state.tasks).filter(([, t]) => t.status === 'error' && (t.retries || 0) >= 3);
  const unfinished = Object.entries(state.tasks).filter(([, t]) => !['completed', 'skipped'].includes(t.status));
  if (errors.length > 0) {
    console.log('BLOCKED:' + errors.map(([id, t]) => `${id}(${t.note},retries=${t.retries})`).join(','));
  } else if (unfinished.length === 0) {
    console.log('ALL_DONE');
  } else {
    console.log('WAITING');
  }
} else {
  console.log('READY:' + ready.join(','));
}
NODE
) 9>"$LOCK_FILE"
