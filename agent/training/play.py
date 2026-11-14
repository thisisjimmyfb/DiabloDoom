#!/usr/bin/env python3
"""Drive a normal, real-time DiabloDoom game with a trained policy.

Launches the game with -rldrive (control and output only: the game runs
in real time with its window and sound, and publishes a decision whenever
it is ready for input, without waiting). At each decision the frame is
turned into the same 84x84 4-frame stack the policy was trained on, the
policy picks an action, and the matching driver command is sent.

The run stops driving when the level's exit is reached, the player dies,
combat starts (CONSULT is the hand-off to Muse's VLM, which isn't
attached here), or after --max-decisions; Ctrl-C stops it too. The game is
then left running for you to play on or close, unless --quit-at-end.

Usage (from agent/training; ./play.sh does the setup):
    .venv/bin/python play.py                                 # newest agent/ckpt_*.pt, E1M1
    .venv/bin/python play.py --checkpoint agent/ckpt_000129.pt --warp "1 2" --greedy
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

import numpy as np
import torch
from torch.distributions import Categorical

from model import Policy, load_weights, privileged
from env import ACTIONS, CONSULT, Engine, EngineConfig, EngineError  # noqa: E402
from env.env import EXPLORE  # noqa: E402
from env.obs import FrameStack, Preprocessor  # noqa: E402

HERE = Path(__file__).resolve().parent
MAX_DROPPED = 20      # policy steps in combat without CONSULT before giving up (EnvConfig.max_combat_steps)


def default_checkpoint() -> Path | None:
    """The newest checkpoint train.sh exported (agent/), else in runs/."""
    for pattern in (HERE.parent.glob("ckpt_*.pt"), HERE.glob("runs/*/ckpt_*.pt")):
        found = sorted(pattern, key=lambda p: p.stat().st_mtime)
        if found:
            return found[-1]
    return None


def load_policy(path: Path, device: torch.device) -> Policy:
    weights = load_weights(str(path))
    n = weights["policy_head.weight"].shape[0]
    if n != len(ACTIONS):
        raise SystemExit(f"{path}: the policy has {n} actions, this game has {len(ACTIONS)} "
                         f"({', '.join(ACTIONS)}); it was trained on a different action set")
    policy = Policy().to(device)
    policy.load_state_dict(weights)
    policy.eval()
    return policy


def parse_args():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--checkpoint", default=None, help="policy checkpoint (default: the newest agent/ckpt_*.pt)")
    p.add_argument("--iwad", default=None, help="IWAD (default: Freedoom Phase 1)")
    p.add_argument("--warp", default="1 1", help="level, as -warp args (default: E1M1)")
    p.add_argument("--skill", type=int, default=3)
    p.add_argument("--monsters", action=argparse.BooleanOptionalAction, default=False,
                   help="monsters on (default off, as in stage-1 training; play.sh turns them on, "
                        "--no-monsters turns them back off); combat ends the drive")
    p.add_argument("--greedy", action="store_true", help="take the most likely action instead of sampling")
    p.add_argument("--on-consult", choices=["skip", "stop"], default="skip",
                   help="CONSULT while exploring: skip it and act again (default), or stop driving")
    p.add_argument("--max-decisions", type=int, default=0, help="stop after this many decisions (0: no limit)")
    p.add_argument("--delay", type=float, default=0.0, help="seconds to wait before each action, for watching")
    p.add_argument("--quit-at-end", action="store_true", help="quit the game when the drive ends")
    p.add_argument("--headless", action="store_true", help="no window and no sound (tests)")
    p.add_argument("--seed", type=int, default=0)
    p.add_argument("--device", default="cuda" if torch.cuda.is_available() else "cpu")
    return p.parse_args()


def main():
    args = parse_args()
    ckpt = Path(args.checkpoint) if args.checkpoint else default_checkpoint()
    if ckpt is None or not ckpt.exists():
        raise SystemExit("no checkpoint: pass --checkpoint, or train first (./train.sh exports one to agent/)")
    device = torch.device(args.device)
    torch.manual_seed(args.seed)
    policy = load_policy(ckpt, device)

    cfg = EngineConfig(warp=args.warp, skill=args.skill, drive=True, headless=args.headless,
                       extra_args=[] if args.monsters else ["-nomonsters"], step_timeout=120.0)
    if args.iwad:
        cfg.iwad = str(Path(args.iwad).resolve())
    engine = Engine(cfg, tag="play")
    print(f"[play] {ckpt} driving {Path(cfg.iwad).stem} {args.warp}"
          f"{'' if args.monsters else ' (no monsters)'}, {'greedy' if args.greedy else 'sampled'} actions")
    d = engine.start()
    pre = Preprocessor(engine.width, engine.height, 84)
    stack = FrameStack(4, 84)

    def screen() -> np.ndarray:
        pix, pal, _ = engine.frame()
        return pre(pix, pal)

    obs = stack.reset(screen())
    n, dropped, why = 0, 0, "max decisions"
    start_sectors = d.get("sectors", 0)
    try:
        while not args.max_decisions or n < args.max_decisions:
            if d.get("dead"):
                why = "the player died"
                break
            with torch.no_grad():
                logits, _ = policy(torch.as_tensor(obs[None], device=device),
                                   torch.as_tensor([privileged({"state": d})], device=device))
                logits = logits[0]
                if d.get("mode") == EXPLORE and args.on_consult == "skip":
                    logits[CONSULT] = -float("inf")    # no VLM here: keep exploring
                a = int(logits.argmax()) if args.greedy else int(Categorical(logits=logits).sample())
            if a == CONSULT:
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
            d, events = engine.wait_decision()
            # A decision can also arrive unprompted (combat, death, level
            # end); the newest one is the screen to act on.
            for ev in engine.poll():
                events.append(ev)
                if ev.get("event") == "decision":
                    d = ev
            obs = stack.push(screen())
            n += 1
            print(f"[play {n:4d}] {ACTIONS[a]:<8s} -> {d.get('mode', '?'):<8s} at ({d.get('x')},{d.get('y')}) "
                  f"sectors {d.get('sectors', 0) - start_sectors:+d} tics {d.get('tics')}")
            if any(ev.get("event") == "exit" for ev in events):
                why = "reached the exit"
                break
    except KeyboardInterrupt:
        why = "interrupted"
    except EngineError as e:
        print(f"[play] game error: {e}", file=sys.stderr)
        why = "game error"
    print(f"[play] stopped after {n} decisions: {why}")
    if args.quit_at_end or why == "game error":
        engine.close()
    else:
        pid = engine.proc.pid if engine.proc else None
        engine.detach()
        print(f"[play] game (pid {pid}) left running; play on or close its window")


if __name__ == "__main__":
    main()
