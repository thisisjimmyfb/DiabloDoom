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
    """--capture-envs 1 records timestamped replay segments into replays/env00/."""
    run_dir = tmp_path / "run"
    train(run_dir, "--iterations", "1", "--eval-every", "0", "--capture-envs", "1", "--max-tics", "1200")
    files = sorted((run_dir / "replays").rglob("*"))
    segs = [f for f in files if f.is_file()]
    assert segs and all(f.parent.name == "env00" for f in segs), [str(f) for f in files]
    assert all(re.fullmatch(r"diablodoom_\d{8}_\d{6}_\d{3}(_\d\d)?\.mp4", f.name) for f in segs)
    assert all(f.stat().st_size > 0 for f in segs)
