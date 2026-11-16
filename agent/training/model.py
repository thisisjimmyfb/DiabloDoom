"""Shared pieces of the trainer (SPEC.md §6, §7.1, §7.4): the policy
network, the env factory, IWAD/group allocation and map rotation, progress
saves (Frontier), tic budgets, checkpoint I/O and the Hugging Face Hub copy
of the checkpoint (hub_pull, HubUploader). Used by train.py, with
eval.py (rollouts, logs, held-out eval) and learn.py (the update)."""

from __future__ import annotations

import argparse
import json
import os
import random
import shutil
import subprocess
import sys
from pathlib import Path

import numpy as np
import torch
import torch.nn as nn

sys.path.insert(0, str(Path(__file__).resolve().parent))
from env import ACTIONS, CONSULT  # noqa: E402

# Warm-up uses movement only (no CONSULT), each action held for
# WARMUP_HOLD tics (decisions are per tic: single random tics would
# barely move the player).
WARMUP_ACTIONS = [i for i in range(len(ACTIONS)) if i != CONSULT]
WARMUP_HOLD = 15
# Older action sets, by output count, and the current action each old
# output carries over to (2026-11-15: per-tic PRESS keys replaced the
# multi-tic WALK 0 / TURNL / TURNR / STRAFE_L / STRAFE_R).
OLD_ACTION_SETS = {
    8: ["WALK 0", "TURNL", "TURNR", "STRAFE_L", "STRAFE_R", "USE", "MAP", "CONSULT"],
    7: ["WALK 0", "TURNL", "TURNR", "STRAFE_L", "STRAFE_R", "USE", "CONSULT"],
}
OLD_TO_NEW = {"WALK 0": "PRESS W", "TURNL": "PRESS L", "TURNR": "PRESS R",
              "STRAFE_L": "PRESS A", "STRAFE_R": "PRESS D", "USE": "USE", "CONSULT": "CONSULT"}
N_PRIV = 2  # privileged critic inputs: HP/100, exit path distance (SPEC.md §6)


# ---------------------------------------------------------------------------
# Environment factory (top-level so worker processes can unpickle it)
# ---------------------------------------------------------------------------

def make_env(iwad: str | None, warp: str, stage: int, skill: int, max_tics: int,
             slots_dir: str | None, slots: list[int], capture_dir: str | None = None,
             spawn_tics: int = 0, nodraw: bool = False, noblit: bool = False,
             portal_goal: int = 0, time_penalty: float = 0.0, key_goal: bool = False):
    """time_penalty: reward per game second (negative pays, e.g. 1/3
    costs a point every 3 s), with portal_goal n > 0 ending a rollout
    once it reaches n new sectors, and key_goal ending it on a new key."""
    from env import DiabloDoomEnv, EngineConfig, EnvConfig, RewardConfig, standin_consult
    # Stage 1 (SPEC.md §7.2): no map monsters. Vanilla -nomonsters is the
    # -rlnocombat of the spec. Stage 2: map monsters on. In both,
    # -rlspawn puts practice monsters on screen (a sighting to CONSULT
    # about), and the stand-in answers CONSULT.
    extra = ["-nomonsters"] if stage == 1 else []
    if spawn_tics > 0:
        extra += ["-rlspawn", str(spawn_tics)]
    if noblit:
        # Skip the SDL blit (I_FinishUpdate), which headless nobody sees:
        # most of the game's CPU time. The frame the policy gets and the
        # replays come from the 8-bit buffer, so they are unchanged.
        extra.append("-noblit")
    if nodraw:
        # No rendering at all (D_Display and the blit skipped): faster
        # still, but the frame is never drawn and replays are off.
        extra.append("-nodraw")
    engine = EngineConfig(warp=warp, skill=skill, extra_args=extra,
                          savedir_template=slots_dir, capture_dir=capture_dir)
    if iwad:
        engine.iwad = iwad
    return DiabloDoomEnv(EnvConfig(
        engine=engine,
        max_tics=max_tics,
        start_slots=list(slots),
        consult_handler=standin_consult,
        portal_goal=portal_goal,
        key_goal=key_goal,
        # A fast tic pays back the clock's base rate (RewardConfig.fast);
        # facing a way on pays up to half of it (RewardConfig.look).
        rewards=RewardConfig(time=time_penalty / 35.0, fast=time_penalty / 35.0,
                             look=time_penalty / 35.0 / 2),
    ))


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
                             nodraw=args.nodraw, noblit=args.noblit, time_penalty=args.time_penalty,
                             **rules)


def privileged(info: dict) -> list[float]:
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


class Policy(nn.Module):
    """Nature CNN -> 512 features -> action logits; value head on
    detached features plus privileged inputs (critic, training only)."""

    def __init__(self, n_actions: int = len(ACTIONS), frames: int = 4):
        super().__init__()
        self.trunk = nn.Sequential(
            layer_init(nn.Conv2d(frames, 32, 8, stride=4)), nn.ReLU(),
            layer_init(nn.Conv2d(32, 64, 4, stride=2)), nn.ReLU(),
            layer_init(nn.Conv2d(64, 64, 3, stride=1)), nn.ReLU(),
            nn.Flatten(),
            layer_init(nn.Linear(64 * 7 * 7, 512)), nn.ReLU(),
        )
        self.policy_head = layer_init(nn.Linear(512, n_actions), std=0.01)
        self.value_head = layer_init(nn.Linear(512 + N_PRIV, 1), std=1.0)

    def forward(self, obs: torch.Tensor, priv: torch.Tensor):
        h = self.trunk(obs.float() / 255.0)
        logits = self.policy_head(h)
        value = self.value_head(torch.cat([h.detach(), priv], dim=1)).squeeze(1)
        return logits, value


def _old_rows(n_old: int) -> dict[int, int]:
    """{current output: older output} for an older action set's head."""
    old = OLD_ACTION_SETS[n_old]
    return {ACTIONS.index(OLD_TO_NEW[name]): i for i, name in enumerate(old) if name in OLD_TO_NEW}


def _upgrade_rows(t: torch.Tensor, fresh: torch.Tensor) -> torch.Tensor:
    """Policy-head rows (or their Adam moments) of an older action set ->
    the current one: each old output that carries over keeps its row,
    every other current output takes the fresh row."""
    out = fresh.to(t).clone()
    for new, old in _old_rows(t.shape[0]).items():
        out[new] = t[old]
    return out


def upgrade_policy_state(state: dict) -> dict:
    """A policy state dict from an older action set (OLD_ACTION_SETS) ->
    the current one. Each old output keeps its weights in the place of
    the action it carries over to (OLD_TO_NEW: WALK 0 -> PRESS W, the
    45-degree turns -> PRESS L/R, the strafes -> PRESS A/D; MAP is
    dropped); the other outputs are initialised like a new head (a fixed
    seed, so every load gives the same policy) with the old outputs'
    mean bias, so each starts near an average action's probability.
    A current-size state, or one of an action set this version doesn't
    know (the caller reports the mismatch), is returned unchanged."""
    w = state.get("policy_head.weight")
    if w is None or w.shape[0] not in OLD_ACTION_SETS:
        return state
    state = dict(state)
    gen = torch.Generator().manual_seed(0)
    fresh_w = torch.empty(len(ACTIONS), w.shape[1])
    nn.init.orthogonal_(fresh_w, 0.01, generator=gen)
    b = state["policy_head.bias"]
    state["policy_head.weight"] = _upgrade_rows(w, fresh_w)
    state["policy_head.bias"] = _upgrade_rows(b, torch.full((len(ACTIONS),), float(b.mean())))
    print(f"[ckpt] upgraded a {w.shape[0]}-action policy to {len(ACTIONS)} actions")
    return state


def upgrade_optimizer_state(opt_state: dict, policy) -> dict:
    """The Adam state of an older action set's checkpoint -> the current
    policy: the policy head's moment estimates follow its rows (zeros
    for the outputs that are new)."""
    names = [n for n, _ in policy.named_parameters()]
    for idx in (names.index("policy_head.weight"), names.index("policy_head.bias")):
        st = opt_state["state"].get(idx)
        if not st:
            continue
        for k in ("exp_avg", "exp_avg_sq"):
            if k in st and st[k].shape[0] != len(ACTIONS):
                st[k] = _upgrade_rows(st[k], torch.zeros((len(ACTIONS), *st[k].shape[1:])))
    return opt_state


def load_weights(path: str) -> dict:
    ckpt = torch.load(path, map_location="cpu", weights_only=False)
    return upgrade_policy_state(ckpt["policy"] if "policy" in ckpt else ckpt)


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
    IWAD record, into root/env<i>_<IWAD>/ (groups take the IWADs in
    rotation, so "the first n envs" alone would all be the first IWAD)."""
    seen: dict = {}
    out = []
    for i, (w, _) in enumerate(env_pairs):
        k = seen.get(w, 0)
        seen[w] = k + 1
        out.append(str(root / f"env{i:02d}_{iwad_label(w)}") if k < n else None)
    return out


def all_pairs(iwads: list[str | None], maps: list[str]) -> list[tuple[str | None, str]]:
    """Every IWAD/map pair, map by map with every IWAD on each: E1M1 on
    each IWAD, then E1M2 on each, ..."""
    return [(w, m) for m in maps for w in iwads]


def allocate_groups(iwads: list[str | None], maps: list[str], groups: int,
                    tag: str = "train", rotation: int | None = None) -> list[tuple[str | None, str]]:
    """Group g -> (iwad, map): pair (rotation * groups + g) of all_pairs,
    wrapping around. Rotation 0 gives group g maps[(g // I) % M] on
    iwads[g % I]; I*M groups cover every pair at once. With fewer groups
    each rotation (MapRotation) moves them on to the next pairs, so every
    pair gets its turn; rotation None (no rotation) warns about the pairs
    no group plays."""
    pairs = all_pairs(iwads, maps)
    r = rotation or 0
    out = [pairs[(r * groups + g) % len(pairs)] for g in range(groups)]
    missing = [env_label(w, m, iwads) for w, m in pairs if (w, m) not in out]
    if missing and rotation is None:
        print(f"[{tag}] warning: {groups} groups don't cover {missing}; use --rotate-every to "
              f"take turns, or --groups {len(pairs)} to train every IWAD/map pair at once")
    return out


# Maps whose exit needs a boss death (E1M8's walls, E2M8/E3M8/E4M6/E4M8
# end the level when the boss dies; Doom II's MAP07 and MAP30): with
# stage 1's -nomonsters they can't be finished, so --maps all skips them.
BOSS_MAPS = {"1 8", "2 8", "3 8", "4 6", "4 8", "7", "30"}
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


def resolve_maps(spec: str, iwads: list[str | None], stage: int, eval_maps: list[str],
                 tag: str = "train") -> list[str]:
    """--maps: a comma-separated -warp list, or "all": every map all the
    IWADs have, minus the held-out eval maps and, in stage 1, BOSS_MAPS."""
    if spec.strip().lower() != "all":
        return parse_list(spec)
    from env.engine import DEFAULT_IWAD
    lists = [wad_maps(w or str(DEFAULT_IWAD)) for w in iwads]
    maps = [m for m in lists[0] if all(m in l for l in lists[1:])]
    skip = set(eval_maps) | (BOSS_MAPS if stage == 1 else set())
    maps = [m for m in maps if m not in skip]
    print(f"[{tag}] --maps all: {len(maps)} maps ({', '.join(maps)}); skipped "
          f"{sorted(skip & set(lists[0]))} (held out" + (", boss exits" if stage == 1 else "") + ")")
    return maps


def default_groups(iwads: list, maps: list, cores: int | None = None) -> int:
    """One group per IWAD/map pair, at most the logical cores minus
    FREE_CORES (more pairs than that rotate, MapRotation)."""
    return min(len(iwads) * len(maps), max_default_groups(cores))


class MapRotation:
    """Groups take turns on the IWAD/map pairs when there are more pairs
    than groups: every `every` batches they move on to the next block of
    pairs (allocate_groups' rotation), so all of them get trained. Off
    (one fixed allocation) when the groups cover every pair or every is 0."""

    def __init__(self, iwads: list, maps: list, groups: int, every: int, tag: str = "train"):
        self.iwads, self.maps, self.groups, self.tag = iwads, maps, groups, tag
        self.n_pairs = len(iwads) * len(maps)
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
                               rotation=r if self.every else None)


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
    p.add_argument("--stage", type=int, choices=[1, 2], default=1,
                   help="1: no map monsters (exploration); 2: map monsters on")
    p.add_argument("--spawn-tics", type=int, default=700,
                   help="mean game tics between practice monsters spawned on screen while exploring "
                        "(-rlspawn: a sighting the policy should CONSULT about; 0: none)")
    p.add_argument("--noblit", action=argparse.BooleanOptionalAction, default=False,
                   help="launch the games with -noblit: skip the SDL blit nobody sees headless "
                        "(~4x more decisions/s; frames and replays unchanged; train.sh's default)")
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
    p.add_argument("--eval-maps", default="1 5,1 6", help="held-out maps for evaluation ('' to skip)")
    p.add_argument("--eval-episodes", type=int, default=2, help="rollouts per eval map")
    p.add_argument("--skill", type=int, default=3)
    p.add_argument("--slots-dir", default=None, help="prepared save slots copied into each instance")
    p.add_argument("--slots", default="", help="start slots in --slots-dir, e.g. '3,4,5'")
    p.add_argument("--groups", type=int, default=None,
                   help="GRPO groups (default: one per IWAD/map pair, len(--iwads) x len(--maps), "
                        "at most the logical cores minus 2; with more pairs than groups they take "
                        "turns, --rotate-every)")
    p.add_argument("--group-size", type=int, default=8)
    p.add_argument("--max-tics", type=int, default=525,
                   help="rollout limit in game tics (35/s; default 15 s)")
    p.add_argument("--portal-goal", type=int, default=1,
                   help="a rollout ends (+portal bonus) once it enters this many sectors not yet "
                        "visited -- through a portal; with --frontier the next batch starts beyond it "
                        "(0: rollouts run to the exit, death or --max-tics)")
    p.add_argument("--time-penalty", type=float, default=1 / 3,
                   help="reward lost per game second until the rollout ends, so the faster portal "
                        "crossing wins its group (default 1/3: a point every 3 s)")
    p.add_argument("--eval-max-tics", type=int, default=2100,
                   help="held-out eval rollout limit (the training rewards, but no portal or key goal "
                        "ending it early: sectors visited in 60 s, comparable across versions)")
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
    p.add_argument("--hub-repo", default=None,
                   help="Hugging Face model repo (e.g. user/name): its doom-runner.pt is pulled into "
                        "--run-dir before training (resumed from, unless the run directory has a newer "
                        "one) and pushed back every --hub-every updates and at the end (uses the `hf` "
                        "CLI, logged in with `hf auth login`)")
    p.add_argument("--no-hub-download", dest="hub_download", action="store_false",
                   help="don't pull the --hub-repo checkpoint (start from --run-dir alone, e.g. fresh)")
    p.add_argument("--no-hub-upload", dest="hub_upload", action="store_false",
                   help="don't push checkpoints to --hub-repo")
    p.add_argument("--hub-every", type=int, default=100,
                   help="push the checkpoint to --hub-repo every N updates (the end always pushes)")
    p.add_argument("--max-tics-cap", type=int, default=2100,
                   help="per-map tic budget: after a batch where no rollout on a map reaches a new "
                        "sector (or the exit), that map's rollout limit grows by --tics-growth, up to "
                        "this; progress puts it back to --max-tics (cap <= --max-tics: off)")
    p.add_argument("--tics-growth", type=float, default=1.5,
                   help="factor the per-map tic budget grows by after a batch without progress")
    p.add_argument("--warmup-max", type=int, default=40,
                   help="random warm-up actions per group start (0..N, each held for WARMUP_HOLD "
                        "tics), for start-state variety")


# ---------------------------------------------------------------------------
# Frontier starts (SPEC.md §5.4): progress kept across the rollout limit
# ---------------------------------------------------------------------------

# Every group loads its start from slot 4 (the env keeps the map start
# there, EnvConfig.bootstrap_slot, until a frontier replaces it); the best
# rollout saves its progress to slot 5, so the start is never overwritten
# mid-batch; then slots 4 and 5 swap on every instance of that map.
FRONTIER_SLOT = 4
SAVE_SLOT = 5


class Frontier:
    """Per (IWAD, map): the end state of the best rollout so far, as a
    start state. After each batch, the highest-return rollout on a map
    that ended alive, exploring, short of the exit and with progress
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
                  and recs[i]["end_info"].get("mode") == "EXPLORE"]
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


# ---------------------------------------------------------------------------
# Hugging Face Hub (--hub-repo): the checkpoint is pulled before training
# and pushed back while it runs, with the `hf` CLI (logged in with
# `hf auth login`), so the venv needs no extra package.
# ---------------------------------------------------------------------------

HUB_FILE = CKPT_NAME   # the same name in the repo (its history is the repo's commits)


def _hf() -> str:
    exe = shutil.which("hf")
    if exe is None:
        sys.exit("--hub-repo needs the Hugging Face CLI (`hf`) on PATH, logged in with `hf auth login`")
    return exe


class HubError(Exception):
    pass


def _hub_files(repo: str) -> list[str]:
    out = subprocess.run([_hf(), "models", "info", repo], capture_output=True, text=True)
    if out.returncode != 0 or "{" not in out.stdout:
        raise HubError(f"cannot read {repo}: {(out.stderr or out.stdout).strip()[-500:]}")
    info = json.loads(out.stdout[out.stdout.index("{"):])
    return [s["rfilename"] for s in info.get("siblings", [])]


def hub_files(repo: str) -> list[str]:
    """The files in the model repo. Exits when the repo can't be read, so
    a network or login problem never starts a fresh run that would then
    replace the repo's checkpoint."""
    try:
        return _hub_files(repo)
    except HubError as e:
        sys.exit(f"[hub] {e}")


def hub_fetch_checkpoint(repo: str, dest: Path) -> Path | None:
    """Download just the repo's checkpoint into dest (for play.py).
    Returns its path, or None (with the reason printed) when the repo has
    none or can't be reached (or the hf CLI isn't on PATH)."""
    if shutil.which("hf") is None:
        print("[hub] the hf CLI isn't on PATH")
        return None
    try:
        if HUB_FILE not in _hub_files(repo):
            print(f"[hub] {repo} has no {HUB_FILE} yet")
            return None
    except HubError as e:
        print(f"[hub] {e}")
        return None
    out = subprocess.run([_hf(), "download", repo, HUB_FILE, "--local-dir", str(dest)],
                         capture_output=True, text=True)
    if out.returncode != 0:
        print(f"[hub] cannot download {repo}/{HUB_FILE}: {(out.stderr or out.stdout).strip()[-500:]}")
        return None
    return dest / HUB_FILE


# The repo holds just the checkpoint: a run on a fresh machine resumes the
# policy and the map rotation (from the iteration) and starts each map
# from its beginning. Progress saves are never uploaded (and never kept
# across runs): a save only loads in the exact IWAD file it was made with,
# and machines may have different releases (Freedoom's maps change).
HUB_STALE = "frontier/*"   # saves an older version uploaded: removed at the next push


def hub_pull(repo: str, run_dir: Path) -> Path | None:
    """Download the repo's checkpoint (CKPT_NAME) into run_dir, so
    training resumes from it, unless run_dir's checkpoint is at that
    iteration or a newer one (then the local run wins). Returns the new
    checkpoint, or None."""
    files = hub_files(repo)
    if HUB_FILE not in files:
        print(f"[hub] {repo} has no {HUB_FILE} yet")
        return None
    dl = run_dir / "hub" / "download"
    shutil.rmtree(dl, ignore_errors=True)
    out = subprocess.run([_hf(), "download", repo, HUB_FILE, "--local-dir", str(dl)],
                         capture_output=True, text=True)
    if out.returncode != 0:
        sys.exit(f"[hub] cannot download from {repo}: {(out.stderr or out.stdout).strip()[-500:]}")
    ckpt = torch.load(dl / HUB_FILE, map_location="cpu", weights_only=False)
    it = int(ckpt["iteration"])
    local = load_latest_checkpoint(run_dir, quiet=True)
    if local is not None and int(local["iteration"]) >= it:
        print(f"[hub] {repo} has iteration {it}; {run_dir} is at {local['iteration']}, so it is kept")
        return None
    dst = run_dir / CKPT_NAME
    os.replace(dl / HUB_FILE, dst)
    print(f"[hub] pulled iteration {it} from {repo} -> {dst}")
    return dst


class HubUploader:
    """Pushes the checkpoint to the repo as HUB_FILE, in the background
    (one upload at a time; a push while one is running is skipped). It is
    staged in run_dir/hub/upload first, so the next save can't change it
    mid-upload. The hf output goes to run_dir/hub_upload.log."""

    def __init__(self, repo: str, run_dir: Path):
        self.repo, self.dir, self.hf = repo, run_dir / "hub", _hf()
        self.log = open(run_dir / "hub_upload.log", "a")
        self.proc: subprocess.Popen | None = None
        self.it: int | None = None         # the upload in flight (or last started)
        self.done: int | None = None       # the last iteration uploaded

    def busy(self) -> bool:
        return self.proc is not None and self.proc.poll() is None

    def _reap(self) -> None:
        if self.proc is None:
            return
        rc = self.proc.wait()
        self.proc = None
        if rc == 0:
            self.done = self.it
            print(f"[hub] uploaded iteration {self.it} to {self.repo}")
        else:
            print(f"[hub] upload of iteration {self.it} failed (exit {rc}); see hub_upload.log")

    def _stage(self, ckpt: Path) -> Path:
        staged = self.dir / "upload"
        shutil.rmtree(staged, ignore_errors=True)
        staged.mkdir(parents=True)
        shutil.copyfile(ckpt, staged / HUB_FILE)
        return staged

    def push(self, ckpt: Path, it: int) -> bool:
        """Start uploading ckpt (at iteration it); False (skipped) if an
        upload is running."""
        if self.busy():
            return False
        self._reap()
        staged = self._stage(ckpt)
        self.it = it
        self.log.write(f"--- iteration {self.it}\n")
        self.log.flush()
        # --delete: progress saves an older version of this code uploaded
        # are removed, so no machine finds them.
        self.proc = subprocess.Popen([self.hf, "upload", self.repo, str(staged), ".",
                                      "--delete", HUB_STALE,
                                      "--commit-message", f"iteration {self.it}"],
                                     stdout=self.log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
        return True

    def finish(self, ckpt: Path | None, it: int | None) -> None:
        """At the end: wait for the running upload, then upload ckpt (the
        newest checkpoint, at iteration it) if the repo doesn't have it yet."""
        if self.busy():
            print(f"[hub] waiting for the upload of iteration {self.it}...")
        self._reap()
        if ckpt is not None and it is not None and it != self.done:
            self.push(ckpt, it)
            print(f"[hub] uploading iteration {self.it}...")
            self._reap()
        self.log.close()


def load_latest_checkpoint(run_dir: Path, quiet: bool = False) -> dict | None:
    """run_dir's checkpoint (CKPT_NAME), or for a run directory from before
    2026-11-15 its newest ckpt_<iteration>.pt; None when there is none.
    Exits if the checkpoint can't be loaded: a silent fresh start would
    replace it (and the Hub's copy) at the next save."""
    for tmp in run_dir.glob("*.pt.tmp"):
        tmp.unlink(missing_ok=True)       # interrupted write
    path = run_dir / CKPT_NAME
    if not path.exists():
        old = sorted(run_dir.glob("ckpt_*.pt"))
        if not old:
            return None
        path = old[-1]
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
