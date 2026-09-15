#!/usr/bin/env node
// Clawdmeter agent working/idle hook.
//
// Registered in Claude Code's settings.json for the events below; Claude Code
// runs it with the hook JSON on stdin ({session_id, hook_event_name, ...}) and
// CLAUDE_CONFIG_DIR in the environment. It records one file per session:
//   ~/.local/state/clawdmeter/agents/<account>/<session_id>.json
//   {"state": "working" | "idle", "ts": <epoch seconds>}
// The daemon folds these into an "ag" flag per account (any working session
// = working) and the wide layout draws a yellow (working) / green (idle) dot.
//
//   working  UserPromptSubmit, PreToolUse, PostToolUse   (agent is busy)
//   idle     Stop, StopFailure, Notification, PermissionRequest (waiting for you)
//   removed  SessionEnd
//
// Always exits 0 and prints nothing: a PermissionRequest hook that talks back
// would be taken as a decision, and no hook may ever stall Claude Code.
'use strict';
const fs = require('fs');
const path = require('path');
const { accountInfo, stateDir } = require('./account');

const WORKING = new Set(['UserPromptSubmit', 'PreToolUse', 'PostToolUse']);
const IDLE = new Set(['Stop', 'StopFailure', 'Notification', 'PermissionRequest']);

function record(event, sessionId) {
  const sid = String(sessionId || '').replace(/[^A-Za-z0-9_-]/g, '_') || 'default';
  const dir = path.join(stateDir(), 'agents', accountInfo().key);
  const file = path.join(dir, `${sid}.json`);
  if (event === 'SessionEnd') {
    try { fs.unlinkSync(file); } catch (e) {}
    return;
  }
  const state = WORKING.has(event) ? 'working' : IDLE.has(event) ? 'idle' : null;
  if (!state) return;
  fs.mkdirSync(dir, { recursive: true, mode: 0o700 });
  const tmp = `${file}.${process.pid}.tmp`;
  fs.writeFileSync(tmp, JSON.stringify({ state, ts: Math.floor(Date.now() / 1000) }), { mode: 0o600 });
  fs.renameSync(tmp, file);
}

let input = '';
const stdinTimeout = setTimeout(() => process.exit(0), 3000);
process.stdin.setEncoding('utf8');
process.stdin.on('data', (chunk) => { input += chunk; });
process.stdin.on('end', () => {
  clearTimeout(stdinTimeout);
  try {
    const data = JSON.parse(input);
    record(data.hook_event_name, data.session_id);
  } catch (e) { /* never break Claude Code */ }
});
