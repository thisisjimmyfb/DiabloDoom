"""Tests for eval.py's group starts (no game). Its rollouts, rollout log
and held-out eval run inside train.py's end-to-end tests (test_train.py)."""

from __future__ import annotations

import numpy as np

from env import ACTIONS, CONSULT
from eval import sample_group_options
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
    """Warm-up actions never toggle the map or consult."""
    assert CONSULT not in WARMUP_ACTIONS
    assert len(WARMUP_ACTIONS) == len(ACTIONS) - 1
    opts = sample_group_options(np.random.default_rng(0), 20, 1, [], warmup_max=40)
    assert all(a in WARMUP_ACTIONS for o in opts for a in o["warmup"])


def test_no_slots_no_warmup():
    opts = sample_group_options(np.random.default_rng(0), 2, 2, [], 0)
    assert all("slot" not in o and o["warmup"] == [] for o in opts)
