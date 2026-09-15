# Statusline / usage data sources (no API calls)

Two ways to feed the display without the daemon ever calling the Anthropic API
itself. Both put one column per account on wide displays.

| `source =`   | How                                                      | Refreshes                       | Model weekly (Fable/Opus) |
|--------------|----------------------------------------------------------|---------------------------------|---------------------------|
| `usage`      | daemon runs `claude -p /usage --output-format json` in each `config_dirs` entry | always (no session needed) | yes, third row |
| `statusline` | Claude Code's statusLine hook writes files via `host/statusline-export.js` | only while a session for that account is open | no |

`usage` is the recommended one; `statusline` files are still read for their
exact `resets_at` epochs when they are fresh. Payload fields per account:
`k` label, `s`/`sr` session % and minutes to reset, `w`/`wr` weekly, `age`
seconds since the numbers were obtained, and optionally `m`/`mw`/`mwr` for the
model-scoped weekly window.

## `usage` source

```ini
source = usage
config_dirs = ~/.claude, ~/.claude-work   # one column each
accounts = personal, work                 # optional column order (account keys)
clock = 24
```

`claude -p /usage` is Claude Code's own command (its client, its token, its
5-minute cache); the daemon only parses the three "Current …" lines. It runs
once per poll (60 s) per config dir and takes about a second.

## `statusline` source (hook files)

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

## Agent working/idle dot (`ag`)

`host/agent-status.js` runs as a Claude Code **hook** (not the status line) and
records what each session is doing:

| Hook events | Record |
|---|---|
| `UserPromptSubmit`, `PreToolUse`, `PostToolUse` | `working` |
| `Stop`, `StopFailure`, `Notification`, `PermissionRequest` | `idle` (waiting for you) |
| `SessionEnd` | record removed |

One file per session: `~/.local/state/clawdmeter/agents/<account>/<session_id>.json`
= `{"state": "working", "ts": 1789350175}`. The account key is derived exactly
like the exporter's (`host/account.js`), so the daemon can join it to the
column. The daemon folds the records into `"ag"` per account — `1` if **any**
session of that account is working, `0` if all are idle, absent when no
session is known — and the wide layout draws a **yellow** (working) / **green**
(idle) dot under the battery, on the column's side of the gap; no dot when
unknown.

- A `working` record older than 15 min counts as idle (`AGENT_WORKING_TTL`): a
  Claude Code that died without `SessionEnd` cannot stay yellow forever, and
  tool hooks refresh the stamp far more often than that. Records older than
  24 h are deleted.
- The daemon re-checks the records every tick (5 s) and, when a flag flips,
  re-sends the **cached** last payload with the new flags and honestly
  advanced `age` — no extra `claude -p /usage` run, so the dot follows the
  hook within ~5 s instead of waiting for the 60 s poll.
- `install-statusline-mac.sh --patch-settings` registers the hook; by hand,
  add `node "/path/to/Clawdmeter/host/agent-status.js"` (`"async": true`) to
  the eight events above in `settings.json`. Sessions already open pick up the
  new hook only after a restart.

## Limits

- A column only refreshes while a Claude Code session for that account is open
  (the hook is the only writer).
- `resets_at` is stored, so reset countdowns stay correct while a file ages.
