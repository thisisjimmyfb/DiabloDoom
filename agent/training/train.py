#!/usr/bin/env python3
"""GRPO trainer for the DiabloDoom level runner, single box (SPEC.md §6,
§7.1, §7.4).

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

This runs rollouts and updates in one process, strictly alternating.
To spread rollouts over several machines, run learn.py with one or more
eval.py workers instead (SPEC.md §7.5); they share this file's pieces.

Usage (from agent/training):
    .venv/bin/python train.py --run-dir runs/stage1                # stage 1
    .venv/bin/python train.py --run-dir runs/stage1                # resumes
    .venv/bin/python train.py --stage 2 --run-dir runs/stage2 \\
        --init runs/stage1/ckpt_000500.pt
    .venv/bin/python train.py --iwads FREEDOOM1.WAD DOOM.WAD --groups 8   # two IWADs
"""

from __future__ import annotations

import argparse
import functools
import json
import signal
import time
from pathlib import Path

import numpy as np
import torch

from model import (add_env_args, allocate_groups, check_slots, env_label, export_latest_checkpoint,
                   iwad_label, make_env, parse_list, resolve_iwads, rng_state, save_checkpoint)
from env import cleanup_stale  # noqa: E402  (model.py put env on sys.path)
from env.vector import GroupVecEnv  # noqa: E402
from eval import heldout_eval, make_heldout_vec, print_eval, run_rollouts, sample_group_options
from learn import (add_learn_args, build_batch, group_advantages, print_train, setup_learner,
                   train_metrics, update)


def parse_args():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--run-dir", default="runs/grpo")
    add_env_args(p)
    add_learn_args(p)
    p.add_argument("--eval-every", type=int, default=10)
    p.add_argument("--capture-envs", type=int, default=1,
                   help="training envs (from env 0) that record continuous replay segments "
                        "into <run-dir>/replays/env<i>/ (0: none)")
    p.add_argument("--capture-eval", action="store_true",
                   help="also record the eval envs (<run-dir>/replays/eval<i>/)")
    return p.parse_args()


def main():
    args = parse_args()
    run_dir = Path(args.run_dir)
    run_dir.mkdir(parents=True, exist_ok=True)
    device = torch.device(args.device)
    maps, eval_maps = parse_list(args.maps), parse_list(args.eval_maps)
    slots = [int(s) for s in args.slots.split(",") if s.strip()]
    G, groups = args.group_size, args.groups
    n_envs = G * groups
    iwads = resolve_iwads(args.iwads)
    check_slots(iwads, slots)
    group_pairs = allocate_groups(iwads, maps, groups)

    # SIGTERM (the execution environment stopping) -> clean shutdown;
    # the last checkpoint is at the previous update boundary.
    def on_sigterm(signum, frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, on_sigterm)
    cleanup_stale()

    sampler = np.random.default_rng(args.seed)
    policy, optimizer, ref, start_it, total_steps, total_tics, run_id = \
        setup_learner(args, run_dir, device, sampler)

    (run_dir / "config.json").write_text(json.dumps({**vars(args), "run_id": run_id}, indent=2))
    n_params = sum(p.numel() for p in policy.parameters())
    print(f"[train] run {run_id}: {n_envs} envs ({groups} groups x {G}), stage {args.stage}, "
          f"maps {maps}, eval {eval_maps}, {n_params / 1e6:.2f}M params, device {device}")
    print(f"[train] iwads {[iwad_label(w) for w in iwads]}; groups: "
          f"{[env_label(w, m, iwads) for w, m in group_pairs]}")

    env_pairs = [group_pairs[i // G] for i in range(n_envs)]
    replays = run_dir / "replays"
    vec = GroupVecEnv([functools.partial(make_env, w, m, args.stage, args.skill, args.max_tics,
                                         args.slots_dir, slots,
                                         str(replays / f"env{i:02d}") if i < args.capture_envs else None)
                       for i, (w, m) in enumerate(env_pairs)])
    eval_vec = None
    if eval_maps and args.eval_every > 0:
        eval_vec, eval_labels = make_heldout_vec(iwads, eval_maps, args, slots,
                                                 replays if args.capture_eval else None)
    metrics_f = open(run_dir / "metrics.jsonl", "a")

    def evaluate(it: int):
        out = {"iteration": it, "kind": "eval", **heldout_eval(eval_vec, eval_labels, policy, device)}
        metrics_f.write(json.dumps(out) + "\n")
        metrics_f.flush()
        print_eval(it, out)

    try:
        if eval_vec is not None and start_it == 0:
            evaluate(0)  # baseline before any update
        for it in range(start_it, args.iterations):
            t0 = time.time()
            recs = run_rollouts(vec, policy, device,
                                sample_group_options(sampler, groups, G, slots, args.warmup_max))
            t_roll = time.time() - t0
            for i, r in enumerate(recs):
                r["iwad"] = iwad_label(env_pairs[i][0])

            advs, collapsed = group_advantages([recs[g * G:(g + 1) * G] for g in range(groups)],
                                               args.advantage)
            batch = build_batch(recs, advs, args.gamma_tic)
            upd = update(policy, optimizer, ref, batch, args, device) if batch is not None else {}
            t_all = time.time() - t0

            m = train_metrics(recs, groups, collapsed)
            total_steps += m["steps"]
            total_tics += m["tics"]
            m = {"iteration": it, "kind": "train", **m,
                 "total_steps": total_steps, "total_tics": total_tics,
                 "sps": m["steps"] / t_roll, "rollout_s": t_roll, "iter_s": t_all, **upd}
            metrics_f.write(json.dumps(m) + "\n")
            metrics_f.flush()
            print_train(it, m, upd, f" ({m['sps']:.0f}/s) {t_all:.1f}s")

            save_checkpoint(run_dir / f"ckpt_{it:06d}.pt", {
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
        if args.outpath:
            dst = export_latest_checkpoint(run_dir, Path(args.outpath))
            print(f"[ckpt] final checkpoint copied to {dst}" if dst else "[ckpt] no checkpoint to copy")


if __name__ == "__main__":
    main()
