"""DiabloDoomEnv: the Gymnasium environment (SPEC.md §5).

The policy sees pixels only (84x84 grayscale, last 4 decisions). Engine
state from the STATE line drives rewards, episode ends and CONSULT
routing, and is exposed in `info["state"]` for logging and the critic,
never in the observation.
"""

from __future__ import annotations

import math
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
    "MAP",
    "CONSULT",
]
CONSULT = ACTIONS.index("CONSULT")
MAP = ACTIONS.index("MAP")
EXPLORE = "EXPLORE"
COMBAT_MODES = {"PLANNING", "TARGETING", "CONFIRM", "PULSE", "REACTION", "ROUND_END"}


@dataclass
class RewardConfig:
    """SPEC.md §5.3. Magnitudes are placeholders, to be tuned from curves."""
    sector: float = 1.0               # per newly visited sector
    exit: float = 50.0                # level exit reached (ends the rollout)
    mechanism: float = 2.0            # USE that sets a new mechanism going (once per mechanism)
    death: float = -20.0              # ends the rollout
    stuck: float = -1.0               # per stuck_tics without moving stuck_dist
    stuck_tics: int = 105
    stuck_dist: int = 16
    consult_hit: float = 5.0          # first CONSULT on an explore trigger
    trigger_miss: float = -8.0        # trigger unanswered after the grace window
    grace_decisions: int = 5
    consult_false: float = -0.5       # CONSULT with no live, unanswered trigger
    combat_unconsulted_step: float = -0.5  # per step in combat before CONSULT
    hp_loss: float = 0.0              # per HP lost (third stage only)
    map_toggle: float = 0.0           # per MAP action (only if flicker appears)


# A CONSULT handler answers a CONSULT: it may drive the engine with any
# driver commands and returns the decision it ends on. `kinds` names the
# live triggers ("monster", "backpack", "combat"; empty for a false
# consult). On the farm this signals Muse VM; in training a stand-in.
ConsultHandler = Callable[["DiabloDoomEnv", dict, list], dict]


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
    # warp start is saved to bootstrap_slot once and reloaded.
    start_slots: list[int] = field(default_factory=list)
    bootstrap_slot: int = 5
    max_combat_steps: int = 20        # steps in combat before truncation
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
        self._bootstrapped = False
        # Per-env reset request for vector envs:
        #   envs.set_attr("next_reset", [{"slot": 3, "seed": 17, "group": 0}, ...])
        self.next_reset: dict | None = None
        self._installed: dict[int, bytes | None] = {}   # install_state slots
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
        return path.read_bytes() if path.exists() else None

    def install_state(self, slot: int, data: bytes | None) -> None:
        """Put a savegame (another instance's save_state) in a slot of
        this instance, for reset(options={"slot": slot}); None removes it.
        Kept, and written again whenever the game (re)starts: the game
        starts at the first reset, and a crashed one gets a new savedir."""
        self._installed[slot] = data
        if self.engine.workdir is not None:
            self._write_installed(slot)

    def _write_installed(self, slot: int) -> None:
        path = self.engine.savedir / f"doomsav{slot}.dsg"
        data = self._installed[slot]
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
            for slot in self._installed:
                self._write_installed(slot)
            self._pre = Preprocessor(self.engine.width, self.engine.height, self.cfg.size)
            self._bootstrapped = False

        slot = opts.get("slot")
        if slot is None and self.cfg.start_slots:
            slot = int(self.np_random.choice(self.cfg.start_slots))
        if slot is None:
            if not self._bootstrapped:
                # First episode on a fresh process: save the warp start
                # so later resets return to it.
                self.engine.command(f"SAVE {self.cfg.bootstrap_slot}")
                self._bootstrapped = True
            slot = self.cfg.bootstrap_slot
        rng_seed = opts.get("seed")
        if rng_seed is None:
            rng_seed = int(self.np_random.integers(0, 256))
        # LOAD takes effect at the next tic; SEED is applied before that
        # tic's game logic, and the decision after it carries the first
        # frame rendered from the loaded state.
        self.engine.command(f"LOAD {slot}")
        d, _ = self.engine.command(f"SEED {int(rng_seed) % 256}")

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
        self._prev_hp = d.get("hp", 0)
        self._anchor = (d.get("x", 0), d.get("y", 0))
        self._stuck_acc = 0
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
        reward = 0.0
        prev = self.state
        in_combat = prev.get("mode") in COMBAT_MODES
        self._handler_tics = 0
        self._handler_events: list = []
        events: list = []

        if a == CONSULT:
            kinds = [k for k, t in self._trig.items() if t["active"] and not t["consulted"]]
            if not kinds:
                reward += rc.consult_false
            for k in kinds:
                if k in ("monster", "backpack"):
                    reward += rc.consult_hit
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
            if a == MAP:
                reward += rc.map_toggle

        self.state = d
        self._tics += tics

        # Combat before CONSULT: per-step penalty; capped (frozen world).
        if d.get("mode") in COMBAT_MODES:
            self._combat_steps += 1
            if not self._trig["combat"]["consulted"]:
                reward += rc.combat_unconsulted_step
        else:
            self._combat_steps = 0

        # Exploration: newly visited sectors.
        sec = d.get("sectors", self._best_sectors)
        if sec > self._best_sectors:
            reward += rc.sector * (sec - self._best_sectors)
            self._best_sectors = sec

        # Stuck: no displacement of stuck_dist over stuck_tics game tics.
        if "x" in d:
            dx, dy = d["x"] - self._anchor[0], d["y"] - self._anchor[1]
            if math.hypot(dx, dy) >= rc.stuck_dist:
                self._anchor = (d["x"], d["y"])
                self._stuck_acc = 0
            else:
                self._stuck_acc += tics
                if self._stuck_acc >= rc.stuck_tics:
                    reward += rc.stuck
                    self._stuck_acc = 0

        # HP loss (third stage only; 0 by default).
        hp = d.get("hp", self._prev_hp)
        if hp < self._prev_hp:
            reward += rc.hp_loss * (self._prev_hp - hp)
        self._prev_hp = hp

        # Triggers: start/age/end, and the grace-window miss penalty.
        reward += self._update_triggers(d)

        # Mechanisms: a USE that sets a door, lift, floor, switch...
        # going pays once per mechanism per rollout (SPEC.md §5.3), so
        # reusing the same door or switch never pays again.
        if self._new_mechanisms(events):
            reward += rc.mechanism

        terminated = truncated = False
        exited = any(e.get("event") == "exit" for e in events)
        if exited:
            reward += rc.exit
            terminated = True
        elif d.get("dead"):
            reward += rc.death
            terminated = True
        if not terminated:
            if self._tics >= self.cfg.max_tics:
                truncated = True
            elif self._combat_steps >= self.cfg.max_combat_steps:
                truncated = True

        obs = self._stack.push(self._frame()) if sent else self._stack.get()
        info = self._info(tics, d)
        info["exit"] = exited
        return obs, float(reward), terminated, truncated, info

    def render(self):
        if self.render_mode == "rgb_array":
            return self.engine.frame_rgb()
        return None

    def close(self):
        self.engine.close()

    # -- internals -------------------------------------------------
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
