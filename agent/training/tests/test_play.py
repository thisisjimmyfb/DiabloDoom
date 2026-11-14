"""Tests for play.py, which drives a normal, real-time game (-rldrive) with a
trained policy. The game tests run it headless."""

from __future__ import annotations

import os
import re
import signal
import subprocess
import sys
import tempfile
from pathlib import Path

import pytest
import torch
import torch.nn as nn

from model import Policy

HERE = Path(__file__).resolve().parents[1]
PY = sys.executable


@pytest.fixture
def checkpoint(tmp_path) -> Path:
    """A random-weights checkpoint in train.py's format."""
    path = tmp_path / "ckpt_000000.pt"
    torch.save({"policy": Policy().state_dict(), "iteration": 0}, path)
    return path


def play(*args: str, timeout: float = 300) -> subprocess.CompletedProcess:
    return subprocess.run([PY, "play.py", *args], cwd=HERE, capture_output=True, text=True, timeout=timeout)


def test_wrong_action_count(tmp_path):
    """A checkpoint from another action set is refused before any game starts."""
    policy = Policy()
    policy.policy_head = nn.Linear(512, 12)
    path = tmp_path / "old.pt"
    torch.save({"policy": policy.state_dict()}, path)
    p = play("--checkpoint", str(path), "--headless", timeout=120)
    assert p.returncode != 0 and "12 actions" in p.stderr and "different action set" in p.stderr


@pytest.mark.game
def test_drive_and_quit(checkpoint):
    """The policy drives decision after decision, then --quit-at-end quits."""
    p = play("--checkpoint", str(checkpoint), "--headless", "--max-decisions", "15", "--quit-at-end")
    assert p.returncode == 0, p.stdout[-2000:] + p.stderr[-2000:]
    steps = [l for l in p.stdout.splitlines() if re.match(r"\[play +\d+\]", l)]
    assert len(steps) == 15 and "stopped after 15 decisions: max decisions" in p.stdout
    assert "left running" not in p.stdout


@pytest.mark.game
def test_drive_leaves_game_running(checkpoint):
    """Without --quit-at-end the game outlives the drive, detached: its work
    directory and shared memory are gone, so cleanup_stale never kills it."""
    p = play("--checkpoint", str(checkpoint), "--headless", "--max-decisions", "3")
    assert p.returncode == 0, p.stdout[-2000:] + p.stderr[-2000:]
    m = re.search(r"game \(pid (\d+)\) left running", p.stdout)
    assert m, p.stdout[-2000:]
    pid = int(m.group(1))
    try:
        os.kill(pid, 0)                                   # still running
        assert not any("_play_" in d.name for d in Path(tempfile.gettempdir()).glob("diablodoom_rl_*")), \
            "work directory removed"
        assert not any("_play" in f.name for f in Path("/dev/shm").glob("diablodoom_rl_*")), "shm removed"
    finally:
        os.kill(pid, signal.SIGKILL)
