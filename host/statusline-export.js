#!/usr/bin/env node
// Clawdmeter statusline tee.
//
// Claude Code invokes the statusLine command every refresh and feeds it a JSON
// object on stdin that includes `rate_limits` (five_hour / seven_day buckets
// with used_percentage + resets_at). This wrapper snapshots that block to
//   ~/.local/state/clawdmeter/<account>.json
// and then runs the real statusline script with the same stdin, so the
// terminal status line is unchanged. The Clawdmeter daemon (source =
// statusline) reads those files and pushes them to the display over BLE —
// no direct API calls, the data comes from Claude Code itself.
//
// Multi-account: the account key is derived the same way the statusline
// labels it — organizationName for team/enterprise orgs, the email's local
// part otherwise — read from <CLAUDE_CONFIG_DIR>/.claude.json. Each account
// lands in its own file, so two Claude Code installs (CLAUDE_CONFIG_DIR) that
// share this settings.json produce two files.
//
// Env overrides:
//   CLAWDMETER_STATUSLINE  path to the real statusline script
//   CLAWDMETER_STATE_DIR   output directory
'use strict';
const fs = require('fs');
const path = require('path');
const os = require('os');
const { spawn } = require('child_process');

const HOME = os.homedir();
const STATUSLINE = process.env.CLAWDMETER_STATUSLINE ||
  path.join(HOME, 'git', 'claude-code-statusline', 'statusline.js');
const STATE_DIR = process.env.CLAWDMETER_STATE_DIR ||
  path.join(HOME, '.local', 'state', 'clawdmeter');

function accountInfo() {
  const claudeDir = process.env.CLAUDE_CONFIG_DIR || path.join(HOME, '.claude');
  let label = '';
  try {
    const raw = fs.readFileSync(path.join(claudeDir, '.claude.json'), 'utf8');
    const at = raw.indexOf('"oauthAccount"');
    if (at !== -1) {
      const slice = raw.slice(at, at + 4096);
      const field = (name) => {
        const m = slice.match(new RegExp(`"${name}"\\s*:\\s*("(?:[^"\\\\]|\\\\.)*")`));
        try { return m ? JSON.parse(m[1]) : ''; } catch (e) { return ''; }
      };
      const email = field('emailAddress');
      const orgType = field('organizationType');
      const shared = /team|enterprise/.test(orgType);
      label = (shared && field('organizationName')) || email.split('@')[0] || '';
    }
  } catch (e) {}
  if (!label) label = path.basename(claudeDir).replace(/^\./, '') || 'claude';
  const key = label.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-+|-+$/g, '') || 'claude';
  return { key, label, claudeDir };
}

function bucket(b) {
  if (!b || typeof b !== 'object') return null;
  const pct = Number(b.used_percentage);
  const reset = Number(b.resets_at);
  if (!Number.isFinite(pct)) return null;
  return {
    used_percentage: pct,
    resets_at: Number.isFinite(reset) ? reset : null,
  };
}

function exportUsage(data) {
  const rl = data && data.rate_limits;
  if (!rl) return;
  const five = bucket(rl.five_hour);
  const seven = bucket(rl.seven_day);
  if (!five && !seven) return;
  const acct = accountInfo();
  const out = {
    key: acct.key,
    label: acct.label,
    config_dir: acct.claudeDir,
    updated: Math.floor(Date.now() / 1000),
    five_hour: five,
    seven_day: seven,
    // Everything else Claude Code reports (model-specific weekly buckets etc.),
    // kept verbatim so the daemon can pick extra buckets without a hook change.
    rate_limits: rl,
  };
  fs.mkdirSync(STATE_DIR, { recursive: true, mode: 0o700 });
  const file = path.join(STATE_DIR, `${acct.key}.json`);
  const tmp = `${file}.${process.pid}.tmp`;
  fs.writeFileSync(tmp, JSON.stringify(out), { mode: 0o600 });
  fs.renameSync(tmp, file);
}

let input = '';
const stdinTimeout = setTimeout(() => process.exit(0), 3000);
process.stdin.setEncoding('utf8');
process.stdin.on('data', (chunk) => { input += chunk; });
process.stdin.on('end', () => {
  clearTimeout(stdinTimeout);
  // Forward first so the status line never waits on our file I/O.
  if (fs.existsSync(STATUSLINE)) {
    const child = spawn(process.execPath, [STATUSLINE], {
      stdio: ['pipe', 'inherit', 'inherit'],
      env: process.env,
    });
    child.on('exit', (code) => { process.exitCode = code == null ? 0 : code; });
    child.on('error', () => {});
    child.stdin.on('error', () => {});
    child.stdin.end(input);
  }
  try {
    exportUsage(JSON.parse(input));
  } catch (e) { /* never break the status line */ }
});
