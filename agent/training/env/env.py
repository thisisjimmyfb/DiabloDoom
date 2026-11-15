"""DiabloDoomEnv: the Gymnasium environment (SPEC.md §5).

The policy sees pixels only (84x84 grayscale, last 4 decisions). Engine
state from the STATE line drives rewards, episode ends and CONSULT
routing, and is exposed in `info["state"]` for logging and the critic,
never in the observation.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Callable, Optional

import gymnasium as gym
import numpy as np
from gymnasium import spaces

from .engine import Engine, EngineConfig, EngineError
from .obs import FrameStack, Preprocessor

# SPEC.md §5.2. Index = action id.
ACTIONS = [
    # Forward step only: turning is TURNL/TURNR (smooth, 5 deg/tic).
    # WALK <angle> snaps the facing in one tic, so the policy never
    # sends an angle.
    "WALK 0",
    "TURNL", "TURNR",
    "STRAFE_L", "STRAFE_R",
    "USE",
    # No MAP (removed 2026-11-14): the policy never opens the map itself;
    # the env's forced glances (EnvConfig.force_map_every) do.
    "CONSULT",
]
CONSULT = ACTIONS.index("CONSULT")
EXPLORE = "EXPLORE"
COMBAT_MODES = {"PLANNING", "TARGETING", "CONFIRM", "PULSE", "REACTION", "ROUND_END"}


@dataclass
class RewardConfig:
    """SPEC.md §5.3. Magnitudes are placeholders, to be tuned from curves."""
    sector: float = 1.0               # per newly visited sector
    exit: float = 100.0               # level complete triggered (ends the rollout): the top reward
    death: float = -100.0             # the player died (ends the rollout): as big as the exit
    mechanism: float = 10.0           # USE that sets a new mechanism going (once per mechanism)
    key: float = 25.0                 # per key colour newly held (blue, yellow, red; once each)
    # Pickups, from the engine's pickup events: each Diablo item picked
    # up (or armor, which also grants one) pays `loot`; a health pickup
    # pays `health` per HP it actually healed (a medikit 25 HP, a bonus
    # 1), the mirror of hp_loss. A thing left on the ground pays nothing.
    loot: float = 5.0
    health: float = 0.2
    consult_hit: float = 5.0          # first CONSULT on an explore trigger
    trigger_miss: float = -8.0        # trigger unanswered after the grace window
    grace_decisions: int = 5
    consult_false: float = -0.5       # CONSULT with no live, unanswered trigger
    # Time and distance, one penalty: every game tic costs
    #   time * (1 + d / far_scale)
    # where d is the distance (map units) to the nearest goal -- an exit
    # line or a frontier portal (a passable opening from a visited sector
    # into an unvisited one), the mean of the step's start and end -- so
    # the clock runs at `time` beside a goal and twice as fast far_scale
    # units away: being far costs more the longer it lasts. 0 turns it
    # off. Only works with a portal goal (EnvConfig.portal_goal): rollouts
    # that all run to max_tics pay alike, and a group-relative advantage
    # cancels most of it.
    time: float = 0.0
    far_scale: float = 256.0
    portal: float = 5.0               # the portal goal reached (ends the rollout)
    # Per HP lost, whatever the cause: passive damage (nukage, slime,
    # crushers, barrels) in stage 1. Healing never pays; a death costs
    # the HP it lost on the way plus `death`.
    hp_loss: float = -0.2


# A CONSULT handler answers a CONSULT: it may drive the engine with any
# driver commands and returns the decision it ends on. `kinds` names the
# live triggers ("monster", "backpack", "combat"; empty for a false
# consult). On the farm this signals Muse VM; in training a stand-in.
ConsultHandler = Callable[["DiabloDoomEnv", dict, list], dict]


def goal_dist(state: dict) -> float | None:
    """Distance (map units) to the nearest exit line or frontier portal in
    a state line, or None when there is neither (or no level)."""
    ds = [state[k] for k in ("exit_dist", "frontier_dist") if state.get(k) is not None and state[k] >= 0]
    return float(min(ds)) if ds else None


def null_consult(env: "DiabloDoomEnv", state: dict, kinds: list) -> dict:
    """Does nothing; the policy keeps control."""
    return state


def standin_consult(env: "DiabloDoomEnv", state: dict, kinds: list,
                    max_rounds: int = 10, salvage_extra: int = 10) -> dict:
    """Training stand-in for Muse's VLM (SPEC.md §5.4): it clears what the
    policy consulted about, cheaply, so the policy learns *when* to hand
    off, not how to fight.
      - a monster sighting or combat: the CONSULT driver command (-rlenv
        only) removes the monsters in sight and, in combat, those
        targeting the player; in combat, END then closes the round (back
        to explore once no combatant is visible), up to max_rounds;
      - a full backpack: salvage the oldest items until it has room, plus
        salvage_extra more.
    A false consult (no live trigger) does nothing."""
    s = state
    if "backpack" in kinds and s.get("mode") == EXPLORE:
        extra = salvage_extra
        while s.get("bp_count", 0) > 0 and (s.get("backpack_full") or extra > 0):
            if not s.get("backpack_full"):
                extra -= 1
            s, _ = env.consult_command("SALVAGE 0")
    if "monster" in kinds or "combat" in kinds:
        s, _ = env.consult_command("CONSULT")
        rounds = 0
        while s["mode"] in COMBAT_MODES and not s.get("dead") and rounds < max_rounds:
            s, _ = env.consult_command("END")
            if s["mode"] in COMBAT_MODES:
                s, _ = env.consult_command("CONSULT")
            rounds += 1
    return s


@dataclass
class EnvConfig:
    engine: EngineConfig = field(default_factory=EngineConfig)
    rewards: RewardConfig = field(default_factory=RewardConfig)
    max_tics: int = 2100              # rollout length in game tics (60 s)
    frame_stack: int = 4
    size: int = 84
    # Reset start states: save slots (3-5 manual, 0-2 auto) in the
    # instance savedir (EngineConfig.savedir_template). Empty: the
    # bootstrap slot. The warp start is saved to bootstrap_slot when the
    # game starts and kept, so the slot holds the map start unless an
    # install_state replaces it (training's progress save), and holds
    # it again when that is removed.
    start_slots: list[int] = field(default_factory=list)
    bootstrap_slot: int = 4
    max_combat_steps: int = 20        # steps in combat before truncation
    # n > 0: the rollout ends (terminated, + RewardConfig.portal) once it
    # has entered n sectors not yet visited -- through n portals. A start
    # installed with a visited list (install_state) counts those as
    # visited, so going back through the portal it came in by is not new.
    portal_goal: int = 0
    # n > 0: a forced map glance every n actions, done by the engine
    # (FORCEMAP, sent when the game starts): after every n gameplay
    # commands the next decision shows the automap for force_map_tics,
    # so it is in the frame stack and the vision layers learn to read it.
    force_map_every: int = 0
    force_map_tics: int = 1           # game tics a forced glance keeps the map up (MAP <tics>): 1 = only the next frame
    consult_handler: Optional[ConsultHandler] = None


class DiabloDoomEnv(gym.Env):
    metadata = {"render_modes": ["rgb_array"]}
    _instances = 0

    def __init__(self, config: EnvConfig | None = None, render_mode: str | None = None,
                 **overrides):
        super().__init__()
        cfg = config or EnvConfig()
        for k, v in overrides.items():
            setattr(cfg, k, v)
        self.cfg = cfg
        self.render_mode = render_mode
        self.consult_handler = cfg.consult_handler or null_consult
        DiabloDoomEnv._instances += 1
        self.engine = Engine(cfg.engine, tag=str(DiabloDoomEnv._instances))
        self.observation_space = spaces.Box(0, 255, (cfg.frame_stack, cfg.size, cfg.size), np.uint8)
        self.action_space = spaces.Discrete(len(ACTIONS))
        self._pre: Preprocessor | None = None
        self._stack = FrameStack(cfg.frame_stack, cfg.size)
        self._map_start: bytes | None = None    # the warp start, saved in bootstrap_slot
        # Per-env reset request for vector envs:
        #   envs.set_attr("next_reset", [{"slot": 3, "seed": 17, "group": 0}, ...])
        self.next_reset: dict | None = None
        self._installed: dict[int, bytes | None] = {}   # install_state slots
        self._installed_visited: dict[int, str | None] = {}  # their visited lists (VISITED hex)
        self.state: dict = {}

    # -- engine access for CONSULT handlers ------------------------
    def consult_command(self, line: str) -> tuple[dict, list]:
        """Driver command sent by a CONSULT handler; its tics count
        against the rollout budget and its events are scored."""
        d, others = self.engine.command(line)
        self._handler_tics += max(d.get("tics", 0), 0)
        self._handler_events.extend(others)
        return d, others

    # -- start states (frontier starts, SPEC.md §5.4) ----------------
    def save_state(self, slot: int) -> bytes | None:
        """Save the game as it is now (normally where the last rollout
        ended) to a manual slot (3-5) and return the savegame's bytes;
        None if no save was written (e.g. not in a level)."""
        path = self.engine.savedir / f"doomsav{slot}.dsg"
        path.unlink(missing_ok=True)
        self.engine.command(f"SAVE {slot}")
        if path.exists():
            return path.read_bytes()
        if slot in self._installed or slot == self.cfg.bootstrap_slot:
            self._write_installed(slot)   # no save: put back what the slot held
        return None

    def visited(self) -> str | None:
        """The visited sectors now, as the engine's VISITED hex bitmask
        (with save_state, the visited list of a progress save)."""
        _, events = self.engine.command("VISITED")
        for e in events:
            if e.get("event") == "visited":
                return e.get("hex")
        return None

    def install_state(self, slot: int, data: bytes | None, visited: str | None = None) -> None:
        """Put a savegame (another instance's save_state) in a slot of
        this instance, for reset(options={"slot": slot}); None removes it
        (the bootstrap slot then holds the map start again). visited (a
        visited() bitmask) is marked after every reset from the slot.
        Kept, and written again whenever the game (re)starts: the game
        starts at the first reset, and a crashed one gets a new savedir."""
        self._installed[slot] = data
        self._installed_visited[slot] = visited if data is not None else None
        if self.engine.workdir is not None:
            self._write_installed(slot)

    def swap_slots(self, a: int, b: int) -> None:
        """Swap two slots' savegames and visited lists (training: the new
        progress saved to 5 becomes the start in 4, the old start goes
        to 5). The bootstrap slot without an installed save holds the
        map start, and swaps as that."""
        def held(slot):
            data = self._installed.get(slot)
            if data is None and slot == self.cfg.bootstrap_slot:
                data = self._map_start
            return data, self._installed_visited.get(slot)
        (da, va), (db, vb) = held(a), held(b)
        self._installed[a], self._installed_visited[a] = db, vb
        self._installed[b], self._installed_visited[b] = da, va
        if self.engine.workdir is not None:
            self._write_installed(a)
            self._write_installed(b)

    def _write_installed(self, slot: int) -> None:
        path = self.engine.savedir / f"doomsav{slot}.dsg"
        data = self._installed.get(slot)
        if data is None and slot == self.cfg.bootstrap_slot:
            data = self._map_start
        if data is None:
            path.unlink(missing_ok=True)
        else:
            tmp = path.with_suffix(".tmp")
            tmp.write_bytes(data)
            tmp.replace(path)

    # -- gym API ---------------------------------------------------
    def reset(self, *, seed: int | None = None, options: dict | None = None):
        super().reset(seed=seed)
        opts = dict(options or {})
        if self.next_reset:
            opts.update(self.next_reset)
            self.next_reset = None

        if self.engine.proc is None or self.engine.proc.poll() is not None:
            self.engine.start()
            # Save the warp start first: an installed save for the same
            # slot is written over it below, and the copy kept here
            # comes back when that save is removed.
            boot = self.engine.savedir / f"doomsav{self.cfg.bootstrap_slot}.dsg"
            boot.unlink(missing_ok=True)
            self.engine.command(f"SAVE {self.cfg.bootstrap_slot}")
            self._map_start = boot.read_bytes() if boot.exists() else None
            for slot in self._installed:
                self._write_installed(slot)
            # Forced map glances run in the engine (FORCEMAP).
            self.engine.command(f"FORCEMAP {self.cfg.force_map_every} {self.cfg.force_map_tics}")
            self._pre = Preprocessor(self.engine.width, self.engine.height, self.cfg.size)

        slot = opts.get("slot")
        if slot is None and self.cfg.start_slots:
            slot = int(self.np_random.choice(self.cfg.start_slots))
        if slot is None:
            slot = self.cfg.bootstrap_slot
        rng_seed = opts.get("seed")
        if rng_seed is None:
            rng_seed = int(self.np_random.integers(0, 256))
        # LOAD takes effect at the next tic; SEED is applied before that
        # tic's game logic, and the decision after it carries the first
        # frame rendered from the loaded state.
        self.engine.command(f"LOAD {slot}")
        d, _ = self.engine.command(f"SEED {int(rng_seed) % 256}")
        # A progress save's visited list (the save doesn't store it; the
        # load reset it): VISIT in pieces that fit a command line.
        vis = self._installed_visited.get(slot)
        if vis:
            for off in range(0, len(vis), 200):
                d, _ = self.engine.command(f"VISIT {off} {vis[off:off + 200]}")

        # Optional warm-up: explore actions played after LOAD + SEED and
        # before the rollout starts (not scored, not counted against
        # max_tics). Deterministic, so a GRPO group given the same list
        # starts from the same varied state. Stops early if the player
        # leaves explore mode or dies.
        # Mechanisms used during the warm-up count as already used: the
        # reward is for setting something going within the rollout.
        self._used: set = set()
        for wa in opts.get("warmup") or []:
            if d.get("mode") != EXPLORE or d.get("dead") or int(wa) == CONSULT:
                break
            d, wevents = self.engine.command(ACTIONS[int(wa)])
            self._new_mechanisms(wevents)

        self.state = d
        self._group = opts.get("group")
        self._slot, self._seed = slot, int(rng_seed) % 256
        self._tics = 0
        self._best_sectors = d.get("sectors", 0)
        self._start_sectors = self._best_sectors
        self._goal = goal_dist(d)
        self._keys = d.get("keys", 0)     # keys held at the start (or from the warm-up) never pay
        self._prev_hp = d.get("hp", 0)
        self._combat_steps = 0
        self._trig = {k: self._new_trigger() for k in ("monster", "backpack", "combat")}
        self._update_triggers(d)
        obs = self._stack.reset(self._frame())
        return obs, self._info(0, d)

    def step(self, action):
        a = int(action)
        if not self.action_space.contains(a):
            raise ValueError(f"invalid action {action!r}")
        rc = self.cfg.rewards
        terms: dict[str, float] = {}      # the reward by term (info["terms"], for logs)

        def add(term: str, value: float) -> None:
            if value:
                terms[term] = terms.get(term, 0.0) + value
        prev = self.state
        in_combat = prev.get("mode") in COMBAT_MODES
        self._handler_tics = 0
        self._handler_events: list = []
        events: list = []

        if a == CONSULT:
            kinds = [k for k, t in self._trig.items() if t["active"] and not t["consulted"]]
            if not kinds:
                add("consult", rc.consult_false)
            for k in kinds:
                if k in ("monster", "backpack"):
                    add("consult", rc.consult_hit)
                self._trig[k]["consulted"] = True
            d = self.consult_handler(self, prev, kinds)
            if d.get("mode") in COMBAT_MODES and not self._trig["combat"]["consulted"]:
                # The handler took the fight on (or left it unfinished):
                # this combat is consulted, no per-step penalty for it.
                self._trig["combat"] = dict(self._new_trigger(), active=True, consulted=True)
            events = self._handler_events
            tics = self._handler_tics
            sent = True
        elif in_combat:
            # Combat: everything but CONSULT is dropped (SPEC.md §5.2).
            d, tics, sent = prev, 0, False
        else:
            d, events = self.engine.command(ACTIONS[a])
            tics = max(d.get("tics", 0), 0)
            sent = True

        self.state = d
        self._tics += tics

        # Steps in combat (capped: the world is frozen until CONSULT).
        if d.get("mode") in COMBAT_MODES:
            self._combat_steps += 1
        else:
            self._combat_steps = 0

        # Time and distance: every tic costs time * (1 + d / far_scale),
        # d the distance to the nearest exit line or frontier portal.
        sec = d.get("sectors", self._best_sectors)
        goal = goal_dist(d)
        if tics > 0 and rc.time:
            known = [g for g in (self._goal, goal) if g is not None]
            dist = sum(known) / len(known) if known else 0.0
            add("time", -rc.time * tics * (1.0 + dist / rc.far_scale))
        if goal is not None or "x" in d:
            self._goal = goal

        # Exploration: newly visited sectors.
        if sec > self._best_sectors:
            add("sector", rc.sector * (sec - self._best_sectors))
            self._best_sectors = sec

        # Triggers: start/age/end, and the grace-window miss penalty.
        add("consult", self._update_triggers(d))

        # Mechanisms: a USE that sets a door, lift, floor, switch...
        # going pays once per mechanism per rollout (SPEC.md §5.3), so
        # reusing the same door or switch never pays again.
        if self._new_mechanisms(events):
            add("mechanism", rc.mechanism)

        # Keys: each colour pays once per rollout, when first held.
        add("key", rc.key * self._new_keys(d))

        # Pickups: loot and armor per item, health per HP healed.
        for e in events:
            if e.get("event") == "pickup":
                if e.get("kind") in ("loot", "armor"):
                    add("loot", rc.loot)
                elif e.get("kind") == "health":
                    add("health", rc.health * max(int(e.get("hp", 0)), 0))

        # HP lost (a death to 0 included); healing just moves the mark up.
        hp = max(d.get("hp", self._prev_hp), 0)
        if hp < self._prev_hp:
            add("hp_loss", rc.hp_loss * (self._prev_hp - hp))
        self._prev_hp = hp

        terminated = truncated = False
        portal = False
        exited = any(e.get("event") == "exit" for e in events)
        end = None
        if exited:
            add("exit", rc.exit)
            terminated, end = True, "exit"
        elif d.get("dead"):
            add("death", rc.death)
            terminated, end = True, "death"
        elif self.cfg.portal_goal > 0 and self._best_sectors - self._start_sectors >= self.cfg.portal_goal:
            add("portal", rc.portal)
            terminated = portal = True
            end = "portal"
        if not terminated:
            if self._tics >= self.cfg.max_tics:
                truncated, end = True, "time"
            elif self._combat_steps >= self.cfg.max_combat_steps:
                truncated, end = True, "combat"
        reward = sum(terms.values())

        obs = self._stack.push(self._frame()) if sent else self._stack.get()
        info = self._info(tics, d)
        info["exit"] = exited
        info["portal"] = portal
        info["forced_map"] = bool(d.get("forced_map"))   # the engine's forced glance (FORCEMAP)
        info["terms"] = terms             # the reward by term
        info["end"] = end                 # why the rollout ended: exit, death, portal, time, combat
        info["max_tics"] = self.cfg.max_tics
        return obs, float(reward), terminated, truncated, info

    def set_max_tics(self, n: int) -> None:
        """Change the rollout limit in game tics; it takes effect from the
        next rollout (training's per-map tic budget, model.TicBudget)."""
        self.cfg.max_tics = int(n)

    def set_force_map(self, every: int, tics: int | None = None) -> None:
        """Change the forced map glance period (0: off) and optionally how
        many tics a forced glance shows the map (the engine's FORCEMAP)."""
        self.cfg.force_map_every = int(every)
        if tics is not None:
            self.cfg.force_map_tics = int(tics)
        if self.engine.proc is not None and self.engine.proc.poll() is None:
            self.engine.command(f"FORCEMAP {self.cfg.force_map_every} {self.cfg.force_map_tics}")

    def render(self):
        if self.render_mode == "rgb_array":
            return self.engine.frame_rgb()
        return None

    def close(self):
        self.engine.close()

    # -- internals -------------------------------------------------
    def _new_keys(self, state: dict) -> int:
        """Record the key colours in this state's "keys" mask (1 blue,
        2 yellow, 4 red) and return how many were not held before."""
        held = state.get("keys", self._keys)
        new = held & ~self._keys
        self._keys |= held
        return bin(new).count("1")

    def _new_mechanisms(self, events: list) -> bool:
        """Record the mechanisms in this step's "use" events (the sectors
        a USE set moving and the switch lines it flipped). True if any of
        them is new this rollout."""
        new = False
        for e in events:
            if e.get("event") != "use":
                continue
            keys = {("sector", s) for s in e.get("sectors", [])} | {("line", n) for n in e.get("lines", [])}
            if keys - self._used:
                new = True
            self._used |= keys
        return new

    def _frame(self) -> np.ndarray:
        pix, pal, _ = self.engine.frame()
        return self._pre(pix, pal)

    # Absolute counters that keep running across resets; left out of
    # info["state"] so identical rollouts give identical infos (they
    # stay available on env.state).
    _PROCESS_KEYS = ("gametic", "frame")

    def _info(self, tics: int, d: dict) -> dict:
        state = {k: v for k, v in d.items() if k not in self._PROCESS_KEYS}
        return {"tics": tics, "rollout_tics": self._tics, "mode": d.get("mode"),
                "group": self._group, "slot": self._slot, "seed": self._seed, "state": state}

    @staticmethod
    def _new_trigger() -> dict:
        return {"active": False, "consulted": False, "age": 0, "penalized": False}

    @staticmethod
    def _monster_visible(d: dict) -> bool:
        """A monster seen before it acquires the player, while exploring:
        the engine's monstervis field (a living monster among the visible
        targets; enemies are frozen in explore and an acquisition starts
        combat at once). Older engines without it: any visible target."""
        if d.get("mode") != EXPLORE:
            return False
        if "monstervis" in d:
            return bool(d["monstervis"])
        return len(d.get("targets", [])) > 0

    def _update_triggers(self, d: dict) -> float:
        rc = self.cfg.rewards
        live = {
            "monster": self._monster_visible(d),
            "backpack": bool(d.get("backpack_full", False)),
            "combat": d.get("mode") in COMBAT_MODES,
        }
        penalty = 0.0
        for k, on in live.items():
            t = self._trig[k]
            if on and not t["active"]:
                self._trig[k] = t = {"active": True, "consulted": False, "age": 0, "penalized": False}
            elif on:
                t["age"] += 1
            else:
                t["active"] = False
            if (t["active"] and not t["consulted"] and not t["penalized"]
                    and t["age"] >= rc.grace_decisions):
                penalty += rc.trigger_miss
                t["penalized"] = True
        return penalty
