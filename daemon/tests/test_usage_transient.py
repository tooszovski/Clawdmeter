#!/usr/bin/env python3
"""`usage` source: a starved/timed-out `claude -p /usage` is a transient miss,
not "no token". The device must keep its last numbers instead of dropping to
"No data" on the first slow cycle (e.g. a heavy job hogging every core).

Run: daemon/.venv/bin/python -m pytest daemon/tests/test_usage_transient.py -x -q
"""
import subprocess
from pathlib import Path

import daemon.claude_usage_daemon as mod

NOW = 1_800_000_000.0
USAGE_TEXT = (
    "Current session: 35% used · resets Sep 14 at 2:49pm (Europe/Moscow)\n"
    "Current week (all models): 8% used · resets Sep 20 at 10:59am (Europe/Moscow)\n"
)


def _setup(tmp_path, monkeypatch, results: dict):
    monkeypatch.setattr(mod, "STATE_DIR", tmp_path)
    monkeypatch.setattr(mod, "CONFIG_FILE", tmp_path / "config")
    monkeypatch.setattr(mod, "read_config_dirs", lambda: [Path(p) for p in results])
    monkeypatch.setattr(mod, "run_claude_usage", lambda d: results[str(d)])
    monkeypatch.setattr(mod, "account_label_for", lambda d: (d.name, d.name.capitalize()))


def test_all_dirs_timed_out_is_transient_not_dead(tmp_path, monkeypatch):
    _setup(tmp_path, monkeypatch, {"/x/a": mod.USAGE_TRANSIENT, "/x/b": mod.USAGE_TRANSIENT})
    payload, dead = mod.read_usage_payload(NOW)
    assert payload is None
    assert dead is False


def test_all_dirs_without_report_is_dead(tmp_path, monkeypatch):
    # Not logged in: the CLI answers with a cost summary, not usage windows.
    _setup(tmp_path, monkeypatch, {"/x/a": None, "/x/b": "Total cost: $0.0000"})
    payload, dead = mod.read_usage_payload(NOW)
    assert payload is None
    assert dead is True


def test_one_dir_timed_out_other_answers(tmp_path, monkeypatch):
    _setup(tmp_path, monkeypatch, {"/x/a": mod.USAGE_TRANSIENT, "/x/b": USAGE_TEXT})
    payload, dead = mod.read_usage_payload(NOW)
    assert dead is False
    assert [a["k"] for a in payload["accounts"]] == ["B"]


def test_run_claude_usage_maps_timeout_to_transient(monkeypatch):
    def boom(*a, **kw):
        raise subprocess.TimeoutExpired(cmd="claude", timeout=mod.USAGE_CMD_TIMEOUT)
    monkeypatch.setattr(mod.subprocess, "run", boom)
    assert mod.run_claude_usage(Path("/x/a")) is mod.USAGE_TRANSIENT


def test_run_claude_usage_os_error_is_not_transient(monkeypatch):
    def boom(*a, **kw):
        raise FileNotFoundError("claude")
    monkeypatch.setattr(mod.subprocess, "run", boom)
    assert mod.run_claude_usage(Path("/x/a")) is None


def test_cache_expires_after_grace():
    cache = mod.CachedPayload({"ok": True}, [], built_at=NOW)
    assert not mod.cache_expired(cache, NOW + mod.STALE_GRACE - 1)
    assert mod.cache_expired(cache, NOW + mod.STALE_GRACE + 1)
    assert not mod.cache_expired(None, NOW + 10 * mod.STALE_GRACE)
