"""Tests for model.py, the pieces shared by train.py, eval.py and learn.py
(SPEC.md §7.1, §7.4, §7.5): the batch file format, IWAD/group allocation,
checkpoint I/O and the spool's weights file. No game instances."""

from __future__ import annotations

import json
from pathlib import Path

import numpy as np
import torch

from model import (BATCH_DIR, FRONTIER_SLOT, Frontier, Policy, allocate_groups, capture_dirs, export_latest_checkpoint,
                   init_weights_if_absent, load_latest_checkpoint, pack_rollouts, publish_weights, read_weights,
                   restore_rng, rng_state, save_checkpoint, unpack_rollouts)

MAPS = ["1 1", "1 2", "1 3", "1 4"]
TWO_IWADS = ["/w/FREEDOOM1.WAD", "/w/DOOM.WAD"]


def test_pack_roundtrip(rng, make_rollout):
    """Batch frames are stored once; every stack rebuilds exactly."""
    recs = [make_rollout(int(n)) for n in rng.integers(1, 60, 6)]
    arrays = pack_rollouts(recs, [0, 0, 0, 1, 1, 1], [0, 0, 1, 1, 0, 1])
    back = unpack_rollouts(arrays, ["FREEDOOM1", "DOOM"])
    for r, b in zip(recs, back):
        assert np.array_equal(np.stack(r["obs"]), b["obs"]), "stacks must rebuild exactly"
        assert np.allclose(np.stack(r["priv"]), b["priv"])
        assert list(b["act"]) == r["act"] and b["tics"] == r["tics"] and b["len"] == r["len"]
        assert np.allclose(b["logp"], r["logp"]) and np.allclose(b["rew"], r["rew"], atol=1e-6)
        assert b["exit"] == r["exit"] and b["sectors_gained"] == r["sectors_gained"]
    assert [b["group"] for b in back] == [0, 0, 0, 1, 1, 1]
    assert [b["iwad"] for b in back] == ["FREEDOOM1", "FREEDOOM1", "DOOM", "DOOM", "FREEDOOM1", "DOOM"]
    pushed = sum(1 for r in recs for a, b in zip(r["obs"], r["obs"][1:]) if not np.array_equal(a, b))
    # One frame per rollout start plus one per push: nothing else stored.
    assert arrays["frames"].shape[0] == len(recs) + pushed


def test_allocate_groups_every_pair(capsys):
    """SPEC.md §7.1: I*M groups cover every IWAD/map pair once, no warning."""
    pairs = allocate_groups(TWO_IWADS, MAPS, 8)
    assert sorted(pairs) == sorted((w, m) for w in TWO_IWADS for m in MAPS)
    assert capsys.readouterr().out == ""


def test_allocate_groups_split_and_warning(capsys):
    """M groups: every map trained, IWADs split evenly, the gaps are named."""
    pairs = allocate_groups(TWO_IWADS, MAPS, 4)
    warn = capsys.readouterr().out
    assert [m for _, m in pairs] == MAPS
    assert sum(w == TWO_IWADS[0] for w, _ in pairs) == 2 and "warning" in warn
    missing = {(w, m) for w in TWO_IWADS for m in MAPS} - set(pairs)
    assert all(f"{Path(w).stem}:{m}" in warn for w, m in missing)


def test_allocate_groups_multiples_and_default():
    """2*I*M groups give every pair twice; one default IWAD cycles the maps."""
    pairs = allocate_groups(TWO_IWADS, MAPS, 16)
    assert all(pairs.count((w, m)) == 2 for w in TWO_IWADS for m in MAPS)
    assert [m for _, m in allocate_groups([None], MAPS, 6)] == MAPS + MAPS[:2]


def test_checkpoints(tmp_path):
    """SPEC.md §7.4: keep N, fall back past a corrupt newest, clear
    interrupted writes, export the newest."""
    run = tmp_path / "run"
    run.mkdir()
    for it in range(5):
        save_checkpoint(run / f"ckpt_{it:06d}.pt", {"iteration": it}, keep=3)
    assert sorted(p.name for p in run.iterdir()) == [f"ckpt_{i:06d}.pt" for i in (2, 3, 4)]
    (run / "ckpt_000004.pt").write_bytes(b"truncated")       # newest is corrupt
    (run / "ckpt_000005.pt.tmp").write_bytes(b"half")         # interrupted write
    assert load_latest_checkpoint(run)["iteration"] == 3
    assert not list(run.glob("*.tmp"))
    dst = export_latest_checkpoint(run, tmp_path / "out")
    assert dst.name == "ckpt_000004.pt" and dst.read_bytes() == b"truncated"
    empty = tmp_path / "empty"
    empty.mkdir()
    assert export_latest_checkpoint(empty, tmp_path / "out") is None
    assert load_latest_checkpoint(empty) is None


def test_rng_roundtrip():
    """Every RNG, the start-state sampler included, resumes exactly."""
    sampler = np.random.default_rng(9)
    state = rng_state(sampler)
    a = (np.random.rand(), torch.rand(1).item(), sampler.integers(1 << 30))
    sampler2 = np.random.default_rng(0)
    restore_rng(state, sampler2)
    assert (np.random.rand(), torch.rand(1).item(), sampler2.integers(1 << 30)) == a


def test_weights_file(tmp_path):
    """weights.pt is versioned and loads into a fresh Policy; a missing or
    half-written file reads as 'no weights yet'."""
    assert read_weights(tmp_path) is None
    (tmp_path / "weights.pt").write_bytes(b"partial")         # replaced mid-read
    assert read_weights(tmp_path) is None
    policy = Policy()
    publish_weights(tmp_path, 7, policy, "run1")
    w = read_weights(tmp_path)
    assert w["version"] == 7 and w["run_id"] == "run1"
    p2 = Policy()
    p2.load_state_dict(w["policy"])
    assert all(torch.equal(a, b) for a, b in zip(policy.state_dict().values(), p2.state_dict().values()))
    assert not list(tmp_path.glob("*.tmp"))


def test_batch_file(tmp_path, make_batch):
    """A batch file as a worker writes it reads back through unpack_rollouts."""
    bd = tmp_path / BATCH_DIR
    bd.mkdir()
    path = make_batch(bd, "vm0", 4, groups=2, G=3)
    z = np.load(path, allow_pickle=False)
    meta = json.loads(str(z["meta"]))
    back = unpack_rollouts(z, meta["iwads"])
    assert meta["version"] == 4 and [b["group"] for b in back] == [0, 0, 0, 1, 1, 1]
    assert path.name.startswith("vm0_") and not list(bd.glob("*.tmp"))


def test_init_weights_if_absent(tmp_path):
    """Random version-0 weights are created once, never over existing ones."""
    spool = tmp_path / "spool"
    first, second = Policy(), Policy()
    assert init_weights_if_absent(spool, first, "init-a") is True
    assert init_weights_if_absent(spool, second, "init-b") is False, "never overwrites"
    w = read_weights(spool)
    assert w["version"] == 0 and w["run_id"] == "init-a"
    assert all(torch.equal(a, b) for a, b in zip(first.state_dict().values(), w["policy"].values()))
    assert sorted(p.name for p in spool.iterdir()) == ["weights.pt"], "no temp files left"
    publish_weights(spool, 3, second, "learner")      # a learner's weights are never replaced
    assert init_weights_if_absent(spool, Policy(), "init-c") is False
    assert read_weights(spool)["run_id"] == "learner"


class _FakeVec:
    """GroupVecEnv stand-in for Frontier: save_state returns a fake
    savegame naming the env; install_state is recorded per env."""

    def __init__(self, n):
        self.slots = [None] * n

    def call(self, idx, name, args=()):
        out = []
        for i in idx:
            if name == "save_state":
                out.append(f"save-of-env{i}".encode())
            elif name == "install_state":
                assert args[0] == FRONTIER_SLOT
                self.slots[i] = args[1]
                out.append(None)
        return out


def _end(ret, mode="EXPLORE", death=False, exit=False):
    return {"ret": ret, "death": death, "exit": exit, "end_info": {"mode": mode}}


def test_frontier(tmp_path):
    """Frontier starts: each map's best live, exploring, non-exit rollout
    is saved and copied to that map's instances (and kept on disk); groups
    start from it with probability prob; an exit clears the map's frontier."""
    pairs = [(None, "1 1")] * 2 + [(None, "1 2")] * 2      # 2 groups of G=2
    vec = _FakeVec(4)
    f = Frontier(tmp_path, 1.0, pairs)
    opts = f.options([{"seed": 0} for _ in range(4)], np.random.default_rng(0), 2)
    assert not any("slot" in o for o in opts), "no frontier yet: map starts"

    # E1M1: env 0 scores higher but died, env 1 is saved. E1M2: env 3
    # scores higher but ended in combat, env 2 is saved.
    saved = f.update(vec, [_end(9, death=True), _end(3), _end(1), _end(7, mode="PLANNING")])
    assert saved == {"default_1_1": 3, "default_1_2": 1}
    assert vec.slots == [b"save-of-env1"] * 2 + [b"save-of-env2"] * 2
    assert (tmp_path / "default_1_1.dsg").read_bytes() == b"save-of-env1"

    opts = f.options([{"seed": 0} for _ in range(4)], np.random.default_rng(0), 2)
    assert all(o["slot"] == FRONTIER_SLOT and o["frontier"] for o in opts)

    # A restart finds the frontiers on disk and reinstalls them.
    vec2 = _FakeVec(4)
    Frontier(tmp_path, 1.0, pairs).install_all(vec2)
    assert vec2.slots == vec.slots

    # An exit on E1M1 clears its frontier (memory, instances, disk).
    saved = f.update(vec, [_end(50, exit=True), _end(2), _end(1), _end(2)])
    assert saved["default_1_1"] is None and not (tmp_path / "default_1_1.dsg").exists()
    assert vec.slots[:2] == [None, None] and vec.slots[2:] == [b"save-of-env3"] * 2
    opts = f.options([{"seed": 0} for _ in range(4)], np.random.default_rng(0), 2)
    assert "slot" not in opts[0] and opts[2]["slot"] == FRONTIER_SLOT

    # prob 0: off (no saves, map starts only).
    off = Frontier(tmp_path / "off", 0.0, pairs)
    assert off.update(vec, [_end(1)] * 4) == {}
    assert not any("slot" in o for o in off.options([{} for _ in range(4)], np.random.default_rng(0), 2))


def test_capture_dirs():
    """--capture-envs n records the first n envs of each IWAD (groups take
    the IWADs in rotation, so env 0..7 are all the first IWAD)."""
    groups = allocate_groups(TWO_IWADS, MAPS, 4)
    pairs = [groups[i // 8] for i in range(32)]
    dirs = capture_dirs(pairs, 1, Path("/r"))
    assert {i: d for i, d in enumerate(dirs) if d} == {0: "/r/env00_FREEDOOM1", 8: "/r/env08_DOOM"}
    assert sum(d is not None for d in capture_dirs(pairs, 2, Path("/r"))) == 4
    assert capture_dirs(pairs, 0, Path("/r")) == [None] * 32
    assert capture_dirs([(None, "1 1")] * 2, 1, Path("/r")) == ["/r/env00_default", None]
