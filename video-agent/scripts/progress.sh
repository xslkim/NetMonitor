#!/usr/bin/env bash
# progress.sh <state-file>
# 输出当前流水线进度摘要
set -euo pipefail

STATE_FILE="${1:?用法: $0 <state-file>}"

if [ ! -f "$STATE_FILE" ]; then
  echo "状态文件不存在: $STATE_FILE"
  exit 1
fi

node - "$STATE_FILE" <<'NODE'
const fs = require('fs');
const [, , stateFile] = process.argv;
const state = JSON.parse(fs.readFileSync(stateFile, 'utf-8'));
const counts = { pending: 0, running: 0, completed: 0, error: 0, skipped: 0 };
for (const task of Object.values(state.tasks)) counts[task.status] = (counts[task.status] || 0) + 1;
const total = Object.keys(state.tasks).length;
const done = (counts.completed || 0) + (counts.skipped || 0);
const pct = total > 0 ? Math.round(done / total * 100) : 0;

const bar = '█'.repeat(Math.round(pct / 5)) + '░'.repeat(20 - Math.round(pct / 5));
console.log(`\n进度: [${bar}] ${done}/${total} (${pct}%)`);
console.log(`  ✅ completed: ${counts.completed || 0}`);
console.log(`  ⏭  skipped:   ${counts.skipped || 0}`);
console.log(`  🔄 running:   ${counts.running || 0}`);
console.log(`  ❌ error:     ${counts.error || 0}`);
console.log(`  ⏳ pending:   ${counts.pending || 0}`);

const errors = Object.entries(state.tasks).filter(([, t]) => t.status === 'error');
if (errors.length > 0) {
  console.log('\n错误任务:');
  for (const [id, t] of errors) {
    console.log(`  ${id}: ${t.note || '(无信息)'} [retries: ${t.retries || 0}]`);
  }
}
console.log('');
NODE
