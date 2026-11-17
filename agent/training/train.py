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
(--overlap plays the next batch during the update); rollout.py has the
rollout and held-out eval pieces, learn.py the update.

Usage (from agent/training):
    .venv/bin/python train.py --run-dir runs/stage1                # stage 1
    .venv/bin/python train.py --run-dir runs/stage1                # resumes; 100 more updates each run (--more)
    .venv/bin/python train.py --stage 2 --run-dir runs/stage2 \\
        --init runs/stage1/doom-runner.pt
    .venv/bin/python train.py --iwads FREEDOOM1.WAD DOOM.WAD --groups 8   # two IWADs
    .venv/bin/python train.py --run-dir runs/stage1 --hub-repo user/name  # resume from / push to the Hub
"""

from __future__ import annotations

import argparse
import copy
import json
import secrets
import signal
import sys
import threading
import time
from pathlib import Path

import numpy as np
import torch

from model import (Frontier, HubUploader, MapRotation, TicBudget, add_env_args, capture_dirs, check_slots,
                   FREE_CORES, available_cores, default_groups, env_label, hub_pull, move_groups,
                   pick_eval_maps, resolve_maps,
                   iwad_label, run_env, resolve_iwads, rng_state,
                   save_checkpoint, CKPT_NAME)
from env import cleanup_stale  # noqa: E402  (model.py put env on sys.path)
from env.vector import GroupVecEnv  # noqa: E402
from rollout import (heldout_eval, make_heldout_vec, nvtx, print_eval, print_eval_delta, run_rollouts, sample_group_options,
                  write_rollout_log)
from learn import (GRAD_WINDOW, add_learn_args, build_batch, group_advantages, print_train, setup_learner,
                   train_metrics, training_health, update)


def parse_args():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--run-dir", default="runs/grpo")
    add_env_args(p)
    add_learn_args(p)
    p.set_defaults(iterations=None)
    p.add_argument("--more", type=int, default=100,
                   help="without --iterations: train this many updates past the newest checkpoint "
                        "(a fresh run: this many in all)")
    p.add_argument("--eval-every", type=int, default=0,
                   help="also a held-out eval every N updates; 0 (default): only at the start and end of "
                        "the run (--eval-maps '' turns evals off; nothing in training reads them)")
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


def format_duration(seconds: float) -> str:
    """A wall-clock duration as h:mm:ss (days added past 24 hours)."""
    s = int(round(seconds))
    d, s = divmod(s, 86400)
    h, s = divmod(s, 3600)
    m, s = divmod(s, 60)
    return (f"{d}d " if d else "") + f"{h}:{m:02d}:{s:02d}"


def main():
    t_start = time.time()
    args = parse_args()
    run_dir = Path(args.run_dir)
    run_dir.mkdir(parents=True, exist_ok=True)
    device = torch.device(args.device)
    slots = [int(s) for s in args.slots.split(",") if s.strip()]
    iwads = resolve_iwads(args.iwads)
    # The session's map choices: a fresh seed each session unless
    # --map-seed fixes it (saved in config.json below, so it can be rerun).
    if args.map_seed is None:
        args.map_seed = secrets.randbelow(2 ** 31)
    map_rng = np.random.default_rng(args.map_seed)
    eval_maps = pick_eval_maps(args.eval_maps, args.eval_count, args.maps, iwads, args.stage, map_rng)
    maps = resolve_maps(args.maps, iwads, args.stage, eval_maps)
    if args.groups is None:                 # one group per IWAD/map pair, at most cores - 2
        args.groups = default_groups(iwads, maps)
        print(f"[train] {args.groups} groups: one per IWAD/map pair, at most the "
              f"{available_cores()} logical cores minus {FREE_CORES}")
    G, groups = args.group_size, args.groups
    n_envs = G * groups
    check_slots(iwads, slots)
    rotation = MapRotation(iwads, maps, groups, args.rotate_every, rng=map_rng)

    # SIGTERM (the execution environment stopping) -> clean shutdown;
    # the last checkpoint is at the previous update boundary.
    # Only the first one: a second (nsys and supervisors resend it) would
    # interrupt the shutdown itself and leave the games running.
    def on_sigterm(signum, frame):
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, on_sigterm)
    cleanup_stale()

    # --hub-repo: the repo's checkpoint joins run_dir's, so the newest of
    # the two is resumed below.
    if args.hub_repo and args.hub_download:
        hub_pull(args.hub_repo, run_dir)
    elif args.hub_repo:
        print(f"[hub] --no-hub-download: not pulling from {args.hub_repo}")

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
    hub = HubUploader(args.hub_repo, run_dir) if args.hub_repo and args.hub_upload else None
    if args.hub_repo and not args.hub_upload:
        print(f"[hub] --no-hub-upload: not pushing to {args.hub_repo}")
    saved_it = None                         # the last iteration saved by this run
    (run_dir / "config.json").write_text(json.dumps({**vars(args), "run_id": run_id}, indent=2))
    n_params = sum(p.numel() for p in policy.parameters())
    print(f"[train] run {run_id}: {n_envs} envs ({groups} groups x {G}), stage {args.stage}, "
          f"maps {maps}, eval {eval_maps}, {n_params / 1e6:.2f}M params, device {device}, "
          f"map seed {args.map_seed}")
    print(f"[train] iwads {[iwad_label(w) for w in iwads]}; groups: "
          f"{[env_label(w, m, iwads) for w, m in group_pairs]}")
    if rotation.every:
        print(f"[train] {rotation.n_pairs} IWAD/map pairs, {groups} groups: they move on to the next "
              f"pairs every {rotation.every} batches (a full cycle is "
              f"{-(-rotation.n_pairs // groups) * rotation.every} batches)")

    env_pairs = [group_pairs[i // G] for i in range(n_envs)]
    replays = run_dir / "replays"
    captures = capture_dirs(env_pairs, args.capture_envs, replays)
    vec = GroupVecEnv([run_env(args, slots, w, m, captures[i]) for i, (w, m) in enumerate(env_pairs)])
    eval_vec = None
    if eval_maps and start_it < args.iterations:
        eval_vec, eval_labels = make_heldout_vec(iwads, eval_maps, args, slots,
                                                 replays if args.capture_eval else None)
    metrics_f = open(run_dir / "metrics.jsonl", "a")
    # One row per training rollout, for debugging (rollout.rollout_log_rows).
    rollouts_f = open(run_dir / "rollouts.jsonl", "a")
    env_labels = [env_label(w, m, iwads) for w, m in env_pairs]

    eval_busy = False   # an eval was interrupted: its games still have requests in flight

    def evaluate(it: int):
        nonlocal eval_busy
        eval_busy = True
        out = {"iteration": it, "kind": "eval", **heldout_eval(eval_vec, eval_labels, policy, device)}
        eval_busy = False
        metrics_f.write(json.dumps(out) + "\n")
        metrics_f.flush()
        print_eval(it, out)
        return out

    # --overlap: the games act with a frozen copy of the policy, refreshed
    # after each update, so the next batch can be played while the GPU
    # trains on the current one.
    actor = policy
    if args.overlap:
        actor = copy.deepcopy(policy)
        actor.requires_grad_(False)

    # Frontier starts: each map's best rollout end state so far.
    frontier = Frontier(args.frontier, env_pairs, patience=args.frontier_patience)
    budget = TicBudget(env_pairs, args.max_tics, args.max_tics_cap, args.tics_growth)
    print(f"[train] rollout limit {args.max_tics} tics per map, growing x{args.tics_growth} after a batch "
          f"without progress, up to {budget.cap}")

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
        with nvtx("rollout"):
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

    # Held-out evals: one at the start of the run and one at the end, whose
    # difference is printed per IWAD and map; --eval-every N adds more.
    first_eval = last_eval = None
    upd_thread = None   # --overlap: the update running in the background
    unhealthy = None    # why training stopped (learn.training_health): no save, no upload
    grad_recent: list[float] = []   # healthy updates' grad_norm_max (the relative check)

    def end_of_run_eval(last: dict, done_it: int) -> dict:
        """The eval after done_it updates (unless one just ran) and the
        start-to-end delta."""
        if last["iteration"] != done_it:
            last = evaluate(done_it)
        print_eval_delta(first_eval, last)
        return last

    def timed_update(batch) -> dict:
        """One update, its wall time in upd["update_s"] (NVTX "update")."""
        t = time.perf_counter()
        with nvtx("update"):
            upd = update(policy, optimizer, ref, batch, args, device) if batch is not None else {}
        return {**upd, "update_s": time.perf_counter() - t}

    try:
        if eval_vec is not None:
            first_eval = last_eval = evaluate(start_it)
        ahead = None        # --overlap: the next batch, played during the last update
        for it in range(start_it, args.iterations):
            t0 = time.time()
            if ahead is None:
                (recs, t_roll), lag = play(it), 0
            else:
                (recs, t_roll), lag = ahead, 1
                ahead = None

            with nvtx("batch"):
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
                        box["upd"] = timed_update(batch)
                    except BaseException as e:      # re-raised on the main thread
                        box["err"] = e
                th = upd_thread = threading.Thread(target=run_update, daemon=True)
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
                upd = timed_update(batch)
                rng = rng_state(sampler)
            if actor is not policy:
                actor.load_state_dict(policy.state_dict())
            t_all = time.time() - t0

            m = train_metrics(recs, groups, collapsed)
            total_steps += m["steps"]
            total_tics += m["tics"]
            # Rollout wall time by phase (rollout.run_rollouts): env steps (the
            # workers and games), policy forward passes, resets, bookkeeping.
            timing = recs[0].get("timing", {})
            m = {"iteration": it, "kind": "train", **m,
                 "total_steps": total_steps, "total_tics": total_tics,
                 "sps": m["steps"] / t_roll, "rollout_s": t_roll, "iter_s": t_all, "lag": lag,
                 **timing, **upd}
            unhealthy = training_health(upd, args, grad_recent)
            if unhealthy:
                m["stopped"] = unhealthy
            elif upd.get("grad_norm_max") is not None:
                grad_recent = (grad_recent + [upd["grad_norm_max"]])[-GRAD_WINDOW:]
            metrics_f.write(json.dumps(m) + "\n")
            metrics_f.flush()
            print_train(it, m, upd, f" ({m['sps']:.0f}/s) {t_all:.1f}s"
                        + (f" [env {timing['env_s']:.1f}s policy {timing['policy_s']:.1f}s "
                           f"update {upd.get('update_s', 0.0):.1f}s]" if timing else ""))
            if unhealthy:
                # This update's weights are never saved, and nothing more is
                # uploaded (an upload already running, of an earlier
                # checkpoint that passed this check, finishes).
                print(f"[train] stopping: {unhealthy}. Iteration {it} is not saved and nothing is "
                      f"uploaded; the last checkpoint is "
                      + (f"iteration {saved_it}" if saved_it is not None else "from before this run"))
                break

            ckpt_path = run_dir / CKPT_NAME
            save_checkpoint(ckpt_path, {
                "policy": policy.state_dict(), "optimizer": optimizer.state_dict(),
                "iteration": it, "total_steps": total_steps, "total_tics": total_tics,
                "rng": rng, "run_id": run_id, "ref": args.ref, "args": vars(args),
            })
            saved_it = it
            if hub is not None and args.hub_every > 0 and (it + 1) % args.hub_every == 0:
                if not hub.push(ckpt_path, it):
                    print(f"[hub] iteration {hub.it} is still uploading; skipped iteration {it}")

            if eval_vec is not None and args.eval_every > 0 and (it + 1) % args.eval_every == 0:
                last_eval = evaluate(it + 1)
        if eval_vec is not None and saved_it is not None and not unhealthy:
            last_eval = end_of_run_eval(last_eval, saved_it + 1)
    except KeyboardInterrupt:
        # Ctrl-C (or SIGTERM): still the end-of-run eval, of the last saved
        # checkpoint -- the policy in memory may be part-way through an
        # update, so its weights are reloaded from that checkpoint first.
        msg = "[train] interrupted; the last checkpoint is at the previous update boundary"
        if eval_vec is None:
            print(msg)
        elif eval_busy or first_eval is None:
            print(msg + "; no end-of-run eval (interrupted during an eval)")
        elif saved_it is None:
            print(msg + "; no end-of-run eval (no update finished)")
        else:
            print(msg + f"; end-of-run eval of that checkpoint (iteration {saved_it}; Ctrl-C again to skip it)")
            try:
                if upd_thread is not None:
                    upd_thread.join()       # --overlap: let a background update finish first
                ckpt = torch.load(run_dir / CKPT_NAME, map_location=device, weights_only=False)
                policy.load_state_dict(ckpt["policy"])
                # the checkpoint's own iteration: an interrupt between its
                # write and saved_it's update would leave saved_it behind
                last_eval = end_of_run_eval(last_eval, ckpt["iteration"] + 1)
            except KeyboardInterrupt:
                print("[train] end-of-run eval skipped")
            except Exception as e:          # e.g. the games were stopped too (SIGTERM to the group)
                print(f"[train] end-of-run eval failed: {e!r}")
    finally:
        metrics_f.close()
        rollouts_f.close()
        vec.close()
        if eval_vec is not None:
            eval_vec.close()
        if hub is not None:
            if unhealthy:
                hub.finish(None, None)      # wait for a running upload; start none
            else:
                hub.finish(run_dir / CKPT_NAME if saved_it is not None else None, saved_it)
        done = 0 if saved_it is None else saved_it + 1 - start_it
        print(f"[train] run took {format_duration(time.time() - t_start)} ({done} update{'' if done == 1 else 's'} this run)")
    if unhealthy:
        sys.exit(1)


if __name__ == "__main__":
    main()
