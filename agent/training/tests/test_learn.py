"""Tests for learn.py, the update step train.py uses (SPEC.md §7.1, §7.4):
advantages, value targets, batch building and one update (no game).
train.py's end-to-end runs are in test_train.py."""

from __future__ import annotations

import types

import numpy as np
import pytest
import torch

from learn import GRAD_WARMUP, build_batch, entropy_loss, group_advantages, train_metrics, training_health, policy_update, value_targets
from model import DEFAULT_OBS_SIZE, Policy

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
    """Per-step rewards normalized across the group, summed from each step
    on, the sums scaled to unit std over the group."""
    advs, collapsed = group_advantages(groups, "process")
    assert collapsed == 0, "differing per-step rewards never collapse"
    i = 0
    for g in groups:
        allr = np.concatenate([r["rew"] for r in g])
        togo = [np.cumsum(((np.array(r["rew"]) - allr.mean()) / (allr.std() + 1e-8))[::-1])[::-1] for r in g]
        cstd = np.concatenate(togo).std()
        for t in togo:
            assert np.allclose(advs[i], t / cstd, atol=1e-5)
            i += 1


def test_process_one_off_rewards():
    """A one-off reward (rew_now) counts on its own step only, as it is: its
    value is standard deviations of advantage (2026-11-21); it never reaches
    earlier steps, and the value targets leave it out."""
    rew = [0.0, 0.0, 5.0, 0.0, -1.0]
    a = {"rew": rew, "rew_now": [0.0, 0.0, 5.0, 0.0, 0.0], "len": 5, "tics": [1] * 5}
    b = {"rew": [0.0, 0.0, 0.0, 0.0, -1.0], "rew_now": [0.0] * 5, "len": 5, "tics": [1] * 5}
    (adv_a, adv_b), collapsed = group_advantages([[a, b]], "process")
    assert collapsed == 0
    assert np.allclose(adv_a - adv_b, [0, 0, 5.0, 0, 0], atol=1e-5), adv_a - adv_b
    assert np.allclose(value_targets(np.array(rew) - a["rew_now"], a["tics"], 1.0), [-1] * 5)


def test_value_targets():
    """Return-to-go discounted per tic: gamma_tic ** tics per step, 0 bootstrap."""
    rew, tics, g = [1.0, 0.0, 2.0], [10, 30, 5], 0.99
    expect = [1.0 + g ** 10 * (0.0 + g ** 30 * 2.0), 0.0 + g ** 30 * 2.0, 2.0]
    assert np.allclose(value_targets(rew, tics, g), expect)
    assert np.allclose(value_targets(rew, tics, 1.0), [3.0, 2.0, 2.0])


def test_policy_update(rng, make_rollout):
    """build_batch keeps only rollouts with an advantage; one update
    changes the weights."""
    G = 4
    back = [make_rollout(int(n)) for n in rng.integers(5, 30, 3 * G)]
    for r in back[2 * G:]:                            # third group collapses
        r["ret"] = 0.5
    gs = [back[:G], back[G:2 * G], back[2 * G:]]
    advs, collapsed = group_advantages(gs, "outcome")
    batch = build_batch(back, advs, 0.999)
    w, h = DEFAULT_OBS_SIZE
    assert collapsed == 1 and batch["obs"].shape == (sum(r["len"] for r in back[:2 * G]), 4, h, w)
    assert build_batch(back[2 * G:], advs[2 * G:], 0.999) is None, "nothing to train on"
    args = types.SimpleNamespace(epochs=1, minibatch=64, clip=0.2, ent_coef=0.01, vf_coef=0.5,
                                 beta=0.0, max_grad_norm=0.5)
    policy = Policy()
    before = [p.detach().clone() for p in policy.parameters()]
    upd = policy_update(policy, torch.optim.Adam(policy.parameters(), lr=1e-3), None, batch, args, torch.device("cpu"))
    assert any(not torch.equal(a, b) for a, b in zip(before, policy.parameters()))
    assert {"pg_loss", "v_loss", "entropy", "approx_kl", "clipfrac"} <= set(upd)
    # The split norms (value head, the rest) are each part of the clip's norm.
    assert 0 < upd["pg_grad_norm"] <= upd["grad_norm"] + 1e-6 and 0 < upd["v_grad_norm"] <= upd["grad_norm"] + 1e-6
    m = train_metrics(back, len(gs), collapsed)
    assert m["steps"] == sum(r["len"] for r in back) and abs(m["collapsed_share"] - 1 / 3) < 1e-9


def _health_batch(rng, make_rollout):
    gs = [[make_rollout(int(n)) for n in rng.integers(20, 40, 4)]]
    advs, _ = group_advantages(gs, "outcome")
    return build_batch(gs[0], advs, 0.999)


HEALTH_ARGS = dict(epochs=1, minibatch=64, clip=0.2, ent_coef=0.01, vf_coef=0.5, beta=0.0,
                   max_grad_norm=0.5, stop_dead_units=0.5, stop_grad_norm=1000.0,
                   stop_grad_ratio=100.0, norm_adv=True)


def test_relu_collapse(rng, make_rollout):
    """A policy whose trunk's last layer never fires (every unit dead, as
    in the 2026-11-16 run: the policy ignored the screen) reports
    dead_units 1.0 and training_health stops it; a fresh policy is
    healthy. The share is of the batch's samples, over the whole update."""
    batch = _health_batch(rng, make_rollout)
    args = types.SimpleNamespace(**HEALTH_ARGS)
    fresh = Policy()
    upd = policy_update(fresh, torch.optim.Adam(fresh.parameters(), lr=1e-4), None, batch, args, torch.device("cpu"))
    assert upd["dead_units"] < 0.5 and training_health(upd, args) is None, upd["dead_units"]

    dead = Policy()
    with torch.no_grad():
        dead.trunk[7].bias.fill_(-1e3)                 # every pre-activation negative
    upd = policy_update(dead, torch.optim.Adam(dead.parameters(), lr=1e-4), None, batch, args, torch.device("cpu"))
    assert upd["dead_units"] == 1.0
    assert "ReLU collapse" in training_health(upd, args)
    assert training_health(upd, types.SimpleNamespace(**{**HEALTH_ARGS, "stop_dead_units": 0})) is None, \
        "0 turns the check off"


def test_exploding_gradient(rng, make_rollout):
    """A non-finite gradient never reaches the weights (the step is
    skipped and counted) and always stops training; a finite gradient norm
    past --stop-grad-norm stops it too."""
    batch = _health_batch(rng, make_rollout)
    args = types.SimpleNamespace(**HEALTH_ARGS)
    policy = Policy()
    before = [p.detach().clone() for p in policy.parameters()]
    bad = {**batch, "adv": np.full_like(batch["adv"], np.inf)}
    upd = policy_update(policy, torch.optim.Adam(policy.parameters(), lr=1e-3), None, bad, args, torch.device("cpu"))
    assert upd["nonfinite_steps"] > 0
    assert all(torch.equal(a, b) for a, b in zip(before, policy.parameters())), "weights untouched"
    off = types.SimpleNamespace(**{**HEALTH_ARGS, "stop_grad_norm": 0, "stop_dead_units": 0})
    assert "exploding gradient" in training_health(upd, off), "non-finite: even with the checks off"

    upd = policy_update(policy, torch.optim.Adam(policy.parameters(), lr=1e-4), None, batch, args, torch.device("cpu"))
    assert upd["nonfinite_steps"] == 0 and upd["grad_norm_max"] >= upd["grad_norm"] > 0
    assert training_health(upd, args) is None
    tight = types.SimpleNamespace(**{**HEALTH_ARGS, "stop_grad_norm": upd["grad_norm_max"] / 2})
    assert "exploding gradient" in training_health(upd, tight)


def test_grad_ratio():
    """The relative gradient check: a gradient norm past --stop-grad-ratio
    times the median of the recent healthy updates stops training, once
    GRAD_WARMUP of them exist; a spike under the ratio, a short history,
    and ratio 0 don't."""
    args = types.SimpleNamespace(**{**HEALTH_ARGS, "stop_grad_norm": 0})
    recent = [10.0] * GRAD_WARMUP
    why = training_health({"grad_norm_max": 1001.0}, args, recent)
    assert why and "exploding gradient" in why and "median" in why, why
    assert training_health({"grad_norm_max": 500.0}, args, recent) is None, "a 50x spike is no explosion"
    assert training_health({"grad_norm_max": 1e6}, args, recent[:-1]) is None, "warm-up: too little history"
    assert training_health({"grad_norm_max": 1e6}, types.SimpleNamespace(**{**vars(args), "stop_grad_ratio": 0}),
                           recent) is None, "0 turns it off"


def test_norm_adv(rng, make_rollout):
    """--norm-adv: the update doesn't depend on the advantages' overall
    size (a batch of long rollouts, with advantages 1000x larger, moves the
    policy exactly as far); without it, it does. adv_std reports the
    spread before scaling."""
    import copy
    batch = _health_batch(rng, make_rollout)
    big = {**batch, "adv": batch["adv"] * 1000}

    def after(b, norm):
        torch.manual_seed(0)
        policy = Policy()
        torch.manual_seed(1)
        args = types.SimpleNamespace(**{**HEALTH_ARGS, "norm_adv": norm})
        upd = policy_update(policy, torch.optim.Adam(policy.parameters(), lr=1e-3), None, b, args, torch.device("cpu"))
        return [p.detach() for p in policy.parameters()], upd

    small_w, small_u = after(batch, True)
    big_w, big_u = after(big, True)
    assert big_u["adv_std"] == pytest.approx(1000 * small_u["adv_std"], rel=1e-4)
    assert all(torch.allclose(a, b, atol=1e-6) for a, b in zip(small_w, big_w)), "scaled: same update"
    small_w, _ = after(batch, False)
    big_w, _ = after(big, False)
    assert any(not torch.allclose(a, b, atol=1e-6) for a, b in zip(small_w, big_w)), "unscaled: size matters"


def test_micro_batch(rng, make_rollout):
    """--micro-batch only changes the GPU memory: a minibatch's gradient
    accumulated over pieces gives the same update (weights and stats) as
    the whole minibatch at once."""
    batch = _health_batch(rng, make_rollout)

    def after(micro):
        torch.manual_seed(0)
        policy = Policy()
        torch.manual_seed(1)
        args = types.SimpleNamespace(**{**HEALTH_ARGS, "micro_batch": micro})
        upd = policy_update(policy, torch.optim.Adam(policy.parameters(), lr=1e-3), None, batch, args, torch.device("cpu"))
        return [p.detach() for p in policy.parameters()], upd

    whole_w, whole = after(0)
    split_w, split = after(16)               # minibatch 64: four pieces
    assert all(torch.allclose(a, b, atol=1e-5) for a, b in zip(whole_w, split_w))
    for k in ("pg_loss", "v_loss", "entropy", "approx_kl", "grad_norm"):
        assert split[k] == pytest.approx(whole[k], rel=1e-4, abs=1e-6), k


def test_reward_conflict(rng, make_rollout):
    """The gradient conflict check (2026-11-19): a term paying exactly what
    another costs has a policy gradient cosine of -1 with it and +1 with a
    copy of itself; a pair averaging below --stop-conflict over
    --conflict-window checks stops training."""
    from learn import term_advantages, term_grad_cosines
    gs = [[make_rollout(int(n)) for n in rng.integers(5, 40, 8)]]
    for r in gs[0]:
        r["step_terms"] = [{"a": v, "b": -v, "c": 2 * v} for v in r["rew"]]
    advs, _ = group_advantages(gs, "process")
    tadv = term_advantages(gs, "process", advs, ())
    batch = {**build_batch(gs[0], advs, 0.999), "term_adv": tadv}
    args = types.SimpleNamespace(**HEALTH_ARGS, conflict_samples=4096, micro_batch=0,
                                 conflict_window=2, stop_conflict=-0.1)
    cos = term_grad_cosines(Policy(), batch, args, torch.device("cpu"))
    assert cos["a|b"] == pytest.approx(-1, abs=1e-4) and cos["a|c"] == pytest.approx(1, abs=1e-4)
    assert training_health({}, args, None, {"a|b": [-1.0]}) is None, "one check: not yet"
    assert "reward conflict: a and b" in training_health({}, args, None, {"a|b": [-1.0, -0.5]})
    assert training_health({}, args, None, {"a|c": [1.0, 0.9]}) is None
    # time and waypoint_time split one per-tic cost: checked as one term.
    for r in gs[0]:
        r["step_terms"] = [{"time": -1.0} if i % 2 else {"waypoint_time": -1.0} for i in range(len(r["rew"]))]
    merged = term_advantages(gs, "process", advs, ())
    assert set(merged) == {"time"}


def test_entropy_floor():
    """--ent-target: a share of each sample's highest possible entropy
    (max_ent); no term (and no gradient) above the floor, the floor's
    shortfall below it, a drill sample's (ln 6) as anyone's; 0 keeps the
    plain bonus."""
    args = types.SimpleNamespace(ent_coef=0.01, ent_target=0.5)
    two = torch.tensor([2.0, 2.0])                    # floor 0.5 * 2.0 = 1.0
    ent = torch.tensor([3.3, 1.5], requires_grad=True)
    loss = entropy_loss(ent, args, two)
    loss.backward()
    assert loss.item() == 0.0 and ent.grad.abs().sum().item() == 0.0
    assert entropy_loss(torch.tensor([0.6, 3.0]), args, two).item() == pytest.approx(0.01 * 0.4 / 2)
    import math
    from env import ACTIONS
    assert entropy_loss(torch.tensor([0.0]), args).item() == pytest.approx(0.01 * 0.5 * math.log(len(ACTIONS)))
    assert entropy_loss(torch.tensor([0.5]), args, torch.tensor([math.log(6)])).item() == \
        pytest.approx(0.01 * (0.5 * math.log(6) - 0.5))
    args.ent_target = 0.0
    assert entropy_loss(torch.tensor([2.0, 4.0]), args).item() == pytest.approx(-0.03)


def test_redundant_pairs():
    """A pair whose gradient cosine averages --redundant-cos or more over the
    last --redundant-window checks is named (a warning); a shorter history,
    a lower mean, or a window of 0 names nothing."""
    from learn import redundant_pairs
    args = types.SimpleNamespace(redundant_cos=0.9, redundant_window=3)
    hist = {"damage|kill": [0.2, 0.95, 0.92, 0.93], "turn|turn_waste": [0.9, 0.9, 0.3], "a|b": [0.99, 0.99]}
    assert redundant_pairs(hist, args) == {"damage|kill": pytest.approx((0.95 + 0.92 + 0.93) / 3)}
    args.redundant_window = 0
    assert redundant_pairs(hist, args) == {}
