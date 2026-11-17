"""Environment tests (SPEC.md §5.3, §9 Phase 1, §10). Tests marked `game` launch the game."""

from __future__ import annotations

import json
import time

import gymnasium as gym
import numpy as np
import pytest

from env import ACTIONS, DiabloDoomEnv, EngineConfig, EnvConfig, RewardConfig, standin_consult

RANDOM_STEPS = 10000      # random agent: decisions across resets
DETERM_STEPS = 1500       # determinism: decisions per run
VECTOR_K = 8              # throughput: instances
VECTOR_STEPS = 1000       # throughput: steps per instance
MOVES = [i for i, a in enumerate(ACTIONS) if a != "CONSULT"]   # every action but CONSULT


def make_env(**kw) -> DiabloDoomEnv:
    return DiabloDoomEnv(EnvConfig(**kw))


@pytest.fixture
def env():
    e = make_env(consult_handler=standin_consult)
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
        e = make_env(consult_handler=standin_consult)
        try:
            rng = np.random.default_rng(123)
            e.reset(options={"seed": 42})
            # Start the measured run from a reset (LOAD + SEED), as training does.
            e.reset(options={"seed": 42})
            state, frames = None, []
            for _ in range(DETERM_STEPS):
                obs, r, term, trunc, info = e.step(int(rng.choice(MOVES)))
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
        [lambda: make_env(consult_handler=standin_consult) for _ in range(VECTOR_K)])
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
    """Holding A (PRESS A, 10 tics) moves sideways without turning."""
    e = make_env()
    try:
        e.reset(options={"seed": 0})
        s0 = e.state
        for _ in range(10):
            _, _, _, _, info = e.step(ACTIONS.index("PRESS A"))
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


def test_set_map():
    """set_map moves an instance to another map: installed saves (the old
    map's progress) are dropped and the warp changes; the same map is a
    no-op. Never started: no game needed."""
    e = make_env(engine=EngineConfig(warp="1 1"))
    e.install_state(4, b"old-map-progress", "ff")
    assert e.set_map(None, "1 1") is False and e._installed
    assert e.set_map(None, "1 2") is True
    assert e.cfg.engine.warp == "1 2" and not e._installed and not e._installed_visited


def test_keys_once():
    """The key reward's bookkeeping: once per colour per rollout, keys
    held at the start never pay, and a state without the field (between
    levels) changes nothing."""
    e = make_env()                        # never started: no game needed
    e._keys = 0
    assert e._new_keys({"keys": 0}) == 0
    assert e._new_keys({"keys": 1}) == 1  # blue
    assert e._new_keys({"keys": 1}) == 0  # still blue
    assert e._new_keys({}) == 0           # no level: nothing changes
    assert e._new_keys({"keys": 7}) == 2  # yellow and red at once
    assert e._new_keys({"keys": 7}) == 0
    e._keys = 4                           # red held at reset
    assert e._new_keys({"keys": 4}) == 0 and e._new_keys({"keys": 5}) == 1


def test_hp_loss_and_death():
    """HP lost costs hp_loss per point, healing pays nothing (it only moves
    the mark up), and a death ends the rollout with the death penalty on
    top of the HP it lost. Engine faked: no game needed."""
    only = RewardConfig(sector=0, exit=0, mechanism=0, consult_hit=0, trigger_miss=0,
                        consult_false=0, key=0, time=0, hp_loss=-0.2, death=-100.0,
                        loot=0, health=0, fast=0, look=0, turn_waste=0, move_flip=0, use_miss=0, fall=0, wall_look=0)
    e = make_env(rewards=only, max_tics=100000)
    states = iter([{"mode": "EXPLORE", "hp": 90, "tics": 10},     # nukage: -10 hp
                   {"mode": "EXPLORE", "hp": 100, "tics": 10},    # healed: nothing
                   {"mode": "EXPLORE", "hp": 95, "tics": 10},     # -5 from the new mark
                   {"mode": "EXPLORE", "hp": -3, "dead": True, "tics": 10}])
    e.engine.command = lambda line: (next(states), [])
    e._frame = lambda: np.zeros((200, 320), dtype=np.uint8)
    e.state = {"mode": "EXPLORE", "hp": 100}
    e._prev_hp, e._tics, e._combat_steps = 100, 0, 0
    e._best_sectors = e._start_sectors = 0
    e._keys, e._goal, e._group, e._slot, e._seed = 0, None, 0, None, 0
    e._trig = {k: e._new_trigger() for k in ("monster", "backpack", "combat")}
    e._stack.reset(e._frame())
    rewards, ends = [], []
    for _ in range(4):
        _, r, term, trunc, _ = e.step(0)
        rewards.append(round(r, 6))
        ends.append(term)
    assert rewards == [-2.0, 0.0, -1.0, -119.0], rewards     # 95 hp to 0: -19, plus -100
    assert ends == [False, False, False, True]


@pytest.mark.game
def test_fast_tics():
    """E1M1: running (PRESS W) passes 4 units/tic within a few tics and
    then every tic counts (fast_tics, -rlfast added by the env), each
    paying `fast`; running into a wall has none."""
    only = RewardConfig(sector=0, exit=0, mechanism=0, consult_hit=0, trigger_miss=0,
                        consult_false=0, key=0, time=0, hp_loss=0, death=0,
                        loot=0, health=0, fast=0.005, fast_speed=4, look=0, turn_waste=0, move_flip=0, use_miss=0, fall=0, wall_look=0)
    e = make_env(engine=EngineConfig(warp="1 1", extra_args=["--eval"]), rewards=only, max_tics=100000)
    try:
        assert e.cfg.engine.extra_args[-2:] == ["-rlfast", "4"]
        e.reset(options={"seed": 1})
        w = ACTIONS.index("PRESS W")
        fast = []
        for _ in range(12):
            _, r, _, _, info = e.step(w)
            fast.append(info["state"]["fast_tics"])
            assert info["tics"] == 1 and r == pytest.approx(0.005 * fast[-1])
        assert fast[:2] == [0, 0] and fast[-6:] == [1] * 6, fast
        for _ in range(200):                # on until a wall stops the player
            _, r, _, _, info = e.step(w)
        assert info["state"]["fast_tics"] == 0 and r == 0.0, "pushing into a wall is not moving"
    finally:
        e.close()


@pytest.mark.game
def test_press():
    """E1M1, a decision every tic: PRESS W runs (accelerating, facing
    unchanged), PRESS L turns about 7 degrees a tic in place, PRESS WL
    does both at once, PRESS alone lets the player coast to a stop. The
    engine's multi-tic commands still run to their end (WALK 0, TURNL),
    and the next decision is one tic later again."""
    e = make_env(engine=EngineConfig(warp="1 1", extra_args=["--eval"]), max_tics=100000)
    try:
        def hold(name, n):
            x, y, a = e.state["x"], e.state["y"], e.state["angle"]
            for _ in range(n):
                _, _, _, _, info = e.step(ACTIONS.index(name))
                assert info["tics"] == 1, (name, info["tics"])
            d = info["state"]
            return float(np.hypot(d["x"] - x, d["y"] - y)), (d["angle"] - a) % 360
        e.reset(options={"seed": 1})
        moved, turned = hold("PRESS W", 10)
        assert moved > 45 and turned == 0, (moved, turned)
        moved, turned = hold("PRESS", 40)
        assert moved > 10 and e.state["x"] == e.step(ACTIONS.index("PRESS"))[4]["state"]["x"], "coasts, then stops"
        moved, turned = hold("PRESS L", 10)
        assert moved < 3 and 55 <= turned <= 75, (moved, turned)
        moved, turned = hold("PRESS WL", 10)
        assert moved > 40 and 55 <= turned <= 75, (moved, turned)
        hold("PRESS", 40)
        d, _ = e.engine.command("TURNL")
        assert d["tics"] == 10, "a legacy 45-degree turn runs to its end"
        d, _ = e.engine.command("WALK 0")
        assert d["tics"] > 10
        hold("PRESS", 1)
    finally:
        e.close()


def test_turn_waste():
    """Wasted rotation, from the facing: a turn against the last 8 tics'
    net turn costs turn_waste per 45 degrees of 2 x min(|turn|, |net|) --
    a steady turn either way, holding still, a turn after the window has
    passed and a teleport cost nothing. Engine faked: one tic per step."""
    only = RewardConfig(sector=0, exit=0, mechanism=0, consult_hit=0, trigger_miss=0,
                        consult_false=0, key=0, time=0, hp_loss=0, death=0, loot=0, health=0,
                        fast=0, look=0, turn_waste=-45.0, turn_waste_window=8, move_flip=0,
                        use_miss=0, fall=0, wall_look=0)     # -45 per 45 degrees: reward = -wasted degrees
    e = make_env(rewards=only, max_tics=100000)
    angles = [10, 20, 13, 20, 350, 0, 350] + [350] * 8 + [0]
    xs = [0, 0, 0, 0, 0, 500, 500] + [500] * 8 + [500]          # x jumps 500: a teleport
    states = iter([{"mode": "EXPLORE", "hp": 100, "tics": 1, "x": x, "y": 0, "angle": a}
                   for x, a in zip(xs, angles)])
    e.engine.command = lambda line: (next(states), [])
    e._frame = lambda: np.zeros((200, 320), dtype=np.uint8)
    e.state = {"mode": "EXPLORE", "hp": 100, "x": 0, "y": 0, "angle": 0}
    e._prev_hp, e._tics, e._combat_steps = 100, 0, 0
    e._best_sectors = e._start_sectors = 0
    e._keys, e._goal, e._group, e._slot, e._seed = 0, None, 0, None, 0
    e._trig = {k: e._new_trigger() for k in ("monster", "backpack", "combat")}
    e._stack.reset(e._frame())
    got = [round(e.step(ACTIONS.index("PRESS"))[1], 6) for _ in angles]
    want = [0, 0, -14, 0, -40, 0, 0] + [0] * 8 + [0]
    #  +10 +10, then -7 against +20: 14 wasted; +7 with the net; -30 across
    #  0/360 against +20: 40; teleport resets; -10, then 8 still tics push
    #  it out of the window, so the +10 back costs nothing.
    assert got == want, got


def test_move_flip():
    """W at most move_flip_window (4) tics after S, or S after W, costs
    move_flip; strafing back and forth (A/D) never does, and turn keys
    don't count. Engine faked: one tic per step, no game needed."""
    only = RewardConfig(sector=0, exit=0, mechanism=0, consult_hit=0, trigger_miss=0,
                        consult_false=0, key=0, time=0, hp_loss=0, death=0, loot=0, health=0,
                        fast=0, look=0, turn_waste=0, move_flip=-1.0, move_flip_window=4, use_miss=0, fall=0, wall_look=0)
    e = make_env(rewards=only, max_tics=100000)
    e.engine.command = lambda line: ({"mode": "EXPLORE", "hp": 100, "tics": 1}, [])
    e._frame = lambda: np.zeros((200, 320), dtype=np.uint8)
    e.state = {"mode": "EXPLORE", "hp": 100}
    e._prev_hp, e._tics, e._combat_steps = 100, 0, 0
    e._best_sectors = e._start_sectors = 0
    e._keys, e._goal, e._group, e._slot, e._seed = 0, None, 0, None, 0
    e._trig = {k: e._new_trigger() for k in ("monster", "backpack", "combat")}
    e._stack.reset(e._frame())
    plan = ["PRESS W", "PRESS WA", "PRESS S",             # W, W, then S: a flip
            "PRESS A", "PRESS D", "PRESS A", "PRESS WL",  # strafes free; W 4 tics after S: a flip
            "PRESS L", "PRESS R", "PRESS", "PRESS", "PRESS SD",  # 5 tics later: none
            "PRESS S"]                                    # same way: none
    got = [e.step(ACTIONS.index(a))[1] for a in plan]
    assert got == [0, 0, -1.0, 0, 0, 0, -1.0, 0, 0, 0, 0, 0, 0], got


def test_use_miss():
    """A USE that sets nothing going (no "use" event) costs use_miss; one
    that opens a new mechanism pays mechanism instead, one that reopens a
    mechanism already used costs nothing, a USE facing something usable
    that didn't move (wall_use: a locked door, a pressed switch) costs
    nothing, and other actions never cost it. Engine faked: no game needed."""
    only = RewardConfig(sector=0, exit=0, mechanism=10.0, consult_hit=0, trigger_miss=0,
                        consult_false=0, key=0, time=0, hp_loss=0, death=0, loot=0, health=0,
                        fast=0, look=0, turn_waste=0, move_flip=0, use_miss=-0.1, fall=0, wall_look=0)
    e = make_env(rewards=only, max_tics=100000)
    door = [{"event": "use", "sectors": [7], "lines": []}]
    steps = iter([([], False), (door, False), (door, False), ([], True), ([], False), ([], False), ([], False)])

    def command(line):
        events, facing = next(steps)
        return {"mode": "EXPLORE", "hp": 100, "tics": 1, "wall_use": facing}, events
    e.engine.command = command
    e._frame = lambda: np.zeros((200, 320), dtype=np.uint8)
    e.state = {"mode": "EXPLORE", "hp": 100}
    e._prev_hp, e._tics, e._combat_steps = 100, 0, 0
    e._best_sectors = e._start_sectors = 0
    e._keys, e._goal, e._group, e._slot, e._seed, e._used = 0, None, 0, None, 0, set()
    e._trig = {k: e._new_trigger() for k in ("monster", "backpack", "combat")}
    e._stack.reset(e._frame())
    use, walk = ACTIONS.index("USE"), ACTIONS.index("PRESS W")
    got = [round(e.step(a)[1], 6) for a in (use, use, use, walk, use, walk, use)]
    assert got == [-0.1, 10.0, 0.0, 0.0, 0.0, 0.0, -0.1], got


def test_door_pays_once():
    """A door's own sector never pays (the engine's door_sectors): walking
    through a door pays the sector reward once, for the room behind it,
    and stepping into the door doesn't reach the portal goal. Engine
    faked: no game needed."""
    only = RewardConfig(sector=15.0, exit=0, mechanism=0, consult_hit=0, trigger_miss=0,
                        consult_false=0, key=0, time=0, hp_loss=0, death=0, loot=0, health=0,
                        fast=0, look=0, turn_waste=0, move_flip=0, use_miss=0, fall=0, wall_look=0)
    e = make_env(rewards=only, max_tics=100000, portal_goal=1)
    states = iter([(5, 0), (6, 1), (7, 1)])      # (sectors, door_sectors): room, door, next room

    def command(line):
        sectors, doors = next(states)
        return {"mode": "EXPLORE", "hp": 100, "tics": 1, "sectors": sectors, "door_sectors": doors}, []
    e.engine.command = command
    e._frame = lambda: np.zeros((200, 320), dtype=np.uint8)
    e.state = {"mode": "EXPLORE", "hp": 100, "sectors": 5, "door_sectors": 0}
    e._prev_hp, e._tics, e._combat_steps = 100, 0, 0
    e._best_sectors = e._start_sectors = 5
    e._keys, e._goal, e._group, e._slot, e._seed, e._used = 0, None, 0, None, 0, set()
    e._trig = {k: e._new_trigger() for k in ("monster", "backpack", "combat")}
    e._stack.reset(e._frame())
    w = ACTIONS.index("PRESS W")
    got = [(round(r, 6), term) for _, r, term, _, _ in (e.step(w) for _ in range(3))]
    assert got == [(0.0, False), (0.0, False), (15.0, True)], got


def test_fast_forward():
    """The fast pay goes by the movement's direction against the facing
    (RewardConfig.fast_forward): full forward, about 70% on a forward
    diagonal, nothing strafing or backing up; off, every fast tic pays.
    Engine faked: one tic per step, one fast tic each."""
    def run(dx, dy, angle, forward=True):
        only = RewardConfig(sector=0, exit=0, mechanism=0, consult_hit=0, trigger_miss=0,
                            consult_false=0, key=0, time=0, hp_loss=0, death=0, loot=0, health=0,
                            fast=1.0, fast_forward=forward, look=0, turn_waste=0, move_flip=0,
                            use_miss=0, fall=0, wall_look=0)
        e = make_env(rewards=only, max_tics=100000)
        e.engine.command = lambda line: ({"mode": "EXPLORE", "hp": 100, "tics": 1, "fast_tics": 1,
                                          "x": 100 + dx, "y": 100 + dy, "z": 0, "angle": angle}, [])
        e._frame = lambda: np.zeros((200, 320), dtype=np.uint8)
        e.state = {"mode": "EXPLORE", "hp": 100, "x": 100, "y": 100, "z": 0, "angle": angle}
        e._prev_hp, e._tics, e._combat_steps = 100, 0, 0
        e._best_sectors = e._start_sectors = 0
        e._keys, e._goal, e._group, e._slot, e._seed = 0, None, 0, None, 0
        e._trig = {k: e._new_trigger() for k in ("monster", "backpack", "combat")}
        e._stack.reset(e._frame())
        return e.step(ACTIONS.index("PRESS W"))[1]

    assert run(10, 0, 0) == pytest.approx(1.0), "forward (facing east)"
    assert run(0, 10, 90) == pytest.approx(1.0), "forward (facing north)"
    assert run(7, 7, 0) == pytest.approx(np.cos(np.radians(45))), "forward diagonal"
    assert run(0, 10, 0) == pytest.approx(0.0, abs=1e-9), "strafing"
    assert run(-10, 0, 0) == 0.0, "backing up"
    assert run(-10, 0, 0, forward=False) == pytest.approx(1.0), "off: every fast tic pays"


def test_fall():
    """A freefall of 64+ units (accelerating past 10 units a tic) costs fall,
    once, on landing, whatever its height; a lift lowering 4 or 8 a tic
    (the blazing one), stairs down,
    going up and a teleport never do. Engine faked: one tic per step."""
    only = RewardConfig(sector=0, exit=0, mechanism=0, consult_hit=0, trigger_miss=0,
                        consult_false=0, key=0, time=0, hp_loss=0, death=0, loot=0, health=0,
                        fast=0, look=0, turn_waste=0, move_flip=0, use_miss=0, fall=-1.0, wall_look=0)

    def run(zs, xs=None):
        e = make_env(rewards=only, max_tics=100000)
        xs = xs or [0] * len(zs)
        states = iter([{"mode": "EXPLORE", "hp": 100, "tics": 1, "x": x, "y": 0, "z": z}
                       for x, z in zip(xs[1:], zs[1:])])
        e.engine.command = lambda line: (next(states), [])
        e._frame = lambda: np.zeros((200, 320), dtype=np.uint8)
        e.state = {"mode": "EXPLORE", "hp": 100, "x": xs[0], "y": 0, "z": zs[0]}
        e._prev_hp, e._tics, e._combat_steps = 100, 0, 0
        e._best_sectors = e._start_sectors = 0
        e._keys, e._goal, e._group, e._slot, e._seed = 0, None, 0, None, 0
        e._trig = {k: e._new_trigger() for k in ("monster", "backpack", "combat")}
        e._stack.reset(e._frame())
        return [round(e.step(ACTIONS.index("PRESS W"))[1], 6) for _ in zs[1:]]

    freefall = [128, 127, 125, 122, 118, 113, 107, 100, 92, 83, 73, 62, 50, 37, 23, 8, 0, 0, 0]
    got = run(freefall)
    assert got[:-2] == [0.0] * (len(got) - 2) and got[-2] == -1.0 and got[-1] == 0.0, got
    deep = [256 - sum(range(i + 1)) for i in range(23)] + [0, 0, 0]
    deep = [max(z, 0) for z in deep]
    got = run(deep)
    assert sorted(got) == [-1.0] + [0.0] * (len(got) - 1), f"a 256-unit fall costs the same: {got}"
    assert run([128 - 4 * i for i in range(33)] + [0, 0]) == [0.0] * 34, "a lift: 4 units a tic"
    assert run([256 - 8 * i for i in range(33)] + [0, 0]) == [0.0] * 34, "a blazing lift: 8 units a tic"
    assert run([256 - 9 * i for i in range(29)] + [4, 4]) == [0.0] * 30, "a lift with truncation noise"
    stairs = [64, 64, 56, 48, 48, 48, 40, 32, 32, 32, 24, 16, 16, 16, 8, 0, 0, 0]
    assert run(stairs) == [0.0] * (len(stairs) - 1), "stairs: short drops with treads between"
    assert run(list(reversed(freefall))) == [0.0] * (len(freefall) - 1), "going up never costs"
    assert run([128, 0, 0], xs=[0, 500, 500]) == [0.0, 0.0], "a teleport is no fall"


def test_wall_look_rule():
    """wall_look charges each tic the wall ahead is nearer than
    wall_look_dist, unless a usable wall is in reach (wall_use) or there is
    no wall ahead. Engine faked: one tic per step, no game needed."""
    only = RewardConfig(sector=0, exit=0, mechanism=0, consult_hit=0, trigger_miss=0,
                        consult_false=0, key=0, time=0, hp_loss=0, death=0, loot=0, health=0,
                        fast=0, look=0, turn_waste=0, move_flip=0, use_miss=0, fall=0,
                        wall_look=-1.0, wall_look_dist=48.0)
    e = make_env(rewards=only, max_tics=100000)
    states = iter([{"wall_dist": 20, "wall_use": False}, {"wall_dist": 47, "wall_use": False},
                   {"wall_dist": 48, "wall_use": False}, {"wall_dist": -1, "wall_use": False},
                   {"wall_dist": 20, "wall_use": True}, {"wall_dist": 30}])
    e.engine.command = lambda line: ({"mode": "EXPLORE", "hp": 100, "tics": 1, **next(states)}, [])
    e._frame = lambda: np.zeros((200, 320), dtype=np.uint8)
    e.state = {"mode": "EXPLORE", "hp": 100}
    e._prev_hp, e._tics, e._combat_steps = 100, 0, 0
    e._best_sectors = e._start_sectors = 0
    e._keys, e._goal, e._group, e._slot, e._seed = 0, None, 0, None, 0
    e._trig = {k: e._new_trigger() for k in ("monster", "backpack", "combat")}
    e._stack.reset(e._frame())
    got = [e.step(ACTIONS.index("PRESS"))[1] for _ in range(6)]
    assert got == [-1.0, -1.0, 0.0, 0.0, 0.0, -1.0], got


@pytest.mark.game
def test_wall_dist():
    """E1M1: the state line's wall_dist is the distance to the wall straight
    ahead: running into one brings it under 48 (charged), facing back into
    the room puts it beyond (or -1, nothing within 128)."""
    only = RewardConfig(sector=0, exit=0, mechanism=0, consult_hit=0, trigger_miss=0,
                        consult_false=0, key=0, time=0, hp_loss=0, death=0, loot=0, health=0,
                        fast=0, look=0, turn_waste=0, move_flip=0, use_miss=0, fall=0,
                        wall_look=-1.0, wall_look_dist=48.0)
    e = make_env(engine=EngineConfig(warp="1 1", extra_args=["--eval"]), rewards=only, max_tics=100000)
    try:
        e.reset(options={"seed": 1})
        for _ in range(26):                 # turn round to face the start room's back wall
            e.step(ACTIONS.index("PRESS L"))
        for _ in range(60):                 # run until the wall stops the player
            _, r, _, _, info = e.step(ACTIONS.index("PRESS W"))
        d = info["state"]
        assert 0 <= d["wall_dist"] < 48 and d["wall_use"] is False and r == -1.0, (d["wall_dist"], r)
        for _ in range(26):                 # turn back round, away from the wall
            _, r, _, _, info = e.step(ACTIONS.index("PRESS L"))
        d = info["state"]
        assert (d["wall_dist"] == -1 or d["wall_dist"] >= 48) and r == 0.0, (d["wall_dist"], r)
    finally:
        e.close()


@pytest.mark.game
def test_look_reward():
    """E1M1: turning a full circle in place, every tic pays look x
    max(0, cos(portal_rel)) -- portal_rel, the angle to the nearest
    portal of the current sector into an unvisited sector -- so full
    credit facing it and nothing facing away."""
    only = RewardConfig(sector=0, exit=0, mechanism=0, consult_hit=0, trigger_miss=0,
                        consult_false=0, key=0, time=0, hp_loss=0, death=0,
                        loot=0, health=0, fast=0, look=1.0, turn_waste=0, move_flip=0, use_miss=0, fall=0, wall_look=0)
    e = make_env(engine=EngineConfig(warp="1 1", extra_args=["--eval"]), rewards=only, max_tics=100000)
    try:
        e.reset(options={"seed": 1})
        got = []
        for _ in range(55):                 # about 7 degrees a tic: a full turn
            _, r, _, _, info = e.step(ACTIONS.index("PRESS L"))
            rel = info["state"].get("portal_rel")
            assert rel is not None and -180 <= rel <= 180, info["state"]
            assert r == pytest.approx(max(0.0, np.cos(np.radians(rel))), abs=1e-9), (r, rel)
            got.append(r)
        assert max(got) > 0.99 and min(got) == 0.0, (max(got), min(got))
    finally:
        e.close()


@pytest.mark.game
def test_command_batch():
    """Commands joined with ';' run on one tic: PRESS W; PRESS A holds both
    keys (the same run as PRESS WA), PRESS W; USE runs and uses in one
    1-tic decision, and a line without PRESS releases the keys. Separate
    lines still run one per tic."""
    e = make_env(engine=EngineConfig(warp="1 1", extra_args=["--eval"]), max_tics=100000)
    try:
        def run(lines):
            e.reset(options={"seed": 1})
            x, y = e.state["x"], e.state["y"]
            for line in lines:
                d, _ = e.engine.command(line)
                assert d["tics"] == 1, (line, d["tics"])
            return d["x"], d["y"], float(np.hypot(d["x"] - x, d["y"] - y))
        one = run(["PRESS WA"] * 10)
        assert run(["PRESS W; PRESS A"] * 10) == one, "two PRESS commands in a line add up"
        assert run([" PRESS W ;PRESS A; "] * 10) == one, "spaces and an empty piece are ignored"
        assert run(["PRESS W; PRESS S; PRESS A"] * 10) == run(["PRESS A"] * 10), "W and S cancel"
        assert run(["PRESS W; USE"] * 10)[2] > 45, "USE in the line doesn't release the keys"
        held = run(["PRESS W"] * 15)[2]
        released = run(["PRESS W"] * 10 + ["USE"] * 5)[2]
        assert released < held - 5, (released, held)
        e.reset(options={"seed": 1})
        d, _ = e.engine.command("PRESS W\nPRESS A")
        assert d["tics"] == 2, "separate lines: one per tic"
    finally:
        e.close()


def test_key_goal():
    """With key_goal a new key colour ends the rollout (terminated, end
    "key", the key reward paid); one held from the start never does, and
    without key_goal a key only pays. Engine faked: no game needed."""
    only = RewardConfig(sector=0, exit=0, mechanism=0, consult_hit=0, trigger_miss=0,
                        consult_false=0, key=25.0, time=0, hp_loss=0, death=0,
                        loot=0, health=0, fast=0, look=0, turn_waste=0, move_flip=0, use_miss=0, fall=0, wall_look=0)
    for goal in (True, False):
        e = make_env(rewards=only, max_tics=100000, key_goal=goal)
        states = iter([{"mode": "EXPLORE", "hp": 100, "tics": 5, "keys": 1},    # blue held from the start
                       {"mode": "EXPLORE", "hp": 100, "tics": 5, "keys": 3}])   # yellow picked up
        e.engine.command = lambda line: (next(states), [])
        e._frame = lambda: np.zeros((200, 320), dtype=np.uint8)
        e.state = {"mode": "EXPLORE", "hp": 100, "keys": 1}
        e._prev_hp, e._tics, e._combat_steps = 100, 0, 0
        e._best_sectors = e._start_sectors = 0
        e._keys, e._goal, e._group, e._slot, e._seed = 1, None, 0, None, 0
        e._trig = {k: e._new_trigger() for k in ("monster", "backpack", "combat")}
        e._stack.reset(e._frame())
        _, r, term, _, info = e.step(0)
        assert (r, term, info["end"], info["key"]) == (0.0, False, None, False)
        _, r, term, _, info = e.step(0)
        assert r == 25.0
        if goal:
            assert term and info["end"] == "key" and info["key"]
        else:
            assert not term and info["end"] is None and not info["key"]


def test_pickup_rewards():
    """Pickup events: loot and armor pay `loot` per item, health pays
    `health` per HP healed, keys and other things pay nothing here (keys
    pay through the state's key mask). Engine faked: no game needed."""
    only = RewardConfig(sector=0, exit=0, mechanism=0, consult_hit=0, trigger_miss=0,
                        consult_false=0, key=0, time=0, hp_loss=0, death=0,
                        loot=5.0, health=0.2)
    e = make_env(rewards=only, max_tics=100000)
    st = {"mode": "EXPLORE", "hp": 50, "tics": 5}
    pick = lambda kind, hp=0: {"event": "pickup", "kind": kind, "hp": hp, "gametic": 1}
    steps = iter([[pick("loot")], [pick("health", 25)], [pick("armor"), pick("loot")],
                  [pick("key"), pick("other")], [pick("health", 1)]])
    e.engine.command = lambda line: (dict(st), next(steps))
    e._frame = lambda: np.zeros((200, 320), dtype=np.uint8)
    e.state = dict(st)
    e._prev_hp, e._tics, e._combat_steps = 50, 0, 0
    e._best_sectors = e._start_sectors = 0
    e._keys, e._goal, e._group, e._slot, e._seed = 0, None, 0, None, 0
    e._trig = {k: e._new_trigger() for k in ("monster", "backpack", "combat")}
    e._stack.reset(e._frame())
    rewards = [round(e.step(0)[1], 6) for _ in range(5)]
    assert rewards == [5.0, 5.0, 10.0, 0.0, 0.2], rewards


@pytest.mark.game
def test_mechanism_reward_once():
    """In E1M1: the first USE that opens a door pays the mechanism reward;
    reopening the same door once it has closed again pays nothing."""
    rc = RewardConfig(mechanism=100.0)    # large, so it stands out from the other terms
    e = DiabloDoomEnv(EnvConfig(engine=EngineConfig(warp="1 1", extra_args=["--eval"]),
                                max_tics=100000, rewards=rc))
    use, noop = ACTIONS.index("USE"), ACTIONS.index("PRESS")
    moves = [ACTIONS.index(a) for a in ("PRESS W", "PRESS L", "PRESS R", "PRESS A", "PRESS D", "PRESS WL", "PRESS WR")]
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
        found = False
        for _ in range(4000):                 # random explore, heavy on USE, until a door opens
            # a move is held for 8 tics (single random tics barely move)
            act, n_tics = (use, 1) if rng.random() < 0.3 else (int(rng.choice(moves)), 8)
            for _ in range(n_tics):
                n = len(events)
                _, r, term, trunc, _ = e.step(act)
                if len(events) > n:
                    found = True
                    break
                if term or trunc:
                    e.reset(options={"seed": 0})
            if found:
                break
        else:
            pytest.fail("no mechanism used in 4000 random actions")
        first = events[-1]
        assert first["sectors"] and r >= 90, f"the first use pays ({first}, reward {r})"

        reopened = 0
        for _ in range(3):                    # ~340 tics: the door closes, then reopen it
            for _ in range(340):
                e.step(noop)
            n = len(events)
            _, r, *_ = e.step(use)
            if len(events) > n:
                assert events[-1]["sectors"] == first["sectors"]
                assert r < 50, f"reopening the same door pays nothing (reward {r})"
                reopened += 1
        assert reopened > 0, "the door should reopen once it has closed"
    finally:
        e.close()


class _FakeGame:
    """Stands in for DiabloDoomEnv in a CONSULT handler: answers each
    driver command with the next scripted state, and records them."""

    def __init__(self, replies):
        self.replies, self.sent = list(replies), []

    def consult_command(self, line):
        self.sent.append(line)
        return self.replies.pop(0), []


def test_standin_consult():
    """The training stand-in clears what was consulted about: CONSULT
    then END for combat (and a sighting), salvage for a full backpack,
    nothing for a false consult."""
    explore = {"mode": "EXPLORE", "bp_count": 0}
    plan = {"mode": "PLANNING"}

    g = _FakeGame([plan, explore])
    assert standin_consult(g, plan, ["combat"])["mode"] == "EXPLORE"
    assert g.sent == ["CONSULT", "END"]

    g = _FakeGame([explore])
    standin_consult(g, dict(explore, monstervis=True), ["monster"])
    assert g.sent == ["CONSULT"]

    # Still full after the first salvage, room after the second: then
    # salvage_extra more...
    full = {"mode": "EXPLORE", "backpack_full": True}
    replies = [dict(full, bp_count=4)] + [dict(explore, bp_count=n) for n in (3, 2, 1, 0)]
    g = _FakeGame(replies)
    s = standin_consult(g, dict(full, bp_count=5), ["backpack"], salvage_extra=2)
    assert g.sent == ["SALVAGE 0"] * 4 and s["bp_count"] == 1
    # ...but never past an empty backpack.
    g = _FakeGame(replies)
    s = standin_consult(g, dict(full, bp_count=5), ["backpack"], salvage_extra=10)
    assert g.sent == ["SALVAGE 0"] * 5 and s["bp_count"] == 0

    g = _FakeGame([])
    assert standin_consult(g, explore, []) is explore and g.sent == []


@pytest.mark.game
def test_consult_clears_combat():
    """Monsters on, E1M1: once combat starts, other actions are dropped
    (no tics, no reward); CONSULT (the stand-in's CONSULT command) ends the fight with
    the monster killed and no penalty."""
    e = make_env(engine=EngineConfig(warp="1 1"), consult_handler=standin_consult, max_tics=100000)
    try:
        rng = np.random.default_rng(1)
        _, info = e.reset(options={"seed": 0})
        for k in ("monstervis", "backpack_full", "bp_count"):
            assert k in info["state"], f"state line lacks {k}"
        w, l, r = (ACTIONS.index(a) for a in ("PRESS W", "PRESS L", "PRESS R"))
        act = w
        for t in range(6000):              # run and turn at random, each held 8 tics
            if info["mode"] != "EXPLORE":
                break
            if t % 8 == 0:
                act = int(rng.choice([w, w, w, l, r]))
            _, _, term, trunc, info = e.step(act)
            if term or trunc:
                _, info = e.reset(options={"seed": 0})
        else:
            pytest.fail("no combat in 6000 random tics")
        before = info["state"]
        _, r, _, _, info = e.step(0)                      # PRESS in combat: dropped
        assert info["tics"] == 0 and r == 0, f"dropped (reward {r})"
        _, r, _, _, info = e.step(ACTIONS.index("CONSULT"))
        after = info["state"]
        assert after["mode"] == "EXPLORE", f"consult should end combat, still {after['mode']}"
        assert after["kills"] > before["kills"]
        assert r >= 0, f"consulting on combat costs nothing (reward {r})"
    finally:
        e.close()


@pytest.mark.game
def test_spawn_consult():
    """-rlspawn (no map monsters): a practice monster appears on screen
    without starting combat; CONSULT on it pays consult_hit and removes
    it. Spawns follow SEED: the same seed and actions give the same run."""
    def run():
        e = make_env(engine=EngineConfig(warp="1 1", extra_args=["--eval", "-rlspawn", "60"]),
                     consult_handler=standin_consult, max_tics=100000)
        try:
            rng = np.random.default_rng(7)
            _, info = e.reset(options={"seed": 3})
            trace = []
            for _ in range(600):
                s = info["state"]
                assert s["mode"] == "EXPLORE", "a practice monster never starts combat"
                if s["monstervis"]:
                    _, r, _, _, info = e.step(ACTIONS.index("CONSULT"))
                    return trace, s, r, info["state"]
                _, _, _, _, info = e.step(int(rng.choice([0, 0, 1, 2])))
                trace.append((info["state"]["x"], info["state"]["y"], info["state"]["monstervis"]))
            pytest.fail("no practice monster in 600 steps")
        finally:
            e.close()
    trace, seen, r, after = run()
    assert r >= RewardConfig().consult_hit, f"CONSULT on a sighting pays (reward {r})"
    assert not after["monstervis"] and after["kills"] == seen["kills"], "removed, not killed"
    assert run()[0] == trace, "spawns are deterministic under SEED"


@pytest.mark.game
def test_practice_items():
    """-rlitems: practice loot appears ahead (an "item" event with its
    position); walking onto it picks it up, pays the loot reward and
    counts in info["items"] as [spawned, picked up]."""
    import math
    e = make_env(engine=EngineConfig(warp="1 1", extra_args=["--eval", "-rlitems", "30"]),
                 consult_handler=standin_consult, max_tics=100000)
    target = None
    command = e.engine.command

    def spy(line):
        nonlocal target
        d, events = command(line)
        for ev in events:
            if ev.get("event") == "item":
                target = (ev["x"], ev["y"])
            elif ev.get("event") == "pickup" and ev.get("practice"):
                target = None
        return d, events
    e.engine.command = spy
    try:
        e.reset(options={"seed": 0})
        idle, w, left, right = (ACTIONS.index(a) for a in ("PRESS", "PRESS W", "PRESS L", "PRESS R"))
        loot = 0.0
        for _ in range(2000):
            s, a = e.state, idle
            if target is not None:     # turn to the item, then walk onto it
                rel = (math.degrees(math.atan2(target[1] - s["y"], target[0] - s["x"]))
                       - s["angle"] + 180) % 360 - 180
                a = left if rel > 4 else right if rel < -4 else w
            _, _, _, _, info = e.step(a)
            loot += info["terms"].get("loot", 0.0)
            if info["items"][1] >= 2:
                break
        assert info["items"][1] >= 2, f"two practice items picked up (items {info['items']})"
        assert loot == pytest.approx(RewardConfig().loot * info["items"][1]), "each pays the loot reward"
    finally:
        e.close()


def test_goal_dist():
    """The distance penalty's goal: the nearest portal of the current
    sector (portal_dist); the exit and frontier distances don't count, and
    -1 (none) or a missing field is no distance."""
    from env.env import goal_dist
    assert goal_dist({"portal_dist": 40, "exit_dist": 10, "frontier_dist": 5}) == 40
    assert goal_dist({"portal_dist": 0}) == 0
    assert goal_dist({"portal_dist": -1, "exit_dist": 900}) is None
    assert goal_dist({"exit_dist": 30, "frontier_dist": 40}) is None
    assert goal_dist({"mode": "INTERMISSION"}) is None
    # The walking distance first (path_dist), the straight one without a path.
    assert goal_dist({"path_dist": 300, "portal_dist": 40}) == 300
    assert goal_dist({"path_dist": -1, "portal_dist": 40}) == 40


def test_way_rel():
    """The look reward's angle: along the path (path_rel) when the engine
    has one, else toward the nearest unvisited portal (portal_rel)."""
    from env.env import way_rel
    assert way_rel({"path_rel": -30, "portal_rel": 90}) == -30
    assert way_rel({"path_dist": 50, "portal_rel": 90}) == 90
    assert way_rel({"path_dist": -1}) is None


@pytest.mark.game
def test_path_guidance():
    """E1M1: the state line carries path_dist and path_rel; walking the
    way path_rel points (opening doors on the way) reaches new rooms.
    Engine-side: the grid, the doors, the straight-walkable aim."""
    e = make_env(engine=EngineConfig(warp="1 1", extra_args=["--eval"]),
                 consult_handler=standin_consult, max_tics=100000)
    try:
        e.reset(options={"seed": 0})
        s = e.state
        assert s["path_dist"] >= 0 and "path_rel" in s, s
        w, wl, wr, left, right, use = (ACTIONS.index(a) for a in
                                       ("PRESS W", "PRESS WL", "PRESS WR", "PRESS L", "PRESS R", "USE"))
        last_use = -1000
        for i in range(600):
            s = e.state
            rel = s.get("path_rel")
            if s.get("wall_use") and 0 <= s.get("wall_dist", -1) <= 40 and i - last_use > 70:
                a, last_use = use, i        # once: a second USE would close the door again
            elif rel is None or abs(rel) < 10:
                a = w
            elif abs(rel) < 45:
                a = wl if rel > 0 else wr
            else:
                a = left if rel > 0 else right
            _, _, _, _, info = e.step(a)
        assert info["state"]["sectors"] >= 5, f"following path_rel explores (sectors {info['state']['sectors']})"
    finally:
        e.close()


@pytest.mark.game
def test_time_distance_penalty():
    """E1M1: the state line carries exit_dist, frontier_dist and
    portal_dist, and with every other reward off each step costs time *
    its tics * (1 + d / far_scale), d the mean of its start and end
    distances to the nearest portal of the current sector."""
    only = RewardConfig(sector=0, exit=0, mechanism=0, consult_hit=0,
                        trigger_miss=0, consult_false=0, key=0, hp_loss=0, death=0, loot=0, health=0, fast=0, look=0, turn_waste=0, move_flip=0, use_miss=0, fall=0, wall_look=0,
                        time=1.0, far_scale=256.0)
    e = make_env(engine=EngineConfig(warp="1 1", extra_args=["--eval"]), rewards=only,
                 max_tics=100000, consult_handler=standin_consult)
    try:
        from env.env import goal_dist
        _, info = e.reset(options={"seed": 0})
        s = info["state"]
        assert s["exit_dist"] > 0 and s["frontier_dist"] >= 0 and s["portal_dist"] > 0 and s["keys"] == 0
        rng = np.random.default_rng(2)
        expect, got, prev = 0.0, 0.0, goal_dist(s)
        moves = [ACTIONS.index(a) for a in ("PRESS W", "PRESS W", "PRESS WL", "PRESS L", "PRESS A", "PRESS")]
        for _ in range(400):
            _, r, term, trunc, info = e.step(int(rng.choice(moves)))
            s2 = info["state"]
            g = goal_dist(s2)
            if info["tics"] > 0:
                expect -= info["tics"] * (1 + (prev + g) / 2 / 256.0)
            prev, s = g, s2
            got += r
            if term or trunc:
                break
        assert abs(got - expect) < 1e-6, (got, expect)
    finally:
        e.close()


@pytest.mark.game
def test_portal_goal():
    """E1M1, portal goal 1: the rollout ends (terminated, portal) on its
    first new sector, and with only time and sector rewards on its return
    is the sector's reward minus time * tics. Its end state saved with its visited
    list (VISITED) and reinstalled restores that visited count after the
    load (VISIT), so the next rollout's goal is a sector beyond it."""
    only = RewardConfig(sector=15.0, exit=0, mechanism=0, consult_hit=0,
                        trigger_miss=0, consult_false=0, key=0, hp_loss=0, death=0, loot=0, health=0, fast=0, look=0, turn_waste=0, move_flip=0, use_miss=0, fall=0, wall_look=0,
                        time=0.01, far_scale=1e12)   # time and sector only
    e = DiabloDoomEnv(EnvConfig(engine=EngineConfig(warp="1 1", extra_args=["--eval"]), rewards=only,
                                max_tics=100000, portal_goal=1, consult_handler=standin_consult))
    try:
        _, info = e.reset(options={"seed": 0})
        start = info["state"]["sectors"]
        rng = np.random.default_rng(0)
        ret = 0.0
        for _ in range(3000):
            _, r, term, trunc, info = e.step(int(rng.choice([0, 0, 0, 1, 2, 3, 4])))
            ret += r
            if term or trunc:
                break
        assert term and info["portal"], "a new sector ends the rollout"
        assert info["state"]["sectors"] == start + 1
        assert abs(ret - (15.0 - 0.01 * info["rollout_tics"])) < 1e-6, ret

        visited = e.visited()
        assert bin(int(visited[::-1], 16)).count("1") == start + 1
        data = e.save_state(4)
        e.install_state(4, data, visited)
        _, info = e.reset(options={"seed": 0, "slot": 4})
        assert info["state"]["sectors"] == start + 1, "the visited list survives the load"
        assert e.visited() == visited
    finally:
        e.close()


@pytest.mark.game
def test_map_tics():
    """MAP <tics> keeps the automap up that many game tics (plain MAP: 18)."""
    e = DiabloDoomEnv(EnvConfig(engine=EngineConfig(warp="1 1", extra_args=["--eval"]),
                                max_tics=100000, consult_handler=standin_consult))
    try:
        e.reset(options={"seed": 0})

        def shown_for(cmd):
            d, _ = e.engine.command(cmd)
            assert d["automap"] is True
            t = 0
            while d["automap"]:
                d, _ = e.engine.command("TURNL")     # 10 tics each
                t += d["tics"]
            return t
        assert 10 < shown_for("MAP") <= 20, "plain MAP: 18 tics"
        assert 60 < shown_for("MAP 70") <= 80
    finally:
        e.close()
