"""Environment tests (SPEC.md §5.3, §9 Phase 1, §10). Tests marked `game` launch the game."""

from __future__ import annotations

import json
import time

import gymnasium as gym
import numpy as np
import pytest

from env import ACTIONS, DiabloDoomEnv, EngineConfig, EnvConfig, RewardConfig, scripted_combat_consult

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


@pytest.mark.game
def test_check_env():
    """Gymnasium's own API checks on one instance."""
    from gymnasium.utils.env_checker import check_env
    e = make_env()
    try:
        check_env(e, skip_render_check=True)
    finally:
        e.close()


@pytest.mark.game
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


@pytest.mark.game
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


@pytest.mark.game
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


@pytest.mark.game
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


def test_mechanism_once():
    """The mechanism reward's bookkeeping: once per mechanism per rollout.
    A door used from its other side is the same sector; a repeatable
    switch is the same line; a switch that also opens a new door is new."""
    e = make_env()                        # never started: no game needed
    e._used = set()

    def use(sectors=(), lines=()):
        return e._new_mechanisms([{"event": "use", "sectors": list(sectors), "lines": list(lines)}])
    assert use(sectors=[10]) is True      # open a door
    assert use(sectors=[10]) is False     # reopen it, or use it from the other side
    assert use(lines=[7]) is True         # a light switch (nothing moves)
    assert use(lines=[7]) is False        # the same switch again
    assert use(sectors=[10], lines=[8]) is True   # another switch, even for a known door
    assert use(sectors=[3, 10], lines=[8]) is True   # ...that now also starts a new lift
    assert e._new_mechanisms([{"event": "exit"}]) is False
    assert e._new_mechanisms([]) is False


@pytest.mark.game
def test_mechanism_reward_once():
    """In E1M1: the first USE that opens a door pays the mechanism reward;
    reopening the same door once it has closed again pays nothing."""
    rc = RewardConfig(mechanism=100.0)    # large, so it stands out from the other terms
    e = DiabloDoomEnv(EnvConfig(engine=EngineConfig(warp="1 1", extra_args=["-nomonsters"]),
                                max_tics=100000, rewards=rc))
    use, turn_l, turn_r = ACTIONS.index("USE"), ACTIONS.index("TURNL"), ACTIONS.index("TURNR")
    events = []
    command = e.engine.command

    def logged(line):
        d, ev = command(line)
        events.extend(x for x in ev if x.get("event") == "use")
        return d, ev
    e.engine.command = logged
    try:
        rng = np.random.default_rng(1)
        e.reset(options={"seed": 0})
        for _ in range(4000):                 # random explore, heavy on USE, until a door opens
            n = len(events)
            _, r, term, trunc, _ = e.step(use if rng.random() < 0.3 else int(rng.choice([0, 1, 2, 3, 4])))
            if len(events) > n:
                break
            if term or trunc:
                e.reset(options={"seed": 0})
        else:
            pytest.fail("no mechanism used in 4000 random steps")
        first = events[-1]
        assert first["sectors"] and r >= 90, f"the first use pays ({first}, reward {r})"

        reopened = 0
        for _ in range(3):                    # ~300 tics: the door closes, then reopen it
            for _ in range(17):
                e.step(turn_l)
                e.step(turn_r)
            n = len(events)
            _, r, *_ = e.step(use)
            if len(events) > n:
                assert events[-1]["sectors"] == first["sectors"]
                assert r < 50, f"reopening the same door pays nothing (reward {r})"
                reopened += 1
        assert reopened > 0, "the door should reopen once it has closed"
    finally:
        e.close()
