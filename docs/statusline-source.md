# Statusline data source (no API calls)

By default the daemon polls `api.anthropic.com` with the Claude Code OAuth
token. The `statusline` source replaces that with data Claude Code already
hands out: every status-line refresh, Claude Code pipes a JSON object to the
configured `statusLine` command that includes

```json
"rate_limits": {
  "five_hour": {"used_percentage": 6.4, "resets_at": 1789350175},
  "seven_day": {"used_percentage": 1.0, "resets_at": 1789855615}
}
```

`host/statusline-export.js` is a tee for that command: it snapshots the block to
`~/.local/state/clawdmeter/<account>.json` and then runs your real status-line
script with the same stdin, so the terminal is unchanged. The daemon reads the
files and forwards them over BLE — nothing here talks to the API.

## Setup

macOS shortcut for all of the below: `./install-statusline-mac.sh --accounts "personal, work" --patch-settings`
(venv + config + LaunchAgent; `--patch-settings` rewrites `statusLine` in `~/.claude/settings.json`
with a `.bak-clawdmeter` backup). Manual steps:

1. Point Claude Code at the wrapper (`~/.claude/settings.json`):

   ```json
   "statusLine": {
     "type": "command",
     "command": "node \"/path/to/Clawdmeter/host/statusline-export.js\"",
     "refreshInterval": 60
   }
   ```

   Set `CLAWDMETER_STATUSLINE=/path/to/your/statusline.js` in the environment
   if your status-line script is not `~/git/claude-code-statusline/statusline.js`.
   Without a real script the wrapper just exports and prints nothing.

2. Tell the daemon to use the files (`~/.config/claude-usage-monitor/config`):

   ```ini
   source = statusline
   # optional: column order on wide displays (account keys = file names)
   accounts = personal, work
   clock = 24
   ```

3. Open a Claude Code session. The hook fires on every refresh (≈60 s), the
   file appears, the daemon picks it up on its next tick.

## Multiple accounts

The account key comes from `<CLAUDE_CONFIG_DIR>/.claude.json`: the organization
name for team/enterprise plans, the e-mail's local part otherwise. Two Claude
Code installs selected with `CLAUDE_CONFIG_DIR` and sharing one `settings.json`
therefore produce two files, and the payload carries both:

```json
{"s":6,"sr":216,"w":1,"wr":8640,"st":"allowed","acct":"pro","ok":true,"a":0,
 "accounts":[{"k":"tuzyakin","s":6,"sr":216,"w":1,"wr":8640,"age":40},
             {"k":"Redmadrobot","s":42,"sr":120,"w":33,"wr":5000,"age":600}],
 "t":1789348071,"tf":24}
```

- Legacy top-level fields mirror the *active* account (`a` = index of the file
  written most recently), so single-column firmware works unchanged.
- `age` is seconds since the hook last wrote that account. Wide layouts show it
  as "updated N ago" and keep counting between payloads: a column whose
  Claude Code session is closed simply ages instead of looking live.

## Limits

- A column only refreshes while a Claude Code session for that account is open
  (the hook is the only writer).
- `resets_at` is stored, so reset countdowns stay correct while a file ages.
