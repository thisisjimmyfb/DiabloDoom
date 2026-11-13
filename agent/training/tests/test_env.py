#!/usr/bin/env python3
"""Environment smoke tests (SPEC.md §9 Phase 1, §10).

  check     gymnasium check_env on one instance
  random    random agent for N decisions with resets (never wedges)
  determ    same slot, seed and actions twice -> identical final state
  vector    AsyncVectorEnv throughput with K instances
  strafe    explore strafes move sideways without turning
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import gymnasium as gym  # noqa: E402

from env import (ACTIONS, DiabloDoomEnv, EnvConfig,  # noqa: E402
                            cleanup_stale, scripted_combat_consult)


def make_env(**kw) -> DiabloDoomEnv:
    return DiabloDoomEnv(EnvConfig(**kw))


def cmd_check(args):
    from gymnasium.utils.env_checker import check_env
    env = make_env()
    try:
        check_env(env, skip_render_check=True)
        print("check_env: PASS")
    finally:
        env.close()


def cmd_random(args):
    env = make_env(consult_handler=scripted_combat_consult)
    rng = np.random.default_rng(0)
    obs, info = env.reset(seed=0)
    episodes, steps, ret, rets = 0, 0, 0.0, []
    modes: dict[str, int] = {}
    t0 = time.time()
    try:
        while steps < args.steps:
            a = int(rng.integers(len(ACTIONS)))
            obs, r, term, trunc, info = env.step(a)
            assert obs.shape == env.observation_space.shape and obs.dtype == np.uint8
            steps += 1
            ret += r
            modes[info["mode"]] = modes.get(info["mode"], 0) + 1
            if term or trunc:
                rets.append(ret)
                episodes += 1
                ret = 0.0
                obs, info = env.reset()
        dt = time.time() - t0
        print(f"random: {steps} decisions, {episodes} episodes in {dt:.1f}s "
              f"({steps / dt:.0f} steps/s); mean return {np.mean(rets) if rets else float('nan'):.2f}; "
              f"modes {modes}")
        if args.png:
            save_obs(obs, args.png)
        print("random: PASS")
    finally:
        env.close()


def save_obs(obs: np.ndarray, path: str):
    """Write the frame stack side by side as a grayscale PNG."""
    import struct
    import zlib
    img = np.concatenate(list(obs), axis=1)
    h, w = img.shape
    raw = b"".join(b"\0" + img[y].tobytes() for y in range(h))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    Path(path).write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 0, 0, 0, 0))
                           + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
    print(f"saved observation stack to {path}")


def cmd_determ(args):
    def run():
        env = make_env(consult_handler=scripted_combat_consult)
        rng = np.random.default_rng(123)
        env.reset(options={"seed": 42})
        # Start the measured run from a reset (LOAD + SEED), as training does.
        env.reset(options={"seed": 42})
        state, frames = None, []
        for _ in range(args.steps):
            obs, r, term, trunc, info = env.step(int(rng.integers(len(ACTIONS) - 1)))
            state = info["state"]
            frames.append(int(obs.astype(np.int64).sum()))
            if term or trunc:
                break
        env.close()
        return json.dumps(state, sort_keys=True), frames
    a, fa = run()
    b, fb = run()
    print("run A:", a)
    if a == b and fa == fb:
        print("determ: PASS (states and observations identical)")
    else:
        print("run B:", b)
        print("determ: FAIL")
        sys.exit(1)


def cmd_vector(args):
    envs = gym.vector.AsyncVectorEnv(
        [lambda: make_env(consult_handler=scripted_combat_consult) for _ in range(args.k)])
    try:
        envs.reset(seed=0)
        rng = np.random.default_rng(0)
        t0 = time.time()
        for _ in range(args.steps):
            envs.step(rng.integers(len(ACTIONS), size=args.k))
        dt = time.time() - t0
        n = args.steps * args.k
        print(f"vector: {args.k} instances, {n} decisions in {dt:.1f}s = {n / dt:.0f} steps/s")
    finally:
        envs.close()


def cmd_strafe(args):
    env = make_env()
    try:
        env.reset(options={"seed": 0})
        s0 = env.state
        _, _, _, _, info = env.step(ACTIONS.index("STRAFE_L"))
        s1 = info["state"]
        print(f"before x={s0['x']} y={s0['y']} angle={s0['angle']}; "
              f"after STRAFE_L x={s1['x']} y={s1['y']} angle={s1['angle']} tics={info['tics']}")
        moved = ((s1["x"] - s0["x"]) ** 2 + (s1["y"] - s0["y"]) ** 2) ** 0.5
        ok = s1["angle"] == s0["angle"] and moved > 32
        print("strafe:", "PASS" if ok else "FAIL")
    finally:
        env.close()


def main():
    p = argparse.ArgumentParser()
    p.add_argument("test", choices=["check", "random", "determ", "vector", "strafe"])
    p.add_argument("--steps", type=int, default=10000)
    p.add_argument("--k", type=int, default=8)
    p.add_argument("--png", default=None)
    args = p.parse_args()
    cleanup_stale()
    {"check": cmd_check, "random": cmd_random, "determ": cmd_determ,
     "vector": cmd_vector, "strafe": cmd_strafe}[args.test](args)


if __name__ == "__main__":
    main()
