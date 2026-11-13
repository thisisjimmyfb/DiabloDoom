"""Tests for eval.py, the rollout worker of the split trainer (SPEC.md §7.5).

The game tests run eval.py against a fake learner: the test itself
publishes weights.pt and DONE, then checks what the worker writes."""

from __future__ import annotations

import json
import subprocess
import sys
import time
from pathlib import Path

import numpy as np
import pytest

from env import ACTIONS, CONSULT
from eval import sample_group_options, write_batch
from model import (BATCH_DIR, DONE_FILE, MAP_ACTION, WARMUP_ACTIONS, Policy, pack_rollouts,
                   publish_weights, unpack_rollouts)

HERE = Path(__file__).resolve().parents[1]
PY = sys.executable
# Small and fast: one group of two short rollouts, one held-out map.
TINY = ["--groups", "1", "--group-size", "2", "--max-tics", "300",
        "--eval-maps", "1 5", "--eval-episodes", "1", "--poll", "0.1", "--device", "cpu"]


# ---------------------------------------------------------------------------
# No game
# ---------------------------------------------------------------------------

def test_group_starts_shared():
    """All G members of a group share slot, seed and warm-up; groups differ."""
    G, groups = 4, 6
    opts = sample_group_options(np.random.default_rng(3), groups, G, [3, 4, 5], warmup_max=40)
    assert len(opts) == G * groups
    for g in range(groups):
        members = opts[g * G:(g + 1) * G]
        assert all(m == members[0] for m in members) and members[0]["group"] == g
        assert members[0]["slot"] in (3, 4, 5) and 0 <= members[0]["seed"] < 256
        assert len({id(m) for m in members}) == G, "each env gets its own options dict"
    assert len({(o["seed"], tuple(o["warmup"])) for o in opts[::G]}) > 1


def test_warmup_moves_only():
    """Warm-up actions never toggle the map or consult."""
    assert CONSULT not in WARMUP_ACTIONS and MAP_ACTION not in WARMUP_ACTIONS
    assert len(WARMUP_ACTIONS) == len(ACTIONS) - 2
    opts = sample_group_options(np.random.default_rng(0), 20, 1, [], warmup_max=40)
    assert all(a in WARMUP_ACTIONS for o in opts for a in o["warmup"])


def test_no_slots_no_warmup():
    opts = sample_group_options(np.random.default_rng(0), 2, 2, [], 0)
    assert all("slot" not in o and o["warmup"] == [] for o in opts)


def test_write_batch(tmp_path, make_rollout):
    """Batch files appear complete under <actor>_<time>.npz, never as .tmp."""
    recs = [make_rollout(5) for _ in range(4)]
    meta = {"actor": "vm0", "version": 3, "groups": 2, "group_size": 2, "iwads": ["FREEDOOM1"],
            "group_labels": ["1 1", "1 2"], "rollout_s": 1.0, "created": 0}
    p1 = write_batch(tmp_path, "vm0", pack_rollouts(recs, [0, 0, 1, 1], [0] * 4), meta)
    p2 = write_batch(tmp_path, "vm0", pack_rollouts(recs, [0, 0, 1, 1], [0] * 4), meta)
    assert p1 != p2 and p1.name.startswith("vm0_") and p1.suffix == ".npz"
    assert sorted(f.name for f in tmp_path.iterdir()) == sorted([p1.name, p2.name])
    z = np.load(p1, allow_pickle=False)
    assert json.loads(str(z["meta"])) == meta and len(unpack_rollouts(z, meta["iwads"])) == 4


@pytest.mark.parametrize("bad", ["bad_id", "a/b"])
def test_actor_id_refused(tmp_path, bad):
    """A '_' in --actor-id would let a worker count another's batches as its own."""
    p = subprocess.run([PY, "eval.py", "--spool", str(tmp_path), "--actor-id", bad], cwd=HERE,
                       capture_output=True, text=True, timeout=120)
    assert p.returncode != 0 and "can't contain" in p.stderr, p.stderr[-500:]


# ---------------------------------------------------------------------------
# With the game, against a fake learner
# ---------------------------------------------------------------------------

def wait_until(cond, proc: subprocess.Popen, what: str, timeout: float = 180):
    deadline = time.time() + timeout
    while time.time() < deadline:
        v = cond()
        if v:
            return v
        if proc.poll() is not None:
            pytest.fail(f"eval.py exited (rc {proc.returncode}) before: {what}\n{proc.stdout.read()[-3000:]}")
        time.sleep(0.1)
    pytest.fail(f"timed out waiting for: {what}")


@pytest.fixture
def start_eval():
    """start_eval(*args) -> a running eval.py; killed at teardown if still alive."""
    procs = []

    def start(*args: str) -> subprocess.Popen:
        p = subprocess.Popen([PY, "eval.py", *args, *TINY], cwd=HERE,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        procs.append(p)
        return p
    yield start
    for p in procs:
        if p.poll() is None:
            p.kill()
            p.wait()


def finish(proc: subprocess.Popen, spool: Path) -> None:
    """Write DONE (as the learner does) and expect a clean exit."""
    (spool / DONE_FILE).touch()
    out, _ = proc.communicate(timeout=120)
    assert proc.returncode == 0 and "learner is done" in out, out[-3000:]


@pytest.mark.game
def test_worker(tmp_path, start_eval):
    """Batches hold whole groups from one shared start, played with the
    published version; the worker pauses at --max-pending, picks up new
    weights, and exits on DONE."""
    spool = tmp_path / "spool"
    bd = spool / BATCH_DIR
    publish_weights(spool, 0, Policy(), "fake")
    proc = start_eval("--spool", str(spool), "--actor-id", "t0", "--max-pending", "1")

    def batches():
        return sorted(bd.glob("t0_*.npz")) if bd.exists() else []
    first = wait_until(batches, proc, "the first batch")[0]
    z = np.load(first, allow_pickle=False)
    meta = json.loads(str(z["meta"]))
    recs = unpack_rollouts(z, meta["iwads"])
    assert meta["actor"] == "t0" and meta["version"] == 0
    assert meta["groups"] == 1 and meta["group_size"] == 2 and len(recs) == 2
    assert all(r["group"] == 0 for r in recs)
    assert np.array_equal(recs[0]["obs"][0], recs[1]["obs"][0]), "a group starts from one state"
    for r in recs:
        assert r["obs"].shape == (r["len"], 4, 84, 84) and r["obs"].dtype == np.uint8
        assert r["len"] >= 1 and all(0 <= a < len(ACTIONS) for a in r["act"])
        assert np.all(r["logp"] <= 0) and r["rollout_tics"] > 0

    time.sleep(1.5)                                   # --max-pending 1: no second batch
    assert batches() == [first], "the worker must pause while its batch is unconsumed"

    publish_weights(spool, 1, Policy(), "fake")       # the learner updates and consumes
    first.unlink()
    second = wait_until(batches, proc, "a batch after consumption")[0]
    assert json.loads(str(np.load(second, allow_pickle=False)["meta"]))["version"] == 1, \
        "plays with the newest weights"
    finish(proc, spool)


@pytest.mark.game
def test_heldout(tmp_path, start_eval):
    """One eval.jsonl line per published version, no training data, exits on DONE."""
    spool, run_dir = tmp_path / "spool", tmp_path / "run"
    publish_weights(spool, 0, Policy(), "fake")
    proc = start_eval("--spool", str(spool), "--heldout", "--run-dir", str(run_dir))

    def lines():
        f = run_dir / "eval.jsonl"
        return [json.loads(x) for x in f.read_text().splitlines()] if f.exists() else []
    ev = wait_until(lines, proc, "the eval of version 0")
    assert ev[0]["iteration"] == 0 and ev[0]["kind"] == "eval"
    assert set(ev[0]["eval_per_map"]) == {"1 5"} and "eval_exit_rate" in ev[0]
    publish_weights(spool, 1, Policy(), "fake")
    ev = wait_until(lambda: len(lines()) >= 2 and lines(), proc, "the eval of version 1")
    assert [r["iteration"] for r in ev] == [0, 1], "one eval per new version"
    finish(proc, spool)
    assert not (spool / BATCH_DIR).exists() or not list((spool / BATCH_DIR).iterdir()), \
        "held-out evaluation never writes training data"
