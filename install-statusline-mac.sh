#!/bin/bash
# macOS installer for the Clawdmeter daemon in `source = statusline` mode:
# no API calls — usage comes from Claude Code's statusLine hook via
# host/statusline-export.js. Sets up the Python venv, the daemon config, the
# LaunchAgent, and (with --patch-settings) points Claude Code's statusLine
# command at the export wrapper.
#
#   ./install-statusline-mac.sh [--source usage|statusline] [--config-dirs "~/.claude, ~/.claude-work"]
#                               [--accounts "personal, work"] [--clock off|auto|12|24]
#                               [--statusline /path/to/statusline.js] [--patch-settings]
#
# --source usage (default): the daemon runs `claude -p /usage` in every
# --config-dirs entry; no hook needed, --patch-settings is optional.
#
# Re-running is safe: every step is idempotent. Pairing is unchanged: flash the
# firmware, then System Settings → Bluetooth → Connect "Clawdmeter".
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SERVICE_LABEL="com.user.claude-usage-daemon"
PLIST_SRC="$SCRIPT_DIR/daemon/$SERVICE_LABEL.plist"
PLIST_DST="$HOME/Library/LaunchAgents/$SERVICE_LABEL.plist"
VENV_DIR="$SCRIPT_DIR/daemon/.venv"
DAEMON_PY="$SCRIPT_DIR/daemon/claude_usage_daemon.py"
EXPORT_JS="$SCRIPT_DIR/host/statusline-export.js"
AGENT_JS="$(cd "$(dirname "$0")" && pwd)/host/agent-status.js"
LOG_DIR="$HOME/Library/Logs"
CONFIG_DIR="$HOME/.config/claude-usage-monitor"
CONFIG_FILE="$CONFIG_DIR/config"
STATE_DIR="$HOME/.local/state/clawdmeter"
SETTINGS="${CLAUDE_CONFIG_DIR:-$HOME/.claude}/settings.json"

ACCOUNTS=""
SOURCE="usage"
CONFIG_DIRS=""
CLOCK="24"
STATUSLINE=""
PATCH_SETTINGS=0
while [ $# -gt 0 ]; do
    case "$1" in
        --accounts) ACCOUNTS="$2"; shift 2 ;;
        --source) SOURCE="$2"; shift 2 ;;
        --config-dirs) CONFIG_DIRS="$2"; shift 2 ;;
        --clock) CLOCK="$2"; shift 2 ;;
        --statusline) STATUSLINE="$2"; shift 2 ;;
        --patch-settings) PATCH_SETTINGS=1; shift ;;
        -h|--help) sed -n 2,13p "$0"; exit 0 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

upsert_config_key() {   # key value — replace or append, keep everything else
    local key="$1" value="$2"
    mkdir -p "$CONFIG_DIR"; touch "$CONFIG_FILE"
    grep -vE "^[[:space:]]*$key[[:space:]]*=" "$CONFIG_FILE" > "$CONFIG_FILE.tmp" || true
    mv "$CONFIG_FILE.tmp" "$CONFIG_FILE"
    echo "$key = $value" >> "$CONFIG_FILE"
}

echo "=== Clawdmeter daemon (source = $SOURCE) — macOS ==="
case "$SOURCE" in usage|statusline) ;; *) echo "--source must be usage or statusline"; exit 1 ;; esac
[ "$SOURCE" = usage ] && ! command -v claude >/dev/null && [ ! -x "$HOME/.local/bin/claude" ] && { echo "claude CLI not found (needed for --source usage)"; exit 1; }
command -v python3 >/dev/null || { echo "python3 is required"; exit 1; }
command -v node >/dev/null || { echo "node is required (runs host/statusline-export.js)"; exit 1; }

echo "[1/5] Python venv + bleak/httpx"
[ -d "$VENV_DIR" ] || python3 -m venv "$VENV_DIR"
"$VENV_DIR/bin/pip" install -q --upgrade pip >/dev/null
"$VENV_DIR/bin/pip" install -q bleak httpx
if command -v brew >/dev/null && ! command -v blueutil >/dev/null; then
    echo "      installing blueutil (stale-bond self-heal)"; brew install -q blueutil || true
fi

echo "[2/5] Config: $CONFIG_FILE"
mkdir -p "$STATE_DIR"
upsert_config_key source "$SOURCE"
[ -n "$CONFIG_DIRS" ] && upsert_config_key config_dirs "$CONFIG_DIRS"
upsert_config_key clock "$CLOCK"
[ -n "$ACCOUNTS" ] && upsert_config_key accounts "$ACCOUNTS"
grep -qE "^[[:space:]]*chime[[:space:]]*=" "$CONFIG_FILE" || upsert_config_key chime off
sed 's/^/      /' "$CONFIG_FILE"

echo "[3/5] LaunchAgent: $PLIST_DST"
mkdir -p "$HOME/Library/LaunchAgents" "$LOG_DIR"
sed -e "s|__PYTHON_BIN__|$VENV_DIR/bin/python|" \
    -e "s|__DAEMON_PATH__|$DAEMON_PY|" \
    -e "s|__REPO_DIR__|$SCRIPT_DIR|" \
    -e "s|__LOG_OUT__|$LOG_DIR/claude-usage-daemon.out.log|" \
    -e "s|__LOG_ERR__|$LOG_DIR/claude-usage-daemon.err.log|" \
    -e "s|__HOME__|$HOME|" "$PLIST_SRC" > "$PLIST_DST"
plutil -lint "$PLIST_DST" >/dev/null
launchctl unload "$PLIST_DST" 2>/dev/null || true
launchctl load -w "$PLIST_DST"
echo "      loaded (logs: $LOG_DIR/claude-usage-daemon.out.log)"

echo "[4/5] Claude Code statusLine → $EXPORT_JS"
SNIPPET="\"statusLine\": {\"type\": \"command\", \"command\": \"node \\\"$EXPORT_JS\\\"\", \"refreshInterval\": 60}"
if [ "$PATCH_SETTINGS" = 1 ] && [ -f "$SETTINGS" ]; then
    cp "$SETTINGS" "$SETTINGS.bak-clawdmeter"
    STATUSLINE="$STATUSLINE" EXPORT_JS="$EXPORT_JS" AGENT_JS="$AGENT_JS" SETTINGS="$SETTINGS" python3 - <<'PY'
import json, os
p = os.environ["SETTINGS"]; d = json.load(open(p))
cur = d.get("statusLine", {})
cmd = cur.get("command", "") if isinstance(cur, dict) else ""
# Remember the real status-line script so the wrapper can chain to it.
if cmd and "statusline-export.js" not in cmd and not os.environ.get("STATUSLINE"):
    print("      previous statusLine command:", cmd)
    print("      set CLAWDMETER_STATUSLINE to that script if it is not ~/git/claude-code-statusline/statusline.js")
d["statusLine"] = {"type": "command", "command": f'node "{os.environ["EXPORT_JS"]}"', "refreshInterval": 60}
# Agent working/idle hook (host/agent-status.js) -> yellow/green dot per column.
hook_cmd = f'node "{os.environ["AGENT_JS"]}"'
hooks = d.setdefault("hooks", {})
for ev in ("UserPromptSubmit", "PreToolUse", "PostToolUse", "Stop", "StopFailure",
           "Notification", "PermissionRequest", "SessionEnd"):
    groups = hooks.setdefault(ev, [])
    if not any(h.get("command") == hook_cmd for g in groups for h in g.get("hooks", [])):
        groups.append({"hooks": [{"type": "command", "command": hook_cmd, "timeout": 5, "async": True}]})
json.dump(d, open(p, "w"), indent=2, ensure_ascii=False); open(p, "a").write("\n")
print("      patched", p, "(backup: .bak-clawdmeter) — statusLine + agent-status hooks")
PY
else
    echo "      add to $SETTINGS:"
    echo "        $SNIPPET"
    echo "      and, for the working/idle dot, a hook on UserPromptSubmit, PreToolUse, PostToolUse, Stop,"
    echo "      StopFailure, Notification, PermissionRequest, SessionEnd:"
    echo "        {\"type\": \"command\", \"command\": \"node \\\"$AGENT_JS\\\"\", \"timeout\": 5, \"async\": true}"
fi
if [ -n "$STATUSLINE" ]; then
    echo "      wrapper chains to: $STATUSLINE  (export CLAWDMETER_STATUSLINE=$STATUSLINE in the shell that starts claude,"
    echo "      or edit the default path at the top of host/statusline-export.js)"
fi

echo "[5/5] Bluetooth"
echo "      Grant Bluetooth to python when macOS asks (or System Settings → Privacy & Security → Bluetooth)."
echo "      Pair once: System Settings → Bluetooth → Connect \"Clawdmeter\". The daemon only talks to the device this Mac holds."
echo ""
echo "Done. Open a Claude Code session: within ~60 s $STATE_DIR/<account>.json appears and the display updates."
