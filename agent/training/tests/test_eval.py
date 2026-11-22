"""Tests for eval.py: watching a policy play (--iwad: a normal, real-time
game, -rldrive) and the training-style eval. The game tests run headless."""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

import pytest
import torch
import torch.nn as nn

from env.engine import DEFAULT_IWAD
from model import DEFAULT_OBS_SIZE, Policy

HERE = Path(__file__).resolve().parents[1]
PY = sys.executable


@pytest.fixture
def checkpoint(tmp_path) -> Path:
    """A random-weights checkpoint in train.py's format."""
    path = tmp_path / "doom-runner.pt"
    torch.save({"policy": Policy().state_dict(), "iteration": 0, "obs_size": list(DEFAULT_OBS_SIZE), "obs_depth": True}, path)
    return path


def play(*args: str, timeout: float = 300) -> subprocess.CompletedProcess:
    """eval.py watching the policy play (--iwad: Freedoom Phase 1)."""
    return subprocess.run([PY, "eval.py", "--iwad", str(DEFAULT_IWAD), *args], cwd=HERE,
                          capture_output=True, text=True, timeout=timeout)


def test_wrong_action_count(tmp_path):
    """A checkpoint from another action set is refused before any game starts."""
    policy = Policy()
    policy.policy_head = nn.Linear(512, 12)
    path = tmp_path / "old.pt"
    torch.save({"policy": policy.state_dict(), "obs_size": list(DEFAULT_OBS_SIZE), "obs_depth": True}, path)
    p = play("--checkpoint", str(path), "--headless", timeout=120)
    assert p.returncode != 0 and "12 actions" in p.stderr and "different action set" in p.stderr


@pytest.mark.game
def test_drive_and_quit(checkpoint):
    """The policy drives decision after decision, then --quit-at-end quits."""
    p = play("--checkpoint", str(checkpoint), "--headless", "--max-decisions", "15", "--quit-at-end")
    assert p.returncode == 0, p.stdout[-2000:] + p.stderr[-2000:]
    steps = [l for l in p.stdout.splitlines() if re.match(r"\[watch +\d+\]", l)]
    assert len(steps) == 15 and "stopped after 15 decisions: max decisions" in p.stdout
    # The end-of-drive stats (eval.sh prints them).
    assert "[eval] level FREEDOOM1 1 1: max decisions" in p.stdout, p.stdout[-2000:]
    assert re.search(r"\[eval\]   decisions 15, game time [\d.]+ s", p.stdout)
    assert re.search(r"\[eval\]   sectors gained [+-]\d+.*exit no, died no", p.stdout)
    assert "[eval]   forward " in p.stdout and "[eval]   top actions: PRESS" in p.stdout
    assert "left running" not in p.stdout


@pytest.mark.game
def test_consult(tmp_path):
    """A CONSULT while exploring is sent (the game only shows "CONSULT" as
    the status message) and the drive goes on; --on-consult stop sends it
    and stops. The checkpoint always picks CONSULT."""
    from env import CONSULT
    policy = Policy()
    with torch.no_grad():
        policy.policy_head.weight.zero_()
        policy.policy_head.bias.fill_(-50.0)
        policy.policy_head.bias[CONSULT] = 50.0
    path = tmp_path / "consult.pt"
    torch.save({"policy": policy.state_dict(), "iteration": 0, "obs_size": list(DEFAULT_OBS_SIZE), "obs_depth": True}, path)

    p = play("--checkpoint", str(path), "--headless", "--max-decisions", "3", "--quit-at-end")
    assert p.returncode == 0, p.stdout[-2000:] + p.stderr[-2000:]
    steps = [l for l in p.stdout.splitlines() if re.match(r"\[watch +\d+\]", l)]
    assert len(steps) == 3 and all(" CONSULT " in l and "EXPLORE" in l for l in steps), steps
    assert "stopped after 3 decisions: max decisions" in p.stdout

    p = play("--checkpoint", str(path), "--headless", "--on-consult", "stop", "--quit-at-end")
    assert p.returncode == 0, p.stdout[-2000:] + p.stderr[-2000:]
    assert "stopped after 0 decisions: CONSULT" in p.stdout, p.stdout[-2000:]


@pytest.mark.game
def test_eval_rollouts(checkpoint):
    """eval.py, the training-style eval: every IWAD/map pair plays
    --group-size rollouts with training's rules, --groups pairs at a time,
    and prints one line per pair and an overall one."""
    p = subprocess.run([PY, "eval.py", "--checkpoint", str(checkpoint), "--maps", "1 1,1 2", "--group-size", "2",
                        "--groups", "1", "--max-tics", "70", "--device", "cpu", "--headless"],
                       cwd=HERE, capture_output=True, text=True, timeout=300)
    assert p.returncode == 0, p.stdout[-2000:] + p.stderr[-2000:]
    rows = {l.split(" ret ")[0].strip(): l for l in p.stdout.splitlines() if l.startswith("  ")}
    assert set(rows) == {"1 1", "1 2", "all"}, p.stdout
    assert all("(2 rollouts)" in rows[k] for k in ("1 1", "1 2")) and "(4 rollouts)" in rows["all"]
    assert all(int(re.search(r"tics +(\d+)", l).group(1)) <= 70 for l in rows.values()), "the tic limit holds"
