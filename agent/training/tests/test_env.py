"""Environment tests (SPEC.md §9 Phase 1, §10). All of them launch the game."""

from __future__ import annotations

import json
import time

import gymnasium as gym
import numpy as np
import pytest

from env import ACTIONS, DiabloDoomEnv, EnvConfig, scripted_combat_consult

pytestmark = pytest.mark.game

RANDOM_STEPS = 10000      # random agent: decisions across resets
DETERM_STEPS = 1500       # determinism: decisions per run
VECTOR_K = 8              # throughput: instances
VECTOR_STEPS = 1000       # throughput: steps per instance


def make_env(**kw) -> DiabloDoomEnv:
    return DiabloDoomEnv(EnvConfig(**kw))


@pytest.fixture
def env():
    e = make_env(consult_handler=scripted_combat_consult)
    yield e
    e.close()


def test_check_env():
    """Gymnasium's own API checks on one instance."""
    from gymnasium.utils.env_checker import check_env
    e = make_env()
    try:
        check_env(e, skip_render_check=True)
    finally:
        e.close()


def test_random_agent(env):
    """A random agent for RANDOM_STEPS decisions with resets never wedges."""
    rng = np.random.default_rng(0)
    env.reset(seed=0)
    episodes, modes = 0, {}
    t0 = time.time()
    for _ in range(RANDOM_STEPS):
        obs, r, term, trunc, info = env.step(int(rng.integers(len(ACTIONS))))
        assert obs.shape == env.observation_space.shape and obs.dtype == np.uint8
        modes[info["mode"]] = modes.get(info["mode"], 0) + 1
        if term or trunc:
            episodes += 1
            env.reset()
    dt = time.time() - t0
    print(f"random: {RANDOM_STEPS} decisions, {episodes} episodes, {RANDOM_STEPS / dt:.0f} steps/s; modes {modes}")
    assert episodes > 0


def test_determinism():
    """Same slot, seed and actions twice -> identical states and observations."""
    def run():
        e = make_env(consult_handler=scripted_combat_consult)
        try:
            rng = np.random.default_rng(123)
            e.reset(options={"seed": 42})
            # Start the measured run from a reset (LOAD + SEED), as training does.
            e.reset(options={"seed": 42})
            state, frames = None, []
            for _ in range(DETERM_STEPS):
                obs, r, term, trunc, info = e.step(int(rng.integers(len(ACTIONS) - 1)))
                state = info["state"]
                frames.append(int(obs.astype(np.int64).sum()))
                if term or trunc:
                    break
            return json.dumps(state, sort_keys=True), frames
        finally:
            e.close()
    a, fa = run()
    b, fb = run()
    assert a == b, f"final states differ:\n{a}\n{b}"
    assert fa == fb, "observations differ"


def test_vector_throughput():
    """VECTOR_K instances in AsyncVectorEnv step without error; prints steps/s."""
    envs = gym.vector.AsyncVectorEnv(
        [lambda: make_env(consult_handler=scripted_combat_consult) for _ in range(VECTOR_K)])
    try:
        envs.reset(seed=0)
        rng = np.random.default_rng(0)
        t0 = time.time()
        for _ in range(VECTOR_STEPS):
            envs.step(rng.integers(len(ACTIONS), size=VECTOR_K))
        dt = time.time() - t0
        print(f"vector: {VECTOR_K} instances, {VECTOR_STEPS * VECTOR_K / dt:.0f} steps/s")
    finally:
        envs.close()


def test_explore_strafe():
    """An explore strafe moves sideways without turning."""
    e = make_env()
    try:
        e.reset(options={"seed": 0})
        s0 = e.state
        _, _, _, _, info = e.step(ACTIONS.index("STRAFE_L"))
        s1 = info["state"]
    finally:
        e.close()
    moved = ((s1["x"] - s0["x"]) ** 2 + (s1["y"] - s0["y"]) ** 2) ** 0.5
    assert s1["angle"] == s0["angle"], "a strafe never turns"
    assert moved > 32, f"moved only {moved:.0f} units"
