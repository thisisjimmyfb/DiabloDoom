"""Shared pieces of the trainer (SPEC.md §6, §7.1, §7.4): the policy
network, the env factory, IWAD/group allocation and map rotation, progress
saves (Frontier), tic budgets and checkpoint I/O. Used by train.py, with
rollout.py (rollouts, logs, held-out eval), learn.py (the update) and
hub.py (the Hugging Face Hub copy of the checkpoint)."""

from __future__ import annotations

import argparse
import os
import random
import sys
import time
from pathlib import Path

import numpy as np
import torch
import torch.nn as nn

sys.path.insert(0, str(Path(__file__).resolve().parent))
from env import ACTIONS, CONSULT  # noqa: E402

# Warm-up uses movement only (no CONSULT, no fire: 2026-11-18), each action held for
# WARMUP_HOLD tics (decisions are per tic: single random tics would
# barely move the player).
WARMUP_ACTIONS = [i for i in range(len(ACTIONS)) if i != CONSULT and not ACTIONS[i].endswith("CTRL")]
WARMUP_HOLD = 15
N_PRIV = 2  # privileged critic inputs: HP/100, exit path distance (SPEC.md §6)


# ---------------------------------------------------------------------------
# Environment factory (top-level so worker processes can unpickle it)
# ---------------------------------------------------------------------------

DEFAULT_OBS_SIZE = (128, 80)     # (W, H): width, height of each observation frame (--obs-size)

def make_env(iwad: str | None, warp: str, stage: int, skill: int, max_tics: int,
             slots_dir: str | None, slots: list[int], capture_dir: str | None = None,
             spawn_tics: int = 0, item_tics: int = 0, nodraw: bool = False, noblit: bool = False,
             portal_goal: int = 0, time_penalty: float = 0.0, key_goal: bool = False,
             waypoints: bool = False, combat: float = 0.5,
             headless: bool = True, obs_size: tuple[int, int] = DEFAULT_OBS_SIZE):
    """time_penalty: reward per game second (negative pays, e.g. 1/3
    costs a point every 3 s), with portal_goal n > 0 ending a rollout
    once it reaches n new sectors, and key_goal ending it on a new key."""
    from env import DiabloDoomEnv, EngineConfig, EnvConfig, RewardConfig, standin_consult
    # Stage 1 (SPEC.md §7.2): no map monsters and (2026-11-17) no turn-based
    # combat at all, not even from passive damage: --eval (vanilla's -nomonsters,
    # renamed 2026-11-17), the
    # -rlnocombat of the spec. Stage 2: map monsters on. In both,
    # -rlspawn put practice monsters on screen (a sighting to CONSULT
    # about; off by default since 2026-11-18, --spawn-tics 0), and the
    # stand-in answers CONSULT.
    # Since 2026-11-18 every game has its map monsters (no --eval): the env
    # hides them in the groups that don't fight (EnvConfig.combat; stage
    # 2: all fight), since --eval is for the whole game and a save made
    # under it holds no monsters.
    extra = []
    if spawn_tics > 0:
        extra += ["-rlspawn", str(spawn_tics)]
    if item_tics > 0:
        extra += ["-rlitems", str(item_tics)]    # practice loot to walk to and pick up
    if noblit:
        # Skip the SDL blit (I_FinishUpdate), which headless nobody sees:
        # most of the game's CPU time. The frame the policy gets and the
        # replays come from the 8-bit buffer, so they are unchanged.
        extra.append("-noblit")
    if nodraw:
        # No rendering at all (D_Display and the blit skipped): faster
        # still, but the frame is never drawn and replays are off.
        extra.append("-nodraw")
    extra += avatar_args()
    if not headless:
        extra.append("-window")      # a window each, never the user config's fullscreen
    engine = EngineConfig(warp=warp, skill=skill, extra_args=extra,
                          savedir_template=slots_dir, capture_dir=capture_dir, headless=headless)
    if iwad:
        engine.iwad = iwad
    return DiabloDoomEnv(EnvConfig(
        engine=engine,
        width=obs_size[0], height=obs_size[1],
        max_tics=max_tics,
        start_slots=list(slots),
        consult_handler=standin_consult,
        portal_goal=portal_goal,
        key_goal=key_goal,
        waypoints=waypoints,            # the env stands in for the VLM's WAYPOINT (EnvConfig)
        combat=1.0 if stage == 2 else combat,   # the share of groups that fight (EnvConfig)
        # Facing a way on pays up to half the clock (RewardConfig.look); no
        # fast pay since 2026-11-20 (the clock's base rate before): with
        # combat, moving for its own sake shouldn't pay.
        rewards=RewardConfig(time=0.0, fast=0.0,   # time=time_penalty / 35.0 before 2026-11-21 (off for now)
                             look=time_penalty / 35.0 / 2),   # wall_look=-time_penalty / 35 before 2026-11-21 (std now: RewardConfig)
    ))


# The status-bar avatar (AGENTS.md): every game training or eval.py
# launches loads it with -file when it exists (2026-11-16).
AVATAR_PWAD = Path(__file__).resolve().parents[1] / "jolly.wad"


def avatar_args() -> list[str]:
    """-file agent/jolly.wad (absolute: each game runs in its own temp
    directory), or nothing when the file isn't there."""
    return ["-file", str(AVATAR_PWAD)] if AVATAR_PWAD.is_file() else []


def run_env(args, slots: list[int], iwad: str | None, warp: str, capture_dir: str | None = None,
            held_out: bool = False):
    """The env factory for training and for the held-out eval alike, so
    both are built from the same arguments and scored by the same rewards
    (the time penalty included). Only the rollout rules differ: held out,
    a rollout runs the full --eval-max-tics, never ended early by the
    portal or key goal, so its sectors compare across versions."""
    import functools
    rules = (dict(max_tics=args.eval_max_tics) if held_out else
             dict(max_tics=args.max_tics, portal_goal=args.portal_goal, key_goal=True))
    return functools.partial(make_env, iwad, warp, args.stage, args.skill, slots_dir=args.slots_dir,
                             slots=slots, capture_dir=capture_dir, spawn_tics=args.spawn_tics,
                             item_tics=getattr(args, "item_tics", 0),
                             nodraw=args.nodraw, noblit=args.noblit, time_penalty=args.time_penalty,
                             waypoints=getattr(args, "waypoints", False),
                             combat=getattr(args, "combat", 0.5),
                             headless=getattr(args, "headless", True),
                             obs_size=getattr(args, "obs_size", None) or DEFAULT_OBS_SIZE, **rules)


def get_privileged_info(info: dict) -> list[float]:
    s = info.get("state", {})
    exit_dist = s.get("exit_dist")      # -1: the level has no exit line
    return [s.get("hp", 0) / 100.0, (exit_dist / 4096.0) if exit_dist is not None and exit_dist >= 0 else 0.0]


# ---------------------------------------------------------------------------
# Policy (SPEC.md §6, step 1: feedforward)
# ---------------------------------------------------------------------------

def layer_init(layer: nn.Module, std: float = np.sqrt(2), bias: float = 0.0) -> nn.Module:
    nn.init.orthogonal_(layer.weight, std)
    nn.init.constant_(layer.bias, bias)
    return layer


def conv_out(n: int) -> int:
    """A frame side n after the trunk's three convs (kernel/stride 8/4, 4/2,
    3/1): conv_out(n) = ((n - 8) // 4 + 1 - 4) // 2 + 1 - 2
    (conv_out(W) = 12 at W = 128, conv_out(H) = 6 at H = 80)."""
    return ((n - 8) // 4 + 1 - 4) // 2 + 1 - 2


def parse_obs_size(s: str) -> tuple[int, int]:
    """--obs-size: "WxH" (e.g. 160x100), or one number, the width W, with
    the height that keeps the screen's 320x200 aspect:
    H = round(W * 200 / 320) (H = 80 at W = 128)."""
    try:
        parts = [int(p) for p in s.lower().split("x")]
    except ValueError:
        raise argparse.ArgumentTypeError(f"--obs-size {s!r}: expected WxH, e.g. 84x84 or 160x100")
    if len(parts) == 1:
        parts.append(round(parts[0] * 200 / 320))
    if len(parts) != 2 or min(parts) < 36 or max(parts) > 320:
        raise argparse.ArgumentTypeError(f"--obs-size {s!r}: expected WxH, each 36..320")
    return parts[0], parts[1]


def checkpoint_obs_size(ckpt: dict) -> tuple[int, int]:
    """The (width, height) a checkpoint's policy was trained on."""
    return tuple(ckpt["obs_size"])


FIRE_PRIOR = 0.1


class Policy(nn.Module):
    """Nature CNN -> 512 features -> action logits; value head on
    detached features plus privileged inputs (critic, training only)."""

    def __init__(self, n_actions: int = len(ACTIONS), frames: int = 4,
                 obs_size: tuple[int, int] = DEFAULT_OBS_SIZE):
        super().__init__()
        self.obs_size = tuple(obs_size)          # (W, H) of each frame
        w, h = self.obs_size
        conv_w, conv_h = conv_out(w), conv_out(h)
        if conv_w < 1 or conv_h < 1:
            raise ValueError(f"observation size {w}x{h} is too small for the CNN (at least 36x36)")
        self.trunk = nn.Sequential(
            layer_init(nn.Conv2d(frames, 32, 8, stride=4)), nn.ReLU(),
            layer_init(nn.Conv2d(32, 64, 4, stride=2)), nn.ReLU(),
            layer_init(nn.Conv2d(64, 64, 3, stride=1)), nn.ReLU(),
            nn.Flatten(),
            layer_init(nn.Linear(64 * conv_w * conv_h, 512)), nn.ReLU(),   # in = 64 * conv_out(W) * conv_out(H) (64 * 12 * 6 = 4608)
        )
        self.policy_head = layer_init(nn.Linear(512, n_actions), std=0.01)
        # Fire (2026-11-19): the CTRL actions start FIRE_PRIOR likely in all
        # (half the actions, so a near-uniform policy fired half the time).
        if n_actions == len(ACTIONS):
            fire = torch.tensor([a.endswith("CTRL") for a in ACTIONS])
            with torch.no_grad():
                self.policy_head.bias[fire] = float(np.log(FIRE_PRIOR / fire.sum() * (~fire).sum() / (1 - FIRE_PRIOR)))
        self.value_head = layer_init(nn.Linear(512 + N_PRIV, 1), std=1.0)

    def forward(self, obs: torch.Tensor, priv: torch.Tensor):
        h = self.trunk(obs.float() / 255.0)
        logits = self.policy_head(h)
        value = self.value_head(torch.cat([h.detach(), priv], dim=1)).squeeze(1)
        return logits, value


def parse_mix(text: str) -> dict[str, int]:
    """--mix "combat=2,waypoint=2,maze=2,mixed=2" -> {type: groups}, every
    type of env.TYPES present (0 when left out)."""
    from env.env import TYPES
    out = dict.fromkeys(TYPES, 0)
    for part in (p.strip() for p in text.split(",") if p.strip()):
        name, _, n = part.partition("=")
        name = name.strip()
        if name not in out or not n.strip().isdigit():
            raise SystemExit(f"--mix: bad entry {part!r} (types: {', '.join(TYPES)}; e.g. combat=2)")
        out[name] = int(n)
    if sum(out.values()) == 0:
        raise SystemExit("--mix: no groups")
    return out


def load_weights(path: str) -> dict:
    return torch.load(path, map_location="cpu", weights_only=False)["policy"]


# ---------------------------------------------------------------------------
# IWADs, maps and group allocation (SPEC.md §7.1)
# ---------------------------------------------------------------------------

def parse_list(s: str) -> list[str]:
    return [x.strip() for x in s.split(",") if x.strip()]


def resolve_iwads(paths: list[str] | None) -> list[str | None]:
    """None stands for the env's default IWAD."""
    return [str(Path(w).resolve()) for w in paths] if paths else [None]


def iwad_label(w: str | None) -> str:
    return Path(w).stem if w else "default"


def env_label(w: str | None, m: str, iwads: list[str | None]) -> str:
    return f"{iwad_label(w)}:{m}" if len(iwads) > 1 else m


def capture_dirs(env_pairs: list, n: int, root: Path) -> list[str | None]:
    """Replay directories for --capture-envs n: the first n envs of each
    IWAD record (groups take the IWADs in rotation, so "the first n envs"
    alone would all be the first IWAD), into root/<IWAD>_<map>/ (2026-11-20;
    root/env<i>_<IWAD>/ before, which kept a moved env's later maps there
    too): an env that moves to another map (set_map) records into that
    map's folder."""
    from env.env import replay_dir_name
    seen: dict = {}
    out = []
    for i, (w, m) in enumerate(env_pairs):
        k = seen.get(w, 0)
        seen[w] = k + 1
        out.append(str(root / replay_dir_name(w, m)) if k < n else None)
    return out


def all_pairs(iwads: list[str | None], maps: list[str]) -> list[tuple[str | None, str]]:
    """Every IWAD/map pair, map by map with every IWAD on each: E1M1 on
    each IWAD, then E1M2 on each, ..."""
    return [(w, m) for m in maps for w in iwads]


def allocate_groups(iwads: list[str | None], maps: list[str], groups: int,
                    tag: str = "train", rotation: int | None = None,
                    pairs: list | None = None) -> list[tuple[str | None, str]]:
    """Group g -> (iwad, map): pair (rotation * groups + g) of all_pairs,
    wrapping around. Rotation 0 gives group g maps[(g // I) % M] on
    iwads[g % I]; I*M groups cover every pair at once. With fewer groups
    each rotation (MapRotation) moves them on to the next pairs, so every
    pair gets its turn; rotation None (no rotation) warns about the pairs
    no group plays. `pairs`: the pair order to deal from (MapRotation's
    shuffled one), all_pairs' by default."""
    pairs = pairs or all_pairs(iwads, maps)
    r = rotation or 0
    out = [pairs[(r * groups + g) % len(pairs)] for g in range(groups)]
    missing = [env_label(w, m, iwads) for w, m in pairs if (w, m) not in out]
    if missing and rotation is None:
        print(f"[{tag}] warning: {groups} groups don't cover {missing}; use --rotate-every to "
              f"take turns, or --groups {len(pairs)} to train every IWAD/map pair at once")
    return out


# Maps whose exit needs a boss death (E1M8's walls, E2M8/E3M8/E4M6/E4M8
# end the level when the boss dies; Doom II's MAP07 and MAP30): with
# stage 1's --eval (no monsters) they can't be finished, so --maps all skips them.
BOSS_MAPS = {"1 8", "2 8", "3 8", "4 6", "4 8", "7", "30"}
# The drill maps (2026-11-20; E1M1 of Ultimate Doom alone before): the combat
# and waypoint drills rotate over these, per IWAD label. Picked from a survey
# of every E1-E4 map: the most open area connected to the player start (wide
# openings, floors within a step), no damaging floor there, and at least
# ~400 units of sightline ahead and 20 degrees to each side (the combat
# drill spawns 192-768 units ahead within 20 degrees).
DRILL_MAPS = {"DOOM": ["3 2", "1 2"], "FREEDOOM1": ["1 8", "4 3"]}


def drill_pairs(iwads: list) -> list[tuple]:
    """The (iwad, warp) pairs the drills rotate over, for the IWADs in use."""
    return [(w, m) for w in iwads if w for m in DRILL_MAPS.get(iwad_label(w).upper(), [])]


# --groups default: one group per IWAD/map pair, at most this many (the
# rest take turns, --rotate-every).
FREE_CORES = 2   # logical cores the default group count leaves unused


def available_cores() -> int:
    """Logical cores this process may run on (its CPU affinity)."""
    try:
        return len(os.sched_getaffinity(0))
    except AttributeError:            # not Linux
        return os.cpu_count() or 1


def max_default_groups(cores: int | None = None) -> int:
    """The default group count's ceiling: the logical cores minus
    FREE_CORES (at least 1)."""
    return max(1, (available_cores() if cores is None else cores) - FREE_CORES)


def wad_maps(path: str) -> list[str]:
    """The maps in a WAD, as -warp args ("1 1" for E1M1, "1" for MAP01)."""
    import struct
    with open(path, "rb") as f:
        head = f.read(12)
        n, off = struct.unpack_from("<4xii", head)
        f.seek(off)
        d = f.read(16 * n)
    out = []
    for i in range(n):
        name = d[16 * i + 8:16 * i + 16].rstrip(b"\0").decode("ascii", "replace").upper()
        if len(name) == 4 and name[0] == "E" and name[2] == "M" and name[1].isdigit() and name[3].isdigit():
            out.append(f"{name[1]} {name[3]}")
        elif len(name) == 5 and name.startswith("MAP") and name[3:].isdigit():
            out.append(str(int(name[3:])))
    return out


def shared_maps(iwads: list[str | None]) -> tuple[list[str], list[str]]:
    """The maps every IWAD has, in the first IWAD's order, and that IWAD's
    whole map list."""
    from env.engine import DEFAULT_IWAD
    lists = [wad_maps(w or str(DEFAULT_IWAD)) for w in iwads]
    return [m for m in lists[0] if all(m in l for l in lists[1:])], lists[0]


def pick_eval_maps(spec: str, count: int, maps_spec: str, iwads: list[str | None], stage: int,
                   rng: np.random.Generator, tag: str = "train") -> list[str]:
    """--eval-maps: a comma-separated -warp list, '' (no evals), or
    "random": `count` maps drawn with `rng` (a fresh seed each session)
    from the maps every IWAD has, minus the training maps (unless --maps
    is "all", which is chosen after the eval maps and leaves them out)
    and, in stage 1, BOSS_MAPS. Listed in map order."""
    if spec.strip().lower() != "random":
        return parse_list(spec)
    pool, _ = shared_maps(iwads)
    skip = set(BOSS_MAPS) if stage == 1 else set()
    if maps_spec.strip().lower() != "all":
        skip |= set(parse_list(maps_spec))
    pool = [m for m in pool if m not in skip]
    if count <= 0 or not pool:
        print(f"[{tag}] --eval-maps random: no maps to hold out" + ("" if pool else " (none left)")
              + "; evals off")
        return []
    picked = set(rng.choice(len(pool), size=min(count, len(pool)), replace=False).tolist())
    return [m for i, m in enumerate(pool) if i in picked]


def resolve_maps(spec: str, iwads: list[str | None], stage: int, eval_maps: list[str],
                 tag: str = "train") -> list[str]:
    """--maps: a comma-separated -warp list, or "all": every map all the
    IWADs have, minus the held-out eval maps and, in stage 1, BOSS_MAPS."""
    if spec.strip().lower() != "all":
        return parse_list(spec)
    maps, first = shared_maps(iwads)
    skip = set(eval_maps) | (BOSS_MAPS if stage == 1 else set())
    maps = [m for m in maps if m not in skip]
    print(f"[{tag}] --maps all: {len(maps)} maps ({', '.join(maps)}); skipped "
          f"{sorted(skip & set(first))} (held out" + (", boss exits" if stage == 1 else "") + ")")
    return maps


def default_groups(iwads: list, maps: list, cores: int | None = None) -> int:
    """One group per IWAD/map pair, at most the logical cores minus
    FREE_CORES (more pairs than that rotate, MapRotation)."""
    return min(len(iwads) * len(maps), max_default_groups(cores))


class MapRotation:
    """Groups take turns on the IWAD/map pairs when there are more pairs
    than groups: every `every` batches they move on to the next block of
    pairs (allocate_groups' rotation), so all of them get trained. Off
    (one fixed allocation) when the groups cover every pair or every is 0.
    `rng`: shuffle the pair order once (train.py: a fresh seed each
    session), so each session rotates through the pairs in its own order
    and groups get different pairs; None keeps all_pairs' order."""

    def __init__(self, iwads: list, maps: list, groups: int, every: int, tag: str = "train",
                 rng: np.random.Generator | None = None):
        self.iwads, self.maps, self.groups, self.tag = iwads, maps, groups, tag
        self.pairs = all_pairs(iwads, maps)
        if rng is not None:
            self.pairs = [self.pairs[i] for i in rng.permutation(len(self.pairs))]
        self.n_pairs = len(self.pairs)
        self.every = every if every > 0 and groups < self.n_pairs else 0
        self.rotation: int | None = None

    def at(self, batch: int) -> list | None:
        """The group pairs for batch `batch` if they change (the first
        call always returns them), else None."""
        r = batch // self.every if self.every else 0
        if r == self.rotation:
            return None
        self.rotation = r
        return allocate_groups(self.iwads, self.maps, self.groups, self.tag,
                               rotation=r if self.every else None, pairs=self.pairs)


def move_groups(vec, frontier, budget, old_pairs: list, new_pairs: list) -> list[int]:
    """Move the envs whose (iwad, map) changed: their games restart on the
    new map at the next reset (set_map), and the frontier and tic budget
    follow. Returns the moved envs."""
    by_pair: dict = {}
    for i, (o, n) in enumerate(zip(old_pairs, new_pairs)):
        if o != n:
            by_pair.setdefault(n, []).append(i)
    for (w, m), idx in by_pair.items():
        vec.call(idx, "set_map", (w, m))
    moved = sorted(i for idx in by_pair.values() for i in idx)
    frontier.assign(vec, new_pairs, moved)
    budget.assign(new_pairs)
    return moved


def add_env_args(p) -> None:
    """Arguments shared by everything that spawns training envs."""
    p.add_argument("--obs-size", type=parse_obs_size, default=None, metavar="WxH",
                   help="observation frame size, width x height, or just the width, the height then "
                        "keeping the screen's 320x200 aspect (default: a resumed run's own, else "
                        f"{DEFAULT_OBS_SIZE[0]}x{DEFAULT_OBS_SIZE[1]}). A run keeps the size it started "
                        "with: resuming with another size stops. eval.py takes it from the checkpoint")
    p.add_argument("--stage", type=int, choices=[1, 2], default=1,
                   help="1: --combat of the groups fight the map monsters, the rest have them hidden "
                        "(no map monsters at all before 2026-11-18); 2: every group fights. train.py: only untyped resets (eval.py); --mix sets the training groups since 2026-11-20")
    p.add_argument("--combat", type=float, default=0.5,
                   help="stage 1: the share of groups that fight (the rest explore with the map "
                        "monsters hidden; per group, from its seed). train.py: only untyped resets; "
                        "--mix sets the training groups since 2026-11-20")
    p.add_argument("--spawn-tics", type=int, default=0,   # 350 before 2026-11-18: the policy fights now
                   help="mean game tics between practice monsters spawned on screen while exploring "
                        "(-rlspawn: frozen practice monsters, no longer a CONSULT trigger since 2026-11-18; "
                        "0: none, the default)")
    p.add_argument("--item-tics", type=int, default=420,
                   help="mean game tics between practice items (random Diablo loot: weapons, armor...) "
                        "spawned on screen on the way to a portal, to walk to and pick up (-rlitems; 0: none)")
    p.add_argument("--noblit", action=argparse.BooleanOptionalAction, default=True,   # False before 2026-11-19
                   help="launch the games with -noblit: skip the SDL blit nobody sees headless "
                        "(~4x more decisions/s; frames and replays unchanged; the default, --no-noblit to blit)")
    p.add_argument("--nodraw", action=argparse.BooleanOptionalAction, default=False,
                   help="launch the games with -nodraw: no rendering at all (faster still), but the "
                        "observation frame is never drawn, so the policy is blind, and replay "
                        "capture is off (benchmarks only)")
    p.add_argument("--iwads", nargs="+", default=None, metavar="WAD",
                   help="IWADs to train on (default: the env's, Freedoom Phase 1). Group g plays "
                        "maps[(g // I) %% M] on iwads[g %% I]: every map on every IWAD, map by map; "
                        "the default --groups I*M covers every IWAD/map pair")
    p.add_argument("--maps", default="1 1,1 2,1 3,1 4",
                   help="training maps as -warp args, comma-separated, or 'all': every map the IWADs "
                        "share, minus --eval-maps and (stage 1) the boss-exit maps")
    p.add_argument("--rotate-every", type=int, default=10,
                   help="with more IWAD/map pairs than groups: batches a group plays its map before "
                        "the groups move on to the next pairs (0: no rotation, the extra pairs never play)")
    p.add_argument("--eval-maps", default="random",
                   help="held-out maps for evaluation, comma-separated ('' to skip); 'random' (default): "
                        "--eval-count maps drawn fresh each session from the maps every IWAD has, minus "
                        "the training maps and (stage 1) the boss-exit maps")
    p.add_argument("--eval-count", type=int, default=2, help="--eval-maps random: how many maps")
    p.add_argument("--map-seed", type=int, default=None,
                   help="seed for the session's map choices (the --rotate-every order and --eval-maps "
                        "random); default: a fresh one each session, printed and saved in config.json")
    p.add_argument("--eval-episodes", type=int, default=2, help="rollouts per eval map")
    p.add_argument("--skill", type=int, default=2,   # 3 before 2026-11-20
                   help="Doom skill 1-5; 2 places the easy set of monsters (no skill-3 ambush closets) "
                        "at normal damage")
    p.add_argument("--slots-dir", default=None, help="prepared save slots copied into each instance")
    p.add_argument("--slots", default="", help="start slots in --slots-dir, e.g. '3,4,5'")
    p.add_argument("--groups", type=int, default=None,
                   help="GRPO groups (default: one per IWAD/map pair, len(--iwads) x len(--maps), "
                        "at most the logical cores minus 2; with more pairs than groups they take "
                        "turns, --rotate-every)")
    p.add_argument("--group-size", type=int, default=8)
    p.add_argument("--mix", default="combat=3,waypoint=2,maze=1,mixed=2",   # combat=2,waypoint=2,maze=2,mixed=2 before 2026-11-20
                   help="groups per rollout type (2026-11-20; env.TYPES), adding up to --groups (whose default becomes their sum): combat (the combat drill: the drill maps (DRILL_MAPS), one awake monster ahead at a time, every sector visited from the start), waypoint (the waypoint drill: the drill maps, endless waypoints, frequent practice items), maze (the rotating maps, monsters hidden, no waypoints), mixed (the rotating maps, monsters and waypoints). The drills rotate over the drill maps of the --iwads every --rotate-every batches, always start at the map start and end only on death, the exit or --drill-tics; maze and mixed rotate over the maps with the frontier and tic budget. Every type has the same reward function. E.g. combat=8: the combat drill alone")
    p.add_argument("--drill-tics", type=int, default=1050,
                   help="a drill rollout's limit in game tics (combat and waypoint; default 30 s)")
    p.add_argument("--max-tics", type=int, default=525,
                   help="rollout limit in game tics (35/s; default 15 s)")
    p.add_argument("--portal-goal", type=int, default=1,
                   help="a rollout ends (+portal bonus) once it enters this many sectors not yet "
                        "visited -- through a portal; with --frontier the next batch starts beyond it "
                        "(0: rollouts run to the exit, death or --max-tics)")
    p.add_argument("--time-penalty", type=float, default=1 / 3,
                   help="reward lost per game second until the rollout ends, so the faster portal "
                        "crossing wins its group (default 1/3: a point every 3 s)")
    p.add_argument("--waypoints", action=argparse.BooleanOptionalAction, default=True,
                   help="the env sets WAYPOINTs at random points of the current sector, standing in for "
                        "the VLM in production; the clock, fast and look follow them (2026-11-18); "
                        "eval.py --iwad sends them on the same schedule")
    p.add_argument("--eval-max-tics", type=int, default=4200,  # 2100 before 2026-11-18
                   help="held-out eval rollout limit (the training rewards, but no portal or key goal "
                        "ending it early: sectors visited in 120 s, comparable across versions)")
    p.add_argument("--frontier", action=argparse.BooleanOptionalAction, default=True,
                   help="keep progress: every group loads its start from save slot 4; after each "
                        "batch, each map's highest-scoring rollout that made progress saves to slot 5 "
                        "(with its visited sectors) and slots 4 and 5 swap, so the next batch starts "
                        "from it and training moves on portal by portal past the --max-tics limit; an "
                        "exit, or --frontier-patience batches without progress, clears it (slot 4 holds "
                        "the map start again). --no-frontier: always the map start (still slot 4)")
    p.add_argument("--frontier-patience", type=int, default=5,
                   help="batches in a row without progress from a map's frontier, at the full "
                        "--max-tics-cap, before it is cleared (a dead end: back to the map start)")
    p.add_argument("--max-tics-cap", type=int, default=2100,
                   help="per-map tic budget: after a batch where no rollout on a map reaches a new "
                        "sector (or the exit), that map's rollout limit grows by --tics-growth, up to "
                        "this; progress puts it back to --max-tics (cap <= --max-tics: off)")
    p.add_argument("--tics-growth", type=float, default=1.5,
                   help="factor the per-map tic budget grows by after a batch without progress")
    p.add_argument("--warmup-max", type=int, default=0,   # 40 before 2026-11-20
                   help="random warm-up actions per group start (0..N, each held for WARMUP_HOLD "
                        "tics), for start-state variety; off by default: with real-time combat "
                        "the random walk got the player hurt or killed before the rollout began")


# ---------------------------------------------------------------------------
# Frontier starts (SPEC.md §5.4): progress kept across the rollout limit
# ---------------------------------------------------------------------------

# Every group loads its start from slot 4 (the env keeps the map start
# there, EnvConfig.bootstrap_slot, until a frontier replaces it); the best
# rollout saves its progress to slot 5, so the start is never overwritten
# mid-batch; then slots 4 and 5 swap on every instance of that map.
FRONTIER_SLOT = 4
SAVE_SLOT = 5
FRONTIER_HP = 0.8   # a progress save needs this share of max HP (2026-11-19)


class Frontier:
    """Per (IWAD, map): the end state of the best rollout so far, as a
    start state. After each batch, the highest-return rollout on a map
    that ended alive, exploring, at FRONTIER_HP of max HP or more, with no
    monster in its sector after the player, short of the exit and with progress
    (a new sector: with a portal goal, through the portal; or a new key,
    which ends a training rollout) -- only the
    highest score -- saves the game to slot 5 (SAVE 5, where it stopped;
    slot 4, the start every group loaded, stays untouched) with its
    visited sectors (VISITED). The save is copied into slot 5 of every
    instance on that map, and then slots 4 and 5 swap there: the
    progress is the next batch's start in slot 4 (the visited list is
    replayed after each load, so the portal it came through is not new),
    and the old start sits in 5. The saves live only in memory and in
    the instances: a new run (a restart, another machine) starts every
    map from its beginning, while the map rotation resumes from the
    checkpoint's iteration. Every group starts from slot 4, so all members of a
    group start from the identical state: the agent moves on portal by
    portal. A batch without
    progress keeps the current start (and grows the map's tic budget,
    TicBudget); `patience` of them in a row at the full budget (a
    dead end) clear the map's frontier, and so does a rollout reaching
    the exit (the level is done): slot 4 holds the map start again for
    the next batch. Every rollout of the batch still runs to its own
    end; an exit stops none of the others."""

    def __init__(self, enabled: bool, env_pairs: list, tag: str = "train", patience: int = 5):
        self.enabled, self.tag, self.patience = enabled, tag, patience
        self.labels = [f"{iwad_label(w)}_{m.replace(' ', '_')}" for w, m in env_pairs]
        self.data: dict[str, bytes] = {}
        self.visited: dict[str, str | None] = {}
        self.stale: dict[str, int] = {}
        # What the last update did per map, for the rollout log:
        # {label: {"result": saved|exit|stale|kept, "env": i, "stale": n}}.
        self.last: dict[str, dict] = {}

    def _envs(self, label: str) -> list[int]:
        return [i for i, l in enumerate(self.labels) if l == label]

    def assign(self, vec, env_pairs: list, moved: list[int]) -> None:
        """The envs now play env_pairs (MapRotation): the moved envs get
        their new map's frontier (kept per map while other maps have their
        turn, so a map picks up where it was left)."""
        self.labels = [f"{iwad_label(w)}_{m.replace(' ', '_')}" for w, m in env_pairs]
        for label in set(self.labels[i] for i in moved):
            idx = [i for i in moved if self.labels[i] == label]
            if label in self.data:
                vec.call(idx, "install_state", (FRONTIER_SLOT, self.data[label], self.visited.get(label)))

    def options(self, options: list[dict], G: int) -> list[dict]:
        """Start every group (all G members alike) on a map with a
        frontier from it. The other groups keep their start: the map
        start, in the same slot, unless --slots picked a prepared one."""
        for g in range(len(options) // G):
            if self.enabled and self.labels[g * G] in self.data:
                for o in options[g * G:(g + 1) * G]:
                    o["slot"] = FRONTIER_SLOT
                    o["frontier"] = True
        return options

    def update(self, vec, recs: list[dict], hold=frozenset()) -> dict:
        """After a batch: save each map's best rollout end state (or clear
        the map's frontier on an exit: every rollout still ran to its own
        end). Returns {label: return saved}.
        A batch without progress on a map in `hold` (its tic budget is
        still growing, TicBudget.growing) doesn't count towards patience."""
        self.last = {}
        if not self.enabled:
            return {}
        saved = {}
        for label in sorted(set(self.labels)):
            idx = self._envs(label)
            if any(recs[i]["exit"] for i in idx):
                self._clear(vec, label, idx)
                saved[label] = None
                self.last[label] = {"result": "exit", "env": next(i for i in idx if recs[i]["exit"])}
                continue
            ok = [i for i in idx if not recs[i]["death"]
                  and (recs[i]["sectors_gained"] > 0 or recs[i].get("key"))
                  and recs[i]["end_info"].get("mode") == "EXPLORE"
                  and self._safe(recs[i]["end_info"].get("state", {}))]
            data = None
            if ok:
                best = max(ok, key=lambda i: recs[i]["ret"])   # only the highest score is saved
                data = vec.call([best], "save_state", (SAVE_SLOT,))[0]
            if data is None:
                # No progress from this start: keep it, unless it has
                # been a dead end for `patience` batches in a row.
                self.last[label] = {"result": "kept", "stale": self.stale.get(label, 0)}
                if label in self.data and label not in hold:
                    self.stale[label] = self.stale.get(label, 0) + 1
                    self.last[label]["stale"] = self.stale[label]
                    if self.stale[label] >= self.patience:
                        self._clear(vec, label, idx)
                        saved[label] = None
                        self.last[label]["result"] = "stale"
                continue
            visited = vec.call([best], "visited")[0]
            if visited is None and not getattr(self, "_warned", False):
                self._warned = True
                print(f"[{self.tag}] warning: the game sent no VISITED reply (a binary older than "
                      f"2026-11-14? rebuild with scripts/build.sh --release): progress saves carry no "
                      f"visited list, so the portal a rollout came in by counts as new")
            self.data[label], self.visited[label], self.stale[label] = data, visited, 0
            self.last[label] = {"result": "saved", "env": best}
            vec.call(idx, "install_state", (SAVE_SLOT, data, visited))   # the new progress in 5...
            vec.call(idx, "swap_slots", (FRONTIER_SLOT, SAVE_SLOT))     # ...becomes the start in 4
            saved[label] = round(recs[best]["ret"], 2)
        return saved

    @staticmethod
    def _safe(s: dict) -> bool:
        """Healthy and not hunted (the state line's hunted: a monster in
        the player's sector targets the player)."""
        return not s.get("hunted") and s.get("hp", 0) >= FRONTIER_HP * (s.get("hp_max") or 100)

    def _clear(self, vec, label: str, idx: list[int]) -> None:
        """Back to the map start: forget the map's frontier everywhere."""
        self.stale.pop(label, None)
        self.visited.pop(label, None)
        if label in self.data:
            del self.data[label]
            vec.call(idx, "install_state", (FRONTIER_SLOT, None))


class TicBudget:
    """Per (IWAD, map): the rollout limit in game tics. It starts at
    `base` (--max-tics); after a batch where no rollout on the map reached
    a new sector, a new key or the exit, it grows by `growth`, up to `cap`
    (--max-tics-cap), and any progress puts it back to `base`. Progress
    still means getting through a portal (or a key): a longer budget gives the agent
    time to find the path, without rewarding a start that only got near
    one. Not saved: like the progress saves, a new run starts every map
    over, at `base`."""

    def __init__(self, env_pairs: list, base: int, cap: int, growth: float = 1.5):
        self.base, self.cap, self.growth = base, max(cap, base), growth
        self.tics: dict[str, int] = {}
        self._applied: dict[int, int] = {}   # env -> the limit it was last sent
        self.assign(env_pairs)

    def assign(self, env_pairs: list) -> None:
        """The envs now play env_pairs (MapRotation); a map's budget is
        kept while other maps have their turn."""
        self.labels = [f"{iwad_label(w)}_{m.replace(' ', '_')}" for w, m in env_pairs]
        for label in self.labels:
            self.tics.setdefault(label, self.base)

    def _envs(self, label: str) -> list[int]:
        return [i for i, lab in enumerate(self.labels) if lab == label]

    def apply(self, vec) -> None:
        """Before a batch: set each env's map limit where it changed."""
        by_n: dict[int, list[int]] = {}
        for i, label in enumerate(self.labels):
            n = self.tics[label]
            if self._applied.get(i) != n:
                by_n.setdefault(n, []).append(i)
                self._applied[i] = n
        for n, idx in sorted(by_n.items()):
            vec.call(idx, "set_max_tics", (n,))

    def growing(self) -> frozenset:
        """Maps in play whose budget is still below the cap."""
        return frozenset(label for label in self.labels if self.tics[label] < self.cap)

    def update(self, recs: list[dict], cleared=()) -> dict:
        """After a batch: grow or reset each map's budget; a map whose
        frontier was cleared (back to the map start) resets too. Returns
        {label: new limit} for the maps that changed."""
        changed = {}
        for label in sorted(set(self.labels)):
            idx = self._envs(label)
            progress = label in cleared or any(recs[i]["sectors_gained"] > 0 or recs[i]["exit"] or recs[i].get("key")
                                                for i in idx)
            n = self.base if progress else min(self.cap, int(round(self.tics[label] * self.growth)))
            if n != self.tics[label]:
                self.tics[label] = changed[label] = n
        return changed


def check_slots(iwads: list[str | None], slots: list[int]) -> None:
    # Save slots are IWAD-specific (a save loaded under another IWAD's
    # maps is garbage), so they can't be mixed.
    if len(iwads) > 1 and slots:
        sys.exit("--slots can't be combined with several --iwads (save slots are IWAD-specific)")
    if FRONTIER_SLOT in slots or SAVE_SLOT in slots:
        sys.exit(f"--slots can't use slots {FRONTIER_SLOT} or {SAVE_SLOT}: training loads its start from "
                 f"{FRONTIER_SLOT} and saves progress to {SAVE_SLOT}")


# ---------------------------------------------------------------------------
# Atomic files and checkpoints (SPEC.md §7.4)
# ---------------------------------------------------------------------------

def _fsync_dir(d: Path) -> None:
    fd = os.open(d, os.O_RDONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def atomic_torch_save(obj, path: Path) -> None:
    tmp = path.with_name(path.name + ".tmp")
    with open(tmp, "wb") as f:
        torch.save(obj, f)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)
    _fsync_dir(path.parent)


# The run's one checkpoint, in --run-dir and in the Hub repo: replaced
# (atomically) after every update. Its iteration is inside it ("iteration").
CKPT_NAME = "doom-runner.pt"


def save_checkpoint(path: Path, state: dict) -> None:
    atomic_torch_save(state, path)


def load_latest_checkpoint(run_dir: Path, quiet: bool = False) -> dict | None:
    """run_dir's checkpoint (CKPT_NAME); None when there is none.
    Exits if the checkpoint can't be loaded: a silent fresh start would
    replace it (and the Hub's copy) at the next save."""
    for tmp in run_dir.glob("*.pt.tmp"):
        tmp.unlink(missing_ok=True)       # interrupted write
    path = run_dir / CKPT_NAME
    if not path.exists():
        return None
    try:
        ckpt = torch.load(path, map_location="cpu", weights_only=False)
    except Exception as e:
        sys.exit(f"[ckpt] cannot load {path}: {e}; move it aside to start this run directory fresh")
    if not quiet:
        print(f"[ckpt] resuming from {path} (iteration {ckpt['iteration']})")
    return ckpt


def rng_state(sampler: np.random.Generator | None = None) -> dict:
    return {
        "python": random.getstate(),
        "numpy": np.random.get_state(),
        "torch": torch.get_rng_state(),
        "cuda": torch.cuda.get_rng_state_all() if torch.cuda.is_available() else None,
        "sampler": sampler.bit_generator.state if sampler is not None else None,
    }


def restore_rng(state: dict, sampler: np.random.Generator | None = None) -> None:
    random.setstate(state["python"])
    np.random.set_state(state["numpy"])
    torch.set_rng_state(state["torch"])
    if state.get("cuda") is not None and torch.cuda.is_available():
        torch.cuda.set_rng_state_all(state["cuda"])
    if sampler is not None and state.get("sampler") is not None:
        sampler.bit_generator.state = state["sampler"]
