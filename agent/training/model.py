"""Shared pieces of the trainer (SPEC.md §6, §7.1, §7.4, §7.5): the policy
network, the env factory, IWAD/group allocation, checkpoint I/O and the
spool's weights file. Used by train.py (single box), eval.py (rollout
worker) and learn.py (learner)."""

from __future__ import annotations

import argparse
import os
import random
import shutil
import sys
from pathlib import Path

import numpy as np
import torch
import torch.nn as nn

sys.path.insert(0, str(Path(__file__).resolve().parent))
from env import ACTIONS, CONSULT  # noqa: E402

MAP_ACTION = ACTIONS.index("MAP")
# Warm-up uses movement only (no MAP toggle, no CONSULT).
WARMUP_ACTIONS = [i for i in range(len(ACTIONS)) if i not in (CONSULT, MAP_ACTION)]
N_PRIV = 2  # privileged critic inputs: HP/100, exit path distance (SPEC.md §6)


# ---------------------------------------------------------------------------
# Environment factory (top-level so worker processes can unpickle it)
# ---------------------------------------------------------------------------

def make_env(iwad: str | None, warp: str, stage: int, skill: int, max_tics: int,
             slots_dir: str | None, slots: list[int], capture_dir: str | None = None,
             spawn_tics: int = 0, nodraw: bool = False, noblit: bool = False):
    from env import DiabloDoomEnv, EngineConfig, EnvConfig, standin_consult
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
    ))


def privileged(info: dict) -> list[float]:
    s = info.get("state", {})
    exit_dist = s.get("exit_dist")
    return [s.get("hp", 0) / 100.0, (exit_dist / 4096.0) if exit_dist is not None else 0.0]


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


def load_weights(path: str) -> dict:
    ckpt = torch.load(path, map_location="cpu", weights_only=False)
    return ckpt["policy"] if "policy" in ckpt else ckpt


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


def allocate_groups(iwads: list[str | None], maps: list[str], groups: int,
                    tag: str = "train") -> list[tuple[str | None, str]]:
    """Group g -> (iwad, map). The map is maps[g % M] as with one IWAD, so
    --groups M still trains every map; the IWAD rotates with the map
    index and shifts by one each time the maps wrap, so the IWADs split
    the groups evenly and --groups I*M covers every IWAD/map pair.
    Warns about pairs no group plays."""
    I, M = len(iwads), len(maps)
    pairs = [(iwads[(g % M + g // M) % I], maps[g % M]) for g in range(groups)]
    missing = [env_label(w, m, iwads) for w in iwads for m in maps if (w, m) not in pairs]
    if missing:
        print(f"[{tag}] warning: {groups} groups don't cover {missing}; "
              f"use --groups {I * M} to train every IWAD/map pair")
    return pairs


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
                        "maps[g %% M] on iwads[(g %% M + g // M) %% I]: the IWADs split the groups "
                        "evenly, and --groups I*M covers every IWAD/map pair")
    p.add_argument("--maps", default="1 1,1 2,1 3,1 4",
                   help="training maps as -warp args, comma-separated; group g uses maps[g %% len]")
    p.add_argument("--eval-maps", default="1 5,1 6", help="held-out maps for evaluation ('' to skip)")
    p.add_argument("--eval-episodes", type=int, default=2, help="rollouts per eval map")
    p.add_argument("--skill", type=int, default=3)
    p.add_argument("--slots-dir", default=None, help="prepared save slots copied into each instance")
    p.add_argument("--slots", default="", help="start slots in --slots-dir, e.g. '3,4,5'")
    p.add_argument("--groups", type=int, default=4)
    p.add_argument("--group-size", type=int, default=8)
    p.add_argument("--max-tics", type=int, default=2100)
    p.add_argument("--frontier-prob", type=float, default=0.5,
                   help="share of groups started from their map's frontier: the end state of the best "
                        "rollout so far, saved after each batch (save slot 4), so training keeps its "
                        "progress past the --max-tics limit; an exit clears it (0: always the map start)")
    p.add_argument("--warmup-max", type=int, default=40,
                   help="random warm-up actions per group start (0..N), for start-state variety")


# ---------------------------------------------------------------------------
# Frontier starts (SPEC.md §5.4): progress kept across the rollout limit
# ---------------------------------------------------------------------------

FRONTIER_SLOT = 4


class Frontier:
    """Per (IWAD, map): the end state of the best rollout so far, as a
    start state. After each batch, the highest-return rollout on a map
    that ended alive, exploring and short of the exit saves the game
    (SAVE 4, where it stopped); the save is kept in root/ (so it survives
    a restart) and copied into every instance on that map. The next
    batches start each group from it with probability prob, the rest
    from the map start. A rollout reaching the exit clears the map's
    frontier: the level is done, the cycle starts again from the start."""

    def __init__(self, root: Path | None, prob: float, env_pairs: list, tag: str = "train"):
        self.root, self.prob, self.tag = root, prob, tag
        self.labels = [f"{iwad_label(w)}_{m.replace(' ', '_')}" for w, m in env_pairs]
        self.data: dict[str, bytes] = {}
        if root is not None and prob > 0:
            root.mkdir(parents=True, exist_ok=True)
            for label in set(self.labels):
                f = root / f"{label}.dsg"
                if f.exists():
                    self.data[label] = f.read_bytes()

    def _envs(self, label: str) -> list[int]:
        return [i for i, l in enumerate(self.labels) if l == label]

    def install_all(self, vec) -> None:
        """Copy the frontiers found in root/ into fresh instances."""
        for label, data in self.data.items():
            vec.call(self._envs(label), "install_state", (FRONTIER_SLOT, data))
        if self.data:
            print(f"[{self.tag}] frontier starts restored: {sorted(self.data)}")

    def options(self, options: list[dict], sampler: np.random.Generator, G: int) -> list[dict]:
        """Start a share of the groups (all G members alike) from their
        map's frontier. One draw per group either way, so the sampler's
        stream doesn't depend on which frontiers exist."""
        for g in range(len(options) // G):
            u = sampler.random()
            if self.prob > 0 and self.labels[g * G] in self.data and u < self.prob:
                for o in options[g * G:(g + 1) * G]:
                    o["slot"] = FRONTIER_SLOT
                    o["frontier"] = True
        return options

    def update(self, vec, recs: list[dict]) -> dict:
        """After a batch: save each map's best rollout end state (or clear
        the map's frontier on an exit). Returns {label: return saved}."""
        if self.prob <= 0:
            return {}
        saved = {}
        for label in sorted(set(self.labels)):
            idx = self._envs(label)
            if any(recs[i]["exit"] for i in idx):
                if label in self.data:
                    del self.data[label]
                    vec.call(idx, "install_state", (FRONTIER_SLOT, None))
                    if self.root is not None:
                        (self.root / f"{label}.dsg").unlink(missing_ok=True)
                saved[label] = None
                continue
            ok = [i for i in idx if not recs[i]["death"]
                  and recs[i]["end_info"].get("mode") == "EXPLORE"]
            if not ok:
                continue
            best = max(ok, key=lambda i: recs[i]["ret"])
            data = vec.call([best], "save_state", (FRONTIER_SLOT,))[0]
            if data is None:
                continue
            self.data[label] = data
            vec.call(idx, "install_state", (FRONTIER_SLOT, data))
            if self.root is not None:
                tmp = self.root / f"{label}.dsg.tmp"
                tmp.write_bytes(data)
                os.replace(tmp, self.root / f"{label}.dsg")
            saved[label] = round(recs[best]["ret"], 2)
        return saved


def check_slots(iwads: list[str | None], slots: list[int]) -> None:
    # Save slots are IWAD-specific (a save loaded under another IWAD's
    # maps is garbage), so they can't be mixed.
    if len(iwads) > 1 and slots:
        sys.exit("--slots can't be combined with several --iwads (save slots are IWAD-specific)")
    if FRONTIER_SLOT in slots:
        sys.exit(f"--slots can't use slot {FRONTIER_SLOT}: frontier starts write it (--frontier-prob)")


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


def save_checkpoint(path: Path, state: dict, keep: int) -> None:
    atomic_torch_save(state, path)
    for old in sorted(path.parent.glob("ckpt_*.pt"))[:-keep]:
        old.unlink(missing_ok=True)


def export_latest_checkpoint(run_dir: Path, out_dir: Path) -> Path | None:
    """Copy the newest checkpoint in run_dir into out_dir (same file name)."""
    ckpts = sorted(run_dir.glob("ckpt_*.pt"))
    if not ckpts:
        return None
    src = ckpts[-1]
    out_dir.mkdir(parents=True, exist_ok=True)
    dst = out_dir / src.name
    tmp = dst.with_suffix(".pt.tmp")
    shutil.copyfile(src, tmp)
    os.replace(tmp, dst)
    return dst


def load_latest_checkpoint(run_dir: Path) -> dict | None:
    for tmp in run_dir.glob("ckpt_*.pt.tmp"):
        tmp.unlink(missing_ok=True)       # interrupted write
    for path in sorted(run_dir.glob("ckpt_*.pt"), reverse=True):
        try:
            ckpt = torch.load(path, map_location="cpu", weights_only=False)
            print(f"[ckpt] resuming from {path}")
            return ckpt
        except Exception as e:  # corrupt or truncated: fall back to the previous one
            print(f"[ckpt] cannot load {path}: {e}; trying an older one")
    return None


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


# ---------------------------------------------------------------------------
# Spool: weights file and stop marker (SPEC.md §7.5)
# ---------------------------------------------------------------------------

WEIGHTS_FILE = "weights.pt"
DONE_FILE = "DONE"
BATCH_DIR = "batches"


def publish_weights(spool: Path, version: int, policy: nn.Module, run_id: str) -> None:
    spool.mkdir(parents=True, exist_ok=True)
    state = {k: v.detach().cpu() for k, v in policy.state_dict().items()}
    atomic_torch_save({"version": version, "run_id": run_id, "policy": state}, spool / WEIGHTS_FILE)


def init_weights_if_absent(spool: Path, policy: nn.Module, run_id: str) -> bool:
    """Publish `policy` as version 0 unless the spool already has weights:
    a worker started before any learner begins from random weights, and a
    fresh learner then adopts them (learn.setup_learner). Create-if-absent
    is atomic (a hard link fails if the name exists), so when several
    workers start at once exactly one writes and all load the same file.
    True if this call created weights.pt."""
    spool.mkdir(parents=True, exist_ok=True)
    path = spool / WEIGHTS_FILE
    if path.exists():
        return False
    state = {k: v.detach().cpu() for k, v in policy.state_dict().items()}
    tmp = path.with_name(f"{path.name}.{os.getpid()}.init.tmp")
    with open(tmp, "wb") as f:
        torch.save({"version": 0, "run_id": run_id, "policy": state}, f)
        f.flush()
        os.fsync(f.fileno())
    try:
        os.link(tmp, path)
    except FileExistsError:
        return False
    finally:
        tmp.unlink(missing_ok=True)
    _fsync_dir(spool)
    return True


def read_weights(spool: Path) -> dict | None:
    """The published {version, run_id, policy}, or None if there is none yet."""
    path = spool / WEIGHTS_FILE
    if not path.exists():
        return None
    try:
        return torch.load(path, map_location="cpu", weights_only=False)
    except Exception as e:  # replaced or synced mid-read: try again next poll
        print(f"[spool] cannot read {path}: {e}")
        return None


# ---------------------------------------------------------------------------
# Spool: batch files (SPEC.md §7.5)
# ---------------------------------------------------------------------------

def pack_rollouts(recs: list[dict], ro_group: list[int], ro_iwad: list[int]) -> dict[str, np.ndarray]:
    """Rollout records -> flat arrays for one batch file. Frames are stored
    once: each decision's stack is a row of indices into `frames`, so a
    frame shared by consecutive stacks (or repeated in a reset stack) is
    kept once and the learner rebuilds every stack exactly."""
    frames: list[np.ndarray] = []
    obs_idx: list[list[int]] = []
    for r in recs:
        prev_obs, prev_idx = None, None
        for o in r["obs"]:
            idx: list[int] = []
            for k in range(o.shape[0]):
                j = None
                if prev_obs is not None:
                    if k + 1 < o.shape[0] and np.array_equal(o[k], prev_obs[k + 1]):
                        j = prev_idx[k + 1]          # the stack shifted by one frame
                    elif np.array_equal(o[k], prev_obs[k]):
                        j = prev_idx[k]              # no new frame (action not sent)
                if j is None and k > 0 and np.array_equal(o[k], o[k - 1]):
                    j = idx[k - 1]                   # reset stack: first frame repeated
                if j is None:
                    frames.append(o[k])
                    j = len(frames) - 1
                idx.append(j)
            obs_idx.append(idx)
            prev_obs, prev_idx = o, idx
    cat = lambda key, dtype: np.concatenate([np.asarray(r[key], dtype=dtype) for r in recs])  # noqa: E731
    return {
        "frames": np.stack(frames).astype(np.uint8),
        "obs_idx": np.asarray(obs_idx, dtype=np.int32),
        "priv": np.concatenate([np.asarray(r["priv"], dtype=np.float32).reshape(-1, N_PRIV) for r in recs]),
        "act": cat("act", np.int8),
        "logp": cat("logp", np.float32),
        "rew": cat("rew", np.float32),
        "tics": cat("tics", np.int32),
        "ro_len": np.array([r["len"] for r in recs], dtype=np.int32),
        "ro_group": np.asarray(ro_group, dtype=np.int32),
        "ro_iwad": np.asarray(ro_iwad, dtype=np.int16),
        "ro_rollout_tics": np.array([r["rollout_tics"] for r in recs], dtype=np.int32),
        "ro_sectors": np.array([r["sectors_gained"] for r in recs], dtype=np.int32),
        "ro_exit": np.array([r["exit"] for r in recs], dtype=bool),
        "ro_death": np.array([r["death"] for r in recs], dtype=bool),
        "ro_terminated": np.array([r.get("terminated", False) for r in recs], dtype=bool),
    }


def unpack_rollouts(z, iwad_labels: list[str]) -> list[dict]:
    """Inverse of pack_rollouts: one record per rollout, with the same
    fields run_rollouts produces plus "group" (batch-local) and "iwad"."""
    if hasattr(z, "files"):        # NpzFile decompresses on every access: read each array once
        z = {k: z[k] for k in z.files if k != "meta"}
    frames, obs_idx = z["frames"], z["obs_idx"]
    ends = np.cumsum(z["ro_len"])
    recs = []
    for i, end in enumerate(ends):
        s = slice(int(end - z["ro_len"][i]), int(end))
        rew = z["rew"][s]
        recs.append({
            "obs": frames[obs_idx[s]],                      # (len, stack, H, W)
            "priv": z["priv"][s],
            "act": z["act"][s].astype(np.int64),
            "logp": z["logp"][s],
            "rew": rew.astype(np.float64).tolist(),
            "tics": z["tics"][s].tolist(),
            "len": int(z["ro_len"][i]),
            "ret": float(rew.astype(np.float64).sum()),
            "rollout_tics": int(z["ro_rollout_tics"][i]),
            "sectors_gained": int(z["ro_sectors"][i]),
            "exit": bool(z["ro_exit"][i]),
            "death": bool(z["ro_death"][i]),
            "terminated": bool(z["ro_terminated"][i]),
            "group": int(z["ro_group"][i]),
            "iwad": iwad_labels[int(z["ro_iwad"][i])],
        })
    return recs
