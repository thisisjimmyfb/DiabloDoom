#!/usr/bin/env python3
"""GRPO trainer for the DiabloDoom level runner (SPEC.md §6, §7.1, §7.4).

Each iteration runs `groups` GRPO groups of `group_size` rollouts. Every
rollout in a group starts from the same state (same map, save slot,
RNG seed and warm-up actions) and differs only through sampled actions.
A rollout's score R_i is its total reward; every decision in it gets
the group-normalized advantage

    A_i = (R_i - mean(R_group)) / (std(R_group) + eps)

(or, with --advantage process, normalized per-step rewards summed from
each step onward). The policy is updated with the clipped-ratio loss
for a few epochs per batch. The value head (critic, with privileged
HP/exit-distance inputs) is trained alongside on detached features, so
it never shapes the policy during GRPO; Phase 6 PPO will need it.

Usage (from agent/training):
    .venv/bin/python train.py --run-dir runs/stage1                # stage 1
    .venv/bin/python train.py --run-dir runs/stage1                # resumes
    .venv/bin/python train.py --stage 2 --run-dir runs/stage2 \\
        --init runs/stage1/ckpt_000500.pt
"""

from __future__ import annotations

import argparse
import functools
import json
import os
import random
import signal
import sys
import time
import uuid
from pathlib import Path

import numpy as np
import torch
import torch.nn as nn
from torch.distributions import Categorical

sys.path.insert(0, str(Path(__file__).resolve().parent))
from env import ACTIONS, CONSULT, cleanup_stale  # noqa: E402
from env.vector import GroupVecEnv  # noqa: E402

MAP_ACTION = ACTIONS.index("MAP")
# Warm-up uses movement only (no MAP toggle, no CONSULT).
WARMUP_ACTIONS = [i for i in range(len(ACTIONS)) if i not in (CONSULT, MAP_ACTION)]
N_PRIV = 2  # privileged critic inputs: HP/100, exit path distance (SPEC.md §6)


# ---------------------------------------------------------------------------
# Environment factory (top-level so worker processes can unpickle it)
# ---------------------------------------------------------------------------

def make_env(warp: str, stage: int, skill: int, max_tics: int,
             slots_dir: str | None, slots: list[int], capture_dir: str | None = None):
    from env import (DiabloDoomEnv, EngineConfig, EnvConfig, null_consult,
                     scripted_combat_consult)
    # Stage 1 (SPEC.md §7.2): no monsters. Vanilla -nomonsters is the
    # -rlnocombat of the spec. Stage 2: monsters on, CONSULT answered by
    # the scripted combat stand-in.
    extra = ["-nomonsters"] if stage == 1 else []
    return DiabloDoomEnv(EnvConfig(
        engine=EngineConfig(warp=warp, skill=skill, extra_args=extra,
                            savedir_template=slots_dir, capture_dir=capture_dir),
        max_tics=max_tics,
        start_slots=list(slots),
        consult_handler=null_consult if stage == 1 else scripted_combat_consult,
    ))


def privileged(info: dict) -> list[float]:
    s = info.get("state", {})
    exit_dist = s.get("exit_dist")
    return [s.get("hp", 0) / 100.0, (exit_dist / 4096.0) if exit_dist is not None else 0.0]


# ---------------------------------------------------------------------------
# Policy (SPEC.md §6, step 1: feedforward)
# ---------------------------------------------------------------------------

def layer_init(layer: nn.Module, std: float = np.sqrt(2), bias: float = 0.0) -> nn.Module:
    nn.init.orthogonal_(layer.weight, std)
    nn.init.constant_(layer.bias, bias)
    return layer


class Policy(nn.Module):
    """Nature CNN -> 512 features -> 12 action logits; value head on
    detached features plus privileged inputs (critic, training only)."""

    def __init__(self, n_actions: int = len(ACTIONS), frames: int = 4):
        super().__init__()
        self.trunk = nn.Sequential(
            layer_init(nn.Conv2d(frames, 32, 8, stride=4)), nn.ReLU(),
            layer_init(nn.Conv2d(32, 64, 4, stride=2)), nn.ReLU(),
            layer_init(nn.Conv2d(64, 64, 3, stride=1)), nn.ReLU(),
            nn.Flatten(),
            layer_init(nn.Linear(64 * 7 * 7, 512)), nn.ReLU(),
        )
        self.pi = layer_init(nn.Linear(512, n_actions), std=0.01)
        self.v = layer_init(nn.Linear(512 + N_PRIV, 1), std=1.0)

    def forward(self, obs: torch.Tensor, priv: torch.Tensor):
        h = self.trunk(obs.float() / 255.0)
        logits = self.pi(h)
        value = self.v(torch.cat([h.detach(), priv], dim=1)).squeeze(1)
        return logits, value


# ---------------------------------------------------------------------------
# Checkpoints (SPEC.md §7.4)
# ---------------------------------------------------------------------------

def _fsync_dir(d: Path) -> None:
    fd = os.open(d, os.O_RDONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def save_checkpoint(run_dir: Path, iteration: int, state: dict, keep: int) -> Path:
    path = run_dir / f"ckpt_{iteration:06d}.pt"
    tmp = path.with_suffix(".pt.tmp")
    with open(tmp, "wb") as f:
        torch.save(state, f)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)
    _fsync_dir(run_dir)
    for old in sorted(run_dir.glob("ckpt_*.pt"))[:-keep]:
        old.unlink(missing_ok=True)
    return path


def load_latest_checkpoint(run_dir: Path) -> dict | None:
    for tmp in run_dir.glob("ckpt_*.pt.tmp"):
        tmp.unlink(missing_ok=True)       # interrupted write
    for path in sorted(run_dir.glob("ckpt_*.pt"), reverse=True):
        try:
            ckpt = torch.load(path, map_location="cpu", weights_only=False)
            print(f"[ckpt] resuming from {path}")
            return ckpt
        except Exception as e:  # corrupt or truncated: fall back to the previous one
            print(f"[ckpt] cannot load {path}: {e}; trying an older one")
    return None


def rng_state(sampler: np.random.Generator) -> dict:
    return {
        "python": random.getstate(),
        "numpy": np.random.get_state(),
        "torch": torch.get_rng_state(),
        "cuda": torch.cuda.get_rng_state_all() if torch.cuda.is_available() else None,
        "sampler": sampler.bit_generator.state,
    }


def restore_rng(state: dict, sampler: np.random.Generator) -> None:
    random.setstate(state["python"])
    np.random.set_state(state["numpy"])
    torch.set_rng_state(state["torch"])
    if state.get("cuda") is not None and torch.cuda.is_available():
        torch.cuda.set_rng_state_all(state["cuda"])
    sampler.bit_generator.state = state["sampler"]


# ---------------------------------------------------------------------------
# Rollouts
# ---------------------------------------------------------------------------

def run_rollouts(vec: GroupVecEnv, policy: Policy, device: torch.device,
                 options: list[dict]) -> list[dict]:
    """Reset every env with its options and run each to its end with
    sampled actions. Returns one record per env."""
    n = len(options)
    obs, infos = vec.reset(range(n), options)
    cur_obs = obs.copy()
    cur_priv = np.array([privileged(i) for i in infos], dtype=np.float32)
    recs = [{"obs": [], "priv": [], "act": [], "logp": [], "rew": [], "tics": [],
             "start_sectors": infos[k]["state"].get("sectors", 0), "end_info": None}
            for k in range(n)]
    active = list(range(n))
    policy.eval()
    while active:
        with torch.no_grad():
            o = torch.as_tensor(cur_obs[active], device=device)
            p = torch.as_tensor(cur_priv[active], device=device)
            logits, _ = policy(o, p)
            dist = Categorical(logits=logits)
            a = dist.sample()
            lp = dist.log_prob(a)
        a_np, lp_np = a.cpu().numpy(), lp.cpu().numpy()
        nobs, rew, term, trunc, ninfo = vec.step(active, a_np)
        still = []
        for k, i in enumerate(active):
            r = recs[i]
            r["obs"].append(cur_obs[i].copy())
            r["priv"].append(cur_priv[i].copy())
            r["act"].append(int(a_np[k]))
            r["logp"].append(float(lp_np[k]))
            r["rew"].append(float(rew[k]))
            r["tics"].append(int(ninfo[k]["tics"]))
            cur_obs[i] = nobs[k]
            cur_priv[i] = privileged(ninfo[k])
            if term[k] or trunc[k]:
                r["end_info"] = ninfo[k]
                r["terminated"] = bool(term[k])
            else:
                still.append(i)
        active = still
    for r in recs:
        s = r["end_info"]["state"]
        r["ret"] = float(sum(r["rew"]))
        r["len"] = len(r["rew"])
        r["rollout_tics"] = int(r["end_info"]["rollout_tics"])
        r["sectors_gained"] = int(s.get("sectors", 0)) - int(r["start_sectors"])
        r["exit"] = bool(r["end_info"].get("exit"))
        r["death"] = bool(s.get("dead"))
    return recs


def group_advantages(recs: list[dict], groups: int, size: int, mode: str,
                     eps: float = 1e-8) -> tuple[list[np.ndarray | None], int]:
    """Per-rollout advantage arrays (one value per decision); None for
    rollouts in collapsed (zero-variance) groups. Returns (advs, collapsed)."""
    advs: list[np.ndarray | None] = [None] * len(recs)
    collapsed = 0
    for g in range(groups):
        members = recs[g * size:(g + 1) * size]
        if mode == "outcome":
            rets = np.array([m["ret"] for m in members], dtype=np.float64)
            std = rets.std()
            if std < 1e-6:
                collapsed += 1
                continue
            for j, m in enumerate(members):
                advs[g * size + j] = np.full(m["len"], (rets[j] - rets.mean()) / (std + eps),
                                             dtype=np.float32)
        else:  # process: normalize per-step rewards across the group
            allr = np.concatenate([np.array(m["rew"], dtype=np.float64) for m in members])
            std = allr.std()
            if std < 1e-6:
                collapsed += 1
                continue
            mean = allr.mean()
            for j, m in enumerate(members):
                norm = (np.array(m["rew"], dtype=np.float64) - mean) / (std + eps)
                advs[g * size + j] = np.cumsum(norm[::-1])[::-1].astype(np.float32)
    return advs, collapsed


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


# ---------------------------------------------------------------------------
# Update
# ---------------------------------------------------------------------------

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
    stats = {k: [] for k in ("pg_loss", "v_loss", "entropy", "approx_kl", "clipfrac", "ref_kl")}
    for _ in range(args.epochs):
        perm = torch.randperm(n)
        for start in range(0, n, args.minibatch):
            idx = perm[start:start + args.minibatch]
            mb_obs = obs[idx].to(device, non_blocking=True)
            mb_priv = priv[idx].to(device, non_blocking=True)
            logits, value = policy(mb_obs, mb_priv)
            dist = Categorical(logits=logits)
            lp = dist.log_prob(act[idx])
            ratio = (lp - old_lp[idx]).exp()
            a = adv[idx]
            # Token-level mean over the minibatch: the 1/sum|o_i| of
            # SPEC.md §7.1, estimated per minibatch.
            pg = -torch.min(ratio * a, ratio.clamp(1 - args.clip, 1 + args.clip) * a).mean()
            ent = dist.entropy().mean()
            v_loss = 0.5 * (value - ret[idx]).pow(2).mean()
            loss = pg - args.ent_coef * ent + args.vf_coef * v_loss
            if ref is not None and args.beta > 0:
                with torch.no_grad():
                    ref_logits, _ = ref(mb_obs, mb_priv)
                logp = torch.log_softmax(logits, dim=1)
                kl = (logp.exp() * (logp - torch.log_softmax(ref_logits, dim=1))).sum(1).mean()
                loss = loss + args.beta * kl
                stats["ref_kl"].append(kl.item())
            optimizer.zero_grad(set_to_none=True)
            loss.backward()
            nn.utils.clip_grad_norm_(policy.parameters(), args.max_grad_norm)
            optimizer.step()
            with torch.no_grad():
                log_ratio = lp - old_lp[idx]
                stats["approx_kl"].append(((log_ratio.exp() - 1) - log_ratio).mean().item())
                stats["clipfrac"].append(((ratio - 1).abs() > args.clip).float().mean().item())
            stats["pg_loss"].append(pg.item())
            stats["v_loss"].append(v_loss.item())
            stats["entropy"].append(ent.item())
    return {k: float(np.mean(v)) for k, v in stats.items() if v}


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def parse_args():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--run-dir", default="runs/grpo")
    p.add_argument("--stage", type=int, choices=[1, 2], default=1,
                   help="1: no monsters (exploration); 2: monsters on, CONSULT on sighting")
    p.add_argument("--maps", default="1 1,1 2,1 3,1 4",
                   help="training maps as -warp args, comma-separated; group g uses maps[g %% len]")
    p.add_argument("--eval-maps", default="1 5,1 6", help="held-out maps for evaluation ('' to skip)")
    p.add_argument("--skill", type=int, default=3)
    p.add_argument("--slots-dir", default=None, help="prepared save slots copied into each instance")
    p.add_argument("--slots", default="", help="start slots in --slots-dir, e.g. '3,4,5'")
    p.add_argument("--groups", type=int, default=4)
    p.add_argument("--group-size", type=int, default=8)
    p.add_argument("--iterations", type=int, default=1000)
    p.add_argument("--max-tics", type=int, default=2100)
    p.add_argument("--warmup-max", type=int, default=40,
                   help="random warm-up actions per group start (0..N), for start-state variety")
    p.add_argument("--advantage", choices=["outcome", "process"], default="outcome")
    p.add_argument("--lr", type=float, default=2.5e-4)
    p.add_argument("--epochs", type=int, default=4)
    p.add_argument("--minibatch", type=int, default=256)
    p.add_argument("--clip", type=float, default=0.2)
    p.add_argument("--ent-coef", type=float, default=0.01)
    p.add_argument("--vf-coef", type=float, default=0.5)
    p.add_argument("--max-grad-norm", type=float, default=0.5)
    p.add_argument("--gamma-tic", type=float, default=0.999,
                   help="per-tic discount for value targets (0.999 ~ 30 s horizon)")
    p.add_argument("--beta", type=float, default=0.0, help="KL weight to --ref (0: no reference policy)")
    p.add_argument("--ref", default=None, help="reference policy checkpoint (e.g. a behavior clone)")
    p.add_argument("--init", default=None, help="initialize weights from a checkpoint (new run)")
    p.add_argument("--eval-every", type=int, default=10)
    p.add_argument("--eval-episodes", type=int, default=2, help="rollouts per eval map")
    p.add_argument("--capture-envs", type=int, default=1,
                   help="training envs (from env 0) that record continuous replay segments "
                        "into <run-dir>/replays/env<i>/ (0: none)")
    p.add_argument("--capture-eval", action="store_true",
                   help="also record the eval envs (<run-dir>/replays/eval<i>/)")
    p.add_argument("--keep", type=int, default=3, help="checkpoints to keep")
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--device", default="cuda" if torch.cuda.is_available() else "cpu")
    return p.parse_args()


def load_weights(path: str) -> dict:
    ckpt = torch.load(path, map_location="cpu", weights_only=False)
    return ckpt["policy"] if "policy" in ckpt else ckpt


def main():
    args = parse_args()
    run_dir = Path(args.run_dir)
    run_dir.mkdir(parents=True, exist_ok=True)
    device = torch.device(args.device)
    maps = [m.strip() for m in args.maps.split(",") if m.strip()]
    eval_maps = [m.strip() for m in args.eval_maps.split(",") if m.strip()]
    slots = [int(s) for s in args.slots.split(",") if s.strip()]
    G, groups = args.group_size, args.groups
    n_envs = G * groups

    # SIGTERM (the execution environment stopping) -> clean shutdown;
    # the last checkpoint is at the previous update boundary.
    def on_sigterm(signum, frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, on_sigterm)
    cleanup_stale()

    policy = Policy().to(device)
    optimizer = torch.optim.Adam(policy.parameters(), lr=args.lr, eps=1e-5)
    sampler = np.random.default_rng(args.seed)
    random.seed(args.seed)
    np.random.seed(args.seed)
    torch.manual_seed(args.seed)

    start_it, total_steps, total_tics = 0, 0, 0
    run_id = uuid.uuid4().hex[:8]
    ckpt = load_latest_checkpoint(run_dir)
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
        policy.load_state_dict(load_weights(args.init))
        print(f"[init] weights from {args.init}")

    ref = None
    if args.ref:
        ref = Policy().to(device)
        ref.load_state_dict(load_weights(args.ref))
        ref.eval()
        for prm in ref.parameters():
            prm.requires_grad_(False)

    (run_dir / "config.json").write_text(json.dumps({**vars(args), "run_id": run_id}, indent=2))
    n_params = sum(p.numel() for p in policy.parameters())
    print(f"[train] run {run_id}: {n_envs} envs ({groups} groups x {G}), stage {args.stage}, "
          f"maps {maps}, eval {eval_maps}, {n_params / 1e6:.2f}M params, device {device}")

    env_maps = [maps[(i // G) % len(maps)] for i in range(n_envs)]
    replays = run_dir / "replays"
    vec = GroupVecEnv([functools.partial(make_env, m, args.stage, args.skill, args.max_tics,
                                         args.slots_dir, slots,
                                         str(replays / f"env{i:02d}") if i < args.capture_envs else None)
                       for i, m in enumerate(env_maps)])
    eval_vec = None
    if eval_maps and args.eval_every > 0:
        eval_env_maps = [m for m in eval_maps for _ in range(args.eval_episodes)]
        eval_vec = GroupVecEnv([functools.partial(make_env, m, args.stage, args.skill, args.max_tics,
                                                  args.slots_dir, slots,
                                                  str(replays / f"eval{i:02d}") if args.capture_eval else None)
                                for i, m in enumerate(eval_env_maps)])
    metrics_f = open(run_dir / "metrics.jsonl", "a")

    def evaluate(it: int):
        # Fixed starts (seed 0, no warm-up) so evals are comparable over time.
        recs = run_rollouts(eval_vec, policy, device,
                            [{"seed": 0, "group": -1, "warmup": []} for _ in eval_env_maps])
        per_map = {}
        for m, r in zip(eval_env_maps, recs):
            per_map.setdefault(m, []).append(r)
        out = {"iteration": it, "kind": "eval",
               "eval_sectors": float(np.mean([r["sectors_gained"] for r in recs])),
               "eval_exit_rate": float(np.mean([r["exit"] for r in recs])),
               "eval_return": float(np.mean([r["ret"] for r in recs])),
               "eval_per_map": {m: float(np.mean([r["sectors_gained"] for r in rs]))
                                for m, rs in per_map.items()}}
        metrics_f.write(json.dumps(out) + "\n")
        metrics_f.flush()
        print(f"[eval {it:5d}] sectors {out['eval_sectors']:.2f} exit {out['eval_exit_rate']:.2f} "
              f"return {out['eval_return']:.2f} per map {out['eval_per_map']}")

    try:
        if eval_vec is not None and start_it == 0:
            evaluate(0)  # baseline before any update
        for it in range(start_it, args.iterations):
            t0 = time.time()
            # Group starts: same slot, seed and warm-up for all G members.
            options = []
            for g in range(groups):
                seed = int(sampler.integers(256))
                slot = int(sampler.choice(slots)) if slots else None
                n_warm = int(sampler.integers(args.warmup_max + 1)) if args.warmup_max > 0 else 0
                warm = [int(x) for x in sampler.choice(WARMUP_ACTIONS, size=n_warm)]
                opt = {"seed": seed, "group": g, "warmup": warm}
                if slot is not None:
                    opt["slot"] = slot
                options += [dict(opt) for _ in range(G)]
            recs = run_rollouts(vec, policy, device, options)
            t_roll = time.time() - t0

            advs, collapsed = group_advantages(recs, groups, G, args.advantage)
            batch = {k: [] for k in ("obs", "priv", "act", "logp", "adv", "ret")}
            for r, a in zip(recs, advs):
                if a is None:
                    continue
                batch["obs"].extend(r["obs"])
                batch["priv"].extend(r["priv"])
                batch["act"].extend(r["act"])
                batch["logp"].extend(r["logp"])
                batch["adv"].extend(a.tolist())
                batch["ret"].extend(value_targets(r["rew"], r["tics"], args.gamma_tic).tolist())
            steps = sum(r["len"] for r in recs)
            tics = sum(r["rollout_tics"] for r in recs)
            total_steps += steps
            total_tics += tics

            upd = {}
            if batch["act"]:
                batch = {"obs": np.stack(batch["obs"]), "priv": np.array(batch["priv"], dtype=np.float32),
                         "act": np.array(batch["act"], dtype=np.int64),
                         "logp": np.array(batch["logp"], dtype=np.float32),
                         "adv": np.array(batch["adv"], dtype=np.float32),
                         "ret": np.array(batch["ret"], dtype=np.float32)}
                upd = update(policy, optimizer, ref, batch, args, device)
            t_all = time.time() - t0

            acts = np.bincount(np.concatenate([r["act"] for r in recs]), minlength=len(ACTIONS))
            m = {"iteration": it, "kind": "train", "steps": steps, "tics": tics,
                 "total_steps": total_steps, "total_tics": total_tics,
                 "return": float(np.mean([r["ret"] for r in recs])),
                 "return_std": float(np.std([r["ret"] for r in recs])),
                 "sectors": float(np.mean([r["sectors_gained"] for r in recs])),
                 "exit_rate": float(np.mean([r["exit"] for r in recs])),
                 "death_rate": float(np.mean([r["death"] for r in recs])),
                 "collapsed_share": collapsed / groups,
                 "sps": steps / t_roll, "rollout_s": t_roll, "iter_s": t_all,
                 "action_share": (acts / max(acts.sum(), 1)).round(3).tolist(), **upd}
            metrics_f.write(json.dumps(m) + "\n")
            metrics_f.flush()
            print(f"[it {it:5d}] ret {m['return']:7.2f} sectors {m['sectors']:5.2f} "
                  f"exit {m['exit_rate']:.2f} collapsed {m['collapsed_share']:.2f} "
                  f"steps {steps} ({m['sps']:.0f}/s) ent {upd.get('entropy', float('nan')):.3f} "
                  f"kl {upd.get('approx_kl', float('nan')):.4f} {t_all:.1f}s")

            save_checkpoint(run_dir, it, {
                "policy": policy.state_dict(), "optimizer": optimizer.state_dict(),
                "iteration": it, "total_steps": total_steps, "total_tics": total_tics,
                "rng": rng_state(sampler), "run_id": run_id, "ref": args.ref, "args": vars(args),
            }, args.keep)

            if eval_vec is not None and (it + 1) % args.eval_every == 0:
                evaluate(it + 1)
    except KeyboardInterrupt:
        print("[train] interrupted; the last checkpoint is at the previous update boundary")
    finally:
        metrics_f.close()
        vec.close()
        if eval_vec is not None:
            eval_vec.close()


if __name__ == "__main__":
    main()
