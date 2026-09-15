#!/usr/bin/env python3
"""host/agent-status.js: Claude Code hook -> STATE_DIR/agents/<account>/<session>.json.

Runs the real script under node with a fake CLAUDE_CONFIG_DIR, the same way
Claude Code invokes it (hook JSON on stdin, config dir in the environment).

Run: daemon/.venv/bin/python -m pytest daemon/tests/test_agent_hook.py -x -q
"""
import json
import os
import shutil
import subprocess
import time
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
HOOK = ROOT / "host" / "agent-status.js"
EXPORTER = ROOT / "host" / "statusline-export.js"

pytestmark = pytest.mark.skipif(shutil.which("node") is None, reason="node not installed")


@pytest.fixture
def env(tmp_path):
    cfg = tmp_path / "cfg"
    cfg.mkdir()
    (cfg / ".claude.json").write_text(json.dumps({"oauthAccount": {
        "emailAddress": "a.tuzovskiy@redmadrobot.com",
        "organizationType": "claude_team",
        "organizationName": "Redmadrobot",
    }}))
    state = tmp_path / "state"
    return dict(os.environ, CLAUDE_CONFIG_DIR=str(cfg), CLAWDMETER_STATE_DIR=str(state),
                CLAWDMETER_STATUSLINE=str(tmp_path / "missing-statusline.js")), state


def _hook(env, event, sid="sess-1", **extra):
    payload = {"session_id": sid, "hook_event_name": event, "cwd": "/tmp", **extra}
    r = subprocess.run(["node", str(HOOK)], input=json.dumps(payload), text=True,
                       capture_output=True, env=env, timeout=10)
    assert r.returncode == 0, r.stderr
    assert r.stdout == ""   # never talk back to Claude Code (a PermissionRequest hook must stay silent)
    return r


def _record(state, sid="sess-1"):
    f = state / "agents" / "redmadrobot" / f"{sid}.json"
    return json.loads(f.read_text()) if f.exists() else None


def test_user_prompt_marks_working(env):
    env, state = env
    before = int(time.time())
    _hook(env, "UserPromptSubmit")
    rec = _record(state)
    assert rec["state"] == "working"
    assert before <= rec["ts"] <= before + 10


@pytest.mark.parametrize("event", ["PreToolUse", "PostToolUse"])
def test_tool_events_refresh_working(env, event):
    env, state = env
    _hook(env, event)
    assert _record(state)["state"] == "working"


@pytest.mark.parametrize("event", ["Stop", "StopFailure", "Notification", "PermissionRequest"])
def test_waiting_events_mark_idle(env, event):
    env, state = env
    _hook(env, "UserPromptSubmit")
    _hook(env, event)
    assert _record(state)["state"] == "idle"


def test_session_end_removes_record(env):
    env, state = env
    _hook(env, "UserPromptSubmit")
    _hook(env, "SessionEnd")
    assert _record(state) is None


def test_sessions_are_tracked_separately(env):
    env, state = env
    _hook(env, "UserPromptSubmit", sid="a")
    _hook(env, "Stop", sid="b")
    assert _record(state, "a")["state"] == "working"
    assert _record(state, "b")["state"] == "idle"


def test_unknown_event_writes_nothing(env):
    env, state = env
    _hook(env, "SubagentStop")
    assert _record(state) is None


def test_garbage_stdin_exits_quietly(env):
    env, _ = env
    r = subprocess.run(["node", str(HOOK)], input="{oops", text=True,
                       capture_output=True, env=env, timeout=10)
    assert r.returncode == 0 and r.stdout == ""


def test_account_key_matches_statusline_exporter(env):
    # Both scripts must derive the same account key, or the daemon can't join
    # the usage column with its working/idle flag.
    env, state = env
    _hook(env, "UserPromptSubmit")
    subprocess.run(["node", str(EXPORTER)], text=True, capture_output=True, env=env, timeout=10,
                   input=json.dumps({"rate_limits": {"five_hour": {"used_percentage": 1, "resets_at": 1}}}))
    assert (state / "redmadrobot.json").exists()
    assert (state / "agents" / "redmadrobot" / "sess-1.json").exists()
