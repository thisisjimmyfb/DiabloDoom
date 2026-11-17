#!/usr/bin/env python3
"""Evaluate a trained policy, printed to the console. Two modes:

Both modes always run without map monsters or turn-based combat (stage 1, --eval).

Training-style eval (the default). Every training IWAD/map pair (--iwads x
--maps, as train.py resolves them) plays --group-size rollouts with the
training env and rewards: the --max-tics rollout limit, ended early by the
portal and key goals, from training's random group starts (slot, seed and
warm-up, from --seed). When there are more pairs than --groups (default:
the logical cores minus 2), they take turns, --groups pairs at a time, like
training's map rotation. Each game gets a window (no sound; it runs as fast
as it can) unless --headless. Prints one line per pair and an overall line
in the [it] line's terms (return, sectors, exit, death, steps).

Watching it play (--iwad PATH). Launches a normal game on that IWAD file,
with no monsters (--eval, as in stage-1 training), with
-rldrive (control and output only: the game runs in real time with its
window and sound, and publishes a decision whenever it is ready for input,
without waiting). At each decision the frame is turned into the same 4-frame
stack the policy was trained on (its --obs-size, from the checkpoint), the policy picks an action, and
the matching driver command is sent. The drive stops when the level's exit
is reached, the player dies, combat starts (CONSULT is the hand-off to
Muse's VLM, which isn't attached here), or after --max-decisions; Ctrl-C
stops it too. A CONSULT while exploring is sent and only shows "CONSULT"
as the status message (--on-consult skip, the default, drives on). Then a
summary is printed (drive_summary): the level, why it stopped, decisions
and game time, sectors gained, exit, death, health, keys, distance, and
the policy's action use; the game is left running for you to play on or
close, unless --quit-at-end.

Nothing is trained, saved or uploaded. The policy: --checkpoint, else
--hub-repo's doom-runner.pt, else the newest local runs/*/doom-runner.pt.

Usage (from agent/training; ./eval.sh does the setup):
    .venv/bin/python eval.py --checkpoint runs/stage1/doom-runner.pt --iwads ../../wad/*/*.WAD
    .venv/bin/python eval.py --hub-repo user/name --max-tics 2100 --headless
    .venv/bin/python eval.py --iwad ../../wad/UltimateDoom/DOOM.WAD --warp "1 2" --greedy
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

import numpy as np
import torch
from torch.distributions import Categorical

from model import (Policy, add_env_args, avatar_args, check_slots, checkpoint_obs_size, default_groups, env_label,
                   hub_fetch_checkpoint, load_weights, privileged, resolve_iwads, resolve_maps, run_env)
from env import ACTIONS, CONSULT, Engine, EngineConfig, EngineError, cleanup_stale  # noqa: E402
from env.env import EXPLORE  # noqa: E402
from env.obs import FrameStack, Preprocessor  # noqa: E402
from env.vector import GroupVecEnv  # noqa: E402
from rollout import run_rollouts, sample_group_options  # noqa: E402

HERE = Path(__file__).resolve().parent
MAX_DROPPED = 20      # policy steps in combat without CONSULT before giving up (EnvConfig.max_combat_steps)


def parse_args():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    add_env_args(p)
    p.add_argument("--checkpoint", default=None, help="policy checkpoint (default: --hub-repo's, else the "
                                                      "newest local runs/*/doom-runner.pt)")
    p.add_argument("--seed", type=int, default=0, help="group starts and action sampling")
    p.add_argument("--headless", action=argparse.BooleanOptionalAction, default=False,
                   help="no game windows (and no sound) (default: a window per game; in the training-style "
                        "eval no sound, running as fast as it can, --group-size x --groups at a time)")
    p.add_argument("--device", default="cuda" if torch.cuda.is_available() else "cpu")
    w = p.add_argument_group("watching it play (--iwad)")
    w.add_argument("--iwad", default=None, metavar="PATH",
                   help="watch the policy play this IWAD file (no monsters) instead of the "
                        "training-style eval")
    w.add_argument("--warp", default="1 1", help="level, as -warp args (default: E1M1)")
    w.add_argument("--greedy", action="store_true", help="take the most likely action instead of sampling")
    w.add_argument("--on-consult", choices=["skip", "stop"], default="skip",
                   help="CONSULT while exploring: keep driving (default), or stop driving; either way "
                        "it is sent, so the game shows \"CONSULT\" as the status message")
    w.add_argument("--max-decisions", type=int, default=0, help="stop after this many decisions (0: no limit)")
    w.add_argument("--delay", type=float, default=0.0, help="seconds to wait before each action, for watching")
    w.add_argument("--quit-at-end", action="store_true", help="quit the game when the drive ends")
    return p.parse_args()


# ---------------------------------------------------------------------------
# The policy
# ---------------------------------------------------------------------------

def default_checkpoint() -> Path | None:
    """The newest run checkpoint (runs/*/doom-runner.pt)."""
    found = sorted(HERE.glob("runs/*/doom-runner.pt"), key=lambda p: p.stat().st_mtime)
    return found[-1] if found else None


def load_policy(path: Path, device: torch.device) -> Policy:
    """The checkpoint's policy, at the observation size it was trained on
    (policy.obs_size)."""
    weights = load_weights(str(path))
    n = weights["policy_head.weight"].shape[0]
    if n != len(ACTIONS):
        raise SystemExit(f"{path}: the policy has {n} actions, this game has {len(ACTIONS)} "
                         f"({', '.join(ACTIONS)}); it was trained on a different action set")
    size = checkpoint_obs_size(torch.load(str(path), map_location="cpu", weights_only=False))
    policy = Policy(obs_size=size).to(device)
    policy.load_state_dict(weights)
    policy.eval()
    return policy


def find_checkpoint(args) -> Path:
    ckpt = Path(args.checkpoint) if args.checkpoint else None
    if ckpt is None and args.hub_repo:
        ckpt = hub_fetch_checkpoint(args.hub_repo, HERE / "runs" / "hub-play")
        if ckpt is None:
            print("[eval] using the newest local checkpoint instead")
    if ckpt is None:
        ckpt = default_checkpoint()
    if ckpt is None or not ckpt.exists():
        raise SystemExit("no checkpoint: pass --checkpoint or --hub-repo, or train first (./train.sh)")
    return ckpt


# ---------------------------------------------------------------------------
# Training-style eval
# ---------------------------------------------------------------------------

def pair_line(label: str, recs: list[dict], width: int) -> str:
    def mean(k):
        return float(np.mean([r[k] for r in recs]))
    rets = [r["ret"] for r in recs]
    spawned = sum(r.get("items", [0, 0])[0] for r in recs)
    picked = sum(r.get("items", [0, 0])[1] for r in recs)
    return (f"  {label:<{width}}  ret {np.mean(rets):7.2f} ±{np.std(rets):5.2f}  "
            f"sectors {mean('sectors_gained'):5.2f}  exit {mean('exit'):.2f}  "
            f"death {mean('death'):.2f}  steps {mean('len'):6.0f}  tics {mean('rollout_tics'):5.0f}  "
            + (f"items {picked}/{spawned}  " if spawned else "") + f"({len(recs)} rollouts)")


def rollouts(args, ckpt: Path, policy: Policy, device: torch.device) -> None:
    t0 = time.time()
    if not args.headless and (args.noblit or args.nodraw):
        print("[eval] windows: --noblit / --nodraw ignored (they would leave the windows black)")
        args.noblit = args.nodraw = False
    if args.stage != 1:
        print(f"[eval] --stage {args.stage} ignored: eval never has monsters (stage 1)")
        args.stage = 1
    slots = [int(s) for s in args.slots.split(",") if s.strip()]
    iwads = resolve_iwads(args.iwads)
    check_slots(iwads, slots)
    maps = resolve_maps(args.maps, iwads, args.stage, [])
    pairs = [(w, m) for m in maps for w in iwads]
    groups = args.groups or default_groups(iwads, maps)
    G = args.group_size
    labels = [env_label(w, m, iwads) for w, m in pairs]
    print(f"[eval] {ckpt}: {len(pairs)} IWAD/map pairs x {G} rollouts, {args.max_tics} tics each "
          f"(ended early by the portal goal {args.portal_goal} and keys), {groups} pairs at a time, "
          f"stage {args.stage}, seed {args.seed}, device {device}, "
          f"{'headless' if args.headless else f'{groups * G} windows at a time'}")
    cleanup_stale()
    sampler = np.random.default_rng(args.seed)
    results: dict[str, list[dict]] = {}
    width = max(len(l) for l in labels + ["all"])
    for start in range(0, len(pairs), groups):
        chunk = pairs[start:start + groups]
        vec = GroupVecEnv([run_env(args, slots, w, m) for w, m in chunk for _ in range(G)])
        try:
            recs = run_rollouts(vec, policy, device,
                                sample_group_options(sampler, len(chunk), G, slots, args.warmup_max))
        finally:
            vec.close()
        for j, label in enumerate(labels[start:start + groups]):
            results[label] = recs[j * G:(j + 1) * G]
            print(pair_line(label, results[label], width), flush=True)
    every = [r for rs in results.values() for r in rs]
    print(pair_line("all", every, width))
    acts = np.bincount(np.concatenate([np.asarray(r["act"], dtype=np.int64) for r in every]))
    top = sorted(((c, ACTIONS[a]) for a, c in enumerate(acts) if c), reverse=True)[:6]
    print("[eval] top actions: " + ", ".join(f"{a} {c / acts.sum():.0%}" for c, a in top))
    print(f"[eval] took {time.time() - t0:.0f} s")


# ---------------------------------------------------------------------------
# Watching it play (--iwad)
# ---------------------------------------------------------------------------

def drive_summary(ckpt, level: str, why: str, n: int, counts: np.ndarray, tics: int,
                  dist: float, first: dict, last: dict, practice: dict | None = None) -> list[str]:
    """The end-of-drive stats: what was reached, how long it took, how far
    the player got, and how the policy used its actions (shares of the
    decisions sent: a key held, e.g. W, counts for every action that holds
    it)."""
    def share(pred) -> float:
        tot = counts.sum()
        return sum(c for a, c in zip(ACTIONS, counts) if pred(a)) / tot if tot else 0.0
    keys = lambda a: a[6:] if a.startswith("PRESS") else ""
    sectors = last.get("sectors", 0) - first.get("sectors", 0)
    held = last.get("keys", 0) or 0
    key_names = [c for bit, c in ((1, "blue"), (2, "yellow"), (4, "red")) if held & bit]
    top = sorted(((c, a) for a, c in zip(ACTIONS, counts) if c), reverse=True)[:5]
    secs = tics / 35.0
    return [
        f"[eval] {ckpt}",
        f"[eval] level {level}: {why}",
        f"[eval]   decisions {n}, game time {secs:.1f} s ({tics} tics)",
        f"[eval]   sectors gained {sectors:+d}"
        + (f" ({sectors / secs * 60:.1f} a minute)" if secs > 0 else "")
        + f", exit {'yes' if why == 'reached the exit' else 'no'}"
        + f", died {'yes' if last.get('dead') else 'no'}",
        f"[eval]   health {first.get('hp', '?')} -> {last.get('hp', '?')}, "
        f"keys {', '.join(key_names) if key_names else 'none'}, "
        f"distance {dist:.0f} units" + (f" ({dist / secs:.0f} a second)" if secs > 0 else ""),
        f"[eval]   forward {share(lambda a: 'W' in keys(a)):.0%}  back {share(lambda a: 'S' in keys(a)):.0%}  "
        f"strafe {share(lambda a: 'A' in keys(a) or 'D' in keys(a)):.0%}  "
        f"turn L {share(lambda a: 'L' in keys(a)):.0%}  turn R {share(lambda a: 'R' in keys(a)):.0%}  "
        f"idle {share(lambda a: a == 'PRESS'):.0%}  USE {int(counts[ACTIONS.index('USE')])}  "
        f"CONSULT {int(counts[CONSULT])}",
        "[eval]   top actions: " + ", ".join(f"{a} {c / counts.sum():.0%}" for c, a in top),
    ] + ([f"[eval]   practice: monsters seen {practice['seen']}, consulted on {practice['seen_consulted']}; "
          f"items picked {practice['picked']}/{practice['spawned']}, uniques {practice['uniques']}, "
          f"consulted on {practice['unique_consulted']}"] if practice else [])


def watch(args, ckpt: Path, policy: Policy, device: torch.device) -> None:
    spawns = []      # the practice monsters and items, as in training (2026-11-17)
    if args.spawn_tics > 0:
        spawns += ["-rlspawn", str(args.spawn_tics)]
    if args.item_tics > 0:
        spawns += ["-rlitems", str(args.item_tics)]
    cfg = EngineConfig(warp=args.warp, skill=args.skill, drive=True, headless=args.headless,
                       extra_args=["--eval"] + spawns + avatar_args(),     # never map monsters, as in stage 1
                       step_timeout=120.0)
    if not Path(args.iwad).is_file():
        raise SystemExit(f"--iwad {args.iwad}: no such file (it takes an IWAD path)")
    cfg.iwad = str(Path(args.iwad).resolve())
    engine = Engine(cfg, tag="play")
    print(f"[watch] {ckpt} driving {Path(cfg.iwad).stem} {args.warp} (no monsters), "
          f"{'greedy' if args.greedy else 'sampled'} actions")
    d = engine.start()
    w, h = policy.obs_size
    pre = Preprocessor(engine.width, engine.height, w, h)
    stack = FrameStack(4, h, w)

    def screen() -> np.ndarray:
        pix, pal, _ = engine.frame()
        return pre(pix, pal)

    obs = stack.reset(screen())
    n, dropped, why = 0, 0, "max decisions"
    start_sectors = d.get("sectors", 0)
    first, counts, tics, dist = d, np.zeros(len(ACTIONS), dtype=np.int64), 0, 0.0
    # Practice spawns: sightings (monstervis coming on), items, uniques, and
    # whether a CONSULT came while the sighting lasted or within the
    # unique's flash (0.75 s), as the training triggers count them.
    practice = dict(seen=0, seen_consulted=0, spawned=0, picked=0, uniques=0, unique_consulted=0)
    seeing, seen_done, unique_tic, unique_done = False, False, None, False
    try:
        while not args.max_decisions or n < args.max_decisions:
            if d.get("dead"):
                why = "the player died"
                break
            with torch.no_grad():
                logits, _ = policy(torch.as_tensor(obs[None], device=device),
                                   torch.as_tensor([privileged({"state": d})], device=device))
                logits = logits[0]
                a = int(logits.argmax()) if args.greedy else int(Categorical(logits=logits).sample())
            if a == CONSULT:
                if seeing and not seen_done:
                    practice["seen_consulted"] += 1
                    seen_done = True
                if unique_tic is not None and tics - unique_tic <= 26 and not unique_done:
                    practice["unique_consulted"] += 1
                    unique_done = True
            if a == CONSULT and (args.on_consult == "stop" or d.get("mode") != EXPLORE):
                engine.send(ACTIONS[a])            # shows "CONSULT" on screen
                counts[a] += 1
                why = "CONSULT (hand-off to Muse; no VLM attached)"
                break
            if d.get("mode") != EXPLORE:
                # Combat: in training every action but CONSULT is dropped here.
                dropped += 1
                if dropped >= MAX_DROPPED:
                    why = "combat started (the policy didn't CONSULT)"
                    break
                continue
            dropped = 0
            if args.delay:
                time.sleep(args.delay)
            engine.send(ACTIONS[a])
            counts[a] += 1
            prev = d
            d, events = engine.wait_decision()
            # A decision can also arrive unprompted (combat, death, level
            # end); the newest one is the screen to act on.
            for ev in engine.poll():
                events.append(ev)
                if ev.get("event") == "decision":
                    d = ev
            obs = stack.push(screen())
            n += 1
            step_tics = max(int(d.get("tics") or 0), 0)
            tics += step_tics
            for ev in events:
                if ev.get("event") == "item":
                    practice["spawned"] += 1
                elif ev.get("event") == "pickup" and ev.get("practice"):
                    practice["picked"] += 1
                if ev.get("event") == "pickup" and ev.get("tier") == 4:
                    practice["uniques"] += 1
                    unique_tic, unique_done = tics, False
            vis = bool(d.get("monstervis"))
            if vis and not seeing:
                practice["seen"] += 1
                seen_done = False
            seeing = vis
            if None not in (prev.get("x"), prev.get("y"), d.get("x"), d.get("y")):
                moved = float(np.hypot(d["x"] - prev["x"], d["y"] - prev["y"]))
                if moved <= 64 * max(step_tics, 1):     # a jump is a teleport, not movement
                    dist += moved
            print(f"[watch {n:4d}] {ACTIONS[a]:<8s} -> {d.get('mode', '?'):<8s} at ({d.get('x')},{d.get('y')}) "
                  f"sectors {d.get('sectors', 0) - start_sectors:+d} tics {d.get('tics')}")
            if any(ev.get("event") == "exit" for ev in events):
                why = "reached the exit"
                break
    except KeyboardInterrupt:
        why = "interrupted"
    except EngineError as e:
        print(f"[watch] game error: {e}", file=sys.stderr)
        why = "game error"
    print(f"[watch] stopped after {n} decisions: {why}")
    print("\n".join(drive_summary(ckpt, f"{Path(cfg.iwad).stem} {args.warp}", why, n, counts, tics,
                                   dist, first, d,
                                   practice if args.spawn_tics > 0 or args.item_tics > 0 else None)))
    if args.quit_at_end or why == "game error":
        engine.close()
    else:
        pid = engine.proc.pid if engine.proc else None
        engine.detach()
        print(f"[watch] game (pid {pid}) left running; play on or close its window")


def main():
    args = parse_args()
    ckpt = find_checkpoint(args)
    device = torch.device(args.device)
    torch.manual_seed(args.seed)
    policy = load_policy(ckpt, device)
    if args.obs_size is not None and tuple(args.obs_size) != policy.obs_size:
        print(f"[eval] --obs-size ignored: {ckpt} was trained at {policy.obs_size[0]}x{policy.obs_size[1]}")
    args.obs_size = policy.obs_size          # the env frames match the checkpoint
    if args.iwad:
        watch(args, ckpt, policy, device)
    else:
        rollouts(args, ckpt, policy, device)


if __name__ == "__main__":
    main()
