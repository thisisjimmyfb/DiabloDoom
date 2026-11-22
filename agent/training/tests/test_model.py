"""Tests for model.py, the pieces train.py, rollout.py and learn.py share
(SPEC.md §7.1, §7.4): IWAD/group allocation and map rotation, progress
saves, tic budgets, checkpoint I/O and checkpoint upgrades. No game
instances."""

from __future__ import annotations

from pathlib import Path

import pytest

import numpy as np
import torch

from model import (FRONTIER_SLOT, SAVE_SLOT, Frontier, Policy, allocate_groups, capture_dirs,
                   CKPT_NAME, load_latest_checkpoint, restore_rng, rng_state, save_checkpoint)

MAPS = ["1 1", "1 2", "1 3", "1 4"]
REPO = Path(__file__).resolve().parents[3]
TWO_IWADS = ["/w/FREEDOOM1.WAD", "/w/DOOM.WAD"]


def test_allocate_groups_every_pair(capsys):
    """SPEC.md §7.1: I*M groups cover every IWAD/map pair once, no warning."""
    pairs = allocate_groups(TWO_IWADS, MAPS, 8)
    assert sorted(pairs) == sorted((w, m) for w in TWO_IWADS for m in MAPS)
    assert capsys.readouterr().out == ""


def test_allocate_groups_order():
    """Map by map, every IWAD on each map: E1M1 on both IWADs first, so
    the first group of each IWAD plays the first map."""
    pairs = allocate_groups(TWO_IWADS, MAPS, 8)
    assert pairs == [(w, m) for m in MAPS for w in TWO_IWADS]


def test_allocate_groups_split_and_warning(capsys):
    """Fewer groups: the first maps on every IWAD, the gaps are named."""
    pairs = allocate_groups(TWO_IWADS, MAPS, 4)
    warn = capsys.readouterr().out
    assert pairs == [(w, m) for m in MAPS[:2] for w in TWO_IWADS]
    assert "warning" in warn
    missing = {(w, m) for w in TWO_IWADS for m in MAPS} - set(pairs)
    assert all(f"{Path(w).stem}:{m}" in warn for w, m in missing)


def test_allocate_groups_multiples_and_default():
    """2*I*M groups give every pair twice; one default IWAD cycles the maps."""
    pairs = allocate_groups(TWO_IWADS, MAPS, 16)
    assert all(pairs.count((w, m)) == 2 for w in TWO_IWADS for m in MAPS)
    assert [m for _, m in allocate_groups([None], MAPS, 6)] == MAPS + MAPS[:2]


def test_checkpoints(tmp_path):
    """SPEC.md §7.4: one checkpoint file, replaced at every save;
    interrupted writes cleared; an unreadable checkpoint stops the run
    instead of starting it fresh."""
    run = tmp_path / "run"
    run.mkdir()
    for it in range(5):
        save_checkpoint(run / CKPT_NAME, {"iteration": it})
    assert sorted(p.name for p in run.iterdir()) == [CKPT_NAME]
    (run / f"{CKPT_NAME}.tmp").write_bytes(b"half")           # interrupted write
    assert load_latest_checkpoint(run)["iteration"] == 4
    assert not list(run.glob("*.tmp"))
    empty = tmp_path / "empty"
    empty.mkdir()
    assert load_latest_checkpoint(empty) is None

    (run / CKPT_NAME).write_bytes(b"truncated")
    with pytest.raises(SystemExit):
        load_latest_checkpoint(run)


def test_rng_roundtrip():
    """Every RNG, the start-state sampler included, resumes exactly."""
    sampler = np.random.default_rng(9)
    state = rng_state(sampler)
    a = (np.random.rand(), torch.rand(1).item(), sampler.integers(1 << 30))
    sampler2 = np.random.default_rng(0)
    restore_rng(state, sampler2)
    assert (np.random.rand(), torch.rand(1).item(), sampler2.integers(1 << 30)) == a


class _FakeVec:
    """GroupVecEnv stand-in for Frontier: save_state returns a fake
    savegame naming the env; install_state is recorded per env."""

    def __init__(self, n):
        self.store = [{} for _ in range(n)]   # per env: slot -> (data, visited)
        self.saved_to = []

    @property
    def slots(self):                          # what each env loads its start from (slot 4)
        return [s.get(FRONTIER_SLOT, (None, None))[0] for s in self.store]

    @property
    def vis(self):
        return [s.get(FRONTIER_SLOT, (None, None))[1] for s in self.store]

    def call(self, idx, name, args=()):
        out = []
        for i in idx:
            if name == "save_state":
                self.saved_to.append(args[0])
                out.append(f"save-of-env{i}".encode())
            elif name == "visited":
                out.append(f"v{i}")
            elif name == "install_state":
                self.store[i][args[0]] = (args[1], args[2] if len(args) > 2 else None)
                out.append(None)
            elif name == "swap_slots":
                a, b = args
                s = self.store[i]
                s[a], s[b] = s.get(b, (None, None)), s.get(a, (None, None))
                out.append(None)
        return out


def _end(ret, mode="EXPLORE", death=False, exit=False, gained=1, key=False, hp=100, hunted=False):
    return {"ret": ret, "death": death, "exit": exit, "sectors_gained": gained, "key": key,
            "end_info": {"mode": mode, "state": {"hp": hp, "hp_max": 100, "hunted": hunted}}}


def test_frontier(tmp_path):
    """Frontier starts: each map's best live, exploring, non-exit rollout
    is saved and copied to that map's instances (never to disk: a new run
    starts every map over); groups start from it; an exit clears the map's
    frontier."""
    pairs = [(None, "1 1")] * 2 + [(None, "1 2")] * 2      # 2 groups of G=2
    vec = _FakeVec(4)
    f = Frontier(True, pairs)
    opts = f.options([{"seed": 0} for _ in range(4)], 2)
    assert not any("slot" in o for o in opts), "no frontier yet: map starts"

    # E1M1: env 0 scores higher but died, env 1 is saved. E1M2: env 3
    # scores higher but ended in combat, env 2 is saved.
    saved = f.update(vec, [_end(9, death=True), _end(3), _end(1), _end(7, mode="PLANNING")])
    assert saved == {"default_1_1": 3, "default_1_2": 1}
    assert vec.slots == [b"save-of-env1"] * 2 + [b"save-of-env2"] * 2
    assert vec.vis == ["v1"] * 2 + ["v2"] * 2, "the visited list goes with the save"
    assert set(vec.saved_to) == {SAVE_SLOT}, "progress is saved to slot 5, never over the start in 4"
    assert vec.store[0][SAVE_SLOT] == (None, None), "after the swap slot 5 holds the old start"
    assert not list(tmp_path.iterdir()), "nothing written to disk"

    opts = f.options([{"seed": 0} for _ in range(4)], 2)
    assert all(o["slot"] == FRONTIER_SLOT and o["frontier"] for o in opts)

    # An exit on E1M1 clears its frontier (memory, instances).
    saved = f.update(vec, [_end(50, exit=True), _end(2), _end(1), _end(2)])
    assert saved["default_1_1"] is None
    assert vec.slots[:2] == [None, None] and vec.slots[2:] == [b"save-of-env3"] * 2
    opts = f.options([{"seed": 0} for _ in range(4)], 2)
    assert "slot" not in opts[0] and opts[2]["slot"] == FRONTIER_SLOT

    # Off (no saves, map starts only).
    off = Frontier(False, pairs)
    assert off.update(vec, [_end(1)] * 4) == {}
    assert not any("slot" in o for o in off.options([{} for _ in range(4)], 2))


def test_frontier_progress(tmp_path):
    """Only a rollout with progress (a new sector) replaces a frontier; a
    batch without keeps it, and `patience` such batches in a row clear it
    (a dead end: back to the map start)."""
    pairs = [(None, "1 1")] * 2
    vec = _FakeVec(2)
    f = Frontier(True, pairs, patience=2)
    assert f.update(vec, [_end(9, gained=0), _end(3, gained=0)]) == {}, "no progress: nothing saved"
    assert f.update(vec, [_end(1, gained=0), _end(2, gained=1)]) == {"default_1_1": 2}
    assert f.last == {"default_1_1": {"result": "saved", "env": 1}}
    assert vec.slots == [b"save-of-env1"] * 2
    assert f.update(vec, [_end(5, gained=0)] * 2) == {}, "first stale batch keeps it"
    assert vec.slots == [b"save-of-env1"] * 2
    assert f.last == {"default_1_1": {"result": "kept", "stale": 1}}
    assert f.update(vec, [_end(5, gained=0)] * 2) == {"default_1_1": None}, "second clears it"
    assert f.last == {"default_1_1": {"result": "stale", "stale": 2}}
    assert vec.slots == [None, None]


def test_frontier_key(tmp_path):
    """A rollout that got a key is progress even without a new sector: it
    competes on score with the sector rollouts, and the best one is saved."""
    pairs = [(None, "1 1")] * 3
    vec = _FakeVec(3)
    f = Frontier(True, pairs)
    assert f.update(vec, [_end(30, gained=0, key=True), _end(5, gained=1), _end(40, gained=0)]) \
        == {"default_1_1": 30}, "the key rollout outscores the sector one; no progress, no save"
    assert vec.slots == [b"save-of-env0"] * 3
    assert f.update(vec, [_end(10, gained=0, key=True), _end(12, gained=1), _end(0, gained=0)]) \
        == {"default_1_1": 12}, "a sector rollout with a higher score wins"
    assert f.update(vec, [_end(50, gained=0, key=True, death=True)] + [_end(0, gained=0)] * 2) == {}, \
        "a key and then a death is no start"


def test_frontier_safe(tmp_path):
    """No progress save below 80% of max HP or with a monster in the
    sector after the player."""
    pairs = [(None, "1 1")] * 3
    vec = _FakeVec(3)
    f = Frontier(True, pairs)
    assert f.update(vec, [_end(9, hp=79), _end(8, hunted=True), _end(1, hp=80)]) == {"default_1_1": 1}
    assert f.update(vec, [_end(9, hp=79), _end(8, hunted=True), _end(1, gained=0)]) == {}


def test_tic_budget():
    """A map's rollout limit grows x1.5 after a batch where nobody got a
    new sector, up to the cap; progress (or a cleared frontier) resets it;
    the limit reaches the envs only when it changes."""
    from model import TicBudget
    pairs = [(None, "1 1")] * 2 + [(None, "1 2")] * 2
    b = TicBudget(pairs, 525, 2100)

    class Vec:
        calls = []
        def call(self, idx, name, args):
            self.calls.append((list(idx), name, args))
    vec = Vec()
    b.apply(vec)
    assert vec.calls == [([0, 1, 2, 3], "set_max_tics", (525,))]
    stuck, through = _end(0, gained=0), _end(0, gained=1)
    assert b.update([stuck, stuck, stuck, through]) == {"default_1_1": 788}
    assert b.growing() == {"default_1_1", "default_1_2"}
    vec.calls.clear(); b.apply(vec)
    assert vec.calls == [([0, 1], "set_max_tics", (788,))], "only the changed map"
    for want in (1182, 1773, 2100, 2100):
        b.update([stuck] * 4)
        assert b.tics["default_1_1"] == want
    assert b.growing() == frozenset(), "both maps at the cap"
    assert b.update([_end(0, gained=0, key=True), stuck, stuck, stuck])["default_1_1"] == 525, "a key resets"
    for _ in range(4):
        b.update([stuck] * 4)
    assert b.update([_end(0, gained=0, exit=True), stuck, stuck, stuck])["default_1_1"] == 525, "an exit resets"
    b.update([stuck] * 4)
    assert b.update([stuck] * 4, cleared={"default_1_1"})["default_1_1"] == 525, "a cleared frontier resets"
    assert TicBudget(pairs, 525, 0).update([stuck] * 4) == {}, "cap <= base: off"


def test_frontier_patience_waits_for_budget(tmp_path):
    """A batch without progress on a map whose tic budget is still growing
    doesn't count towards the frontier's patience."""
    pairs = [(None, "1 1")] * 2
    vec = _FakeVec(2)
    f = Frontier(True, pairs, patience=1)
    f.update(vec, [_end(1, gained=1), _end(0, gained=0)])
    assert f.update(vec, [_end(0, gained=0)] * 2, hold={"default_1_1"}) == {}, "budget growing: kept"
    assert f.update(vec, [_end(0, gained=0)] * 2) == {"default_1_1": None}, "at the cap: cleared"


def test_map_rotation():
    """More pairs than groups: the groups move on to the next block of
    pairs every `every` batches and wrap around; enough groups (or every
    0): one fixed allocation."""
    from model import MapRotation
    r = MapRotation(TWO_IWADS, MAPS, 4, every=2)
    pairs = [(w, m) for m in MAPS for w in TWO_IWADS]
    assert r.at(0) == pairs[:4] and r.at(1) is None
    assert r.at(2) == pairs[4:] and r.at(3) is None
    assert r.at(4) == pairs[:4], "a full cycle wraps around"
    resumed = MapRotation(TWO_IWADS, MAPS, 4, every=2)
    assert resumed.at(7) == pairs[4:], "a resumed run starts on its own rotation (3)"
    fixed = MapRotation(TWO_IWADS, MAPS, 8, every=2)
    assert fixed.every == 0 and fixed.at(0) == pairs and fixed.at(50) is None
    off = MapRotation(TWO_IWADS, MAPS, 4, every=0)
    assert off.at(0) == pairs[:4] and off.at(10) is None, "every 0: the first pairs, for good"


def test_map_rotation_shuffled():
    """A seeded rotation deals the pairs in a shuffled order: every pair
    still gets its turn in a cycle, the same seed gives the same order,
    and another seed (another session) a different one."""
    from model import MapRotation
    pairs = [(w, m) for m in MAPS for w in TWO_IWADS]

    def cycle(seed):
        r = MapRotation(TWO_IWADS, MAPS, 4, every=1, rng=np.random.default_rng(seed))
        return r.at(0) + r.at(1)

    assert sorted(cycle(1)) == sorted(pairs), "a cycle covers every pair once"
    assert cycle(1) == cycle(1)
    assert len({tuple(cycle(s)) for s in range(5)}) > 1, "sessions get different orders"


def test_pick_eval_maps(monkeypatch):
    """--eval-maps random: --eval-count maps the training maps and (stage
    1) the boss exits leave over, in map order, seeded; a list passes
    through; '' and an empty pool turn evals off."""
    import model
    monkeypatch.setattr(model, "wad_maps", lambda w: [f"1 {i}" for i in range(1, 10)])
    rng = lambda s=0: np.random.default_rng(s)
    picks = [model.pick_eval_maps("random", 2, "1 1,1 2,1 3,1 4", TWO_IWADS, 1, rng(s)) for s in range(20)]
    for p in picks:
        assert len(p) == 2 and set(p) <= {"1 5", "1 6", "1 7", "1 9"}, p
        assert p == sorted(p)
    assert len({tuple(p) for p in picks}) > 1, "different seeds, different maps"
    assert model.pick_eval_maps("random", 2, "1 1", TWO_IWADS, 1, rng(3)) == \
        model.pick_eval_maps("random", 2, "1 1", TWO_IWADS, 1, rng(3))
    assert "1 8" in {m for s in range(30)
                     for m in model.pick_eval_maps("random", 3, "1 1", TWO_IWADS, 2, rng(s))}, "stage 2: bosses"
    all_eval = model.pick_eval_maps("random", 2, "all", TWO_IWADS, 1, rng())
    assert len(all_eval) == 2 and "1 1" not in model.resolve_maps("all", TWO_IWADS, 1, all_eval[:1] + ["1 1"])
    assert model.pick_eval_maps("1 5, 1 6", 2, "all", TWO_IWADS, 1, rng()) == ["1 5", "1 6"]
    assert model.pick_eval_maps("", 2, "all", TWO_IWADS, 1, rng()) == []
    assert model.pick_eval_maps("random", 2, ",".join(f"1 {i}" for i in range(1, 10)),
                                TWO_IWADS, 1, rng()) == [], "nothing left to hold out"


def test_move_groups():
    """Moved envs restart on their new map (set_map), get that map's
    frontier (kept in memory while it waited for its turn), and the tic
    budget keeps each map's limit while it waits."""
    from model import TicBudget, move_groups

    class Vec:
        def __init__(self):
            self.calls = []
        def call(self, idx, name, args=()):
            self.calls.append((list(idx), name, args))
            return [None] * len(idx)
    a, b = [(None, "1 1")] * 2, [(None, "1 2")] * 2
    f = Frontier(True, a)
    f.data["default_1_2"], f.visited["default_1_2"] = b"e1m2-progress", "ff"   # from its last turn
    budget = TicBudget(a, 525, 2100)
    budget.tics["default_1_1"] = 1182
    vec = Vec()
    assert move_groups(vec, f, budget, a, b) == [0, 1]
    assert ([0, 1], "set_map", (None, "1 2")) in vec.calls
    assert ([0, 1], "install_state", (FRONTIER_SLOT, b"e1m2-progress", "ff")) in vec.calls
    assert f.labels == ["default_1_2"] * 2 and budget.tics == {"default_1_1": 1182, "default_1_2": 525}
    assert budget.growing() == {"default_1_2"}
    vec.calls.clear()
    assert move_groups(vec, f, budget, b, b) == [] and vec.calls == [], "no change: nothing moves"


def test_resolve_maps_all():
    """--maps all: the maps every IWAD has, minus the held-out maps and, in
    stage 1, the boss-exit maps."""
    from model import resolve_maps
    wads = [str(REPO / "wad/FreedoomPhase1/FREEDOOM1.WAD"), str(REPO / "wad/UltimateDoom/DOOM.WAD")]
    if not all(Path(w).exists() for w in wads):
        pytest.skip("IWADs not present")
    maps = resolve_maps("all", wads, 1, ["1 5", "1 6"])
    assert len(maps) == 36 - 2 - 5 and maps[0] == "1 1" and maps[-1] == "4 9"
    assert not {"1 5", "1 6", "1 8", "2 8", "3 8", "4 6", "4 8"} & set(maps)
    assert "1 8" in resolve_maps("all", wads, 2, []), "stage 2 has the bosses"
    assert resolve_maps("1 1, 1 2", wads, 1, []) == ["1 1", "1 2"]


def test_capture_dirs():
    """--capture-envs n records the first n envs of each IWAD (groups take
    the IWADs in rotation, so env 0..7 are all the first IWAD); both are
    on the first map, into <IWAD>_<map>/; set_map moves an env's folder
    with it."""
    groups = allocate_groups(TWO_IWADS, MAPS, 4)
    pairs = [groups[i // 8] for i in range(32)]
    dirs = capture_dirs(pairs, 1, Path("/r"))
    m = "E" + MAPS[0].replace(" ", "M")
    assert {i: d for i, d in enumerate(dirs) if d} == {0: f"/r/FREEDOOM1_{m}", 8: f"/r/DOOM_{m}"}
    assert pairs[0][1] == pairs[8][1] == MAPS[0]
    assert sum(d is not None for d in capture_dirs(pairs, 2, Path("/r"))) == 4
    assert capture_dirs(pairs, 0, Path("/r")) == [None] * 32
    assert capture_dirs([(None, "1 1")] * 2, 1, Path("/r")) == ["/r/default_E1M1", None]
    from env import DiabloDoomEnv, EnvConfig, EngineConfig
    e = DiabloDoomEnv(EnvConfig(engine=EngineConfig(warp="1 1", capture_dir="/r/DOOM_E1M1")))
    assert e.set_map("/w/FREEDOOM1.WAD", "4 3") and e.cfg.engine.capture_dir == "/r/FREEDOOM1_E4M3"


def test_default_groups_leave_two_cores():
    """The default group count is one per IWAD/map pair, at most the
    logical cores minus 2 (at least 1)."""
    from model import available_cores, default_groups, max_default_groups
    assert default_groups(TWO_IWADS, MAPS, cores=16) == 8, "8 pairs fit under 14"
    assert default_groups(TWO_IWADS, ["1 1"] * 29, cores=16) == 14, "58 pairs: 16 - 2"
    assert default_groups(TWO_IWADS, MAPS, cores=8) == 6
    assert default_groups(TWO_IWADS, MAPS, cores=2) == 1, "never below 1"
    assert max_default_groups() == max(1, available_cores() - 2)


def test_obs_size(tmp_path):
    """--obs-size: the policy is built for any WxH frame (DEFAULT_OBS_SIZE
    when none is given); a fresh run takes the size asked for (else
    DEFAULT_OBS_SIZE), and a resumed run keeps its own (asking for another
    stops)."""
    import argparse
    import types
    from learn import setup_learner
    from env import ACTIONS
    from model import DEFAULT_OBS_SIZE, parse_obs_size

    assert parse_obs_size("160x100") == (160, 100) and parse_obs_size("96") == (96, 60) and parse_obs_size("320") == (320, 200)
    for bad in ("x", "20x20", "400x200", "1x2x3", "50"):
        with pytest.raises(argparse.ArgumentTypeError):
            parse_obs_size(bad)
    for size in ((84, 84), (160, 100), (320, 200)):
        p = Policy(obs_size=size)
        logits, value = p(torch.zeros(2, 8, size[1], size[0], dtype=torch.uint8), torch.zeros(2, 2))
        assert logits.shape == (2, len(ACTIONS)) and value.shape == (2,)
    assert Policy().obs_size == DEFAULT_OBS_SIZE
    # Depth (2026-11-22): 8 channels, [gray, depth] a frame; a checkpoint
    # without obs_depth saw grayscale only (4 channels).
    from model import checkpoint_obs_depth
    gray = Policy(depth=False)
    assert gray(torch.zeros(1, 4, 80, 128, dtype=torch.uint8), torch.zeros(1, 2))[0].shape == (1, len(ACTIONS))
    assert not checkpoint_obs_depth({}) and checkpoint_obs_depth({"obs_depth": True})

    def args(size=None):
        return types.SimpleNamespace(lr=1e-4, seed=1, ref=None, init=None, obs_size=size)
    run = tmp_path / "run"
    run.mkdir()
    a = args((160, 100))
    policy, opt, *_ = setup_learner(a, run, torch.device("cpu"))
    assert a.obs_size == (160, 100) and policy.obs_size == (160, 100)
    save_checkpoint(run / CKPT_NAME, {"policy": policy.state_dict(), "optimizer": opt.state_dict(),
                                      "iteration": 0, "total_steps": 0, "total_tics": 0,
                                      "rng": rng_state(), "run_id": "x", "ref": None,
                                      "obs_size": [160, 100], "obs_depth": True})
    a = args()
    policy, *_ = setup_learner(a, run, torch.device("cpu"))
    assert a.obs_size == (160, 100) and policy.obs_size == (160, 100), "a resumed run keeps its size"
    assert a.obs_depth and policy.obs_depth
    # A checkpoint from before 2026-11-22 (no obs_depth): grayscale only, kept.
    old = tmp_path / "old"
    old.mkdir()
    save_checkpoint(old / CKPT_NAME, {"policy": gray.state_dict(),
                                      "optimizer": torch.optim.Adam(gray.parameters()).state_dict(),
                                      "iteration": 0, "total_steps": 0, "total_tics": 0,
                                      "rng": rng_state(), "run_id": "y", "ref": None, "obs_size": [128, 80]})
    a = args()
    policy, *_ = setup_learner(a, old, torch.device("cpu"))
    assert not a.obs_depth and not policy.obs_depth, "an old run stays grayscale"
    with pytest.raises(SystemExit, match="trained at 160x100"):
        setup_learner(args((84, 84)), run, torch.device("cpu"))
    a = args()
    (tmp_path / "fresh").mkdir()
    setup_learner(a, tmp_path / "fresh", torch.device("cpu"))
    assert a.obs_size == DEFAULT_OBS_SIZE


def test_parse_mix():
    """--mix: groups per rollout type, every type present (0 when left
    out); an unknown type or a bad count stops."""
    from model import parse_mix
    assert parse_mix("combat=2,waypoint=2,maze=2,mixed=2") == {"combat": 2, "waypoint": 2, "maze": 2, "mixed": 2}
    assert parse_mix("combat=8") == {"combat": 8, "waypoint": 0, "maze": 0, "mixed": 0}
    for bad in ("combat=x", "drill=2", "combat=0"):
        with pytest.raises(SystemExit):
            parse_mix(bad)
