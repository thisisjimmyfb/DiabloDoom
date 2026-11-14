#!/usr/bin/env python3
"""Learner for the split trainer (SPEC.md §7.1, §7.4, §7.5).

Waits until the spool holds at least --min-groups fresh groups from the
rollout workers (eval.py), then takes every fresh batch there, computes
the group-normalized advantages

    A_i = (R_i - mean(R_group)) / (std(R_group) + eps)

(or, with --advantage process, normalized per-step rewards summed from
each step onward) and tic-discounted value targets, runs the clipped-ratio
update, checkpoints, and publishes the new weights for the workers.
Batches played with weights more than --max-lag versions old are
dropped. When --iterations updates are done it writes the spool's DONE
marker, and the workers exit.

Usage (from agent/training):
    .venv/bin/python learn.py --run-dir runs/stage1          # spool: runs/stage1/spool
    .venv/bin/python learn.py --run-dir runs/stage1          # resumes
"""

from __future__ import annotations

import argparse
import json
import os
import random
import signal
import time
import uuid
from pathlib import Path

import numpy as np
import torch
import torch.nn as nn
from torch.distributions import Categorical

from model import (ACTIONS, BATCH_DIR, DONE_FILE, Policy, export_latest_checkpoint,
                   load_latest_checkpoint, load_weights, publish_weights, read_weights, restore_rng,
                   rng_state, save_checkpoint, unpack_rollouts)


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
    for members in groups:
        if mode == "outcome":
            rets = np.array([m["ret"] for m in members], dtype=np.float64)
            std = rets.std()
            if std < 1e-6:
                collapsed += 1
                advs += [None] * len(members)
                continue
            advs += [np.full(m["len"], (rets[j] - rets.mean()) / (std + eps), dtype=np.float32)
                     for j, m in enumerate(members)]
        else:  # process: normalize per-step rewards across the group
            allr = np.concatenate([np.array(m["rew"], dtype=np.float64) for m in members])
            std = allr.std()
            if std < 1e-6:
                collapsed += 1
                advs += [None] * len(members)
                continue
            mean = allr.mean()
            for m in members:
                norm = (np.array(m["rew"], dtype=np.float64) - mean) / (std + eps)
                advs.append(np.cumsum(norm[::-1])[::-1].astype(np.float32))
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


def build_batch(recs: list[dict], advs: list[np.ndarray | None], gamma_tic: float) -> dict | None:
    """Training arrays from every rollout that has an advantage (collapsed
    groups contribute nothing). None if nothing is left."""
    keep = [(r, a) for r, a in zip(recs, advs) if a is not None]
    if not keep:
        return None
    return {"obs": np.concatenate([np.asarray(r["obs"], dtype=np.uint8) for r, _ in keep]),
            "priv": np.concatenate([np.asarray(r["priv"], dtype=np.float32) for r, _ in keep]),
            "act": np.concatenate([np.asarray(r["act"], dtype=np.int64) for r, _ in keep]),
            "logp": np.concatenate([np.asarray(r["logp"], dtype=np.float32) for r, _ in keep]),
            "adv": np.concatenate([a for _, a in keep]).astype(np.float32),
            "ret": np.concatenate([value_targets(r["rew"], r["tics"], gamma_tic) for r, _ in keep])}


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
         "collapsed_share": collapsed / max(n_groups, 1),
         "action_share": (acts / max(acts.sum(), 1)).round(3).tolist()}
    if any("frontier" in r for r in recs):
        m["frontier_share"] = float(np.mean([bool(r.get("frontier")) for r in recs]))
    if recs and recs[0].get("frontier_saved"):
        m["frontier_saved"] = recs[0]["frontier_saved"]
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
          + (f" front {m['frontier_share']:.2f}" if "frontier_share" in m else "") + extra)


def add_learn_args(p) -> None:
    """Optimizer and checkpoint arguments shared with train.py."""
    p.add_argument("--iterations", type=int, default=1000)
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
    p.add_argument("--outpath", default=None,
                   help="directory the final checkpoint is copied to when training ends "
                        "(checkpoints are saved to and resumed from --run-dir)")
    p.add_argument("--keep", type=int, default=3, help="checkpoints to keep")
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--device", default="cuda" if torch.cuda.is_available() else "cpu")


def setup_learner(args, run_dir: Path, device: torch.device, sampler: np.random.Generator | None = None,
                  spool: Path | None = None):
    """Policy, optimizer and reference policy, resumed from the newest
    checkpoint in run_dir, initialized from --init, or (learn.py, a fresh
    run) adopted from the spool's weights.pt: workers that started first
    published random weights there and played with them, so their batches
    stay valid. Returns (policy, optimizer, ref, start_it, total_steps,
    total_tics, run_id)."""
    policy = Policy().to(device)
    optimizer = torch.optim.Adam(policy.parameters(), lr=args.lr, eps=1e-5)
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
    elif spool is not None and (w := read_weights(spool)) is not None:
        policy.load_state_dict(w["policy"])
        if w["version"] == 0 and w.get("run_id"):
            # Version-0 weights from a worker: keep their run id, so the
            # batches already played with them are accepted.
            run_id = w["run_id"]
        print(f"[init] adopted the spool's weights (run {w.get('run_id')}, version {w['version']}) "
              f"as run {run_id}")
    ref = None
    if args.ref:
        ref = Policy().to(device)
        ref.load_state_dict(load_weights(args.ref))
        ref.eval()
        for prm in ref.parameters():
            prm.requires_grad_(False)
    return policy, optimizer, ref, start_it, total_steps, total_tics, run_id


# ---------------------------------------------------------------------------
# Spool intake
# ---------------------------------------------------------------------------

def collect(batch_dir: Path, version: int, args, stop_poll,
            run_id: str | None = None) -> tuple[list[list[dict]], dict]:
    """Block until at least --min-groups fresh groups are in the spool, then
    take every fresh batch there. Stale batches (too old, or played with
    another run's weights) are deleted after reading only their meta.
    Returns (groups, intake stats)."""
    taken: dict[Path, tuple[dict, object]] = {}
    n_groups, dropped = 0, 0
    while True:
        for path in sorted(batch_dir.glob("*.npz"), key=lambda p: p.stat().st_mtime):
            if path in taken:
                continue
            try:
                z = np.load(path, allow_pickle=False)
                meta = json.loads(str(z["meta"]))
            except Exception as e:  # half-synced copy: try again next poll
                print(f"[learn] cannot read {path.name}: {e}")
                continue
            other_run = run_id is not None and meta.get("run_id") not in (None, run_id)
            if other_run or meta["version"] < version - args.max_lag:
                dropped += 1
                z.close()
                path.unlink(missing_ok=True)
                continue
            taken[path] = (meta, z)
            n_groups += meta["groups"]
        if n_groups >= args.min_groups:
            break
        stop_poll()
    groups: list[list[dict]] = []
    actors, lags, rollout_s = set(), [], []
    for path, (meta, z) in taken.items():
        recs = unpack_rollouts(z, meta["iwads"])
        by_group: dict[int, list[dict]] = {}
        for r in recs:
            by_group.setdefault(r["group"], []).append(r)
        groups += list(by_group.values())
        actors.add(meta["actor"])
        lags.append(version - meta["version"])
        rollout_s.append(meta["rollout_s"])
        z.close()
        path.unlink(missing_ok=True)
    return groups, {"batches": len(taken), "actors": sorted(actors), "dropped_batches": dropped,
                    "max_lag": max(lags), "mean_lag": float(np.mean(lags)),
                    "rollout_s": float(np.mean(rollout_s))}


def parse_args():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--run-dir", default="runs/grpo")
    p.add_argument("--spool", default=None, help="spool directory (default: <run-dir>/spool)")
    p.add_argument("--min-groups", type=int, default=4, help="groups needed before an update")
    p.add_argument("--max-lag", type=int, default=2,
                   help="drop batches played with weights more than this many versions old")
    p.add_argument("--poll", type=float, default=1.0, help="seconds between spool checks")
    add_learn_args(p)
    return p.parse_args()


def main():
    args = parse_args()
    run_dir = Path(args.run_dir)
    run_dir.mkdir(parents=True, exist_ok=True)
    spool = Path(args.spool) if args.spool else run_dir / "spool"
    batch_dir = spool / BATCH_DIR
    batch_dir.mkdir(parents=True, exist_ok=True)
    (spool / DONE_FILE).unlink(missing_ok=True)
    for tmp in batch_dir.glob("*.tmp"):       # a worker died mid-write
        tmp.unlink(missing_ok=True)
    device = torch.device(args.device)
    if device.type == "cpu":
        # A CPU learner usually shares its machine with rollout workers
        # (a Muse VM has no GPU): PyTorch would take every core and starve
        # the games, so the update gets half of them.
        cores = len(os.sched_getaffinity(0))
        torch.set_num_threads(max(1, cores // 2))
        print(f"[learn] CPU update: {torch.get_num_threads()} of {cores} cores (the rest for the games)")

    def on_sigterm(signum, frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, on_sigterm)

    policy, optimizer, ref, start_it, total_steps, total_tics, run_id = \
        setup_learner(args, run_dir, device, spool=spool)
    (run_dir / "config.json").write_text(json.dumps({**vars(args), "run_id": run_id}, indent=2))
    n_params = sum(p.numel() for p in policy.parameters())
    # Version = updates done; the workers play with the newest one.
    version = start_it
    publish_weights(spool, version, policy, run_id)
    print(f"[learn] run {run_id}: spool {spool}, min {args.min_groups} groups, max lag {args.max_lag}, "
          f"{n_params / 1e6:.2f}M params, device {device}; published weights v{version}")

    metrics_f = open(run_dir / "metrics.jsonl", "a")
    finished = False
    try:
        for it in range(start_it, args.iterations):
            t_wait = time.time()
            groups, intake = collect(batch_dir, version, args, lambda: time.sleep(args.poll), run_id)
            t0 = time.time()
            recs = [r for grp in groups for r in grp]
            advs, collapsed = group_advantages(groups, args.advantage)
            batch = build_batch(recs, advs, args.gamma_tic)
            upd = update(policy, optimizer, ref, batch, args, device) if batch is not None else {}
            m = train_metrics(recs, len(groups), collapsed)
            total_steps += m["steps"]
            total_tics += m["tics"]
            m = {"iteration": it, "kind": "train", "version": version, **m,
                 "total_steps": total_steps, "total_tics": total_tics, "groups": len(groups),
                 **intake, "wait_s": t0 - t_wait, "update_s": time.time() - t0, **upd}
            metrics_f.write(json.dumps(m) + "\n")
            metrics_f.flush()
            print_train(it, m, upd, f" | {len(groups)} groups from {intake['actors']} lag "
                                    f"{intake['max_lag']} dropped {intake['dropped_batches']} "
                                    f"wait {m['wait_s']:.1f}s update {m['update_s']:.1f}s")

            save_checkpoint(run_dir / f"ckpt_{it:06d}.pt", {
                "policy": policy.state_dict(), "optimizer": optimizer.state_dict(),
                "iteration": it, "total_steps": total_steps, "total_tics": total_tics,
                "rng": rng_state(), "run_id": run_id, "ref": args.ref, "args": vars(args),
            }, args.keep)
            version = it + 1
            publish_weights(spool, version, policy, run_id)
        finished = True
    except KeyboardInterrupt:
        print("[learn] interrupted; the last checkpoint is at the previous update boundary")
    finally:
        metrics_f.close()
        if finished:
            (spool / DONE_FILE).touch()
            print("[learn] done; workers will exit")
        if args.outpath:
            dst = export_latest_checkpoint(run_dir, Path(args.outpath))
            print(f"[ckpt] final checkpoint copied to {dst}" if dst else "[ckpt] no checkpoint to copy")


if __name__ == "__main__":
    main()
