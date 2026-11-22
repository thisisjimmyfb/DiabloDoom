"""Tests for rollout.py's group starts (no game). Its rollouts, rollout log
and held-out eval run inside train.py's end-to-end tests (test_train.py)."""

from __future__ import annotations

import numpy as np

from env import ACTIONS, CONSULT
from rollout import eval_delta_lines, sample_group_options
from model import WARMUP_ACTIONS


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
    """Warm-up actions never consult or fire."""
    assert CONSULT not in WARMUP_ACTIONS
    assert not any(ACTIONS[a].endswith("CTRL") for a in WARMUP_ACTIONS)
    assert len(WARMUP_ACTIONS) == len(ACTIONS) - 1 - 27
    opts = sample_group_options(np.random.default_rng(0), 20, 1, [], warmup_max=40)
    assert all(a in WARMUP_ACTIONS for o in opts for a in o["warmup"])


def test_no_slots_no_warmup():
    opts = sample_group_options(np.random.default_rng(0), 2, 2, [], 0)
    assert all("slot" not in o and o["warmup"] == [] for o in opts)


def test_eval_delta():
    """The start-to-end change per IWAD and map, then overall."""
    first = {"iteration": 3900, "eval_sectors": 2.0, "eval_exit_rate": 0.0, "eval_return": -20.0,
             "eval_per_map": {"FREEDOOM1:1 5": 3.0, "DOOM:1 5": 1.0},
             "eval_per_map_exit": {"FREEDOOM1:1 5": 0.0, "DOOM:1 5": 0.0},
             "eval_per_map_return": {"FREEDOOM1:1 5": -15.0, "DOOM:1 5": -25.0}}
    last = {"iteration": 4000, "eval_sectors": 3.5, "eval_exit_rate": 0.25, "eval_return": 30.0,
            "eval_per_map": {"FREEDOOM1:1 5": 6.5, "DOOM:1 5": 0.5},
            "eval_per_map_exit": {"FREEDOOM1:1 5": 0.5, "DOOM:1 5": 0.0},
            "eval_per_map_return": {"FREEDOOM1:1 5": 85.0, "DOOM:1 5": -25.0}}
    lines = eval_delta_lines(first, last)
    assert lines[0] == "[eval delta] iteration 3900 -> 4000"
    assert "FREEDOOM1:1 5" in lines[1] and "3.00 ->  6.50 (+3.50)" in lines[1] and "(+0.50)" in lines[1]
    assert "DOOM:1 5" in lines[2] and "(-0.50)" in lines[2]
    assert "-15.00 ->   85.00 (+100.00)" in lines[1] and "(+0.00)" in lines[2].split("return")[1]
    assert lines[3].lstrip().startswith("all") and "(+1.50)" in lines[3] and "(+0.25)" in lines[3]
    assert "(+50.00)" in lines[3]
    # An eval row from before per-map exit rates and returns were logged.
    del first["eval_per_map_exit"], first["eval_per_map_return"]
    assert "exit 0.00 -> 0.50" in eval_delta_lines(first, last)[1]


def test_reward_back():
    """info["reward_back"] (2026-11-22): a one-off reward for an earlier step
    (a projectile's result, for the step that pulled the trigger) is added to
    that step's rew, rew_now and step_terms and to the term totals; the
    rollout still ends where the env says. Fake vector env, no game."""
    import torch
    from model import DEFAULT_OBS_SIZE, Policy
    from rollout import run_rollouts
    w, h = DEFAULT_OBS_SIZE

    class Vec:
        n = 0

        def reset(self, idx, options):
            self.n = 0
            return np.zeros((1, 8, h, w), dtype=np.uint8), [{"state": {"hp": 100}}]

        def step(self, active, actions):
            self.n += 1
            done = self.n == 4
            info = {"state": {"hp": 100}, "tics": 1, "terms": {}, "reward_now": 0.0, "rollout_tics": self.n,
                    "reward_back": [[2, "proj", 6.0]] if self.n == 3 else []}
            return (np.zeros((1, 8, h, w), dtype=np.uint8), np.array([1.0]), np.array([False]),
                    np.array([done]), [info])

    (r,) = run_rollouts(Vec(), Policy(), torch.device("cpu"), [{}])
    assert r["len"] == 4 and r["rew"] == [7.0, 1.0, 1.0, 1.0], r["rew"]
    assert r["rew_now"] == [6.0, 0.0, 0.0, 0.0] and r["step_terms"][0] == {"proj": 6.0}
    assert r["terms"] == {"proj": 6.0} and r["ret"] == 10.0
