"""Rollouts for train.py (SPEC.md §7.1): run_rollouts plays every env of a
batch to its end with the policy, sample_group_options draws the group
starts (shared seed, slot and warm-up per group), rollout_log_rows /
write_rollout_log write the per-rollout debugging log, and
make_heldout_vec / heldout_eval / print_eval run the held-out maps."""

from __future__ import annotations

import contextlib
import json
import time
from pathlib import Path

import numpy as np
import torch
from torch.distributions import Categorical

from model import WARMUP_ACTIONS, WARMUP_HOLD, Policy, env_label, get_privileged_info, run_env
from env import ACTIONS, goal_dist  # noqa: E402  (model.py put env on sys.path)
from env.env import GUN_NAMES, N_MOVE  # noqa: E402
from env.vector import GroupVecEnv  # noqa: E402


# ---------------------------------------------------------------------------
# Rollouts (shared with train.py)
# ---------------------------------------------------------------------------

def nvtx(name: str):
    """An NVTX range for nsys timelines (`nsys stats --report nvtx_sum`);
    a no-op without CUDA."""
    return torch.cuda.nvtx.range(name) if torch.cuda.is_available() else contextlib.nullcontext()


def pick_actions(logits: torch.Tensor, temperature: float = 1.0) -> torch.Tensor:
    """Actions sampled at a temperature, one key at a time (the chain
    rule): move / USE / CONSULT, then the forward key, the strafe key
    given it, the turn key given both, the fire key given all three
    (env.ACTIONS: PRESS index 18*forward + 6*strafe + 2*turn + fire,
    then USE, CONSULT; 2026-11-18, before it no fire key), each factor
    sharpened to p^(1/T). At 1 that is the policy as trained; toward 0
    each key takes its most likely value, so a forward push split over 9
    PRESS combinations still wins over one larger turn; 0 (or less) is
    that limit."""
    def draw(q):
        if temperature <= 0:
            return q.argmax(dim=-1)
        return Categorical(logits=torch.log(q.clamp_min(1e-12)) / temperature).sample()
    p = torch.softmax(logits.float(), dim=-1)
    rows = torch.arange(p.shape[0], device=p.device)
    move = p[:, :N_MOVE].reshape(-1, 3, 3, 3, 2)
    kind = draw(torch.stack([move.sum((1, 2, 3, 4)), p[:, N_MOVE], p[:, N_MOVE + 1]], dim=-1))
    f = draw(move.sum((2, 3, 4)))
    s = draw(move[rows, f].sum((-1, -2)))
    t = draw(move[rows, f, s].sum(-1))
    c = draw(move[rows, f, s, t])
    return torch.where(kind == 0, 18 * f + 6 * s + 2 * t + c, N_MOVE - 1 + kind)


def run_rollouts(vec: GroupVecEnv, policy: Policy, device: torch.device,
                 options: list[dict], temperature: float = 1.0) -> list[dict]:
    """Reset every env with its options and run each to its end with
    sampled actions (at temperature, eval only: training samples at 1,
    which the logp it records assumes). Returns one record per env."""
    n = len(options)
    t0 = time.perf_counter()
    with nvtx("reset"):
        obs, infos = vec.reset(range(n), options)
    timing = {"reset_s": time.perf_counter() - t0, "env_s": 0.0, "policy_s": 0.0, "record_s": 0.0}
    t_start = time.time()
    cur_obs = obs.copy()
    cur_priv = np.array([get_privileged_info(i) for i in infos], dtype=np.float32)
    recs = [{"obs": [], "priv": [], "act": [], "logp": [], "rew": [], "rew_now": [], "tics": [],
             "start_sectors": infos[k]["state"].get("sectors", 0), "end_info": None,
             "start_state": infos[k]["state"], "wall_start": t_start, "terms": {},
             "step_terms": []}
            for k in range(n)]
    active = list(range(n))
    policy.eval()
    while active:
        t0 = time.perf_counter()
        with nvtx("policy"), torch.no_grad():
            o = torch.as_tensor(cur_obs[active], device=device)
            p = torch.as_tensor(cur_priv[active], device=device)
            logits, _ = policy(o, p)
            dist = Categorical(logits=logits)
            a = dist.sample() if temperature == 1.0 else pick_actions(logits, temperature)
            lp = dist.log_prob(a)
            a_np, lp_np = a.cpu().numpy(), lp.cpu().numpy()   # waits for the GPU
        t1 = time.perf_counter()
        with nvtx("env_step"):
            nobs, rew, term, trunc, ninfo = vec.step(active, a_np)
        t2 = time.perf_counter()
        still = []
        for k, i in enumerate(active):
            r = recs[i]
            r["obs"].append(cur_obs[i].copy())
            r["priv"].append(cur_priv[i].copy())
            r["act"].append(int(a_np[k]))
            r["logp"].append(float(lp_np[k]))
            r["rew"].append(float(rew[k]))
            r["rew_now"].append(float(ninfo[k].get("reward_now", 0.0)))   # its one-off part
            r["tics"].append(int(ninfo[k]["tics"]))
            for t, v in ninfo[k].get("terms", {}).items():
                r["terms"][t] = r["terms"].get(t, 0.0) + v
            r["step_terms"].append(dict(ninfo[k].get("terms", {})))   # the reward by term, per step
            # One-off rewards for an earlier step (2026-11-22: a projectile's
            # result, credited to the step that pulled the trigger).
            for back, name, v in ninfo[k].get("reward_back", []):
                j = len(r["rew"]) - 1 - int(back)
                if j >= 0:
                    r["rew"][j] += v
                    r["rew_now"][j] += v
                    r["step_terms"][j][name] = r["step_terms"][j].get(name, 0.0) + v
                    r["terms"][name] = r["terms"].get(name, 0.0) + v
            cur_obs[i] = nobs[k]
            cur_priv[i] = get_privileged_info(ninfo[k])
            if term[k] or trunc[k]:
                r["end_info"] = ninfo[k]
                r["terminated"] = bool(term[k])
                r["wall_end"] = time.time()
            else:
                still.append(i)
        active = still
        t3 = time.perf_counter()
        timing["policy_s"] += t1 - t0
        timing["env_s"] += t2 - t1
        timing["record_s"] += t3 - t2
    recs[0]["timing"] = timing    # rollout wall time by phase, for the train metrics
    for r in recs:
        s = r["end_info"]["state"]
        r["ret"] = float(sum(r["rew"]))
        r["len"] = len(r["rew"])
        r["rollout_tics"] = int(r["end_info"]["rollout_tics"])
        r["sectors_gained"] = int(s.get("sectors", 0)) - int(r["start_sectors"])
        r["exit"] = bool(r["end_info"].get("exit"))
        r["death"] = bool(s.get("dead")) or r["end_info"].get("deaths", 0) > 0   # a drill respawns (2026-11-22)
        r["deaths"] = int(r["end_info"].get("deaths", 0)) + bool(s.get("dead"))
        r["key"] = bool(r["end_info"].get("key"))
        r["items"] = list(r["end_info"].get("items", [0, 0]))   # practice items [spawned, picked up]
        r["waypoints"] = list(r["end_info"].get("waypoints", [0, 0]))   # [set, reached]
        r["combat"] = list(r["end_info"].get("combat", [0, 0, 0, 0, 0]))   # [on, shots, hits, kills, damage]
        r["drill"] = bool(r["end_info"].get("drill"))     # the combat drill: the action mask
        r["type"] = r["end_info"].get("type")              # env.TYPES, None untyped
        r["consult_stats"] = list(r["end_info"].get("consult_stats", [0, 0, 0, 0]))
        r["fire_stats"] = list(r["end_info"].get("fire_stats", [0, 0, 0, 0]))   # [steps seen, fire; unseen, fire]
        # r["aim_stats"] = ...   [steps on target, fire; off, fire] (until 2026-11-24)
        r["gun"] = r["end_info"].get("gun")      # weapontype_t at the end: the rollout's gun (2026-11-24)
        r["proj_stats"] = list(r["end_info"].get("proj_stats", [0, 0, 0]))      # [fired, hitting any, monsters hit]
    return recs


def _clock(t: float) -> str:
    """Local wall-clock time as the replay file names spell it."""
    return time.strftime("%Y%m%d_%H%M%S", time.localtime(t)) + f"_{int(t * 1000) % 1000:03d}"


def rollout_log_rows(recs: list[dict], options: list[dict], labels: list[str], **common) -> list[dict]:
    """One debugging row per rollout (rollouts.jsonl): where it started,
    how and where it ended, its return by reward term, and wall-clock
    times spelled like the replay file names, so a cut in a replay can be
    matched to its rollout. `common` (iteration, batch...) goes on every
    row."""
    rows = []
    for i, (r, o) in enumerate(zip(recs, options)):
        s0, e = r.get("start_state") or {}, r.get("end_info") or {}
        s1 = e.get("state") or {}
        acts = np.bincount(np.asarray(r["act"], dtype=np.int64), minlength=len(ACTIONS))
        rows.append({
            **common, "env": i, "group": o.get("group"), "map": labels[i],
            "start": "frontier" if o.get("frontier") else "map",
            "slot": e.get("slot", o.get("slot")), "seed": o.get("seed"), "warmup": len(o.get("warmup", [])),
            "end": e.get("end"), "tics": r.get("rollout_tics"), "max_tics": e.get("max_tics"),
            "steps": r.get("len"), "return": round(r.get("ret", 0.0), 3),
            "terms": {k: round(v, 3) for k, v in sorted(r.get("terms", {}).items())},
            "sectors": r.get("sectors_gained"),
            "hp": [s0.get("hp"), s1.get("hp")],
            "pos": [[s0.get("x"), s0.get("y"), s0.get("sector")], [s1.get("x"), s1.get("y"), s1.get("sector")]],
            "goal_dist": [goal_dist(s0), goal_dist(s1)],
            "mode": s1.get("mode"), "keys": s1.get("keys"), "items": r.get("items"),
            "waypoints": r.get("waypoints"),
            "combat": r.get("combat"), "type": r.get("type"), "fire": r.get("fire_stats"), "gun": GUN_NAMES.get(r.get("gun"), r.get("gun")),
            "proj": r.get("proj_stats"),
            "actions": dict(zip(ACTIONS, acts.tolist())),
            "wall_start": _clock(r["wall_start"]) if "wall_start" in r else None,
            "wall_end": _clock(r["wall_end"]) if "wall_end" in r else None,
        })
    return rows


def write_rollout_log(f, recs: list[dict], options: list[dict], labels: list[str], frontier=None,
                      **common) -> None:
    """Append the batch's rollout rows to the open file f, with what the
    frontier did for each row's map ("frontier": saved / exit / stale /
    kept, and "picked" on the rollout it saved or that exited)."""
    rows = rollout_log_rows(recs, options, labels, **common)
    if frontier is not None:
        for row, lab in zip(rows, frontier.labels):
            last = frontier.last.get(lab)
            if last:
                row["frontier"] = last["result"]
                if last.get("env") == row["env"]:
                    row["picked"] = True
                if "stale" in last:
                    row["stale"] = last["stale"]
    for row in rows:
        f.write(json.dumps(row) + "\n")
    f.flush()


def sample_group_options(sampler: np.random.Generator, groups: int, G: int,
                         slots: list[int], warmup_max: int) -> list[dict]:
    """Group starts: same slot, seed and warm-up for all G members."""
    options = []
    for g in range(groups):
        seed = int(sampler.integers(256))
        slot = int(sampler.choice(slots)) if slots else None
        n_warm = int(sampler.integers(warmup_max + 1)) if warmup_max > 0 else 0
        warm = [int(x) for x in sampler.choice(WARMUP_ACTIONS, size=n_warm) for _ in range(WARMUP_HOLD)]
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
    # Held out: the training env and rewards, with the held-out rollout
    # rules (model.run_env).
    vec = GroupVecEnv([run_env(args, slots, w, m, str(capture_root / f"eval{i:02d}") if capture_root else None,
                               held_out=True)
                       for i, (w, m) in enumerate(pairs)])
    return vec, labels


def heldout_eval(vec: GroupVecEnv, labels: list[str], policy: Policy, device: torch.device) -> dict:
    # Fixed starts (seed 0, no warm-up) and a fixed action-sampling seed,
    # so evals are comparable over time: the same weights play the same
    # games. The RNG is forked, so training's own RNG stream is untouched.
    with torch.random.fork_rng(devices=[device] if device.type == "cuda" else []):
        torch.manual_seed(0)
        # type maze (2026-11-20; untyped seed 0 drew the same: monsters hidden, no waypoints)
        recs = run_rollouts(vec, policy, device, [{"seed": 0, "group": -1, "warmup": [], "type": "maze"}
                                                 for _ in labels])
    per_map: dict[str, list[dict]] = {}
    for m, r in zip(labels, recs):
        per_map.setdefault(m, []).append(r)
    return {"eval_sectors": float(np.mean([r["sectors_gained"] for r in recs])),
            "eval_exit_rate": float(np.mean([r["exit"] for r in recs])),
            "eval_return": float(np.mean([r["ret"] for r in recs])),
            "eval_per_map": {m: float(np.mean([r["sectors_gained"] for r in rs]))
                             for m, rs in per_map.items()},
            "eval_per_map_exit": {m: float(np.mean([r["exit"] for r in rs]))
                                  for m, rs in per_map.items()},
            "eval_per_map_return": {m: float(np.mean([r["ret"] for r in rs]))
                                    for m, rs in per_map.items()}}


def eval_delta_lines(first: dict, last: dict) -> list[str]:
    """The change from one eval to another, per IWAD and map (sectors,
    exit rate, return) and overall."""
    def row(name, v0, v1):
        (s0, e0, r0), (s1, e1, r1) = v0, v1
        return (f"  {name:<{width}}  sectors {s0:5.2f} -> {s1:5.2f} ({s1 - s0:+.2f})  "
                f"exit {e0:.2f} -> {e1:.2f} ({e1 - e0:+.2f})  "
                f"return {r0:7.2f} -> {r1:7.2f} ({r1 - r0:+.2f})")

    def per_map(out, m):
        return (out["eval_per_map"].get(m, 0.0), out.get("eval_per_map_exit", {}).get(m, 0.0),
                out.get("eval_per_map_return", {}).get(m, 0.0))

    def overall(out):
        return out["eval_sectors"], out["eval_exit_rate"], out["eval_return"]
    maps = list(last["eval_per_map"])
    width = max(len(m) for m in maps + ["all"])
    lines = [f"[eval delta] iteration {first['iteration']} -> {last['iteration']}"]
    lines += [row(m, per_map(first, m), per_map(last, m)) for m in maps]
    lines.append(row("all", overall(first), overall(last)))
    return lines


def print_eval_delta(first: dict, last: dict) -> None:
    print("\n".join(eval_delta_lines(first, last)))


def print_eval(version: int, out: dict) -> None:
    print(f"[eval {version:5d}] sectors {out['eval_sectors']:.2f} exit {out['eval_exit_rate']:.2f} "
          f"return {out['eval_return']:.2f} per map {out['eval_per_map']}")


