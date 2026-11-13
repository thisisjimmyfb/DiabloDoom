"""Tests for learn.py, the learner of the split trainer (SPEC.md §7.1, §7.4,
§7.5). train.py has its own tests in test_train.py.

test_learner runs a real learn.py on hand-written batches (no game);
test_split runs it with real eval.py workers (game)."""

from __future__ import annotations

import json
import subprocess
import sys
import time
import types
from pathlib import Path

import numpy as np
import pytest
import torch

from learn import build_batch, collect, group_advantages, train_metrics, update, value_targets
from model import BATCH_DIR, DONE_FILE, Policy, pack_rollouts, unpack_rollouts

HERE = Path(__file__).resolve().parents[1]
PY = sys.executable
# Small and fast: one group of two short rollouts, one held-out map.
TINY = ["--groups", "1", "--group-size", "2", "--max-tics", "300",
        "--eval-maps", "1 5", "--eval-episodes", "1"]


# ---------------------------------------------------------------------------
# No game
# ---------------------------------------------------------------------------

@pytest.fixture
def groups(rng, make_rollout):
    """Four groups of different sizes; the second has equal returns."""
    gs = [[make_rollout(int(n)) for n in rng.integers(5, 40, size)] for size in (8, 8, 5, 3)]
    for r in gs[1]:
        r["ret"], r["rew"] = 1.0, [1.0 / r["len"]] * r["len"]
    return gs


def test_outcome_advantages(groups):
    """SPEC.md §7.1: A_i = (R_i - mean) / (std + eps) within each group;
    a zero-variance group is dropped and counted."""
    advs, collapsed = group_advantages(groups, "outcome")
    assert collapsed == 1 and len(advs) == sum(len(g) for g in groups)
    i = 0
    for g in groups:
        rets = np.array([r["ret"] for r in g])
        for r in g:
            if g is groups[1]:
                assert advs[i] is None
            else:
                assert advs[i].shape == (r["len"],)
                assert np.allclose(advs[i], (r["ret"] - rets.mean()) / (rets.std() + 1e-8))
            i += 1


def test_process_advantages(groups):
    """Per-step rewards normalized across the group, summed from each step on."""
    advs, collapsed = group_advantages(groups, "process")
    assert collapsed == 0, "differing per-step rewards never collapse"
    i = 0
    for g in groups:
        allr = np.concatenate([r["rew"] for r in g])
        for r in g:
            norm = (np.array(r["rew"]) - allr.mean()) / (allr.std() + 1e-8)
            assert np.allclose(advs[i], np.cumsum(norm[::-1])[::-1], atol=1e-5)
            i += 1


def test_value_targets():
    """Return-to-go discounted per tic: gamma_tic ** tics per step, 0 bootstrap."""
    rew, tics, g = [1.0, 0.0, 2.0], [10, 30, 5], 0.99
    expect = [1.0 + g ** 10 * (0.0 + g ** 30 * 2.0), 0.0 + g ** 30 * 2.0, 2.0]
    assert np.allclose(value_targets(rew, tics, g), expect)
    assert np.allclose(value_targets(rew, tics, 1.0), [3.0, 2.0, 2.0])


def test_update(rng, make_rollout):
    """build_batch keeps only rollouts with an advantage; one update from
    unpacked batch data changes the weights."""
    G = 4
    recs = [make_rollout(int(n)) for n in rng.integers(5, 30, 3 * G)]
    back = unpack_rollouts(pack_rollouts(recs, [i // G for i in range(3 * G)], [0] * (3 * G)), ["FREEDOOM1"])
    for r in back[2 * G:]:                            # third group collapses
        r["ret"] = 0.5
    gs = [back[:G], back[G:2 * G], back[2 * G:]]
    advs, collapsed = group_advantages(gs, "outcome")
    batch = build_batch(back, advs, 0.999)
    assert collapsed == 1 and batch["obs"].shape == (sum(r["len"] for r in back[:2 * G]), 4, 84, 84)
    assert build_batch(back[2 * G:], advs[2 * G:], 0.999) is None, "nothing to train on"
    args = types.SimpleNamespace(epochs=1, minibatch=64, clip=0.2, ent_coef=0.01, vf_coef=0.5,
                                 beta=0.0, max_grad_norm=0.5)
    policy = Policy()
    before = [p.detach().clone() for p in policy.parameters()]
    upd = update(policy, torch.optim.Adam(policy.parameters(), lr=1e-3), None, batch, args, torch.device("cpu"))
    assert any(not torch.equal(a, b) for a, b in zip(before, policy.parameters()))
    assert {"pg_loss", "v_loss", "entropy", "approx_kl", "clipfrac"} <= set(upd)
    m = train_metrics(back, len(gs), collapsed)
    assert m["steps"] == sum(r["len"] for r in back) and abs(m["collapsed_share"] - 1 / 3) < 1e-9


@pytest.fixture
def batch_dir(tmp_path) -> Path:
    bd = tmp_path / BATCH_DIR
    bd.mkdir()
    return bd


def never_poll():
    raise AssertionError("collect blocked")


def test_collect_drops_stale(batch_dir, make_batch):
    """Stale batches are dropped, and consumed and stale files deleted."""
    args = types.SimpleNamespace(min_groups=4, max_lag=2)
    make_batch(batch_dir, "vm0", 0)                   # stale at learner version 5
    make_batch(batch_dir, "vm0", 4)
    make_batch(batch_dir, "local", 5)
    gs, intake = collect(batch_dir, 5, args, never_poll)
    assert len(gs) == 4 and all(len(g) == 3 for g in gs)
    assert intake["dropped_batches"] == 1 and intake["actors"] == ["local", "vm0"]
    assert intake["max_lag"] == 1 and not list(batch_dir.glob("*.npz"))


def test_collect_waits_for_min_groups(batch_dir, make_batch):
    args = types.SimpleNamespace(min_groups=4, max_lag=2)
    make_batch(batch_dir, "vm0", 5, groups=1)
    polls = []

    def poll():
        polls.append(1)
        if len(polls) == 1:
            make_batch(batch_dir, "local", 5, groups=3)
    gs, intake = collect(batch_dir, 5, args, poll)
    assert len(polls) == 1 and len(gs) == 4 and intake["batches"] == 2


def test_collect_skips_half_written(batch_dir, make_batch):
    """A half-written batch never crashes the learner; it waits for the next poll."""
    args = types.SimpleNamespace(min_groups=4, max_lag=2)
    bad = batch_dir / "vm1_1.npz"
    bad.write_bytes(b"PK\x03\x04 half-synced")
    make_batch(batch_dir, "vm0", 5, groups=4)
    gs, intake = collect(batch_dir, 5, args, never_poll)
    assert len(gs) == 4 and intake["batches"] == 1
    assert [p.name for p in batch_dir.glob("*.npz")] == [bad.name]


# ---------------------------------------------------------------------------
# Process runs
# ---------------------------------------------------------------------------

def metrics(path: Path) -> list[dict]:
    return [json.loads(line) for line in path.read_text().splitlines() if line.strip()]


def wait_until(cond, proc: subprocess.Popen, what: str, timeout: float = 120):
    deadline = time.time() + timeout
    while time.time() < deadline:
        v = cond()
        if v:
            return v
        if proc.poll() is not None:
            pytest.fail(f"exited (rc {proc.returncode}) before: {what}\n{proc.stdout.read()[-3000:]}")
        time.sleep(0.1)
    pytest.fail(f"timed out waiting for: {what}")


@pytest.fixture
def popen():
    """popen(cmd) -> a running process; killed at teardown if still alive."""
    procs = []

    def start(cmd: list[str]) -> subprocess.Popen:
        p = subprocess.Popen(cmd, cwd=HERE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        procs.append(p)
        return p
    yield start
    for p in procs:
        if p.poll() is None:
            p.kill()
            p.wait()


def test_learner(tmp_path, popen, make_batch):
    """learn.py on hand-written batches, no game: waits for --min-groups,
    updates, checkpoints, publishes each version, writes DONE and
    --outpath; then resumes and drops a stale batch."""
    run_dir, out = tmp_path / "run", tmp_path / "out"
    spool = run_dir / "spool"
    bd = spool / BATCH_DIR

    def version():
        try:
            return torch.load(spool / "weights.pt", weights_only=False)["version"]
        except Exception:
            return None

    def start(iterations: int) -> subprocess.Popen:
        return popen([PY, "learn.py", "--run-dir", str(run_dir), "--min-groups", "2",
                      "--iterations", str(iterations), "--max-lag", "1", "--poll", "0.1",
                      "--device", "cpu", "--outpath", str(out)])

    proc = start(2)
    wait_until(lambda: version() == 0, proc, "weights v0 at startup")
    make_batch(bd, "a", 0, groups=1)
    time.sleep(1.0)
    assert not list(run_dir.glob("ckpt_*.pt")), "one group is below --min-groups 2: no update"
    make_batch(bd, "b", 0, groups=1)
    wait_until(lambda: version() == 1, proc, "weights v1 after the first update")
    make_batch(bd, "a", 1, groups=2)
    o, _ = proc.communicate(timeout=120)
    assert proc.returncode == 0, o[-3000:]
    m = metrics(run_dir / "metrics.jsonl")
    assert [(r["iteration"], r["version"]) for r in m] == [(0, 0), (1, 1)]
    assert m[0]["actors"] == ["a", "b"] and m[0]["groups"] == 2 and m[1]["actors"] == ["a"]
    assert sorted(f.name for f in run_dir.glob("ckpt_*.pt")) == ["ckpt_000000.pt", "ckpt_000001.pt"]
    assert version() == 2 and (spool / DONE_FILE).exists() and not list(bd.iterdir())
    assert (out / "ckpt_000001.pt").exists(), "--outpath receives the final checkpoint"

    proc = start(3)                                   # resume
    wait_until(lambda: not (spool / DONE_FILE).exists(), proc, "DONE cleared at startup")
    wait_until(lambda: version() == 2, proc, "the resumed weights republished")
    make_batch(bd, "a", 0, groups=2)                  # 2 versions behind, --max-lag 1: dropped
    make_batch(bd, "a", 2, groups=2)
    o, _ = proc.communicate(timeout=120)
    assert proc.returncode == 0, o[-3000:]
    assert "resuming from" in o and "ckpt_000001.pt" in o
    last = metrics(run_dir / "metrics.jsonl")[-1]
    assert (last["iteration"], last["version"], last["dropped_batches"]) == (2, 2, 1)
    assert version() == 3 and (out / "ckpt_000002.pt").exists()


@pytest.mark.game
def test_split(tmp_path, popen):
    """learn.py with real workers: batches flow in, weights flow out, DONE stops all."""
    run_dir = tmp_path / "run"
    spool = run_dir / "spool"
    procs = {
        "learner": popen([PY, "learn.py", "--run-dir", str(run_dir), "--min-groups", "1",
                          "--iterations", "2", "--poll", "0.2"]),
        "actor": popen([PY, "eval.py", "--spool", str(spool), "--actor-id", "t0", *TINY,
                        "--poll", "0.2", "--device", "cpu"]),
        "heldout": popen([PY, "eval.py", "--spool", str(spool), "--heldout", "--run-dir", str(run_dir),
                          *TINY, "--poll", "0.2", "--device", "cpu"]),
    }
    outs = {}
    deadline = time.time() + 600
    for name, proc in procs.items():
        outs[name], _ = proc.communicate(timeout=max(1.0, deadline - time.time()))
        assert proc.returncode == 0, f"{name}:\n{outs[name][-3000:]}"
    m = [r for r in metrics(run_dir / "metrics.jsonl") if r["kind"] == "train"]
    assert [(r["iteration"], r["version"]) for r in m] == [(0, 0), (1, 1)]
    assert all(r["actors"] == ["t0"] for r in m)
    assert torch.load(spool / "weights.pt", weights_only=False)["version"] == 2
    assert all("learner is done" in outs[k] for k in ("actor", "heldout")), "both workers exit on DONE"
    # The worker plays ahead until --max-pending (2) of its batches are
    # unconsumed, so up to 2 complete batches can outlive the last update
    # (a resumed learner consumes them). Never a temp file.
    left = list((spool / BATCH_DIR).glob("*"))
    assert all(f.suffix == ".npz" for f in left) and len(left) <= 2, [f.name for f in left]
    ev = metrics(run_dir / "eval.jsonl")
    assert ev and all("eval_sectors" in r for r in ev)
