#!/usr/bin/env python3
"""Agent working/idle indicator: hook-written session files -> per-account flag.

host/agent-status.js writes STATE_DIR/agents/<account>/<session>.json on Claude
Code hook events; the daemon folds those into an "ag" flag per account (1 =
working, 0 = idle, absent = unknown) and re-sends the cached payload when a
flag flips between polls.

Run: daemon/.venv/bin/python -m pytest daemon/tests/test_agent_status.py -x -q
"""
import json
from pathlib import Path

import daemon.claude_usage_daemon as mod

NOW = 1_800_000_000.0


def _session(root: Path, key: str, sid: str, state: str, ts: float) -> None:
    d = root / "agents" / key
    d.mkdir(parents=True, exist_ok=True)
    (d / f"{sid}.json").write_text(json.dumps({"state": state, "ts": ts}))


# ---------------------------------------------------------------------------
# read_agent_states
# ---------------------------------------------------------------------------

def test_recent_working_session_marks_account_working(tmp_path, monkeypatch):
    monkeypatch.setattr(mod, "STATE_DIR", tmp_path)
    _session(tmp_path, "tuzyakin", "s1", "working", NOW - 10)
    assert mod.read_agent_states(NOW) == {"tuzyakin": True}


def test_idle_session_marks_account_idle(tmp_path, monkeypatch):
    monkeypatch.setattr(mod, "STATE_DIR", tmp_path)
    _session(tmp_path, "tuzyakin", "s1", "idle", NOW - 10)
    assert mod.read_agent_states(NOW) == {"tuzyakin": False}


def test_any_working_session_wins_over_idle_ones(tmp_path, monkeypatch):
    monkeypatch.setattr(mod, "STATE_DIR", tmp_path)
    _session(tmp_path, "redmadrobot", "s1", "idle", NOW - 5)
    _session(tmp_path, "redmadrobot", "s2", "working", NOW - 20)
    assert mod.read_agent_states(NOW) == {"redmadrobot": True}


def test_working_older_than_ttl_counts_as_idle(tmp_path, monkeypatch):
    monkeypatch.setattr(mod, "STATE_DIR", tmp_path)
    _session(tmp_path, "tuzyakin", "s1", "working", NOW - mod.AGENT_WORKING_TTL - 1)
    assert mod.read_agent_states(NOW) == {"tuzyakin": False}


def test_working_just_inside_ttl_still_working(tmp_path, monkeypatch):
    monkeypatch.setattr(mod, "STATE_DIR", tmp_path)
    _session(tmp_path, "tuzyakin", "s1", "working", NOW - mod.AGENT_WORKING_TTL + 1)
    assert mod.read_agent_states(NOW) == {"tuzyakin": True}


def test_missing_agents_dir_is_unknown_for_everyone(tmp_path, monkeypatch):
    monkeypatch.setattr(mod, "STATE_DIR", tmp_path)
    assert mod.read_agent_states(NOW) == {}


def test_garbage_session_file_is_ignored(tmp_path, monkeypatch):
    monkeypatch.setattr(mod, "STATE_DIR", tmp_path)
    d = tmp_path / "agents" / "tuzyakin"
    d.mkdir(parents=True)
    (d / "bad.json").write_text("{not json")
    _session(tmp_path, "tuzyakin", "ok", "idle", NOW)
    assert mod.read_agent_states(NOW) == {"tuzyakin": False}


def test_stale_session_files_are_pruned(tmp_path, monkeypatch):
    # A session whose Claude Code died without SessionEnd must not linger
    # forever: anything older than AGENT_PRUNE_AGE is deleted on read.
    monkeypatch.setattr(mod, "STATE_DIR", tmp_path)
    _session(tmp_path, "tuzyakin", "dead", "working", NOW - mod.AGENT_PRUNE_AGE - 1)
    assert mod.read_agent_states(NOW) == {}
    assert not (tmp_path / "agents" / "tuzyakin" / "dead.json").exists()


# ---------------------------------------------------------------------------
# payload integration
# ---------------------------------------------------------------------------

USAGE_TEXT = (
    "Current session: 35% used · resets Sep 14 at 2:49pm (Europe/Moscow)\n"
    "Current week (all models): 8% used · resets Sep 20 at 10:59am (Europe/Moscow)\n"
)


def _usage_setup(tmp_path, monkeypatch, dirs):
    monkeypatch.setattr(mod, "STATE_DIR", tmp_path)
    monkeypatch.setattr(mod, "CONFIG_FILE", tmp_path / "config")  # absent -> defaults
    monkeypatch.setattr(mod, "read_config_dirs", lambda: list(dirs))
    monkeypatch.setattr(mod, "run_claude_usage", lambda d: USAGE_TEXT)
    monkeypatch.setattr(mod, "account_label_for", lambda d: (d.name, d.name.capitalize()))


def test_usage_payload_carries_agent_flag_per_account(tmp_path, monkeypatch):
    _usage_setup(tmp_path, monkeypatch, [Path("/x/tuzyakin"), Path("/x/redmadrobot")])
    _session(tmp_path, "tuzyakin", "s1", "working", NOW - 1)
    _session(tmp_path, "redmadrobot", "s1", "idle", NOW - 1)
    payload, dead = mod.read_usage_payload(NOW)
    assert not dead
    flags = {a["k"]: a.get("ag") for a in payload["accounts"]}
    assert flags == {"Tuzyakin": 1, "Redmadrobot": 0}


def test_usage_payload_omits_flag_when_no_session_known(tmp_path, monkeypatch):
    _usage_setup(tmp_path, monkeypatch, [Path("/x/tuzyakin")])
    payload, _ = mod.read_usage_payload(NOW)
    assert "ag" not in payload["accounts"][0]


def test_usage_payload_records_account_keys_for_resend(tmp_path, monkeypatch):
    _usage_setup(tmp_path, monkeypatch, [Path("/x/redmadrobot"), Path("/x/tuzyakin")])
    payload, _ = mod.read_usage_payload(NOW)
    # Column order follows the `accounts` config (absent here -> alphabetical);
    # the key list must follow the same order so flags land on the right column.
    assert mod.payload_account_keys(payload) == ["redmadrobot", "tuzyakin"]
    assert "_keys" not in json.dumps(payload)  # never sent over the air


def test_statusline_payload_carries_agent_flag(tmp_path, monkeypatch):
    monkeypatch.setattr(mod, "STATE_DIR", tmp_path)
    monkeypatch.setattr(mod, "CONFIG_FILE", tmp_path / "config")
    (tmp_path / "tuzyakin.json").write_text(json.dumps({
        "key": "tuzyakin", "label": "tuzyakin", "updated": NOW - 30,
        "five_hour": {"used_percentage": 6, "resets_at": NOW + 3600},
        "seven_day": {"used_percentage": 1, "resets_at": NOW + 86400},
    }))
    _session(tmp_path, "tuzyakin", "s1", "working", NOW - 1)
    payload, _ = mod.read_statusline_payload(NOW)
    assert payload["accounts"][0]["ag"] == 1


# ---------------------------------------------------------------------------
# refresh_agent_flags: status-only resend between polls
# ---------------------------------------------------------------------------

def _cached():
    p = {"s": 1, "sr": 2, "w": 3, "wr": 4, "ok": True, "a": 0,
         "accounts": [{"k": "Tuzyakin", "s": 1, "sr": 2, "w": 3, "wr": 4, "age": 0, "ag": 0},
                      {"k": "Redmadrobot", "s": 5, "sr": 6, "w": 7, "wr": 8, "age": 40}]}
    return mod.CachedPayload(p, ["tuzyakin", "redmadrobot"], built_at=NOW)


def test_refresh_returns_none_when_flags_unchanged():
    c = _cached()
    assert c.refresh_agent_flags({"tuzyakin": False}, NOW + 5) is None


def test_refresh_returns_payload_with_new_flag_and_aged_accounts():
    c = _cached()
    out = c.refresh_agent_flags({"tuzyakin": True, "redmadrobot": False}, NOW + 7)
    assert out is not None
    a, b = out["accounts"]
    assert a["ag"] == 1 and b["ag"] == 0
    assert a["age"] == 7 and b["age"] == 47   # ages keep counting from the original poll
    # The cache remembers what was sent (so the same states don't resend next
    # tick) but keeps its base ages.
    assert c.payload["accounts"][0]["ag"] == 1 and c.payload["accounts"][0]["age"] == 0
    assert c.refresh_agent_flags({"tuzyakin": True, "redmadrobot": False}, NOW + 8) is None


def test_refresh_drops_flag_when_account_becomes_unknown():
    c = _cached()
    out = c.refresh_agent_flags({}, NOW + 1)
    assert out is not None
    assert "ag" not in out["accounts"][0]


def test_refresh_does_not_touch_ages_of_unknown_age():
    c = _cached()
    c.payload["accounts"][1]["age"] = -1
    out = c.refresh_agent_flags({"redmadrobot": True}, NOW + 9)
    assert out["accounts"][1]["age"] == -1
