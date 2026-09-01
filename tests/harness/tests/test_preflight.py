"""preflight.check(): missing tools are reported, never silently assumed
present. Runs entirely without a GPU."""

from unittest import mock

import preflight


def test_check_reports_missing_compiler():
    def fake_which(name):
        return None if name in ("cc", "gcc", "clang") else f"/usr/bin/{name}"

    with mock.patch("preflight.shutil.which", side_effect=fake_which):
        result = preflight.check("medium")

    assert result.ok is False
    assert [r.name for r in result.missing] == ["c_compiler"]
    assert result.resolved["bash"] == "/usr/bin/bash"


def test_check_all_present():
    with mock.patch("preflight.shutil.which", return_value="/usr/bin/tool"):
        result = preflight.check("medium")
    assert result.ok is True
    assert result.missing == []


def test_small_tier_only_needs_bash():
    with mock.patch("preflight.shutil.which", side_effect=lambda n: "/usr/bin/bash" if n == "bash" else None):
        result = preflight.check("small")
    assert result.ok is True


def test_install_missing_never_called_implicitly(monkeypatch):
    """install_missing() must only ever run when the caller explicitly
    invokes it -- this test just pins that check()/preflight results never
    trigger installation as a side effect."""
    calls = []
    monkeypatch.setattr(preflight.subprocess, "run", lambda *a, **k: calls.append(a))
    with mock.patch("preflight.shutil.which", return_value=None):
        preflight.check("medium")
    assert calls == []
