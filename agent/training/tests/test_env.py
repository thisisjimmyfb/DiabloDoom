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
                        consult_false=0, key=0, time=0, portal=0, hp_loss=-0.2, death=-100.0,
                        loot=0, health=0)
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


def test_pickup_rewards():
    """Pickup events: loot and armor pay `loot` per item, health pays
    `health` per HP healed, keys and other things pay nothing here (keys
    pay through the state's key mask). Engine faked: no game needed."""
    only = RewardConfig(sector=0, exit=0, mechanism=0, consult_hit=0, trigger_miss=0,
                        consult_false=0, key=0, time=0, portal=0, hp_loss=0, death=0,
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
        for _ in range(500):
            if info["mode"] != "EXPLORE":
                break
            _, _, term, trunc, info = e.step(int(rng.choice([0, 0, 0, 1, 2])))
            if term or trunc:
                _, info = e.reset(options={"seed": 0})
        else:
            pytest.fail("no combat in 500 random steps")
        before = info["state"]
        _, r, _, _, info = e.step(0)                      # WALK in combat: dropped
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
        e = make_env(engine=EngineConfig(warp="1 1", extra_args=["-nomonsters", "-rlspawn", "60"]),
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


def test_goal_dist():
    """The distance penalty's goal: the nearer of the exit line and the
    nearest frontier portal; -1 (none) and missing fields are ignored."""
    from env.env import goal_dist
    assert goal_dist({"exit_dist": 900, "frontier_dist": 40}) == 40
    assert goal_dist({"exit_dist": 30, "frontier_dist": 40}) == 30
    assert goal_dist({"exit_dist": 900, "frontier_dist": -1}) == 900
    assert goal_dist({"exit_dist": -1, "frontier_dist": -1}) is None
    assert goal_dist({"mode": "INTERMISSION"}) is None


@pytest.mark.game
def test_time_distance_penalty():
    """E1M1: the state line carries exit_dist and frontier_dist, and with
    every other reward off each step costs time * its tics * (1 + d /
    far_scale), d the mean of its start and end goal distances."""
    only = RewardConfig(sector=0, exit=0, mechanism=0, consult_hit=0,
                        trigger_miss=0, consult_false=0, key=0, hp_loss=0, death=0, loot=0, health=0,
                        time=1.0, far_scale=256.0)
    e = make_env(engine=EngineConfig(warp="1 1", extra_args=["-nomonsters"]), rewards=only,
                 max_tics=100000, consult_handler=standin_consult)
    try:
        from env.env import goal_dist
        _, info = e.reset(options={"seed": 0})
        s = info["state"]
        assert s["exit_dist"] > 0 and s["frontier_dist"] >= 0 and s["keys"] == 0
        rng = np.random.default_rng(2)
        expect, got, prev = 0.0, 0.0, goal_dist(s)
        for _ in range(200):
            _, r, term, trunc, info = e.step(int(rng.choice([0, 0, 1, 2, 3, 4])))
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
    first new sector, and with only time and portal rewards on its return
    is the bonus minus time * tics. Its end state saved with its visited
    list (VISITED) and reinstalled restores that visited count after the
    load (VISIT), so the next rollout's goal is a sector beyond it."""
    only = RewardConfig(sector=0, exit=0, mechanism=0, consult_hit=0,
                        trigger_miss=0, consult_false=0, key=0, hp_loss=0, death=0, loot=0, health=0,
                        time=0.01, far_scale=1e12, portal=5.0)   # time only
    e = DiabloDoomEnv(EnvConfig(engine=EngineConfig(warp="1 1", extra_args=["-nomonsters"]), rewards=only,
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
        assert abs(ret - (5.0 - 0.01 * info["rollout_tics"])) < 1e-6, ret

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
def test_forced_map_glance():
    """force_map_every=2: after every 2nd step the env sends MAP itself --
    the next frame is the automap (state automap true) and nothing is
    charged."""
    only = RewardConfig(sector=0, exit=0, mechanism=0, consult_hit=0,
                        trigger_miss=0, consult_false=0, key=0, hp_loss=0, death=0, loot=0, health=0, time=0)
    e = DiabloDoomEnv(EnvConfig(engine=EngineConfig(warp="1 1", extra_args=["-nomonsters"]), rewards=only,
                                max_tics=100000, force_map_every=2, consult_handler=standin_consult))
    tl = ACTIONS.index("TURNL")
    try:
        _, info = e.reset(options={"seed": 0})
        assert info["state"]["automap"] is False
        forced = []
        for _ in range(6):
            obs, r, *_, info = e.step(tl)
            forced.append(info["forced_map"])
            assert r == 0, "a forced glance is never charged"
            if info["forced_map"]:
                assert info["state"]["automap"] is True
                assert (obs[-1] < 8).mean() > 0.5, "the frame the policy sees is the automap"
        assert forced == [False, True] * 3 or forced == [False, True, False, False, True, False], forced
        e.set_force_map(0)
        for _ in range(4):
            _, _, _, _, info = e.step(tl)
            assert not info["forced_map"]
    finally:
        e.close()


@pytest.mark.game
def test_map_tics():
    """MAP <tics> keeps the automap up that many game tics (plain MAP: 18),
    and a forced glance uses force_map_tics."""
    e = DiabloDoomEnv(EnvConfig(engine=EngineConfig(warp="1 1", extra_args=["-nomonsters"]),
                                max_tics=100000, consult_handler=standin_consult))
    tl = ACTIONS.index("TURNL")
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
        e.set_force_map(1, 40)
        _, _, _, _, info = e.step(tl)
        assert info["forced_map"] and info["state"]["automap"]
        t = 0
        while True:
            _, _, _, _, info = e.step(tl)
            t += info["tics"] - (1 if info["forced_map"] else 0)
            if info["forced_map"] or not info["state"]["automap"]:
                break
        assert 30 < t <= 50, t
    finally:
        e.close()
