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
            for t, n in zip(togo, now):
                advs.append((t / (cstd + eps) + n / (std + eps)).astype(np.float32))
    return advs, collapsed


def _rew_now(r: dict) -> np.ndarray:
    """A rollout's per-step one-off rewards (zeros if it has none)."""
    n = r.get("rew_now")
    return np.zeros(len(r["rew"])) if n is None else np.array(n, dtype=np.float64)


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


def update(policy: Policy, optimizer: torch.optim.Optimizer, ref: Policy | None,
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
                             "grad_norm")}
    # Health (training_health): which units of the trunk's last layer (the
    # ReLU features both heads read) fire on any sample of the batch, and
    # the optimizer steps skipped for a non-finite loss or gradient.
    alive = None
    nonfinite = 0

    def watch(_module, _inp, out):
        nonlocal alive
        fired = (out.detach() > 0).any(0)
        alive = fired if alive is None else alive | fired
    hook = policy.trunk.register_forward_hook(watch)
    try:
        nonfinite = _update_epochs(policy, optimizer, ref, args, device, n, obs, priv, act,
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
    return out


def _update_epochs(policy, optimizer, ref, args, device, n, obs, priv, act, old_lp, adv, ret,
                   stats) -> int:
    """The PPO epochs of update(); returns the optimizer steps skipped for
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
                ent = dist.entropy().mean()
                v_loss = 0.5 * (value - ret[sub_idx]).pow(2).mean()
                loss = pg - args.ent_coef * ent + args.vf_coef * v_loss
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
            # The norm before clipping: what the gradient would have been.
            gn = nn.utils.clip_grad_norm_(policy.parameters(), args.max_grad_norm).item()
            if not (np.isfinite(gn) and np.isfinite(sums["loss"])):
                nonfinite += 1
                optimizer.zero_grad(set_to_none=True)
                continue
            optimizer.step()
            stats["grad_norm"].append(gn)
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


def training_health(upd: dict, args, recent: list[float] | None = None) -> str | None:
    """Why training must stop after this update, or None if it's healthy:
    - exploding gradient: an optimizer step had a non-finite loss or
      gradient (skipped); the gradient norm before clipping went past
      --stop-grad-ratio times the median of the last GRAD_WINDOW healthy
      updates (`recent`, once it holds GRAD_WARMUP of them: a resumed run
      starts the history again); or past --stop-grad-norm;
    - ReLU collapse: at least --stop-dead-units of the trunk's last-layer
      units never fired on the whole batch (a dead unit gets no gradient
      and never comes back; with all of them dead the policy ignores the
      screen and only its biases still learn).
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
         "collapsed_share": collapsed / max(n_groups, 1),
         "action_share": (acts / max(acts.sum(), 1)).round(3).tolist()}
    cs = np.sum([r.get("consult_stats", [0, 0, 0, 0]) for r in recs], axis=0)
    if cs[0]:
        m["consult_trig"] = float(cs[1] / cs[0])    # P(CONSULT | a monster/unique trigger pending)
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
    print(f"[it {it:5d}] ret {m['return']:7.2f} sectors {m['sectors']:5.2f} "
          f"exit {m['exit_rate']:.2f} collapsed {m['collapsed_share']:.2f} steps {m['steps']} "
          f"ent {upd.get('entropy', float('nan')):.3f} kl {upd.get('approx_kl', float('nan')):.4f}"
          + (f" front {m['frontier_share']:.2f}" if "frontier_share" in m else "")
          + (f" dead {upd['dead_units']:.2f}" if "dead_units" in upd else "")
          + (f" items {m['items_picked'] / m['items_spawned']:.2f}" if m.get("items_spawned") else "")
          + (f" wp {m['waypoints_reached'] / m['waypoints_set']:.2f}" if m.get("waypoints_set") else "")
          + (f" consult {m.get('consult_trig', 0):.3f}/{m['consult_idle']:.3f}" if "consult_idle" in m else "")
          + extra)


def add_learn_args(p) -> None:
    """Optimizer and checkpoint arguments shared with train.py."""
    p.add_argument("--iterations", type=int, default=1000)
    p.add_argument("--advantage", choices=["outcome", "process"], default="process")
    p.add_argument("--lr", type=float, default=2.5e-4)
    p.add_argument("--epochs", type=int, default=4)
    p.add_argument("--minibatch", type=int, default=8192,
                   help="samples per optimizer step (default 8192: with a decision every tic a batch is 20,000-70,000 samples, and every step is clipped to the same size by --max-grad-norm, so the policy moves by about the number of steps per update; 2048 made about 100 steps and a KL of about 0.1)")
    p.add_argument("--micro-batch", type=int, default=2048,
                   help="samples per forward/backward pass on the GPU; a --minibatch's gradient is "
                        "accumulated over pieces this size before its one optimizer step, so the "
                        "update is the same, only the memory changes (0: the whole minibatch at once; "
                        "8192 at 160x100 ran out of 16 GB)")
    p.add_argument("--clip", type=float, default=0.2)
    p.add_argument("--ent-coef", type=float, default=0.01)
    p.add_argument("--vf-coef", type=float, default=0.5)
    p.add_argument("--max-grad-norm", type=float, default=0.5)
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
