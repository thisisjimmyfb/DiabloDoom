"""Tests for train.py, the single-box trainer (SPEC.md §7.1, §7.4): rollouts
and updates in one process. All of them launch the game."""

from __future__ import annotations

import json
import re
import subprocess
import sys
from pathlib import Path

import pytest

pytestmark = pytest.mark.game

HERE = Path(__file__).resolve().parents[1]
WAD = HERE.parents[1] / "wad"
PY = sys.executable
# Small and fast: one group of two short rollouts, one held-out map.
TINY = ["--groups", "1", "--group-size", "2", "--max-tics", "300",
        "--eval-maps", "1 5", "--eval-episodes", "1"]


def train(run_dir: Path, *args: str) -> str:
    p = subprocess.run([PY, "train.py", "--run-dir", str(run_dir), *TINY, *args], cwd=HERE,
                       capture_output=True, text=True, timeout=600)
    assert p.returncode == 0, p.stdout[-3000:] + p.stderr[-3000:]
    return p.stdout


def metrics(run_dir: Path, kind: str) -> list[dict]:
    lines = (run_dir / "metrics.jsonl").read_text().splitlines()
    return [r for r in map(json.loads, lines) if r["kind"] == kind]


def test_resume(tmp_path):
    """2 iterations with held-out evals and --outpath, then the same command
    with --iterations 3 resumes from the newest checkpoint."""
    run_dir, out = tmp_path / "run", tmp_path / "out"
    args = ["--eval-every", "1", "--capture-envs", "0", "--outpath", str(out)]
    train(run_dir, *args, "--iterations", "2")
    assert sorted(f.name for f in run_dir.glob("ckpt_*.pt")) == ["ckpt_000000.pt", "ckpt_000001.pt"]
    assert [r["iteration"] for r in metrics(run_dir, "train")] == [0, 1]
    assert [r["iteration"] for r in metrics(run_dir, "eval")] == [0, 1, 2], \
        "a baseline eval, then one after every update"
    assert (out / "ckpt_000001.pt").exists(), "--outpath receives the final checkpoint"

    o = train(run_dir, *args, "--iterations", "3")
    assert "resuming from" in o and "ckpt_000001.pt" in o
    assert [r["iteration"] for r in metrics(run_dir, "train")] == [0, 1, 2], \
        "the resumed run continues at iteration 2 (no repeat)"
    assert [r["iteration"] for r in metrics(run_dir, "eval")] == [0, 1, 2, 3], "no new baseline"
    assert (out / "ckpt_000002.pt").exists()


def test_iwads(tmp_path):
    """Groups split over two IWADs (§7.1): per-IWAD train metrics and every
    eval map on every IWAD."""
    iwads = [WAD / "FreedoomPhase1" / "FREEDOOM1.WAD", WAD / "UltimateDoom" / "DOOM.WAD"]
    if not all(w.exists() for w in iwads):
        pytest.skip("needs wad/FreedoomPhase1/FREEDOOM1.WAD and wad/UltimateDoom/DOOM.WAD")
    run_dir = tmp_path / "run"
    o = train(run_dir, "--iwads", *map(str, iwads), "--groups", "2", "--maps", "1 1",
              "--iterations", "1", "--eval-every", "1", "--capture-envs", "0")
    assert "FREEDOOM1:1 1" in o and "DOOM:1 1" in o, "each IWAD gets a group"
    (m,) = metrics(run_dir, "train")
    assert {"sectors_FREEDOOM1", "sectors_DOOM", "exit_rate_FREEDOOM1", "exit_rate_DOOM"} <= set(m)
    for e in metrics(run_dir, "eval"):
        assert set(e["eval_per_map"]) == {"FREEDOOM1:1 5", "DOOM:1 5"}


def test_capture(tmp_path):
    """--capture-envs 1 records timestamped replay segments into replays/env00_<IWAD>/."""
    run_dir = tmp_path / "run"
    train(run_dir, "--iterations", "1", "--eval-every", "0", "--capture-envs", "1", "--max-tics", "1200")
    files = sorted((run_dir / "replays").rglob("*"))
    segs = [f for f in files if f.is_file()]
    assert segs and all(f.parent.name.startswith("env00_") for f in segs), [str(f) for f in files]
    assert all(re.fullmatch(r"diablodoom_\d{8}_\d{6}_\d{3}(_\d\d)?\.mp4", f.name) for f in segs)
    assert all(f.stat().st_size > 0 for f in segs)


def test_overlap(tmp_path):
    """--overlap: each update runs while the next batch plays with the
    pre-update weights (lag 1); a restart replays the batch in flight with
    the resumed weights (lag 0)."""
    run_dir = tmp_path / "run"
    args = ["--overlap", "--eval-every", "0", "--capture-envs", "0"]
    train(run_dir, *args, "--iterations", "3")
    assert sorted(f.name for f in run_dir.glob("ckpt_*.pt")) == [f"ckpt_{i:06d}.pt" for i in range(3)]
    assert [(r["iteration"], r["lag"]) for r in metrics(run_dir, "train")] == [(0, 0), (1, 1), (2, 1)]
    o = train(run_dir, *args, "--iterations", "4")
    assert "resuming from" in o and "ckpt_000002.pt" in o
    assert [(r["iteration"], r["lag"]) for r in metrics(run_dir, "train")][-1] == (3, 0)


def test_frontier_start():
    """A frontier start resumes where the best rollout ended: the save is
    taken after the batch, copied to the group's other instance, and a
    reset from it puts the player at that rollout's end position."""
    from env import ACTIONS
    from env.vector import GroupVecEnv
    from model import FRONTIER_SLOT, Frontier, make_env
    import functools
    import numpy as np

    pairs = [(None, "1 1")] * 2
    vec = GroupVecEnv([functools.partial(make_env, None, "1 1", 1, 3, 150, None, []) for _ in pairs])
    try:
        f = Frontier(None, 1.0, pairs)
        vec.reset([0, 1], [{"seed": 1}, {"seed": 1}])
        walk, turn = ACTIONS.index("WALK 0"), ACTIONS.index("TURNL")
        recs, ends, active = [], [None, None], [0, 1]
        rets = [0.0, 0.0]
        while active:                    # env 0 walks, env 1 turns in place
            _, rew, term, trunc, infos = vec.step(active, [walk if i == 0 else turn for i in active])
            still = []
            for k, i in enumerate(active):
                rets[i] += float(rew[k])
                if term[k] or trunc[k]:
                    ends[i] = infos[k]
                else:
                    still.append(i)
            active = still
        recs = [{"ret": rets[i], "death": bool(ends[i]["state"].get("dead")), "exit": bool(ends[i]["exit"]),
                 "end_info": ends[i]} for i in range(2)]
        best = int(np.argmax(rets))
        saved = f.update(vec, recs)
        assert list(saved) == ["default_1_1"]
        opts = f.options([{"seed": 1}, {"seed": 1}], np.random.default_rng(0), 2)
        assert all(o["slot"] == FRONTIER_SLOT for o in opts)
        _, infos = vec.reset([0, 1], opts)
        want = (ends[best]["state"]["x"], ends[best]["state"]["y"])
        for info in infos:               # both instances, including the one that didn't save
            assert (info["state"]["x"], info["state"]["y"]) == want
    finally:
        vec.close()
