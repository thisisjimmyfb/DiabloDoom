"""The update step of the trainer (SPEC.md §7.1, §7.4), used by train.py:
group-normalized advantages

    A_i = (R_i - mean(R_group)) / (std(R_group) + eps)

(or, with --advantage process, normalized per-step rewards summed from
each step onward, plus the step's own one-off rewards), tic-discounted
value targets, the clipped-ratio update,
the training metrics and line, and the learner's setup (policy, optimizer
and reference policy, resumed from the newest checkpoint)."""

from __future__ import annotations

import argparse
import random
import uuid
from pathlib import Path

import numpy as np
import torch
import torch.nn as nn
from torch.distributions import Categorical

from model import (ACTIONS, DEFAULT_OBS_SIZE, Policy, checkpoint_obs_size, load_latest_checkpoint,
                   load_weights, restore_rng)


# ---------------------------------------------------------------------------
# Advantages, targets, batch (shared with train.py)
# ---------------------------------------------------------------------------

# The one-off stream's cap, in its own stds (2026-11-21, group_advantages).
ONE_OFF_CLIP = 5.0


def group_advantages(groups: list[list[dict]], mode: str,
                     eps: float = 1e-8) -> tuple[list[np.ndarray | None], int]:
    """Per-rollout advantage arrays (one value per decision), in the order
    of the flattened groups; None for rollouts in collapsed (zero-variance)
    groups. Groups may differ in size. Returns (advs, collapsed)."""
    advs: list[np.ndarray | None] = []
    collapsed = 0
    if mode == "outcome":
        for members in groups:
            rets = np.array([m["ret"] for m in members], dtype=np.float64)
            std = rets.std()
            if std < 1e-6:
                collapsed += 1
                advs += [None] * len(members)
                continue
            advs += [np.full(m["len"], (rets[j] - rets.mean()) / (std + eps), dtype=np.float32)
                     for j, m in enumerate(members)]
    else:  # process: normalize per-step rewards across the group
        # Two streams (2026-11-17): chained rewards (rew - rew_now) are
        # summed from each step onward; one-off rewards (rew_now, the
        # env's RewardConfig.immediate terms) count on their own step only,
        # on the same scale (no mean shift: most steps have none).
        for members in groups:
            allr = np.concatenate([np.array(m["rew"], dtype=np.float64) for m in members])
            std = allr.std()
            if std < 1e-6:
                collapsed += 1
                advs += [None] * len(members)
                continue
            now = [_rew_now(m) for m in members]
            chain = [np.array(m["rew"], dtype=np.float64) - n for m, n in zip(members, now)]
            mean = np.concatenate(chain).mean()
            togo = [np.cumsum(((c - mean) / (std + eps))[::-1])[::-1] for c in chain]
            # 2026-11-18: the summed chained stream is scaled to unit std
            # over the group, so its spread no longer grows with rollout
            # length and drowns the one-off stream (a CONSULT hit was
            # 0.3 of its std).
            cstd = np.concatenate(togo).std()
            # 2026-11-21: the one-off stream is scaled by its own std over the
            # group (std, of every per-step reward, before): deaths and hits
            # dominated that std, so a miss's fire_waste was a small fraction
            # of it and shrank as they grew. No one-off rewards: nothing. The
            # stream is sparse (most steps have none), so a rare large one (a
            # CONSULT's +5) would be many stds: clipped at ONE_OFF_CLIP.
            nstd = np.concatenate(now).std()
            for t, n in zip(togo, now):
                one = np.clip(n / nstd, -ONE_OFF_CLIP, ONE_OFF_CLIP) if nstd > 1e-6 else 0.0 * n
                advs.append((t / (cstd + eps) + one).astype(np.float32))
    return advs, collapsed


def _rew_now(r: dict) -> np.ndarray:
    """A rollout's per-step one-off rewards (zeros if it has none)."""
    n = r.get("rew_now")
    return np.zeros(len(r["rew"])) if n is None else np.array(n, dtype=np.float64)


# Terms the conflict check counts as one (2026-11-19): a tic pays either
# waypoint_time (a waypoint set) or time (none), so apart their gradients
# are opposite by construction. waypoint_time is gone since 2026-11-20.
CONFLICT_MERGE = {"waypoint_time": "time"}


def term_advantages(groups: list[list[dict]], mode: str, advs: list[np.ndarray | None],
                    immediate: tuple) -> dict[str, np.ndarray]:
    """The gradient conflict check (2026-11-19): per reward term, the
    advantages group_advantages gives with only that term's rewards
    (r["step_terms"]; a term in `immediate` one-off), concatenated as
    build_batch concatenates the rollouts it keeps (advs not None);
    zeros where the term's own group collapsed."""
    recs = [m for g in groups for m in g]
    names = sorted({CONFLICT_MERGE.get(t, t) for r in recs for st in r["step_terms"] for t in st})
    out = {}
    for name in names:
        tg = []
        for g in groups:
            tg.append([])
            for m in g:
                rew = [sum(v for t, v in st.items() if CONFLICT_MERGE.get(t, t) == name)
                       for st in m["step_terms"]]
                tg[-1].append({"rew": rew, "rew_now": rew if name in immediate else None,
                               "ret": float(sum(rew)), "len": len(rew)})
        ta, _ = group_advantages(tg, mode)
        out[name] = np.concatenate([np.zeros(len(m["step_terms"]), np.float32) if t is None else t
                                    for m, t, a in zip(recs, ta, advs) if a is not None])
    return out


def term_grad_cosines(policy: Policy, batch: dict, args, device: torch.device) -> dict[str, float]:
    """The cosine similarity of each pair of reward terms' policy
    gradients (batch["term_adv"], term_advantages), at the current
    weights (the update's gradient at its start, where the clipped ratio
    is 1), on --conflict-samples random samples of the batch; "a|b" ->
    cosine. A term zero on every sample is left out."""
    n = len(batch["act"])
    k = min(n, args.conflict_samples)
    idx = torch.randperm(n)[:k]
    names = [t for t, a in batch["term_adv"].items() if np.abs(a[idx.numpy()]).max() > 0]
    if len(names) < 2:
        return {}
    params = [p for p in policy.parameters() if p.requires_grad]
    grads = {t: torch.zeros(sum(p.numel() for p in params), device=device) for t in names}
    obs, priv = torch.as_tensor(batch["obs"]), torch.as_tensor(batch["priv"])
    act = torch.as_tensor(batch["act"])
    micro = getattr(args, "micro_batch", 0) or k
    for m0 in range(0, k, micro):
        sub = idx[m0:m0 + micro]
        logits, _ = policy(obs[sub].to(device), priv[sub].to(device))
        lp = Categorical(logits=logits).log_prob(act[sub].to(device))
        for j, t in enumerate(names):
            a = torch.as_tensor(batch["term_adv"][t][sub.numpy()], device=device)
            g = torch.autograd.grad(-(a * lp).sum() / k, params, retain_graph=j < len(names) - 1,
                                    allow_unused=True)
            grads[t] += torch.cat([(torch.zeros_like(p) if gi is None else gi).flatten()
                                   for p, gi in zip(params, g)])
    out = {}
    for i, a in enumerate(names):
        for b in names[i + 1:]:
            na, nb = grads[a].norm(), grads[b].norm()
            if na > 0 and nb > 0:
                out[f"{a}|{b}"] = float((grads[a] @ grads[b]) / (na * nb))
    return out


def value_targets(rew: list[float], tics: list[int], gamma_tic: float) -> np.ndarray:
    """Return-to-go with the discount raised to the tics each step took
    (SPEC.md §7.3), so long walks count as more time than quick turns.
    Rollout ends bootstrap with 0."""
    out = np.zeros(len(rew), dtype=np.float32)
    g = 0.0
    for t in range(len(rew) - 1, -1, -1):
        g = rew[t] + (gamma_tic ** tics[t]) * g
        out[t] = g
    return out


def build_batch(recs: list[dict], advs: list[np.ndarray | None], gamma_tic: float) -> dict | None:
    """Training arrays from every rollout that has an advantage (collapsed
    groups contribute nothing). None if nothing is left. The frames are
    the batch's bulk (frame_stack * W * H bytes a step, 4 * 128 * 80 =
    40 KB), so they are copied into
    one preallocated array and each rollout's own frames are released as
    they go (r["obs"] is None afterwards, collapsed rollouts' too): one
    copy of the batch's frames at a time instead of three."""
    keep = [(r, a) for r, a in zip(recs, advs) if a is not None]
    obs = None
    if keep:
        n = sum(len(r["obs"]) for r, _ in keep)
        obs = np.empty((n, *np.shape(keep[0][0]["obs"][0])), dtype=np.uint8)
        at = 0
        for r, _ in keep:
            for frame in r["obs"]:
                obs[at] = frame
                at += 1
            r["obs"] = None
    for r in recs:
        r["obs"] = None
    if not keep:
        return None
    return {"obs": obs,
            "priv": np.concatenate([np.asarray(r["priv"], dtype=np.float32) for r, _ in keep]),
            "act": np.concatenate([np.asarray(r["act"], dtype=np.int64) for r, _ in keep]),
            "logp": np.concatenate([np.asarray(r["logp"], dtype=np.float32) for r, _ in keep]),
            "adv": np.concatenate([a for _, a in keep]).astype(np.float32),
            # chained rewards only: one-off ones are credited on their own step
            "ret": np.concatenate([value_targets(np.array(r["rew"]) - _rew_now(r), r["tics"], gamma_tic)
                                   for r, _ in keep])}


def policy_update(policy: Policy, optimizer: torch.optim.Optimizer, ref: Policy | None,
           batch: dict, args, device: torch.device) -> dict:
    policy.train()
    n = len(batch["act"])
    obs = torch.as_tensor(batch["obs"])                # stays on CPU; moved per minibatch
    priv = torch.as_tensor(batch["priv"])
    act = torch.as_tensor(batch["act"], device=device)
    old_lp = torch.as_tensor(batch["logp"], device=device)
    adv = torch.as_tensor(batch["adv"], device=device)
    ret = torch.as_tensor(batch["ret"], device=device)
    # --norm-adv: scale the batch's advantages to a standard deviation of 1
    # (no mean shift, so no sign changes and the proportions stay). Process
    # advantages sum every later step's score, so their size follows the
    # rollout length (525-2100 tics); unscaled, long batches swamp the
    # entropy bonus and swing the gradient norm.
    adv_std = adv.std().item() if n > 1 else 0.0
    if getattr(args, "norm_adv", False) and adv_std > 1e-8:
        adv = adv / adv_std
    stats = {k: [] for k in ("pg_loss", "v_loss", "entropy", "approx_kl", "clipfrac", "ref_kl",
                             "grad_norm", "pg_grad_norm", "v_grad_norm")}
    # Health (training_health): which units of the trunk's last layer (the
    # ReLU features both heads read) fire on any sample of the batch, and
    # the optimizer steps skipped for a non-finite loss or gradient.
    alive = None
    nonfinite = 0
    term_cos = term_grad_cosines(policy, batch, args, device) if batch.get("term_adv") else None

    def watch(_module, _inp, out):
        nonlocal alive
        fired = (out.detach() > 0).any(0)
        alive = fired if alive is None else alive | fired
    hook = policy.trunk.register_forward_hook(watch)
    try:
        nonfinite = _policy_epochs(policy, optimizer, ref, args, device, n, obs, priv, act,
                                   old_lp, adv, ret, stats)
    finally:
        hook.remove()
    out = {k: float(np.mean(v)) for k, v in stats.items() if v}
    if stats["grad_norm"]:
        out["grad_norm_max"] = float(np.max(stats["grad_norm"]))
    if alive is not None:
        out["dead_units"] = 1.0 - alive.float().mean().item()
    out["nonfinite_steps"] = nonfinite
    out["adv_std"] = adv_std
    if term_cos is not None:
        out["term_cos"] = term_cos
    return out


def entropy_loss(ent_s: torch.Tensor, args, max_ent: torch.Tensor | None = None) -> torch.Tensor:
    """The loss's entropy term from each sample's entropy: with --ent-target,
    a floor (only states below it pay); else the plain bonus. The floor is
    --ent-target times the sample's highest possible entropy, max_ent = ln
    of the actions it could take (2026-11-20; default every action): one
    rule for every sample (an absolute floor before, 2.5 nats; the combat
    drill's action mask, 2026-11-20, had 6 actions, so ln 6)."""
    if ent_s.numel() == 0:
        return torch.zeros((), device=ent_s.device)
    target = getattr(args, "ent_target", 0.0)
    if target > 0:
        if max_ent is None:
            max_ent = torch.full_like(ent_s, float(np.log(len(ACTIONS))))
        return args.ent_coef * torch.relu(target * max_ent - ent_s).mean()
    return -args.ent_coef * ent_s.mean()


def _policy_epochs(policy, optimizer, ref, args, device, n, obs, priv, act, old_lp, adv, ret,
                   stats) -> int:
    """The epochs of policy_update(); returns the optimizer steps skipped for
    a non-finite loss or gradient norm (so a blow-up never reaches the
    weights)."""
    nonfinite = 0
    for _ in range(args.epochs):
        perm = torch.randperm(n)
        for start in range(0, n, args.minibatch):
            idx = perm[start:start + args.minibatch]
            # One optimizer step per minibatch, its gradient accumulated
            # over --micro-batch pieces (GPU memory): each piece's means
            # are weighted by its share, so the sum is the minibatch's.
            optimizer.zero_grad(set_to_none=True)
            micro = getattr(args, "micro_batch", 0) or len(idx)
            sums = dict.fromkeys(("pg", "v", "ent", "kl", "clip", "ref_kl", "loss"), 0.0)
            for m0 in range(0, len(idx), micro):
                sub_idx = idx[m0:m0 + micro]
                w = len(sub_idx) / len(idx)
                mb_obs = obs[sub_idx].to(device, non_blocking=True)
                mb_priv = priv[sub_idx].to(device, non_blocking=True)
                logits, value = policy(mb_obs, mb_priv)
                dist = Categorical(logits=logits)
                lp = dist.log_prob(act[sub_idx])
                ratio = (lp - old_lp[sub_idx]).exp()
                a = adv[sub_idx]
                # Token-level mean over the minibatch: the 1/sum|o_i| of
                # SPEC.md §7.1, estimated per minibatch.
                pg = -torch.min(ratio * a, ratio.clamp(1 - args.clip, 1 + args.clip) * a).mean()
                ent_s = dist.entropy()
                ent = ent_s.mean()
                v_loss = 0.5 * (value - ret[sub_idx]).pow(2).mean()
                loss = pg + entropy_loss(ent_s, args) + args.vf_coef * v_loss
                if ref is not None and args.beta > 0:
                    with torch.no_grad():
                        ref_logits, _ = ref(mb_obs, mb_priv)
                    logp = torch.log_softmax(logits, dim=1)
                    kl = (logp.exp() * (logp - torch.log_softmax(ref_logits, dim=1))).sum(1).mean()
                    loss = loss + args.beta * kl
                    sums["ref_kl"] += w * kl.item()
                (w * loss).backward()
                with torch.no_grad():
                    log_ratio = lp - old_lp[sub_idx]
                    sums["kl"] += w * ((log_ratio.exp() - 1) - log_ratio).mean().item()
                    sums["clip"] += w * ((ratio - 1).abs() > args.clip).float().mean().item()
                sums["pg"] += w * pg.item()
                sums["v"] += w * v_loss.item()
                sums["ent"] += w * ent.item()
                sums["loss"] += w * loss.item()
            # Clipped apart (2026-11-20; one clip over every parameter
            # before): the value head (its loss is unnormalized returns,
            # thousands) and the rest (trunk and policy head), each to
            # --max-grad-norm, so the value error no longer scales the
            # policy's step down (its norm was ~38 against the policy's
            # ~0.13: the policy moved ~250x less than the clip allows).
            # The norms are before clipping; grad_norm is the whole
            # gradient's, as before (the health checks judge it).
            vids = {id(q) for q in policy.value_head.parameters()}
            pgn = nn.utils.clip_grad_norm_([q for q in policy.parameters() if id(q) not in vids],
                                           args.max_grad_norm).item()
            vgn = nn.utils.clip_grad_norm_(list(policy.value_head.parameters()), args.max_grad_norm).item()
            gn = (pgn ** 2 + vgn ** 2) ** 0.5
            sq = [pgn ** 2, vgn ** 2]
            if not (np.isfinite(gn) and np.isfinite(sums["loss"])):
                nonfinite += 1
                optimizer.zero_grad(set_to_none=True)
                continue
            optimizer.step()
            stats["grad_norm"].append(gn)
            stats["pg_grad_norm"].append(sq[0] ** 0.5)
            stats["v_grad_norm"].append(sq[1] ** 0.5)
            stats["approx_kl"].append(sums["kl"])
            stats["clipfrac"].append(sums["clip"])
            stats["pg_loss"].append(sums["pg"])
            stats["v_loss"].append(sums["v"])
            stats["entropy"].append(sums["ent"])
            if ref is not None and args.beta > 0:
                stats["ref_kl"].append(sums["ref_kl"])
    return nonfinite


# The relative gradient check (training_health): the median of the last
# GRAD_WINDOW healthy updates' gradient_norm_max, used once GRAD_WARMUP of
# them exist. Over 1,169 updates of one run (unscaled advantages) the
# largest spike was 25x that median, and clipping made it harmless.
GRAD_WINDOW = 50
GRAD_WARMUP = 20


def redundant_pairs(cos_hist: dict[str, list[float]], args) -> dict[str, float]:
    """Reward-term pairs whose gradient cosine (term_grad_cosines) averages
    --redundant-cos or more over the last --redundant-window checks
    (2026-11-20): candidates for merging, pair -> mean. A warning only:
    terms with different purposes can point the same way for a while."""
    win, thr = getattr(args, "redundant_window", 0), getattr(args, "redundant_cos", 2.0)
    if win <= 0:
        return {}
    return {pair: float(np.mean(cs[-win:])) for pair, cs in (cos_hist or {}).items()
            if len(cs) >= win and np.mean(cs[-win:]) >= thr}


def training_health(upd: dict, args, recent: list[float] | None = None,
                    cos_hist: dict[str, list[float]] | None = None) -> str | None:
    """Why training must stop after this update, or None if it's healthy:
    - exploding gradient: an optimizer step had a non-finite loss or
      gradient (skipped); the gradient norm before clipping went past
      --stop-grad-ratio times the median of the last GRAD_WINDOW healthy
      updates (`recent`, once it holds GRAD_WARMUP of them: a resumed run
      starts the history again); or past --stop-grad-norm;
    - ReLU collapse: at least --stop-dead-units of the trunk's last-layer
      units never fired on the whole batch (a dead unit gets no gradient
      and never comes back; with all of them dead the policy ignores the
      screen and only its biases still learn);
    - reward conflict (2026-11-19): two reward terms' policy gradients
      point apart: their cosine (term_grad_cosines), averaged over the
      last --conflict-window checks (`cos_hist`, pair -> cosines), is
      below --stop-conflict (-1: off).
    A threshold of 0 turns that check off."""
    if upd.get("nonfinite_steps", 0):
        return (f"exploding gradient: {upd['nonfinite_steps']} optimizer steps had a non-finite "
                "loss or gradient (skipped)")
    gmax = upd.get("grad_norm_max")
    ratio = getattr(args, "stop_grad_ratio", 0)
    if ratio > 0 and gmax is not None and recent is not None and len(recent) >= GRAD_WARMUP:
        med = float(np.median(recent[-GRAD_WINDOW:]))
        if gmax > ratio * med:
            return (f"exploding gradient: gradient norm {gmax:.3g} before clipping, "
                    f"{gmax / med:.0f}x the median of the last {min(len(recent), GRAD_WINDOW)} "
                    f"updates ({med:.3g}; --stop-grad-ratio {ratio:g})")
    if args.stop_grad_norm > 0 and gmax is not None and gmax > args.stop_grad_norm:
        return (f"exploding gradient: gradient norm {gmax:.3g} before clipping "
                f"(--stop-grad-norm {args.stop_grad_norm:g})")
    win = getattr(args, "conflict_window", 0)
    worst = None
    for pair, cs in (cos_hist or {}).items():
        if win > 0 and len(cs) >= win and (worst is None or np.mean(cs[-win:]) < worst[1]):
            worst = (pair, float(np.mean(cs[-win:])))
    if worst is not None and worst[1] < getattr(args, "stop_conflict", -1.0):
        return (f"reward conflict: {worst[0].replace('|', ' and ')} have a gradient cosine of "
                f"{worst[1]:.3f} averaged over the last {win} checks (--stop-conflict "
                f"{args.stop_conflict:g})")
    dead = upd.get("dead_units")
    if args.stop_dead_units > 0 and dead is not None and dead >= args.stop_dead_units:
        return (f"ReLU collapse: {dead:.0%} of the trunk's last-layer units never fired on this "
                f"batch (--stop-dead-units {args.stop_dead_units:g})")
    return None


def train_metrics(recs: list[dict], n_groups: int, collapsed: int) -> dict:
    """Rollout statistics for one update. Adds per-IWAD sectors and exit
    rate when the rollouts cover more than one IWAD (recs carry "iwad")."""
    acts = np.bincount(np.concatenate([np.asarray(r["act"], dtype=np.int64) for r in recs]),
                       minlength=len(ACTIONS))
    m = {"steps": sum(r["len"] for r in recs), "tics": sum(r["rollout_tics"] for r in recs),
         "return": float(np.mean([r["ret"] for r in recs])),
         "return_std": float(np.std([r["ret"] for r in recs])),
         "sectors": float(np.mean([r["sectors_gained"] for r in recs])),
         "exit_rate": float(np.mean([r["exit"] for r in recs])),
         "death_rate": float(np.mean([r["death"] for r in recs])),
         "key_rate": float(np.mean([r.get("key", False) for r in recs])),
         "items_spawned": int(sum(r.get("items", [0, 0])[0] for r in recs)),
         "items_picked": int(sum(r.get("items", [0, 0])[1] for r in recs)),
         "waypoints_set": int(sum(r.get("waypoints", [0, 0])[0] for r in recs)),
         "waypoints_reached": int(sum(r.get("waypoints", [0, 0])[1] for r in recs)),
         # Combat (2026-11-18): the fighting rollouts' shots, hits, kills, damage dealt.
         "combat_rollouts": int(sum(r.get("combat", [0])[0] for r in recs)),
         "shots": int(sum(r.get("combat", [0, 0])[1] for r in recs)),
         "hits": int(sum(r.get("combat", [0, 0, 0])[2] for r in recs)),
         "kills": int(sum(r.get("combat", [0, 0, 0, 0])[3] for r in recs)),
         "damage_dealt": float(sum(r.get("combat", [0, 0, 0, 0, 0])[4] for r in recs)),
         "combat_death_rate": float(np.mean([r["death"] for r in recs if r.get("combat", [0])[0]] or [0.0])),
         "collapsed_share": collapsed / max(n_groups, 1),
         "action_share": (acts / max(acts.sum(), 1)).round(3).tolist()}
    cs = np.sum([r.get("consult_stats", [0, 0, 0, 0]) for r in recs], axis=0)
    if cs[0]:
        m["consult_trig"] = float(cs[1] / cs[0])    # P(CONSULT | a unique trigger pending)
    if cs[2]:
        m["consult_idle"] = float(cs[3] / cs[2])    # P(CONSULT | none pending): should fall toward 0
    if any("frontier" in r for r in recs):
        m["frontier_share"] = float(np.mean([bool(r.get("frontier")) for r in recs]))
    if recs and recs[0].get("frontier_saved"):
        m["frontier_saved"] = recs[0]["frontier_saved"]
    if recs and recs[0].get("groups"):
        m["groups"] = recs[0]["groups"]       # the maps the groups played (MapRotation)
    labels = sorted({r["iwad"] for r in recs if "iwad" in r})
    if len(labels) > 1:
        for w in labels:
            rs = [r for r in recs if r.get("iwad") == w]
            m[f"sectors_{w}"] = float(np.mean([r["sectors_gained"] for r in rs]))
            m[f"exit_rate_{w}"] = float(np.mean([r["exit"] for r in rs]))
    return m


def print_train(it: int, m: dict, upd: dict, extra: str = "") -> None:
    print(f"[it {it:5d}] ret {m['return']:7.2f} sectors {m['sectors']:5.2f} exit {m['exit_rate']:.2f}"
          + (f" front {m['frontier_share']:.2f}" if "frontier_share" in m else "")
          + (f" wp {m['waypoints_reached'] / m['waypoints_set']:.2f}" if m.get("waypoints_set") else "")
          + (f" consult {m.get('consult_trig', 0):.3f}/{m['consult_idle']:.3f}" if "consult_idle" in m else "")
          + (f" items {m['items_picked'] / m['items_spawned']:.2f}" if m.get("items_spawned") else "")
          + (f" kills {m['kills']} hit {m['hits'] / m['shots']:.2f}" if m.get("shots") else "")
          + (f" dead {upd['dead_units']:.2f}" if "dead_units" in upd else "")
          + (" cos min {} {:.3f}".format(*min(upd["term_cos"].items(), key=lambda kv: kv[1]))
             if upd.get("term_cos") else "")
          + (f" collapsed {m['collapsed_share']:.2f} steps {m['steps']}")
          + (f" ent {upd.get('entropy', float('nan')):.3f} kl {upd.get('approx_kl', float('nan')):.4f}")
          + extra)


def _share(text: str) -> float:
    """--ent-target: a share of the highest possible entropy, 0..1 (an
    absolute floor in nats before 2026-11-20: 2.5 is now 0.62)."""
    v = float(text)
    if not 0.0 <= v <= 1.0:
        raise argparse.ArgumentTypeError(f"{v:g} is not a share in 0..1 (since 2026-11-20 --ent-target is a share "
                                         f"of the highest possible entropy; the old 2.5 nats is 0.62)")
    return v


def add_learn_args(p) -> None:
    """Optimizer and checkpoint arguments shared with train.py."""
    p.add_argument("--iterations", type=int, default=1000)
    p.add_argument("--advantage", choices=["outcome", "process"], default="process")
    p.add_argument("--lr", type=float, default=2.5e-4)
    p.add_argument("--epochs", type=int, default=4)
    p.add_argument("--minibatch", type=int, default=4096,   # 8192 before 2026-11-20
                   help="samples per optimizer step (default 4096 since 2026-11-20, for a KL of about 0.001 at 8192; 8192 before: with a decision every tic a batch is 20,000-70,000 samples, and every step is clipped to the same size by --max-grad-norm, so the policy moves by about the number of steps per update; 2048 made about 100 steps and a KL of about 0.1)")
    p.add_argument("--micro-batch", type=int, default=2048,
                   help="samples per forward/backward pass on the GPU; a --minibatch's gradient is "
                        "accumulated over pieces this size before its one optimizer step, so the "
                        "update is the same, only the memory changes (0: the whole minibatch at once; "
                        "8192 at 160x100 ran out of 16 GB)")
    p.add_argument("--clip", type=float, default=0.2)
    p.add_argument("--ent-coef", type=float, default=0.1)   # 0.01 before 2026-11-19, as a plain bonus
    p.add_argument("--ent-target", type=_share, default=0.62,   # 2.5 nats (absolute) before 2026-11-20; 1.0 before 2026-11-19
                   help="entropy floor per state, as a share of its highest possible entropy (ln of the actions "
                        "it could take: all 56): --ent-coef then weighs "
                        "max(0, target * ln(actions) - entropy), so only states below it are pushed back up "
                        "(default 0.62: 2.5 nats with every action, as the absolute floor before 2026-11-20; "
                        "0: the plain bonus on all states, which wants a smaller "
                        "--ent-coef, e.g. 0.01)")
    p.add_argument("--vf-coef", type=float, default=0.5)
    p.add_argument("--max-grad-norm", type=float, default=0.5,
                   help="gradient norm limit per optimizer step, applied to the value head and to the rest (trunk and policy head) apart since 2026-11-20 (one limit over both before)")
    p.add_argument("--stop-dead-units", type=float, default=0.5,
                   help="stop training (no save, no upload) when this share of the trunk's last-layer "
                        "units never fire on a batch: ReLU collapse (0: off)")
    p.add_argument("--stop-grad-ratio", type=float, default=100.0,
                   help="stop training (no save, no upload) when the gradient norm before clipping "
                        "goes past this many times the median of the last 50 updates: an exploding "
                        "gradient (0: off; a non-finite one always stops)")
    p.add_argument("--stop-grad-norm", type=float, default=0.0,
                   help="also stop when the gradient norm before clipping goes past this absolute "
                        "value (0: off, the default)")
    p.add_argument("--conflict-every", type=int, default=10,
                   help="every this many updates, the cosine similarity of each pair of reward terms' "
                        "policy gradients (metrics term_cos; 0: off)")
    p.add_argument("--conflict-samples", type=int, default=4096,
                   help="batch samples the reward-term gradients are taken on")
    p.add_argument("--conflict-window", type=int, default=10,   # 5, then 14, before 2026-11-20
                   help="checks a pair's cosine is averaged over before --stop-conflict judges it "
                        "(10 at --conflict-every 10: 100 updates)")
    p.add_argument("--stop-conflict", type=float, default=-0.5,   # -0.1 before 2026-11-20
                   help="stop training (no save, no upload) when two reward terms' gradient cosine, "
                        "averaged over --conflict-window checks, is below this (-1: never)")
    p.add_argument("--redundant-cos", type=float, default=0.9,
                   help="warn (never stop) when two reward terms' gradient cosine averages this or more over "
                        "--redundant-window checks: likely redundant, candidates to merge (2026-11-20)")
    p.add_argument("--redundant-window", type=int, default=30,
                   help="checks the redundancy warning averages over (30 at --conflict-every 10: 300 "
                        "updates; 0: off)")
    p.add_argument("--norm-adv", action=argparse.BooleanOptionalAction, default=True,
                   help="scale each batch's advantages to a standard deviation of 1 (no mean shift), "
                        "so their size doesn't follow the rollout length")
    p.add_argument("--gamma-tic", type=float, default=0.999,
                   help="per-tic discount for value targets (0.999 ~ 30 s horizon)")
    p.add_argument("--beta", type=float, default=0.0, help="KL weight to --ref (0: no reference policy)")
    p.add_argument("--ref", default=None, help="reference policy checkpoint (e.g. a behavior clone)")
    p.add_argument("--init", default=None, help="initialize weights from a checkpoint (new run)")
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--device", default="cuda" if torch.cuda.is_available() else "cpu")


def require_obs_size(path: str, size: tuple[int, int], what: str) -> None:
    """Stop unless the checkpoint at path was trained at this --obs-size."""
    own = checkpoint_obs_size(torch.load(path, map_location="cpu", weights_only=False))
    if own != tuple(size):
        raise SystemExit(f"{what} {path} was trained at {own[0]}x{own[1]}, this run is {size[0]}x{size[1]}")


def setup_learner(args, run_dir: Path, device: torch.device, sampler: np.random.Generator | None = None):
    """Policy, optimizer and reference policy, resumed from the newest
    checkpoint in run_dir or initialized from --init. Returns (policy,
    optimizer, ref, start_it, total_steps, total_tics, run_id)."""
    ckpt = load_latest_checkpoint(run_dir)
    # The observation size (--obs-size): a resumed run keeps its own;
    # another size can't load its weights, so asking for one stops.
    asked = getattr(args, "obs_size", None)
    if ckpt is not None:
        own = checkpoint_obs_size(ckpt)
        if asked is not None and tuple(asked) != own:
            raise SystemExit(f"--obs-size {asked[0]}x{asked[1]}: {run_dir} was trained at {own[0]}x{own[1]}; "
                             f"start a new --run-dir for another size")
        args.obs_size = own
    else:
        args.obs_size = tuple(asked) if asked is not None else DEFAULT_OBS_SIZE
    policy = Policy(obs_size=args.obs_size).to(device)
    optimizer = torch.optim.Adam(policy.parameters(), lr=args.lr, eps=1e-5)
    random.seed(args.seed)
    np.random.seed(args.seed)
    torch.manual_seed(args.seed)
    start_it, total_steps, total_tics = 0, 0, 0
    run_id = uuid.uuid4().hex[:8]
    if ckpt is not None:
        policy.load_state_dict(ckpt["policy"])
        optimizer.load_state_dict(ckpt["optimizer"])
        restore_rng(ckpt["rng"], sampler)
        start_it = ckpt["iteration"] + 1
        total_steps, total_tics = ckpt["total_steps"], ckpt["total_tics"]
        run_id = ckpt["run_id"]
        if ckpt.get("ref") != args.ref:
            print(f"[ckpt] warning: reference policy changed ({ckpt.get('ref')} -> {args.ref})")
    elif args.init:
        require_obs_size(args.init, args.obs_size, "--init")
        policy.load_state_dict(load_weights(args.init))
        print(f"[init] weights from {args.init}")
    ref = None
    if args.ref:
        require_obs_size(args.ref, args.obs_size, "--ref")
        ref = Policy(obs_size=args.obs_size).to(device)
        ref.load_state_dict(load_weights(args.ref))
        ref.eval()
        for prm in ref.parameters():
            prm.requires_grad_(False)
    return policy, optimizer, ref, start_it, total_steps, total_tics, run_id
