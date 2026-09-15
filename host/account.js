// Shared by host/statusline-export.js and host/agent-status.js: which Claude
// Code account is this process running for? Both scripts are launched by
// Claude Code with CLAUDE_CONFIG_DIR in the environment, and the key is derived
// the same way the statusline labels it — organizationName for team/enterprise
// orgs, the email's local part otherwise — read from <CLAUDE_CONFIG_DIR>/.claude.json.
// The daemon (account_label_for) mirrors this so the three agree on the key.
'use strict';
const fs = require('fs');
const path = require('path');
const os = require('os');

function accountInfo() {
  const HOME = os.homedir();
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

function stateDir() {
  return process.env.CLAWDMETER_STATE_DIR ||
    path.join(os.homedir(), '.local', 'state', 'clawdmeter');
}

module.exports = { accountInfo, stateDir };
