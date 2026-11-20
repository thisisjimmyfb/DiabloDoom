"""Tests for hub.py, the Hugging Face Hub copy of the checkpoint
(SPEC.md §7.4): pull, push, stuck uploads and eval.py's download, with a
fake  CLI. No game instances."""

from __future__ import annotations

import os
import time
from pathlib import Path

import pytest
import torch

from model import CKPT_NAME, load_latest_checkpoint, save_checkpoint


_FAKE_HF = r'''#!/usr/bin/env python3
"""A stand-in for the hf CLI: the repo is the directory $FAKE_HUB."""
import fnmatch, json, os, shutil, sys
hub, a = os.environ["FAKE_HUB"], sys.argv[1:]
def files():
    return sorted(os.path.relpath(os.path.join(r, f), hub) for r, _, fs in os.walk(hub) for f in fs)
def opt(name):
    return a[a.index(name) + 1] if name in a else None
if a[:2] == ["models", "info"]:
    print(json.dumps({"id": a[2], "siblings": [{"rfilename": f} for f in files()]}))
elif a[0] == "download":
    d, i = opt("--local-dir"), a.index("--local-dir")
    for f in a[2:i]:
        os.makedirs(os.path.dirname(os.path.join(d, f)), exist_ok=True)
        shutil.copyfile(os.path.join(hub, f), os.path.join(d, f))
elif a[0] == "upload":
    hang = os.environ.get("FAKE_HANG")      # a file holding how many uploads hang, silently
    left = int(open(hang).read() or 0) if hang else 0
    if left > 0:
        open(hang, "w").write(str(left - 1))
        import subprocess, time
        child = subprocess.Popen(["sleep", "600"])     # like hf's workers: must die too
        open(hang + ".pids", "a").write(f"{os.getpid()} {child.pid}\n")
        time.sleep(600)
    src, dst = a[2], a[3]
    if os.path.isdir(src):
        for f in files():
            if opt("--delete") and fnmatch.fnmatch(f, opt("--delete")):
                os.remove(os.path.join(hub, f))
        for r, _, fs in os.walk(src):
            for f in fs:
                rel = os.path.normpath(os.path.join(dst, os.path.relpath(os.path.join(r, f), src)))
                os.makedirs(os.path.dirname(os.path.join(hub, rel)) or hub, exist_ok=True)
                shutil.copyfile(os.path.join(r, f), os.path.join(hub, rel))
    else:
        shutil.copyfile(src, os.path.join(hub, dst))
    with open(os.path.join(hub, "..", "commits"), "a") as c:
        c.write(opt("--commit-message") + "\n")
'''


def test_hub_stuck_upload(tmp_path, monkeypatch):
    """An upload that goes quiet (the Hub closed its connection: hf waits
    forever) is killed after UPLOAD_STALL_S, with everything it started;
    finish() then retries and the checkpoint gets there."""
    import hub as hub_mod
    from hub import HUB_FILE, HubUploader
    bin_dir, hub = tmp_path / "bin", tmp_path / "hub"
    bin_dir.mkdir(); hub.mkdir()
    (bin_dir / "hf").write_text(_FAKE_HF)
    (bin_dir / "hf").chmod(0o755)
    hang = tmp_path / "hang"
    hang.write_text("1")                  # the first upload hangs
    monkeypatch.setenv("PATH", f"{bin_dir}:{Path('/usr/bin')}:{Path('/bin')}")
    monkeypatch.setenv("FAKE_HUB", str(hub))
    monkeypatch.setenv("FAKE_HANG", str(hang))
    monkeypatch.setattr(hub_mod, "UPLOAD_STALL_S", 2)
    run = tmp_path / "run"
    run.mkdir()
    save_checkpoint(run / CKPT_NAME, {"iteration": 7})
    up = HubUploader("u/m", run)
    t0 = time.time()
    up.finish(run / CKPT_NAME, 7)
    assert time.time() - t0 < 60, "the stuck upload was killed, not waited out"
    assert torch.load(hub / HUB_FILE, weights_only=False)["iteration"] == 7, "the retry uploaded it"
    assert "killed, no progress" in (run / "hub_upload.log").read_text()
    for pid in map(int, (tmp_path / "hang.pids").read_text().split()):
        try:
            os.kill(pid, 0)
            alive = open(f"/proc/{pid}/stat").read().split()[2] != "Z"
        except (ProcessLookupError, FileNotFoundError):
            alive = False
        assert not alive, f"pid {pid} (hf or its child) still running"


def test_hub(tmp_path, monkeypatch):
    """--hub-repo with a fake hf CLI: an empty repo pulls nothing; pushes
    upload doom-runner.pt alone (one at a time, the end uploads the
    newest), removing progress saves an older version uploaded; a fresh
    run directory pulls it back, and a run directory with a newer
    checkpoint keeps its own."""
    from hub import HUB_FILE, HubUploader, hub_pull
    bin_dir, hub = tmp_path / "bin", tmp_path / "hub"
    bin_dir.mkdir(); hub.mkdir()
    (bin_dir / "hf").write_text(_FAKE_HF)
    (bin_dir / "hf").chmod(0o755)
    monkeypatch.setenv("PATH", f"{bin_dir}:{Path('/usr/bin')}:{Path('/bin')}")
    monkeypatch.setenv("FAKE_HUB", str(hub))

    run = tmp_path / "run"
    run.mkdir()
    assert hub_pull("u/m", run) is None, "empty repo"
    (hub / "frontier").mkdir()
    (hub / "frontier" / "A_1_1.dsg").write_text("old")    # uploaded by an older version
    up = HubUploader("u/m", run)
    save_checkpoint(run / CKPT_NAME, {"iteration": 9})
    assert up.push(run / CKPT_NAME, 9)
    up.finish(None, None)                 # waits for it
    assert not list((hub / "frontier").iterdir()), "old progress saves removed from the repo"
    save_checkpoint(run / CKPT_NAME, {"iteration": 19})
    up = HubUploader("u/m", run)
    up.finish(run / CKPT_NAME, 19)
    assert (tmp_path / "commits").read_text() == "iteration 9\niteration 19\n"
    assert torch.load(hub / HUB_FILE, weights_only=False)["iteration"] == 19
    assert sorted(f.name for f in hub.iterdir() if f.is_file()) == [HUB_FILE], "only the checkpoint"
    assert "iteration 19" in (run / "hub_upload.log").read_text()

    fresh = tmp_path / "fresh"
    fresh.mkdir()
    assert hub_pull("u/m", fresh) == fresh / CKPT_NAME
    assert load_latest_checkpoint(fresh)["iteration"] == 19

    ahead = tmp_path / "ahead"
    ahead.mkdir()
    save_checkpoint(ahead / CKPT_NAME, {"iteration": 30})
    assert hub_pull("u/m", ahead) is None
    assert load_latest_checkpoint(ahead)["iteration"] == 30, "the newer local checkpoint is kept"

    monkeypatch.setenv("PATH", "/nonexistent")
    with pytest.raises(SystemExit):
        hub_pull("u/m", fresh)


def test_hub_fetch_checkpoint(tmp_path, monkeypatch):
    """eval.py's download: the repo's checkpoint lands in dest; a repo
    without one, an unreadable repo or no hf CLI give None (eval.py then
    uses a local checkpoint) instead of exiting."""
    from hub import hub_fetch_checkpoint
    bin_dir, hub = tmp_path / "bin", tmp_path / "hub"
    bin_dir.mkdir(); hub.mkdir()
    (bin_dir / "hf").write_text(_FAKE_HF)
    (bin_dir / "hf").chmod(0o755)
    monkeypatch.setenv("PATH", f"{bin_dir}:/usr/bin:/bin")
    monkeypatch.setenv("FAKE_HUB", str(hub))
    dest = tmp_path / "play"
    assert hub_fetch_checkpoint("u/m", dest) is None, "no checkpoint in the repo"
    save_checkpoint(hub / CKPT_NAME, {"iteration": 7})
    got = hub_fetch_checkpoint("u/m", dest)
    assert got == dest / CKPT_NAME and torch.load(got, weights_only=False)["iteration"] == 7
    monkeypatch.setenv("FAKE_HUB", str(tmp_path / "missing"))          # the fake fails: unreadable
    assert hub_fetch_checkpoint("u/m", dest) is None
    monkeypatch.setenv("PATH", "/nonexistent")
    assert hub_fetch_checkpoint("u/m", dest) is None
