"""Tests for learn.py, the update step train.py uses (SPEC.md §7.1, §7.4):
advantages, value targets, batch building and one update (no game).
train.py's end-to-end runs are in test_train.py."""

from __future__ import annotations

import types

import numpy as np
import pytest
import torch

from learn import build_batch, group_advantages, train_metrics, update, value_targets
from model import Policy

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
    """build_batch keeps only rollouts with an advantage; one update
    changes the weights."""
    G = 4
    back = [make_rollout(int(n)) for n in rng.integers(5, 30, 3 * G)]
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
