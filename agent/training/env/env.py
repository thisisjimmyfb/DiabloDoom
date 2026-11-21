"""DiabloDoomEnv: the Gymnasium environment (SPEC.md §5).

The policy sees pixels only: frame_stack x height x width grayscale
(4 x 80 x 128), the last frame_stack decisions' frames. Engine
state from the STATE line drives rewards, episode ends and CONSULT
routing, and is exposed in `info["state"]` for logging and the critic,
never in the observation.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Callable, Optional

import collections
from pathlib import Path

import gymnasium as gym
import numpy as np
from gymnasium import spaces

from .engine import Engine, EngineConfig, EngineError
from .obs import FrameStack, Preprocessor

# SPEC.md §5.2. Index = action id. A decision every game tic
# (2026-11-15): each action is the keys held for the next tic, the way a
# keyboard is played. PRESS <keys>: W/S run forward/back, A/D strafe,
# L/R turn (about 7 degrees a tic), every combination of one key from
# each pair or none: 27, "PRESS" alone (nothing held) first; each with
# and without CTRL, the fire key (2026-11-18): 54, index 18*forward +
# 6*strafe + 2*turn + fire. Then USE and CONSULT. The engine's multi-tic commands (WALK, GOTO, the 45-degree
# TURNL/TURNR, ...) stay for the farm driver; the policy doesn't send them.
# No MAP (removed 2026-11-14): the env's forced glances show the map.
ACTIONS = [" ".join(k for k in ("PRESS", f + s + t, c) if k)
           for f in ("", "W", "S") for s in ("", "A", "D") for t in ("", "L", "R")
           for c in ("", "CTRL")] + ["USE", "CONSULT"]
N_MOVE = 54                       # the PRESS actions, before USE and CONSULT
CONSULT = ACTIONS.index("CONSULT")
# The PRESS actions that turn (L or R; the CTRL key's letters don't count).
TURNS = frozenset(i for i, a in enumerate(ACTIONS[:N_MOVE]) if set(a[6:].replace("CTRL", "")) & {"L", "R"})
USE = ACTIONS.index("USE")
# Rollout types (2026-11-20; reset's options["type"]): what the world holds,
# never how it is scored -- every type has the same reward function.
#   combat:   the combat drill: map monsters hidden, DRILL ON (one awake
#             monster ahead), never fists, no waypoints or practice items,
#             every sector visited from the start (no room can pay)
#   waypoint: the waypoint drill: map monsters hidden, endless waypoints
#             (a new one as soon as one is reached), frequent practice items
#             on the way to it, about half uniques (ITEMS)
#   maze:     map monsters hidden, no waypoints
#   mixed:    map monsters shown, waypoints
# Drills (combat, waypoint) end only on death, the exit or time: no portal
# or key goal. No type: the older random split (EnvConfig.combat, waypoint_off).
TYPES = ("combat", "waypoint", "maze", "mixed")
DRILLS = ("combat", "waypoint")


def replay_dir_name(iwad: str | None, warp: str) -> str:
    """A replay folder's name (2026-11-20): IWAD and map, e.g. DOOM_E1M2
    (a Doom II -warp 7: MAP07); its videos are that map's only."""
    parts = warp.split()
    tag = f"E{parts[0]}M{parts[1]}" if len(parts) == 2 else f"MAP{int(parts[0]):02d}"
    return f"{Path(iwad).stem if iwad else 'default'}_{tag}"
# Each action's forward/back key: +1 W, -1 S, 0 none (strafes, A/D,
# aren't tracked).
MOVE_DIR = [(1 if "W" in a[6:].replace("CTRL", "") else -1 if "S" in a[6:].replace("CTRL", "") else 0)
            if a.startswith("PRESS") else 0 for a in ACTIONS]
FIRE = [a.endswith("CTRL") for a in ACTIONS]

EXPLORE = "EXPLORE"
COMBAT_MODES = {"PLANNING", "TARGETING", "CONFIRM", "PULSE", "REACTION", "ROUND_END"}
ITEM_LIFE_TICS = 350    # a practice item lies 10 s (the engine's RL_ITEM_LIFE)


# Range bands per weapon (2026-11-20; the state line's weapon, wp_*), map
# units to the nearest monster in sight: fists and chainsaw up close
# (melee range 64 plus a monster's radius), the rocket and BFG past their
# 128-unit splash, the hitscan guns at medium to long range.
RANGE_BANDS = {0: (0, 80), 1: (256, 768), 2: (128, 512), 3: (256, 768), 4: (192, 768),
               5: (128, 640), 6: (192, 768), 7: (0, 80), 8: (64, 256)}


@dataclass
class RewardConfig:
    """SPEC.md §5.3. Magnitudes are placeholders, to be tuned from curves."""
    # Per newly visited sector: more than a mechanism, since getting
    # somewhere new is the goal and a door only the way there. With the
    # portal goal (training) the first new sector also ends the rollout.
    # A door's own sector never counts (2026-11-17; rooms()): a door
    # crossing pays once, for the room behind it.
    sector: float = 15.0
    exit: float = 100.0               # level complete triggered (ends the rollout): the top reward
    death: float = -150.0             # the player died (ends the rollout): 1.5x the exit (-100 before 2026-11-18)
    mechanism: float = 10.0           # USE that sets a new mechanism going (once per mechanism)
    key: float = 25.0                 # per key colour newly held (blue, yellow, red; once each)
    # Pickups, from the engine's pickup events: each Diablo item picked
    # up (or armor, which also grants one) pays `loot`; a health pickup
    # pays `health` per HP it actually healed (a medikit 25 HP, a bonus
    # 1), the mirror of the hit penalties. A thing left on the ground pays nothing.
    loot: float = 5.0
    health: float = 0.75              # 0.2, then 0.5, before 2026-11-18
    # Practice items (2026-11-17): while one lies on the ground, each map
    # unit closed toward it pays item_approach and each unit away costs
    # the same, so circling nets nothing. 0 turns it off.
    item_approach: float = 0.01
    # Waypoints (2026-11-19): each one reached pays waypoint, and each map
    # unit closed toward the one set pays waypoint_look_approach times
    # max(0, cos) of the angle from the facing (at the step's start) to it,
    # away costing the same way (waypoint_approach, unscaled, and look toward
    # it, before 2026-11-20: look paid per tic facing it, so lingering paid). A rollout ending early on
    # a goal (exit, key, portal) pays waypoint for each of its
    # EnvConfig.waypoint_count not reached, so the new sector a waypoint
    # leads into never costs the waypoints after it.
    waypoint: float = 15.0
    waypoint_look_approach: float = 0.01
    consult_hit: float = 5.0          # first CONSULT on an explore trigger
    # Charged tic by tic while a trigger waits unanswered, trigger_miss
    # over the whole grace window (2026-11-17: once at its end before),
    # so each frame the policy didn't CONSULT on gets its share.
    trigger_miss: float = -2.5        # -8, then -12, before 2026-11-18
    # A unique picked up (2026-11-17; the engine's pickup event, tier 4)
    # is a trigger for unique_grace_tics game tics (3 s, as a monster;
    # the engine's gold border around the avatar, the cue, lasts 3.5 s,
    # so the cue is on screen whenever CONSULT pays). It replaced
    # the full-backpack trigger, whose only cue was a few pixels of
    # message text.
    unique_grace_tics: int = 105
    grace_decisions: int = 5          # combat (frozen, no tics pass): steps to CONSULT
    consult_false: float = -2.5       # CONSULT with no live, unanswered trigger (-1.5 before 2026-11-17, -0.3 then -0.5 before 2026-11-18)
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
    fast: float = 0.0                 # 1 / 3 / 35 before 2026-11-20
    fast_speed: int = 4
    # Forward only (2026-11-16): the fast pay is scaled by max(0, cos b),
    # b the angle between the step's movement (its x/y change) and the
    # facing (the mean of the step's start and end): full running forward,
    # about 70% on a forward diagonal, nothing strafing or backing up --
    # which then pays the whole clock, like standing still. No penalty of
    # its own, so a short back-off out of a dead end stays cheaper than
    # turning round. False pays every fast tic alike.
    fast_forward: bool = True
    # Along the way on (2026-11-17): with fast_forward, b is measured from
    # the way on (way_rel at the step's start: along the walking path,
    # else toward the nearest unvisited portal) instead of the facing, so
    # running a circle is refunded only on the part pointing the right
    # way and a detour not at all; no way known: the facing, as before.
    fast_path: bool = True
    # Looking toward a way on (2026-11-16): every tic pays
    #   look * max(0, cos(a))
    # a the angle between the player's facing and the closest point of
    # the nearest portal of the current sector into an unvisited sector
    # (the engine's portal_rel): full credit facing it, nothing facing
    # 90 degrees or more away, nothing when the sector has no such portal.
    # Only unvisited ones: right after a crossing the nearest portal is
    # usually the one just used, behind the player. The default is half
    # the clock's base rate (model.make_env ties it to --time-penalty), so
    # it can't pay for standing still. 0 turns it off. Since 2026-11-17
    # a is the way along the walking path instead when the engine has one
    # (way_rel(): path_rel), so the reward follows a corridor round a bend.
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
    # Turning (2026-11-20): each tic a turn key (L or R) was held costs
    # turn, all paid when the rollout ends; against the circling habit
    # (a steady turn one way, which turn_waste never charges). Not while a
    # monster is on screen at the step's start (the engine's foe_dist):
    # turning is the only aim, so circle-strafing stays free (2026-11-20).
    turn: float = -0.1                # -0.01 before 2026-11-20
    turn_waste_window: int = 8
    # The same for forward and back: W at most move_flip_window tics after
    # S, or S after W, costs move_flip. Strafing back and forth (A/D) is
    # free.
    move_flip: float = -0.015
    move_flip_window: int = 4
    # Falls (2026-11-16): a quick large drop usually means the player left
    # the path the map intends. A fall is an unbroken run of tics going
    # down that at some point drops faster than fall_speed units a tic
    # (only freefall does: the fastest lift, the blazing one, lowers 8 a
    # tic and carries the player down exactly with it, so 10 leaves it a
    # margin; freefall starts at 2 a tic and gains 1 a tic, so it passes
    # 10 after about 65 units, right at fall_min; a stair step is 8-24
    # units with flat treads between) and totals at least fall_min
    # units; on landing it costs fall, the same whatever the height (Doom
    # has little verticality: a fall is the event, not its size). Going up
    # never costs; a teleport (a jump of more than 64 units sideways) is
    # no fall. 0 turns it off.
    fall: float = -10.0               # -3 before 2026-11-18
    fall_min: float = 64.0
    fall_speed: float = 10.0
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
    # Per hit taken (2026-11-19; per HP lost before): each tic with an
    # engine `hurt` event, whatever the amount (gear scales it) and even
    # when block, armour or resistance leave the HP alone: monsters,
    # nukage, slime, crushers, barrels. Healing never pays.
    # Split 2026-11-20 (hp_loss -7.5 before) by the event's kind: a
    # hitscan hit can't be dodged, so it only presses for an early kill;
    # the rest (projectile, melee, nukage, crusher, barrel) can be avoided.
    hit_unavoidable: float = -1.0
    hit_avoidable: float = -15.0
    # Combat (2026-11-18), half again as much as the maze run (a kill
    # 22.5 against a new sector's 15; hits, health and death x1.5 too):
    # damage per a monster's full HP dealt (so a kill pays the same for
    # any monster), from the engine's damage events, plus kill on the
    # kill; fire_waste per shot that hurt nothing at once (a projectile's
    # hit comes later: charged too since 2026-11-19, its hit paying
    # damage), so firing at nothing costs. Since 2026-11-19 damage and kill
    # are paid when the rollout ends: a death pays no combat at all (times
    # hit_decay 0.8 per hit taken too, 2026-11-19 to 2026-11-20).
    damage: float = 11.25
    kill: float = 11.25
    fire_waste: float = -0.3          # -0.1 before 2026-11-19
    # Range (2026-11-20): each map unit the nearest monster on screen,
    # woken or not (the engine's foe_dist), moves toward the ready
    # weapon's band in RANGE_BANDS pays range, each unit away costs it.
    # Only the nearest, so many don't pay more; a change of distance, never a per-tic pay,
    # so holding the band or stalling a fight earns nothing.
    range: float = 0.01
    # One-off terms (2026-11-17): they judge only the step's own action,
    # so the learner adds them to that step's advantage alone instead of
    # summing them into every earlier step's reward-to-go
    # (info["reward_now"], learn.group_advantages).
    immediate: tuple = ("consult", "move_flip", "turn_waste", "wall_look", "fire_waste")


# A CONSULT handler answers a CONSULT: it may drive the engine with any
# driver commands and returns the decision it ends on. `kinds` names the
# live triggers ("unique", "combat"; no "monster" since 2026-11-18, the policy fights; empty for a false
# consult). On the farm this signals Muse VM; in training a stand-in.
ConsultHandler = Callable[["DiabloDoomEnv", dict, list], dict]


def rooms(state: dict, default: int = 0) -> int:
    """The visited sectors the sector reward and the portal goal count:
    all of them but doors (the engine's door_sectors, 2026-11-17), so
    walking through a door pays for the room behind it only, not for the
    door's own small sector too."""
    if "sectors" not in state:
        return default
    return int(state["sectors"]) - int(state.get("door_sectors", 0))


def goal_dist(state: dict) -> float | None:
    """The time penalty's distance (map units). Since 2026-11-17 the
    walking distance to the nearest unvisited room (the engine's
    path_dist: a path on its 32-unit grid, round walls and through doors
    the player can open), so being far round a bend costs what it should.
    No path (none reachable, or an engine without it): the straight
    distance to the nearest portal of the current sector (portal_dist,
    2026-11-16), else None (the base rate)."""
    d = state.get("path_dist")
    if d is not None and d >= 0:
        return float(d)
    d = state.get("portal_dist")
    return float(d) if d is not None and d >= 0 else None


def way_rel(state: dict) -> float | None:
    """The look reward's angle (degrees off the facing, positive left):
    along the walking path to the nearest unvisited room (the engine's
    path_rel, 2026-11-17: to the farthest of the next 4 path cells the
    player can walk to straight, so it turns with a corridor and never
    points into a wall), else to the nearest unvisited portal of the
    current sector (portal_rel), else None."""
    if state.get("path_rel") is not None:
        return float(state["path_rel"])
    if state.get("portal_rel") is not None:
        return float(state["portal_rel"])
    return None


def null_consult(env: "DiabloDoomEnv", state: dict, kinds: list) -> dict:
    """Does nothing; the policy keeps control."""
    return state


def standin_consult(env: "DiabloDoomEnv", state: dict, kinds: list,
                    max_rounds: int = 10, salvage_extra: int = 10) -> dict:
    """Training stand-in for Muse's VLM (SPEC.md §5.4): it clears what the
    policy consulted about, cheaply, so the policy learns *when* to hand
    off, not how to fight. env.step has already sent the CONSULT driver
    command (every CONSULT: it removes every practice monster; map monsters
    never since 2026-11-18, the policy fights them).
      - combat (turn-based, -turnbased only): in combat, END then closes the round (back
        to explore once no combatant is visible), up to max_rounds;
      - a unique picked up (or, on the farm, a full backpack): when the
        backpack is full, salvage the oldest items until it has room, plus
        salvage_extra more; otherwise nothing (the VLM would look the
        item over).
    A false consult (no live trigger) does nothing."""
    s = state
    if ("backpack" in kinds or ("unique" in kinds and s.get("backpack_full"))) and s.get("mode") == EXPLORE:
        extra = salvage_extra
        while s.get("bp_count", 0) > 0 and (s.get("backpack_full") or extra > 0):
            if not s.get("backpack_full"):
                extra -= 1
            s, _ = env.consult_command("SALVAGE 0")
    if "combat" in kinds:
        # env.step already sent the policy's CONSULT.
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
    width: int = 128                   # observation size (--obs-size WxH)
    height: int = 80
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
    # Waypoints (2026-11-18): the env stands in for the VLM, which sets
    # the engine's WAYPOINT in production. waypoint_place "sector": the
    # engine picks a random standable point of a sector next to the
    # player's, waypoint_min or more away (WAYPOINT RANDOM); "path": at the
    # engine's path_ahead (about 192 units along the walking path to the
    # nearest unvisited room) plus waypoint_noise. It keeps it until it is
    # reached (or replaces it every waypoint_every game tics), and sets the next one
    # waypoint_gap tics after one is reached. A group in waypoint_off
    # gets none (all its members alike, from the group's seed), so the
    # policy still learns to explore without. While one is set the
    # clock runs at its base rate (toward it, distance-scaled, before
    # 2026-11-19) and fast / look point at it (DiabloDoomEnv.step).
    # Combat (2026-11-18): the share of groups that fight. The rest hide
    # every living map monster (the engine's MONSTERS HIDE) and only
    # explore; all members of a group alike, from the group's seed.
    combat: float = 0.5
    # A random gun equipped at every rollout's start (2026-11-18): the
    # player starts on fists otherwise.
    give_gun: bool = True
    # With gear for it (2026-11-20, GIVEGUN ... KIT): fists armour and
    # strength, an AD gun the most AD, an AP gun the most AP.
    gun_kit: bool = True
    waypoints: bool = False
    waypoint_place: str = "sector"     # or "path"
    waypoint_every: int = 0            # 0: kept until reached (175, 5 s, before 2026-11-18)
    waypoint_gap: int = 35             # 1 s
    waypoint_off: float = 0.3
    waypoint_noise: float = 16.0       # map units, std per axis
    # None nearer than this (map units): path_ahead stops at the target
    # room's first cell, and one within the engine's 64-unit reach would
    # clear before the player is in the room; the clock without one leads
    # the last stretch.
    waypoint_min: float = 256.0      # 128 before 2026-11-18
    waypoint_count: int = 5            # per rollout (2026-11-19); 0: no limit (the waypoint drill's)
    # Reached within this many map units of the player (2026-11-20, every
    # type; before, only the engine's touch, as a pickup): the env clears it.
    waypoint_reach: float = 96.0
    # The waypoint drill's practice items (ITEMS): mean tics between them
    # and the unique share, percent.
    drill_item_tics: int = 175
    drill_item_unique: int = 50


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
        self.observation_space = spaces.Box(0, 255, (cfg.frame_stack, cfg.height, cfg.width), np.uint8)
        self.action_space = spaces.Discrete(len(ACTIONS))
        self._pre: Preprocessor | None = None
        self._stack = FrameStack(cfg.frame_stack, cfg.height, cfg.width)
        self._map_start: bytes | None = None    # the warp start, saved in bootstrap_slot
        # Per-env reset request for vector envs:
        #   envs.set_attr("next_reset", [{"slot": 3, "seed": 17, "group": 0}, ...])
        self.next_reset: dict | None = None
        self._dmg_share = 0.0
        self._turn_tics = 0
        self._type, self._drill = None, False
        self._wp_clear = False
        self._wp_on, self._wp, self._wp_tic, self._wp_stats = False, None, -10 ** 9, [0, 0]   # waypoints (reset sets them)
        self._wp_count, self._wp_gap = self.cfg.waypoint_count, self.cfg.waypoint_gap
        self._wp_pending = False
        self._combat_on, self._combat_stats = False, [0, 0, 0, 0.0]   # combat (reset sets them)
        self._installed: dict[int, bytes | None] = {}   # install_state slots
        self._installed_visited: dict[int, str | None] = {}  # their visited lists (VISITED hex)
        # the last turn_waste_window turns (degrees), for RewardConfig.turn_waste
        self._turn_hist = collections.deque(maxlen=self.cfg.rewards.turn_waste_window)
        self._move_dir, self._move_tic = 0, -10 ** 9          # the last W/S key and its rollout tic
        self._item, self._consult_stats = None, [0, 0, 0, 0]  # set per rollout in reset
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
            self._pre = Preprocessor(self.engine.width, self.engine.height, self.cfg.width, self.cfg.height)

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
        # The rollout type (TYPES, 2026-11-20); "drill" is the combat drill's
        # older name. self._drill: the combat drill.
        self._type = opts.get("type") or ("combat" if opts.get("drill") else None)
        if self._type is not None and self._type not in TYPES:
            raise ValueError(f"unknown rollout type {self._type!r}")
        self._drill = self._type == "combat"
        if self._type is None:
            self._combat_on = (int(rng_seed) * 40503 + 7919) % 1000 / 1000.0 < self.cfg.combat
        else:
            self._combat_on = self._type == "mixed"
        d, _ = self.engine.command("MONSTERS SHOW" if self._combat_on else "MONSTERS HIDE")
        if self.cfg.give_gun:
            # A random gun equipped (2026-11-18; the engine's GIVEGUN), the
            # same for the whole group (from its seed).
            # A drill never gets fists (RANGED, 2026-11-20): no melee there.
            d, _ = self.engine.command(f"GIVEGUN {int(rng_seed) * 7 + 3}" + (" KIT" if self.cfg.gun_kit else "")
                                       + (" RANGED" if self._drill else ""))
        if self._drill:
            d, _ = self.engine.command("DRILL ON")
            d, _ = self.engine.command("ITEMS 0")
            # Every sector visited from the start (2026-11-20): a new room
            # can't pay here, so fighting is the drill's only payoff.
            vis = self.visited()
            if vis:
                for off in range(0, len(vis), 200):
                    d, _ = self.engine.command(f"VISIT {off} {'f' * len(vis[off:off + 200])}")
        elif self._type == "waypoint":
            d, _ = self.engine.command(f"ITEMS {self.cfg.drill_item_tics} {self.cfg.drill_item_unique}")
        self._combat_stats = [0, 0, 0, 0.0]   # shots, hits, kills, damage dealt (HP)
        self._dmg_share = 0.0                  # monster HP shares removed (paid at the end)
        self._turn_tics = 0                    # tics a turn key was held (RewardConfig.turn)
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
        warm_item = 0      # a practice item the warm-up spawned and left lying (it lives 10 s)
        self._item = None  # the practice item lying now: (x, y, gametic spawned)
        for wa in opts.get("warmup") or []:
            if d.get("mode") != EXPLORE or d.get("dead") or int(wa) == CONSULT:
                break
            d, wevents = self.engine.command(ACTIONS[int(wa)])
            self._new_mechanisms(wevents)
            for e in wevents:
                if e.get("event") == "item":
                    warm_item = 1
                    self._item = (e["x"], e["y"], e.get("gametic", 0))
                elif e.get("event") == "pickup" and e.get("practice"):
                    warm_item = 0
                    self._item = None

        self.state = d
        self._group = opts.get("group")
        self._slot, self._seed = slot, int(rng_seed) % 256
        self._tics = 0
        self._best_sectors = rooms(d)
        self._start_sectors = self._best_sectors
        self._goal = goal_dist(d)
        self._keys = d.get("keys", 0)     # keys held at the start (or from the warm-up) never pay
        self._items_spawned, self._items_picked = warm_item, 0   # -rlitems practice items this rollout
        self._consult_stats = [0, 0, 0, 0]   # explore steps with a unique trigger pending, CONSULTs on them; without, CONSULTs
        self._prev_hp = d.get("hp", 0)
        self._combat_steps = 0
        self._turn_hist = collections.deque(maxlen=self.cfg.rewards.turn_waste_window)
        self._move_dir, self._move_tic = 0, -10 ** 9
        self._fall_top, self._fall_peak = None, 0.0
        self._trig = {k: self._new_trigger() for k in ("unique", "combat")}
        self._update_triggers(d)
        # Waypoints (EnvConfig.waypoints): this group's on or off, from
        # its seed; the first one goes out with the first action.
        if self._type is None:
            self._wp_on = self.cfg.waypoints and (int(rng_seed) * 2654435761 % 1000) / 1000.0 >= self.cfg.waypoint_off
        else:
            self._wp_on = self.cfg.waypoints and self._type in ("waypoint", "mixed")
        # The waypoint drill: no limit, the next one at once.
        endless = self._type == "waypoint"
        self._wp_count = 0 if endless else self.cfg.waypoint_count
        self._wp_gap = 0 if endless else self.cfg.waypoint_gap
        self._wp, self._wp_tic, self._wp_stats, self._wp_pending = None, -10 ** 9, [0, 0], False
        self._wp_clear = False
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

        if not in_combat:                 # the metric: CONSULT with and without a trigger pending
            pending = any(self._trig[k]["active"] and not self._trig[k]["consulted"]
                          for k in ("unique",))
            self._consult_stats[0 if pending else 2] += 1
            self._consult_stats[1 if pending else 3] += a == CONSULT
        if a == CONSULT:
            kinds = [k for k, t in self._trig.items() if t["active"] and not t["consulted"]]
            if not kinds:
                add("consult", rc.consult_false)
            for k in kinds:
                if k == "unique":
                    add("consult", rc.consult_hit)
                self._trig[k]["consulted"] = True
            # Every CONSULT reaches the game (status message; under
            # it also removes every practice monster), then the
            # handler does the rest.
            d, _ = self.consult_command("CONSULT")
            d = self.consult_handler(self, d, kinds)
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
            line = ACTIONS[a]
            wl = self._waypoint_due(prev)
            if wl:
                line = f"{wl};{line}"     # the same tic as the action
            if self._wp_clear:            # reached within waypoint_reach last step: off the screen
                line = f"WAYPOINT;{line}"
                self._wp_clear = False
            d, events = self.engine.command(line)
            tics = max(d.get("tics", 0), 0)
            sent = True
        if self._wp_pending:              # WAYPOINT RANDOM: the point the engine picked, if any
            self._wp_pending = False
            if d.get("waypoint") is not None:
                self._wp = tuple(d["waypoint"])
                self._wp_stats[0] += 1
            else:
                self._wp = None
        wp = self._wp                     # the waypoint this step ran toward

        self.state = d
        self._tics += tics
        if any(e.get("event") == "waypoint_reached" for e in events) or (wp is not None and "waypoint" not in d):
            if any(e.get("event") == "waypoint_reached" for e in events):
                self._wp_stats[1] += 1
                add("waypoint", rc.waypoint)
            self._wp, self._wp_tic = None, self._tics
        elif (wp is not None and "x" in d and self.cfg.waypoint_reach > 0
              and np.hypot(d["x"] - wp[0], d["y"] - wp[1]) <= self.cfg.waypoint_reach):
            # Within waypoint_reach (2026-11-20): reached, the engine's marker
            # cleared with the next action.
            self._wp_stats[1] += 1
            add("waypoint", rc.waypoint)
            self._wp, self._wp_tic, self._wp_clear = None, self._tics, True

        # Steps in combat (capped: the world is frozen until CONSULT).
        if d.get("mode") in COMBAT_MODES:
            self._combat_steps += 1
        else:
            self._combat_steps = 0

        # Time and distance: every tic costs time * (1 + d / far_scale),
        # d the distance to the nearest exit line or frontier portal.
        # With a waypoint (EnvConfig.waypoints), the distance closed toward
        # it, facing it, pays waypoint_look_approach. A group with waypoints
        # has no clock (2026-11-20; "waypoint_time", the base rate, before):
        # 5 fixed waypoint rewards and the rollout limit are its urgency.
        sec = rooms(d, self._best_sectors)
        goal = goal_dist(d)
        if wp is not None and "x" in prev and "x" in d:
            closed = np.hypot(prev["x"] - wp[0], prev["y"] - wp[1]) - np.hypot(d["x"] - wp[0], d["y"] - wp[1])
            if abs(closed) <= 64:         # a jump (teleport) is not walking
                facing = 1.0
                if prev.get("angle") is not None:
                    ang = np.degrees(np.arctan2(wp[1] - prev["y"], wp[0] - prev["x"])) - prev["angle"]
                    facing = max(0.0, float(np.cos(np.radians(ang))))
                add("waypoint_look_approach", rc.waypoint_look_approach * float(closed) * facing)
        elif tics > 0 and rc.time and not self._wp_on:
            known = [g for g in (self._goal, goal) if g is not None]
            dist = sum(known) / len(known) if known else 0.0
            add("time", -rc.time * tics * (1.0 + dist / rc.far_scale))
        if goal is not None or "x" in d:
            self._goal = goal

        # High velocity: the tics the engine counted at fast_speed or more
        # (a dropped action re-reads the last state line: nothing new).
        if sent:
            add("fast", rc.fast * max(int(d.get("fast_tics", 0)), 0) * self._forward_share(prev, d, wp))
        # Looking the way on (way_rel: along the path, else toward the
        # nearest unvisited portal), by the facing at the step's end (one
        # tic, under per-tic decisions). With waypoints: nothing (2026-11-20;
        # toward the waypoint before: waypoint_look_approach has the facing).
        rel = None if self.cfg.waypoints else way_rel(d)
        if sent and rc.look and rel is not None:
            add("look", rc.look * tics * max(0.0, float(np.cos(np.radians(rel)))))
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
        add("consult", self._update_triggers(d, tics, events))

        # Mechanisms: a USE that sets a door, lift, floor, switch...
        # going pays once per mechanism per rollout (SPEC.md §5.3), so
        # reusing the same door or switch never pays again.
        if self._new_mechanisms(events):
            add("mechanism", rc.mechanism)

        # Keys: each colour pays once per rollout, when first held.
        new_keys = self._new_keys(d)
        add("key", rc.key * new_keys)

        # Range (RewardConfig.range): distance moved toward the ready
        # weapon's band from the nearest monster in sight.
        if rc.range and prev.get("foe_dist", -1) >= 0 and d.get("foe_dist", -1) >= 0:
            lo, hi = RANGE_BANDS.get(d.get("weapon"), (0, 10 ** 9))
            off = lambda x: max(0, lo - x) + max(0, x - hi)
            if abs(d["foe_dist"] - prev["foe_dist"]) <= 64:   # a jump: another monster
                add("range", rc.range * float(off(prev["foe_dist"]) - off(d["foe_dist"])))

        # Practice item approach (RewardConfig.item_approach): distance
        # closed toward the item lying since the step began.
        if rc.item_approach and self._item is not None and "x" in prev and "x" in d:
            ix, iy, _ = self._item
            closed = (np.hypot(prev["x"] - ix, prev["y"] - iy)
                      - np.hypot(d["x"] - ix, d["y"] - iy))
            if abs(closed) <= 64:         # a jump (teleport) is not walking
                add("item", rc.item_approach * float(closed))

        # Pickups: loot and armor per item, health per HP healed.
        for e in events:
            if e.get("event") == "item":
                self._items_spawned += 1
                self._item = (e["x"], e["y"], e.get("gametic", d.get("gametic", 0)))
            if e.get("event") == "pickup" and e.get("practice"):
                self._items_picked += 1
                self._item = None
            if e.get("event") == "damage" and e.get("max"):
                self._dmg_share += min(int(e.get("amount", 0)), int(e["max"])) / int(e["max"])
                self._combat_stats[3] += int(e.get("amount", 0))
                if e.get("kill"):
                    self._combat_stats[2] += 1
            if e.get("event") == "shot":
                self._combat_stats[0] += 1
                self._combat_stats[1] += bool(e.get("hit"))
                if not e.get("hit"):
                    add("fire_waste", rc.fire_waste)
            if e.get("event") == "pickup":
                if e.get("kind") in ("loot", "armor"):
                    add("loot", rc.loot)
                elif e.get("kind") == "health":
                    add("health", rc.health * max(int(e.get("hp", 0)), 0))

        if self._item is not None and d.get("gametic", 0) - self._item[2] >= ITEM_LIFE_TICS:
            self._item = None             # the engine removed it

        # Hits: the tics the engine hurt the player on (a blast's pellets
        # once), even for no HP. The HP mark is kept for the state.
        hurt = [e for e in events if e.get("event") == "hurt"]
        scan = len({e.get("gametic") for e in hurt if e.get("kind") == "hitscan"})
        hits = len({e.get("gametic") for e in hurt if e.get("kind") != "hitscan"})
        if scan:
            add("hit_unavoidable", rc.hit_unavoidable * scan)
        if hits:
            add("hit_avoidable", rc.hit_avoidable * hits)
        hp = max(d.get("hp", self._prev_hp), 0)
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
        elif self.cfg.key_goal and new_keys and self._type not in DRILLS:
            terminated = got_key = True
            end = "key"
        elif (self.cfg.portal_goal > 0 and self._type not in DRILLS
              and self._best_sectors - self._start_sectors >= self.cfg.portal_goal):
            terminated = portal = True
            end = "portal"
        if not terminated:
            if self._tics >= self.cfg.max_tics:
                truncated, end = True, "time"
            elif self._combat_steps >= self.cfg.max_combat_steps:
                truncated, end = True, "combat"
        if a in TURNS and prev.get("foe_dist", -1) < 0:   # aiming at a monster on screen is free
            self._turn_tics += tics
        # Turning, paid at the end (RewardConfig.turn).
        if terminated or truncated:
            add("turn", rc.turn * self._turn_tics)
        # Combat, paid at the end, nothing after a death (RewardConfig.damage).
        if terminated or truncated:
            hpf = 0.0 if d.get("dead") else 1.0
            add("damage", rc.damage * self._dmg_share * hpf)
            add("kill", rc.kill * self._combat_stats[2] * hpf)
        # Waypoints not reached, paid when a goal ends the rollout early.
        if end in ("exit", "key", "portal") and self._wp_on and self._wp_count > 0:
            add("waypoint", rc.waypoint * max(self._wp_count - self._wp_stats[1], 0))
        reward = sum(terms.values())

        obs = self._stack.push(self._frame()) if sent else self._stack.get()
        info = self._info(tics, d)
        info["exit"] = exited
        info["portal"] = portal
        info["key"] = got_key             # the key goal reached (EnvConfig.key_goal)
        info["items"] = [getattr(self, "_items_spawned", 0), getattr(self, "_items_picked", 0)]
        info["consult_stats"] = list(self._consult_stats)
        # on (a drill counts: it fights), shots, hits, kills, damage
        info["combat"] = [int(self._combat_on or self._drill)] + list(self._combat_stats)
        info["type"] = self._type
        info["drill"] = self._drill
        info["waypoints"] = list(self._wp_stats)   # [set, reached]
        info["terms"] = terms             # the reward by term
        info["reward_now"] = sum(v for t, v in terms.items() if t in rc.immediate)
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
        if self.cfg.engine.capture_dir:   # replays follow the map (2026-11-20)
            self.cfg.engine.capture_dir = str(Path(self.cfg.engine.capture_dir).parent
                                              / replay_dir_name(self.cfg.engine.iwad, warp))
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

    def _forward_share(self, prev: dict, d: dict, wp: tuple | None = None) -> float:
        """max(0, cos) of the angle between the step's movement and the
        way on at the step's start (RewardConfig.fast_path; else, or with
        no way known, the facing) (RewardConfig.fast_forward); 1 when
        it's off or unknown. With waypoints (EnvConfig.waypoints): toward
        the waypoint wp, else the facing (never fast_path)."""
        if not self.cfg.rewards.fast_forward:
            return 1.0
        if None in (prev.get("x"), prev.get("y"), d.get("x"), d.get("y"), prev.get("angle"), d.get("angle")):
            return 1.0
        dx, dy = d["x"] - prev["x"], d["y"] - prev["y"]
        n = np.hypot(dx, dy)
        if n == 0:
            return 1.0                    # no displacement: fast_tics is 0 anyway
        a0, a1 = np.radians(prev["angle"]), np.radians(d["angle"])
        fx, fy = np.cos(a0) + np.cos(a1), np.sin(a0) + np.sin(a1)   # mean facing
        rel = way_rel(prev) if self.cfg.rewards.fast_path and not self.cfg.waypoints else None
        if wp is not None:
            rel = float(np.degrees(np.arctan2(wp[1] - prev["y"], wp[0] - prev["x"]))) - prev["angle"]
        if rel is not None:                                           # the way on (degrees, left +)
            w = np.radians(prev["angle"] + rel)
            fx, fy = np.cos(w), np.sin(w)
        f = np.hypot(fx, fy)
        if f == 0:
            return 1.0
        return max(0.0, float((dx * fx + dy * fy) / (n * f)))

    def _waypoint_due(self, d: dict) -> str | None:
        """The WAYPOINT command to send with this step's action, or None
        (EnvConfig.waypoints): a random point of the current sector
        (waypoint_place "sector"), or at path_ahead plus noise, when none is set
        and waypoint_gap tics have passed since the last was reached, or
        when the one set is waypoint_every tics old; never within
        waypoint_min of the player."""
        cfg = self.cfg
        ahead = d.get("path_ahead")
        if not self._wp_on or d.get("mode") != EXPLORE or (
                self._wp_count > 0 and self._wp is None and self._wp_stats[0] >= self._wp_count):
            return None
        age = self._tics - self._wp_tic
        if self._wp is not None and cfg.waypoint_every <= 0:
            return None
        if age < (self._wp_gap if self._wp is None else cfg.waypoint_every):
            return None
        if cfg.waypoint_place == "sector":
            # The engine picks the point; the step reads it back.
            self._wp_tic, self._wp_pending = self._tics, True
            return f"WAYPOINT RANDOM {int(self.np_random.integers(0, 2 ** 31))} {int(cfg.waypoint_min)}"
        if ahead is None:
            return None
        if "x" in d and np.hypot(ahead[0] - d["x"], ahead[1] - d["y"]) < cfg.waypoint_min:
            return None
        x, y = (int(round(v + self.np_random.normal(0.0, cfg.waypoint_noise))) for v in ahead[:2])
        self._wp, self._wp_tic = (x, y), self._tics
        self._wp_stats[0] += 1
        return f"WAYPOINT {x} {y}"

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
                out = rc.fall
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

    def _update_triggers(self, d: dict, tics: int = 0, events: list | None = None) -> float:
        """Start, age and end the CONSULT triggers; the miss penalty while
        one waits unanswered, spread over its grace: unique_grace_tics game
        tics for a unique picked up, grace_decisions steps in combat
        (frozen: no tics pass there). Combat is live while it lasts; a
        unique from its pickup event, until consulted or its grace runs
        out. (No monster sighting trigger since 2026-11-18: the policy
        fights.)"""
        rc = self.cfg.rewards
        u = self._trig["unique"]
        live = {
            "unique": (any(e.get("event") == "pickup" and e.get("tier") == 4 for e in events or [])
                       or (u["active"] and not u["consulted"] and not u["penalized"])),
            "combat": d.get("mode") in COMBAT_MODES,
        }
        penalty = 0.0
        for k, on in live.items():
            t = self._trig[k]
            step = 0
            if on and not t["active"]:
                self._trig[k] = t = {"active": True, "consulted": False, "age": 0, "penalized": False}
            elif on:
                step = 1 if k == "combat" else tics
                t["age"] += step
            else:
                t["active"] = False
            grace = rc.grace_decisions if k == "combat" else rc.unique_grace_tics
            if t["active"] and not t["consulted"] and not t["penalized"]:
                # This step's share of the window (trigger_miss in all).
                waited = min(t["age"], grace) - min(t["age"] - step, grace)
                penalty += rc.trigger_miss * waited / max(grace, 1)
                t["penalized"] = t["age"] >= grace
        return penalty
