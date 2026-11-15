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

This runs rollouts and updates in one process, strictly alternating
(--overlap plays the next batch during the update); eval.py has the
rollout and held-out eval pieces, learn.py the update.

Usage (from agent/training):
    .venv/bin/python train.py --run-dir runs/stage1                # stage 1
    .venv/bin/python train.py --run-dir runs/stage1                # resumes; 100 more updates each run (--more)
    .venv/bin/python train.py --stage 2 --run-dir runs/stage2 \\
        --init runs/stage1/ckpt_000500.pt
    .venv/bin/python train.py --iwads FREEDOOM1.WAD DOOM.WAD --groups 8   # two IWADs
"""

from __future__ import annotations

import argparse
import copy
import functools
import json
import signal
import threading
import time
from pathlib import Path

import numpy as np
import torch

from model import (Frontier, MapRotation, TicBudget, add_env_args, capture_dirs, check_slots, default_groups,
                   env_label, move_groups, resolve_maps,
                   export_latest_checkpoint, iwad_label, make_env, parse_list, resolve_iwads, rng_state,
                   save_checkpoint)
from env import cleanup_stale  # noqa: E402  (model.py put env on sys.path)
from env.vector import GroupVecEnv  # noqa: E402
from eval import (heldout_eval, make_heldout_vec, print_eval, run_rollouts, sample_group_options,
                  write_rollout_log)
from learn import (add_learn_args, build_batch, group_advantages, print_train, setup_learner,
                   train_metrics, update)


def parse_args():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--run-dir", default="runs/grpo")
    add_env_args(p)
    add_learn_args(p)
    p.set_defaults(iterations=None)
    p.add_argument("--more", type=int, default=100,
                   help="without --iterations: train this many updates past the newest checkpoint "
                        "(a fresh run: this many in all)")
    p.add_argument("--eval-every", type=int, default=10)
    p.add_argument("--overlap", action="store_true",
                   help="update on the GPU while the games play the next batch with the weights from "
                        "before the update (batches one version old; the clipped ratio allows for it)")
    p.add_argument("--capture-envs", type=int, default=0,
                   help="training envs per IWAD (the first of each) that record continuous replay "
                        "segments into <run-dir>/replays/env<i>_<IWAD>/ (default 0: none; "
                        "train.sh --capture sets 1)")
    p.add_argument("--capture-eval", action="store_true",
                   help="also record the eval envs (<run-dir>/replays/eval<i>/)")
    return p.parse_args()


def main():
    args = parse_args()
    run_dir = Path(args.run_dir)
    run_dir.mkdir(parents=True, exist_ok=True)
    device = torch.device(args.device)
    slots = [int(s) for s in args.slots.split(",") if s.strip()]
    iwads = resolve_iwads(args.iwads)
    eval_maps = parse_list(args.eval_maps)
    maps = resolve_maps(args.maps, iwads, args.stage, eval_maps)
    if args.groups is None:                 # one group per IWAD/map pair, at most 8
        args.groups = default_groups(iwads, maps)
    G, groups = args.group_size, args.groups
    n_envs = G * groups
    check_slots(iwads, slots)
    rotation = MapRotation(iwads, maps, groups, args.rotate_every)

    # SIGTERM (the execution environment stopping) -> clean shutdown;
    # the last checkpoint is at the previous update boundary.
    def on_sigterm(signum, frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, on_sigterm)
    cleanup_stale()

    sampler = np.random.default_rng(args.seed)
    policy, optimizer, ref, start_it, total_steps, total_tics, run_id = \
        setup_learner(args, run_dir, device, sampler)
    # --iterations is the run's total number of updates; without it, each
    # run of this script adds --more updates (100) to what's there.
    if args.iterations is None:
        args.iterations = start_it + args.more
    if start_it >= args.iterations:
        print(f"[train] {run_dir} already has {start_it} updates (--iterations {args.iterations}); "
              f"nothing to do: pass a higher --iterations, or leave it out to train --more ({args.more}) more")

    group_pairs = rotation.at(start_it)     # a resumed run picks up its rotation
    (run_dir / "config.json").write_text(json.dumps({**vars(args), "run_id": run_id}, indent=2))
    n_params = sum(p.numel() for p in policy.parameters())
    print(f"[train] run {run_id}: {n_envs} envs ({groups} groups x {G}), stage {args.stage}, "
          f"maps {maps}, eval {eval_maps}, {n_params / 1e6:.2f}M params, device {device}")
    print(f"[train] iwads {[iwad_label(w) for w in iwads]}; groups: "
          f"{[env_label(w, m, iwads) for w, m in group_pairs]}")
    if rotation.every:
        print(f"[train] {rotation.n_pairs} IWAD/map pairs, {groups} groups: they move on to the next "
              f"pairs every {rotation.every} batches (a full cycle is "
              f"{-(-rotation.n_pairs // groups) * rotation.every} batches)")

    env_pairs = [group_pairs[i // G] for i in range(n_envs)]
    replays = run_dir / "replays"
    captures = capture_dirs(env_pairs, args.capture_envs, replays)
    vec = GroupVecEnv([functools.partial(make_env, w, m, args.stage, args.skill, args.max_tics,
                                         args.slots_dir, slots, captures[i],
                                         spawn_tics=args.spawn_tics, nodraw=args.nodraw, noblit=args.noblit,
                                         portal_goal=args.portal_goal, time_penalty=args.time_penalty,
                                         force_map=args.force_map, force_map_tics=args.force_map_tics,
                                         key_goal=True)
                       for i, (w, m) in enumerate(env_pairs)])
    eval_vec = None
    if eval_maps and args.eval_every > 0:
        eval_vec, eval_labels = make_heldout_vec(iwads, eval_maps, args, slots,
                                                 replays if args.capture_eval else None)
    metrics_f = open(run_dir / "metrics.jsonl", "a")
    # One row per training rollout, for debugging (eval.rollout_log_rows).
    rollouts_f = open(run_dir / "rollouts.jsonl", "a")
    env_labels = [env_label(w, m, iwads) for w, m in env_pairs]

    def evaluate(it: int):
        out = {"iteration": it, "kind": "eval", **heldout_eval(eval_vec, eval_labels, policy, device)}
        metrics_f.write(json.dumps(out) + "\n")
        metrics_f.flush()
        print_eval(it, out)

    # --overlap: the games act with a frozen copy of the policy, refreshed
    # after each update, so the next batch can be played while the GPU
    # trains on the current one.
    actor = policy
    if args.overlap:
        actor = copy.deepcopy(policy)
        actor.requires_grad_(False)

    # Frontier starts: each map's best rollout end state so far.
    frontier = Frontier(run_dir / "frontier", args.frontier, env_pairs, patience=args.frontier_patience)
    frontier.install_all(vec)
    budget = TicBudget(env_pairs, args.max_tics, args.max_tics_cap, args.tics_growth)
    print(f"[train] rollout limit {args.max_tics} tics per map, growing x{args.tics_growth} after a batch "
          f"without progress, up to {budget.cap}")
    print("[train] forced map glance " + (f"every {args.force_map} steps, {args.force_map_tics} tics on screen"
                                           if args.force_map > 0 else "off"))

    def play(it: int) -> tuple[list[dict], float]:
        """The batch for update `it`."""
        nonlocal group_pairs, env_pairs, env_labels
        t = time.time()
        new = rotation.at(it)
        if new is not None:
            pairs = [new[i // G] for i in range(n_envs)]
            moved = move_groups(vec, frontier, budget, env_pairs, pairs)
            group_pairs, env_pairs = new, pairs
            env_labels = [env_label(w, m, iwads) for w, m in env_pairs]
            print(f"[train] rotation {rotation.rotation}: groups now play "
                  f"{[env_label(w, m, iwads) for w, m in group_pairs]} ({len(moved)} envs restart)")
        options = frontier.options(sample_group_options(sampler, groups, G, slots, args.warmup_max),
                                   G)
        budget.apply(vec)
        recs = run_rollouts(vec, actor, device, options)
        for i, r in enumerate(recs):
            r["iwad"] = iwad_label(env_pairs[i][0])
            r["frontier"] = bool(options[i].get("frontier"))
        # Before the next reset: every game is still where its rollout ended.
        saved = frontier.update(vec, recs, hold=budget.growing())
        recs[0]["frontier_saved"] = saved
        if rotation.every:
            recs[0]["groups"] = [env_label(w, m, iwads) for w, m in group_pairs]
        changed = budget.update(recs, cleared={k for k, v in saved.items() if v is None})
        if changed:
            print(f"[train] rollout limit (tics): {changed}")
        write_rollout_log(rollouts_f, recs, options, env_labels, frontier, iteration=it)
        return recs, time.time() - t

    try:
        if eval_vec is not None and start_it == 0:
            evaluate(0)  # baseline before any update
        ahead = None        # --overlap: the next batch, played during the last update
        for it in range(start_it, args.iterations):
            t0 = time.time()
            if ahead is None:
                (recs, t_roll), lag = play(it), 0
            else:
                (recs, t_roll), lag = ahead, 1
                ahead = None

            advs, collapsed = group_advantages([recs[g * G:(g + 1) * G] for g in range(groups)],
                                               args.advantage)
            batch = build_batch(recs, advs, args.gamma_tic)
            # What a checkpoint for this update must hold: the RNGs as they
            # are before the next batch is drawn and played, so a restart
            # (which discards a batch in flight) replays the same starts.
            rng = rng_state(sampler)
            if args.overlap and it + 1 < args.iterations:
                box: dict = {}

                def run_update():
                    try:
                        box["upd"] = update(policy, optimizer, ref, batch, args, device) if batch is not None else {}
                    except BaseException as e:      # re-raised on the main thread
                        box["err"] = e
                th = threading.Thread(target=run_update, daemon=True)
                th.start()
                ahead = play(it + 1)                # still the pre-update weights
                th.join()
                if "err" in box:
                    raise box["err"]
                upd = box["upd"]
                # The update used only the CPU generator (minibatch order);
                # the rollouts the CUDA one, so keep the update's CPU state.
                rng["torch"] = torch.get_rng_state()
            else:
                upd = update(policy, optimizer, ref, batch, args, device) if batch is not None else {}
                rng = rng_state(sampler)
            if actor is not policy:
                actor.load_state_dict(policy.state_dict())
            t_all = time.time() - t0

            m = train_metrics(recs, groups, collapsed)
            total_steps += m["steps"]
            total_tics += m["tics"]
            m = {"iteration": it, "kind": "train", **m,
                 "total_steps": total_steps, "total_tics": total_tics,
                 "sps": m["steps"] / t_roll, "rollout_s": t_roll, "iter_s": t_all, "lag": lag, **upd}
            metrics_f.write(json.dumps(m) + "\n")
            metrics_f.flush()
            print_train(it, m, upd, (f" fmap {args.force_map}" if args.force_map > 0 else "")
                        + f" ({m['sps']:.0f}/s) {t_all:.1f}s")

            save_checkpoint(run_dir / f"ckpt_{it:06d}.pt", {
                "policy": policy.state_dict(), "optimizer": optimizer.state_dict(),
                "iteration": it, "total_steps": total_steps, "total_tics": total_tics,
                "rng": rng, "run_id": run_id, "ref": args.ref, "args": vars(args),
            }, args.keep)

            if eval_vec is not None and (it + 1) % args.eval_every == 0:
                evaluate(it + 1)
    except KeyboardInterrupt:
        print("[train] interrupted; the last checkpoint is at the previous update boundary")
    finally:
        metrics_f.close()
        rollouts_f.close()
        vec.close()
        if eval_vec is not None:
            eval_vec.close()
        if args.outpath:
            dst = export_latest_checkpoint(run_dir, Path(args.outpath))
            print(f"[ckpt] final checkpoint copied to {dst}" if dst else "[ckpt] no checkpoint to copy")


if __name__ == "__main__":
    main()
