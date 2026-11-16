"""DiabloDoomEnv: the Gymnasium environment (SPEC.md §5).

The policy sees pixels only (84x84 grayscale, last 4 decisions). Engine
state from the STATE line drives rewards, episode ends and CONSULT
routing, and is exposed in `info["state"]` for logging and the critic,
never in the observation.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Callable, Optional

import collections

import gymnasium as gym
import numpy as np
from gymnasium import spaces

from .engine import Engine, EngineConfig, EngineError
from .obs import FrameStack, Preprocessor

# SPEC.md §5.2. Index = action id. A decision every game tic
# (2026-11-15): each action is the keys held for the next tic, the way a
# keyboard is played. PRESS <keys>: W/S run forward/back, A/D strafe,
# L/R turn (about 7 degrees a tic), every combination of one key from
# each pair or none: 27, "PRESS" alone (nothing held) first. Then USE
# and CONSULT. The engine's multi-tic commands (WALK, GOTO, the 45-degree
# TURNL/TURNR, ...) stay for the farm driver; the policy doesn't send them.
# No MAP (removed 2026-11-14): the env's forced glances show the map.
ACTIONS = [("PRESS " + f + s + t).rstrip()
           for f in ("", "W", "S") for s in ("", "A", "D") for t in ("", "L", "R")] + ["USE", "CONSULT"]
CONSULT = ACTIONS.index("CONSULT")
USE = ACTIONS.index("USE")
# Each action's forward/back key: +1 W, -1 S, 0 none (strafes, A/D,
# aren't tracked).
MOVE_DIR = [(1 if "W" in a[6:] else -1 if "S" in a[6:] else 0) if a.startswith("PRESS") else 0
            for a in ACTIONS]

EXPLORE = "EXPLORE"
COMBAT_MODES = {"PLANNING", "TARGETING", "CONFIRM", "PULSE", "REACTION", "ROUND_END"}


@dataclass
class RewardConfig:
    """SPEC.md §5.3. Magnitudes are placeholders, to be tuned from curves."""
    # Per newly visited sector: more than a mechanism, since getting
    # somewhere new is the goal and a door only the way there. With the
    # portal goal (training) the first new sector also ends the rollout.
    sector: float = 15.0
    exit: float = 100.0               # level complete triggered (ends the rollout): the top reward
    death: float = -100.0             # the player died (ends the rollout): as big as the exit
    mechanism: float = 10.0           # USE that sets a new mechanism going (once per mechanism)
    # A USE that sets nothing going (no engine "use" event: a plain wall,
    # empty space) costs use_miss (2026-11-16), so pressing it at random
    # costs more than standing still. Opening a door, even one used
    # before, costs nothing. -0.05 is about 5 tics of the clock's base.
    use_miss: float = -0.05
    key: float = 25.0                 # per key colour newly held (blue, yellow, red; once each)
    # Pickups, from the engine's pickup events: each Diablo item picked
    # up (or armor, which also grants one) pays `loot`; a health pickup
    # pays `health` per HP it actually healed (a medikit 25 HP, a bonus
    # 1), the mirror of hp_loss. A thing left on the ground pays nothing.
    loot: float = 5.0
    health: float = 0.2
    consult_hit: float = 5.0          # first CONSULT on an explore trigger
    trigger_miss: float = -8.0        # trigger unanswered after the grace window
    grace_tics: int = 150             # a monster / full backpack: game tics to CONSULT
    grace_decisions: int = 5          # combat (frozen, no tics pass): steps to CONSULT
    consult_false: float = -1.5       # CONSULT with no live, unanswered trigger
    # Time and distance, one penalty: every game tic costs
    #   time * (1 + d / far_scale)
    # where d is the distance (map units) to the nearest portal of the
    # player's current sector (2026-11-16: a passable two-sided line into
    # any other sector, visited or not; goal_dist), the mean of the step's
    # start and end -- so the clock runs at `time` beside a way out of the
    # room and twice as fast far_scale units away: being far costs more
    # the longer it lasts. No portal (a sealed sector): base rate. 0 turns it
    # off. Only works with a portal goal (EnvConfig.portal_goal): rollouts
    # that all run to max_tics pay alike, and a group-relative advantage
    # cancels most of it.
    time: float = 0.0
    far_scale: float = 256.0
    # High velocity: every game tic on which the player actually moved at
    # least fast_speed map units (position, not momentum: pushing into a
    # wall never counts) pays `fast`. The engine counts the tics
    # (-rlfast, the state line's fast_tics; the env adds the flag). The
    # default equals the time penalty's base rate (1/3 per second, per
    # tic; model.make_env ties it to --time-penalty), so a fast tic pays
    # back the clock's base and only the distance part (d / far_scale)
    # is still charged: moving fast is nearly free, standing still and
    # bumping into walls cost the full clock. Running (PRESS W) passes 4
    # units/tic on its third tic from rest. 0 turns it off. (It replaced
    # the per-step straight-movement reward, 2026-11-15: a step is one
    # tic now, and this pays the unimpeded movement tic by tic.)
    fast: float = 1 / 3 / 35
    fast_speed: int = 4
    # Looking toward a way on (2026-11-16): every tic pays
    #   look * max(0, cos(a))
    # a the angle between the player's facing and the closest point of
    # the nearest portal of the current sector into an unvisited sector
    # (the engine's portal_rel): full credit facing it, nothing facing
    # 90 degrees or more away, nothing when the sector has no such portal.
    # Only unvisited ones: right after a crossing the nearest portal is
    # usually the one just used, behind the player. The default is half
    # the clock's base rate (model.make_env ties it to --time-penalty), so
    # it can't pay for standing still. 0 turns it off.
    look: float = 1 / 3 / 35 / 2
    # Wasted rotation (2026-11-16): turning that gets undone. From the
    # facing in the state line, over the last turn_waste_window tics:
    # wasted = sum |dθ| - |sum dθ|, the turning that didn't change where
    # the player faces. Each tic adds what it wastes -- a turn against the
    # window's net turn S undoes min(|dθ|, |S|), wasting twice that -- and
    # costs turn_waste per 45 degrees of it. A steady turn either way, and
    # holding still, cost nothing; a wobble costs in proportion; a small
    # correction after a long sweep costs little. It replaced a fixed
    # charge per L/R reversal, which made always turning one way the free
    # habit (the policy drifted to 81% right turns). 0 turns it off.
    turn_waste: float = -0.005
    turn_waste_window: int = 8
    # The same for forward and back: W at most move_flip_window tics after
    # S, or S after W, costs move_flip. Strafing back and forth (A/D) is
    # free.
    move_flip: float = -0.015
    move_flip_window: int = 4
    # Falls (2026-11-16): a quick large drop usually means the player left
    # the path the map intends. A fall is an unbroken run of tics going
    # down that at some point drops faster than fall_speed units a tic
    # (only freefall does: lifts lower at most 8 a tic, a stair step is
    # 8-24 units with flat treads between) and totals at least fall_min
    # units; on landing it costs fall per fall_min units dropped (-1 for
    # 64, -2 for 128). Going up never costs; a teleport (a jump of more
    # than 64 units sideways) is no fall. 0 turns it off.
    fall: float = -1.0
    fall_min: float = 64.0
    fall_speed: float = 8.0
    # Staring at a wall up close (2026-11-16): every tic the first line the
    # player can't pass straight ahead (the engine's wall_dist) is nearer
    # than wall_look_dist units costs wall_look -- a view of bare wall
    # tells the policy nothing. Never while a usable wall is in reach (the
    # engine's wall_use: a switch or closed door within USE range and 45
    # degrees of the facing), so lining up on a switch or waiting at a
    # door is free. The default equals the clock's base rate (model.make_env
    # ties it to --time-penalty): staring at a wall costs the clock twice.
    # 0 turns it off.
    wall_look: float = -1 / 3 / 35
    wall_look_dist: float = 48.0
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
    """The time penalty's distance (map units, 2026-11-16): to the nearest
    portal of the player's current sector -- a passable two-sided line
    into any other sector, visited or not (the engine's portal_dist) --
    or None when the sector has none (or there is no level). It used to
    be the nearer of the exit line and the nearest frontier portal."""
    d = state.get("portal_dist")
    return float(d) if d is not None and d >= 0 else None


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
    # n > 0: the rollout ends (terminated; the sectors pay RewardConfig.sector) once it
    # has entered n sectors not yet visited -- through n portals. A start
    # installed with a visited list (install_state) counts those as
    # visited, so going back through the portal it came in by is not new.
    portal_goal: int = 0
    # True: picking up a key of a colour not held yet ends the rollout
    # (terminated, end "key"), so its end state can become the next start.
    key_goal: bool = False
    # No forced map glances (removed 2026-11-16): with 4 frames of memory
    # the policy can't use a map it saw a moment ago. The engine's
    # FORCEMAP / -rlforcemap stay, off unless sent.
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
        if cfg.rewards.fast and cfg.rewards.fast_speed > 0 and "-rlfast" not in cfg.engine.extra_args:
            cfg.engine.extra_args = [*cfg.engine.extra_args, "-rlfast", str(cfg.rewards.fast_speed)]
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
        # the last turn_waste_window turns (degrees), for RewardConfig.turn_waste
        self._turn_hist = collections.deque(maxlen=self.cfg.rewards.turn_waste_window)
        self._move_dir, self._move_tic = 0, -10 ** 9          # the last W/S key and its rollout tic
        self._fall_top, self._fall_peak = None, 0.0           # a fall in progress: its start z, fastest tic
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
        self._turn_hist = collections.deque(maxlen=self.cfg.rewards.turn_waste_window)
        self._move_dir, self._move_tic = 0, -10 ** 9
        self._fall_top, self._fall_peak = None, 0.0
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

        # High velocity: the tics the engine counted at fast_speed or more
        # (a dropped action re-reads the last state line: nothing new).
        if sent:
            add("fast", rc.fast * max(int(d.get("fast_tics", 0)), 0))
        # Looking toward the nearest unvisited portal of the current sector,
        # by the facing at the step's end (one tic, under per-tic decisions).
        if sent and rc.look and d.get("portal_rel") is not None:
            add("look", rc.look * tics * max(0.0, float(np.cos(np.radians(d["portal_rel"])))))
        # Staring at a wall up close (RewardConfig.wall_look).
        wd = d.get("wall_dist")
        if (sent and rc.wall_look and wd is not None and 0 <= wd < rc.wall_look_dist
                and not d.get("wall_use")):
            add("wall_look", rc.wall_look * tics)

        # Falls: charged on landing (RewardConfig.fall).
        if sent and rc.fall:
            add("fall", self._fall_step(prev, d, tics))

        # Wasted rotation, from the facing (RewardConfig.turn_waste).
        if sent and tics > 0 and rc.turn_waste:
            add("turn_waste", self._turn_waste_step(prev, d))

        # Forward/back reversals: this step's W/S key against the last one.
        # The key acts on the tic this step played (self._tics is its end).
        move = MOVE_DIR[a] if sent and a != CONSULT else 0
        if move:
            if (rc.move_flip and self._move_dir == -move
                    and self._tics - self._move_tic <= rc.move_flip_window):
                add("move_flip", rc.move_flip)
            self._move_dir, self._move_tic = move, self._tics

        # Exploration: newly visited sectors.
        if sec > self._best_sectors:
            add("sector", rc.sector * (sec - self._best_sectors))
            self._best_sectors = sec

        # Triggers: start/age/end, and the grace-window miss penalty.
        add("consult", self._update_triggers(d, tics))

        # Mechanisms: a USE that sets a door, lift, floor, switch...
        # going pays once per mechanism per rollout (SPEC.md §5.3), so
        # reusing the same door or switch never pays again.
        if self._new_mechanisms(events):
            add("mechanism", rc.mechanism)
        elif (a == USE and sent and rc.use_miss
              and not any(e.get("event") == "use" for e in events)):
            add("use_miss", rc.use_miss)

        # Keys: each colour pays once per rollout, when first held.
        new_keys = self._new_keys(d)
        add("key", rc.key * new_keys)

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
        portal = got_key = False
        exited = any(e.get("event") == "exit" for e in events)
        end = None
        if exited:
            add("exit", rc.exit)
            terminated, end = True, "exit"
        elif d.get("dead"):
            add("death", rc.death)
            terminated, end = True, "death"
        elif self.cfg.key_goal and new_keys:
            terminated = got_key = True
            end = "key"
        elif self.cfg.portal_goal > 0 and self._best_sectors - self._start_sectors >= self.cfg.portal_goal:
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
        info["key"] = got_key             # the key goal reached (EnvConfig.key_goal)
        info["terms"] = terms             # the reward by term
        info["end"] = end                 # why the rollout ended: exit, death, key, portal, time, combat
        info["max_tics"] = self.cfg.max_tics
        return obs, float(reward), terminated, truncated, info

    def set_map(self, iwad: str | None, warp: str) -> bool:
        """Move this instance to another map (and IWAD; None keeps the
        current one): the game is closed now and starts on the new map at
        the next reset, which saves that map's start again. Installed saves
        (the old map's progress) are dropped. Returns whether it moved."""
        if warp == self.cfg.engine.warp and (iwad is None or iwad == self.cfg.engine.iwad):
            return False
        self.engine.close()
        self.cfg.engine.warp = warp
        if iwad:
            self.cfg.engine.iwad = iwad
        self._installed.clear()
        self._installed_visited.clear()
        self._map_start = None
        return True

    def set_max_tics(self, n: int) -> None:
        """Change the rollout limit in game tics; it takes effect from the
        next rollout (training's per-map tic budget, model.TicBudget)."""
        self.cfg.max_tics = int(n)

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

    def _turn_waste_step(self, prev: dict, d: dict) -> float:
        """This step's wasted rotation (RewardConfig.turn_waste): its turn,
        against the net turn of the window before it."""
        rc = self.cfg.rewards
        a0, a1 = prev.get("angle"), d.get("angle")
        if a0 is None or a1 is None or None in (prev.get("x"), d.get("x")) \
                or np.hypot(d["x"] - prev["x"], d["y"] - prev["y"]) > 64:
            self._turn_hist.clear()                          # no level, or a teleport
            return 0.0
        turn = (a1 - a0 + 180) % 360 - 180                   # degrees, + left
        net = sum(self._turn_hist)
        wasted = 2 * min(abs(turn), abs(net)) if turn * net < 0 else 0
        self._turn_hist.append(turn)
        return rc.turn_waste * wasted / 45.0

    def _fall_step(self, prev: dict, d: dict, tics: int) -> float:
        """Track a fall across steps (RewardConfig.fall); the penalty on the
        step it ends, else 0."""
        rc = self.cfg.rewards
        z0, z1 = prev.get("z"), d.get("z")
        if z0 is None or z1 is None or tics <= 0 or None in (prev.get("x"), d.get("x")) \
                or np.hypot(d["x"] - prev["x"], d["y"] - prev["y"]) > 64 * tics:
            self._fall_top, self._fall_peak = None, 0.0     # no level, or a teleport
            return 0.0
        drop = z0 - z1
        if drop > 0:                                       # going down: start or continue
            if self._fall_top is None:
                self._fall_top, self._fall_peak = z0, 0.0
            self._fall_peak = max(self._fall_peak, drop / tics)
            return 0.0
        out = 0.0
        if self._fall_top is not None:                     # landed (or started going up)
            total = self._fall_top - z0
            if self._fall_peak > rc.fall_speed and total >= rc.fall_min:
                out = rc.fall * total / rc.fall_min
        self._fall_top, self._fall_peak = None, 0.0
        return out

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

    def _update_triggers(self, d: dict, tics: int = 0) -> float:
        """Start, age and end the CONSULT triggers; the miss penalty once
        one goes unanswered past its grace: grace_tics game tics for a
        monster or a full backpack, grace_decisions steps in combat
        (frozen: no tics pass there)."""
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
                t["age"] += 1 if k == "combat" else tics
            else:
                t["active"] = False
            grace = rc.grace_decisions if k == "combat" else rc.grace_tics
            if (t["active"] and not t["consulted"] and not t["penalized"]
                    and t["age"] >= grace):
                penalty += rc.trigger_miss
                t["penalized"] = True
        return penalty
