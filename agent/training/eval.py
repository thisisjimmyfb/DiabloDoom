#!/usr/bin/env python3
"""Rollout worker for the split trainer (SPEC.md §7.5).

Plays GRPO groups with the newest weights the learner has published in
the spool and writes each finished set of groups there as one batch file
for learn.py. Any machine can run it, CPU or GPU, and several workers can
feed one learner, each with its own --actor-id. Workers keep no state of
their own: on restart they pull the newest weights and start new groups.

With --heldout it instead plays the held-out maps from fixed starts once
per new weights version and appends the results to <run-dir>/eval.jsonl;
it never writes training data.

Usage (from agent/training; the learner's spool, or a synced copy of it):
    .venv/bin/python eval.py --spool runs/stage1/spool --actor-id vm0 --device cpu
    .venv/bin/python eval.py --spool runs/stage1/spool --heldout --run-dir runs/stage1
"""

from __future__ import annotations

import argparse
import functools
import json
import os
import signal
import socket
import time
import zlib
from pathlib import Path

import numpy as np
import torch
from torch.distributions import Categorical

from model import (BATCH_DIR, DONE_FILE, WARMUP_ACTIONS, Policy, add_env_args, allocate_groups,
                   check_slots, env_label, iwad_label, make_env, pack_rollouts, parse_list,
                   privileged, read_weights, resolve_iwads)
from env import cleanup_stale  # noqa: E402  (model.py put env on sys.path)
from env.vector import GroupVecEnv  # noqa: E402


# ---------------------------------------------------------------------------
# Rollouts (shared with train.py)
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


def sample_group_options(sampler: np.random.Generator, groups: int, G: int,
                         slots: list[int], warmup_max: int) -> list[dict]:
    """Group starts: same slot, seed and warm-up for all G members."""
    options = []
    for g in range(groups):
        seed = int(sampler.integers(256))
        slot = int(sampler.choice(slots)) if slots else None
        n_warm = int(sampler.integers(warmup_max + 1)) if warmup_max > 0 else 0
        warm = [int(x) for x in sampler.choice(WARMUP_ACTIONS, size=n_warm)]
        opt = {"seed": seed, "group": g, "warmup": warm}
        if slot is not None:
            opt["slot"] = slot
        options += [dict(opt) for _ in range(G)]
    return options


def make_heldout_vec(iwads, eval_maps, args, slots, capture_root: Path | None):
    """Every eval map on every IWAD, --eval-episodes times. Returns
    (vec, per-env labels)."""
    pairs = [(w, m) for w in iwads for m in eval_maps for _ in range(args.eval_episodes)]
    labels = [env_label(w, m, iwads) for w, m in pairs]
    vec = GroupVecEnv([functools.partial(make_env, w, m, args.stage, args.skill, args.max_tics,
                                         args.slots_dir, slots,
                                         str(capture_root / f"eval{i:02d}") if capture_root else None)
                       for i, (w, m) in enumerate(pairs)])
    return vec, labels


def heldout_eval(vec: GroupVecEnv, labels: list[str], policy: Policy, device: torch.device) -> dict:
    # Fixed starts (seed 0, no warm-up) so evals are comparable over time.
    recs = run_rollouts(vec, policy, device, [{"seed": 0, "group": -1, "warmup": []} for _ in labels])
    per_map: dict[str, list[dict]] = {}
    for m, r in zip(labels, recs):
        per_map.setdefault(m, []).append(r)
    return {"eval_sectors": float(np.mean([r["sectors_gained"] for r in recs])),
            "eval_exit_rate": float(np.mean([r["exit"] for r in recs])),
            "eval_return": float(np.mean([r["ret"] for r in recs])),
            "eval_per_map": {m: float(np.mean([r["sectors_gained"] for r in rs]))
                             for m, rs in per_map.items()}}


def print_eval(version: int, out: dict) -> None:
    print(f"[eval {version:5d}] sectors {out['eval_sectors']:.2f} exit {out['eval_exit_rate']:.2f} "
          f"return {out['eval_return']:.2f} per map {out['eval_per_map']}")


# ---------------------------------------------------------------------------
# Worker
# ---------------------------------------------------------------------------

def parse_args():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--spool", required=True, help="the learner's spool directory (or a synced copy)")
    p.add_argument("--actor-id", default=f"{socket.gethostname()}-{os.getpid()}".replace("_", "-"),
                   help="names this worker's batch files; unique per worker, no '_' or '/'")
    add_env_args(p)
    p.add_argument("--max-pending", type=int, default=2,
                   help="pause while this many of this worker's batches are unconsumed")
    p.add_argument("--capture-envs", type=int, default=0,
                   help="envs (from env 0) that record continuous replay into --capture-dir/env<i>/")
    p.add_argument("--capture-dir", default=None, help="replay directory (default: <spool>/replays/<actor-id>)")
    p.add_argument("--heldout", action="store_true",
                   help="evaluate each new weights version on --eval-maps instead of producing batches")
    p.add_argument("--run-dir", default=None, help="--heldout: where eval.jsonl goes (default: the spool's parent)")
    p.add_argument("--capture-eval", action="store_true", help="--heldout: record the eval envs")
    p.add_argument("--poll", type=float, default=1.0, help="seconds between spool checks")
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--device", default="cuda" if torch.cuda.is_available() else "cpu")
    return p.parse_args()


class Stop(Exception):
    pass


def main():
    args = parse_args()
    if "_" in args.actor_id or "/" in args.actor_id:
        # Batch files are <actor>_<time>.npz; a '_' in the id would let one
        # worker count another's files as its own pending batches.
        raise SystemExit(f"--actor-id can't contain '_' or '/': {args.actor_id}")
    spool = Path(args.spool)
    batch_dir = spool / BATCH_DIR
    device = torch.device(args.device)
    maps, eval_maps = parse_list(args.maps), parse_list(args.eval_maps)
    slots = [int(s) for s in args.slots.split(",") if s.strip()]
    iwads = resolve_iwads(args.iwads)
    check_slots(iwads, slots)
    tag = "heldout" if args.heldout else f"actor {args.actor_id}"

    def on_sigterm(signum, frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, on_sigterm)
    cleanup_stale()

    def wait(cond) -> None:
        """Sleep until cond() is true; Stop when the learner is done."""
        while not cond():
            if (spool / DONE_FILE).exists():
                raise Stop
            time.sleep(args.poll)

    policy = Policy().to(device)
    version = -1

    def wait_for_weights(newer_than: int) -> None:
        nonlocal version
        holder = {}

        def got() -> bool:
            w = read_weights(spool)
            if w is not None and w["version"] > newer_than:
                holder["w"] = w
                return True
            return False
        wait(got)
        policy.load_state_dict(holder["w"]["policy"])
        version = holder["w"]["version"]

    def refresh_weights() -> None:
        """Load newer weights if the learner published some; never waits."""
        nonlocal version
        w = read_weights(spool)
        if w is not None and w["version"] > version:
            policy.load_state_dict(w["policy"])
            version = w["version"]

    vec = None
    try:
        if args.heldout:
            run_dir = Path(args.run_dir) if args.run_dir else spool.parent
            run_dir.mkdir(parents=True, exist_ok=True)
            if not eval_maps:
                raise SystemExit("--heldout needs --eval-maps")
            vec, labels = make_heldout_vec(iwads, eval_maps, args, slots,
                                           run_dir / "replays" if args.capture_eval else None)
            print(f"[{tag}] {len(labels)} envs: {sorted(set(labels))}, device {device}")
            with open(run_dir / "eval.jsonl", "a") as f:
                while True:
                    wait_for_weights(version)
                    out = {"iteration": version, "kind": "eval", **heldout_eval(vec, labels, policy, device)}
                    f.write(json.dumps(out) + "\n")
                    f.flush()
                    print_eval(version, out)

        G, groups = args.group_size, args.groups
        group_pairs = allocate_groups(iwads, maps, groups, tag)
        env_pairs = [group_pairs[i // G] for i in range(G * groups)]
        iwad_labels = [iwad_label(w) for w in iwads]
        ro_group = [i // G for i in range(G * groups)]
        ro_iwad = [iwads.index(w) for w, _ in env_pairs]
        capture = Path(args.capture_dir) if args.capture_dir else spool / "replays" / args.actor_id
        vec = GroupVecEnv([functools.partial(make_env, w, m, args.stage, args.skill, args.max_tics,
                                             args.slots_dir, slots,
                                             str(capture / f"env{i:02d}") if i < args.capture_envs else None)
                           for i, (w, m) in enumerate(env_pairs)])
        # Workers keep no state, so a restart must not replay the same
        # starts: the sampler mixes in the actor id and the start time.
        sampler = np.random.default_rng([args.seed, zlib.crc32(args.actor_id.encode()), time.time_ns()])
        print(f"[{tag}] {G * groups} envs ({groups} groups x {G}), stage {args.stage}, device {device}; "
              f"groups: {[env_label(w, m, iwads) for w, m in group_pairs]}")

        wait_for_weights(-1)
        print(f"[{tag}] weights version {version}")
        n = 0
        while True:
            batch_dir.mkdir(parents=True, exist_ok=True)
            wait(lambda: len(list(batch_dir.glob(f"{args.actor_id}_*.npz"))) < args.max_pending)
            refresh_weights()
            played_with = version
            t0 = time.time()
            recs = run_rollouts(vec, policy, device,
                                sample_group_options(sampler, groups, G, slots, args.warmup_max))
            t_roll = time.time() - t0
            arrays = pack_rollouts(recs, ro_group, ro_iwad)
            meta = {"actor": args.actor_id, "version": played_with, "groups": groups, "group_size": G,
                    "iwads": iwad_labels, "group_labels": [env_label(w, m, iwads) for w, m in group_pairs],
                    "rollout_s": t_roll, "created": time.time()}
            path = write_batch(batch_dir, args.actor_id, arrays, meta)
            n += 1
            steps = int(arrays["ro_len"].sum())
            print(f"[{tag}] batch {n} v{played_with}: {steps} steps ({steps / t_roll:.0f}/s), "
                  f"{arrays['frames'].shape[0]} frames, {path.stat().st_size / 1e6:.1f} MB, {t_roll:.1f}s")
    except Stop:
        print(f"[{tag}] learner is done; exiting")
    except KeyboardInterrupt:
        print(f"[{tag}] interrupted")
    finally:
        if vec is not None:
            vec.close()


def write_batch(batch_dir: Path, actor_id: str, arrays: dict, meta: dict) -> Path:
    """Atomic: the learner only ever sees complete .npz files."""
    path = batch_dir / f"{actor_id}_{time.time_ns()}.npz"
    tmp = path.with_name(path.name + ".tmp")
    with open(tmp, "wb") as f:
        np.savez_compressed(f, meta=np.array(json.dumps(meta)), **arrays)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)
    return path


if __name__ == "__main__":
    main()
