"""Tests for train.py, the single-box trainer (SPEC.md §7.1, §7.4): rollouts
and updates in one process. All of them launch the game."""

from __future__ import annotations

import json
import re
import subprocess
import sys
from pathlib import Path

import pytest
import torch

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
    """2 iterations with held-out evals, then the same command
    with --iterations 3 resumes from the newest checkpoint; each run prints
    its start-to-end eval delta."""
    run_dir = tmp_path / "run"
    args = ["--eval-every", "1", "--capture-envs", "0"]
    o = train(run_dir, *args, "--iterations", "2")
    assert "[eval delta] iteration 0 -> 2" in o, "start and end of the run"
    assert sorted(f.name for f in run_dir.glob("*.pt")) == ["doom-runner.pt"], "one checkpoint file"
    assert torch.load(run_dir / "doom-runner.pt", weights_only=False)["iteration"] == 1
    assert [r["iteration"] for r in metrics(run_dir, "train")] == [0, 1]
    assert [r["iteration"] for r in metrics(run_dir, "eval")] == [0, 1, 2], \
        "a baseline eval, then one after every update"
    rows = [json.loads(l) for l in (run_dir / "rollouts.jsonl").read_text().splitlines()]
    assert [(r["iteration"], r["env"]) for r in rows] == [(0, 0), (0, 1), (1, 0), (1, 1)], \
        "one row per training rollout"
    for r in rows:
        assert r["end"] in ("exit", "death", "key", "portal", "time", "combat"), r
        assert r["end"] != "time" or r["tics"] >= r["max_tics"], "time: the limit was used up"
        assert r["steps"] == sum(r["actions"].values())
        assert abs(sum(r["terms"].values()) - r["return"]) < 0.01, "the terms add up to the return"
        assert r["pos"][0][2] >= 0 and r["wall_start"] <= r["wall_end"] and r["frontier"]

    o = train(run_dir, *args, "--iterations", "3")
    assert "resuming from" in o and "doom-runner.pt (iteration 1)" in o
    assert [r["iteration"] for r in metrics(run_dir, "train")] == [0, 1, 2], \
        "the resumed run continues at iteration 2 (no repeat)"
    assert [r["iteration"] for r in metrics(run_dir, "eval")] == [0, 1, 2, 2, 3], \
        "each run evals at its start"
    assert "[eval delta] iteration 2 -> 3" in o
    assert torch.load(run_dir / "doom-runner.pt", weights_only=False)["iteration"] == 2


def test_interrupt_eval(tmp_path):
    """Ctrl-C mid-run (SIGINT to the whole process group, workers included,
    as a terminal sends it): the run still evals the last saved checkpoint
    and prints the start-to-end delta."""
    import os
    import signal
    import time
    run_dir = tmp_path / "run"
    p = subprocess.Popen([PY, "train.py", "--run-dir", str(run_dir), *TINY, "--iterations", "50",
                          "--capture-envs", "0"], cwd=HERE, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True, start_new_session=True)
    try:
        deadline = time.time() + 300
        while not (run_dir / "metrics.jsonl").exists() or not metrics(run_dir, "train"):
            assert p.poll() is None and time.time() < deadline, "no update finished"
            time.sleep(0.5)
        os.killpg(p.pid, signal.SIGINT)
        out, _ = p.communicate(timeout=300)
    finally:
        if p.poll() is None:
            os.killpg(p.pid, signal.SIGKILL)
    assert p.returncode == 0, out[-3000:]
    assert "end-of-run eval of that checkpoint" in out, out[-3000:]
    saved = torch.load(run_dir / "doom-runner.pt", weights_only=False)["iteration"]
    assert [r["iteration"] for r in metrics(run_dir, "eval")] == [0, saved + 1]
    assert f"[eval delta] iteration 0 -> {saved + 1}" in out


def test_rotation(tmp_path):
    """Two maps, one group, --rotate-every 1: the group alternates maps,
    its games restart on the new one, and a map it comes back to starts
    from that map's saved progress."""
    run_dir = tmp_path / "run"
    o = train(run_dir, "--maps", "1 1,1 2", "--rotate-every", "1", "--iterations", "3",
              "--eval-every", "0", "--capture-envs", "0")
    assert "rotation 1: groups now play ['1 2']" in o and "rotation 2: groups now play ['1 1']" in o
    rows = [json.loads(l) for l in (run_dir / "rollouts.jsonl").read_text().splitlines()]
    assert [r["map"] for r in rows if r["env"] == 0] == ["1 1", "1 2", "1 1"]
    assert [m["groups"] for m in metrics(run_dir, "train")] == [["1 1"], ["1 2"], ["1 1"]]
    back = [r for r in rows if r["iteration"] == 2]
    first = [r for r in rows if r["iteration"] == 0]
    if any(r.get("picked") and r["frontier"] == "saved" for r in first):
        assert all(r["start"] == "frontier" for r in back), "E1M1's progress waited for its turn"


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
    assert [(r["iteration"], r["lag"]) for r in metrics(run_dir, "train")] == [(0, 0), (1, 1), (2, 1)]
    o = train(run_dir, *args, "--iterations", "4")
    assert "resuming from" in o and "doom-runner.pt (iteration 2)" in o
    assert [(r["iteration"], r["lag"]) for r in metrics(run_dir, "train")][-1] == (3, 0)


def test_frontier_start():
    """A frontier start resumes where the best rollout ended: the save is
    taken after the batch, copied to the group's other instance, and a
    reset from it puts the player at that rollout's end position; the map
    start is in slot 4 before, and again once the frontier is cleared."""
    from env import ACTIONS
    from env.vector import GroupVecEnv
    from model import FRONTIER_SLOT, Frontier, make_env
    import functools
    import numpy as np

    pairs = [(None, "1 1")] * 2
    vec = GroupVecEnv([functools.partial(make_env, None, "1 1", 1, 3, 150, None, []) for _ in pairs])
    try:
        f = Frontier(True, pairs)
        _, infos = vec.reset([0, 1], [{"seed": 1}, {"seed": 1}])
        starts = [i["state"]["sectors"] for i in infos]
        assert all(i["slot"] == FRONTIER_SLOT for i in infos), "the map start is slot 4 too"
        start = (infos[0]["state"]["x"], infos[0]["state"]["y"])
        walk, turn = ACTIONS.index("PRESS W"), ACTIONS.index("PRESS L")
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
                 "sectors_gained": ends[i]["state"]["sectors"] - starts[i], "end_info": ends[i]} for i in range(2)]
        assert recs[0]["sectors_gained"] > 0, "the walker should reach a new sector"
        best = int(np.argmax(rets))
        saved = f.update(vec, recs)
        assert list(saved) == ["default_1_1"]
        opts = f.options([{"seed": 1}, {"seed": 1}], 2)
        assert all(o["slot"] == FRONTIER_SLOT for o in opts)
        _, infos = vec.reset([0, 1], opts)
        want = (ends[best]["state"]["x"], ends[best]["state"]["y"])
        for info in infos:               # both instances, including the one that didn't save
            assert (info["state"]["x"], info["state"]["y"]) == want

        # Clearing the frontier (an exit) puts the map start back in slot 4.
        vec.call([0, 1], "install_state", (FRONTIER_SLOT, None))
        _, infos = vec.reset([0, 1], [{"seed": 1, "slot": FRONTIER_SLOT}] * 2)
        for info in infos:
            assert (info["state"]["x"], info["state"]["y"]) == start
    finally:
        vec.close()
