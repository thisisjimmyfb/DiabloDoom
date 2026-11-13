"""Shared pytest fixtures: fake rollouts and batch files for the trainer
tests (no game needed), and a one-time cleanup of game instances left by
a crashed run."""

from __future__ import annotations

from pathlib import Path

import numpy as np
import pytest

from env import ACTIONS, cleanup_stale
from env.obs import FrameStack
from model import N_PRIV, pack_rollouts


@pytest.fixture(scope="session", autouse=True)
def _clean_stale_instances():
    """Temp dirs, /dev/shm segments and games whose owner process is gone."""
    cleanup_stale()


@pytest.fixture
def rng() -> np.random.Generator:
    return np.random.default_rng(0)


@pytest.fixture
def make_rollout(rng):
    """make_rollout(n_steps, iwad="FREEDOOM1") -> a rollout record as
    eval.run_rollouts makes it, with observations from the env's own
    FrameStack: a mix of pushed frames and dropped actions (no new frame)."""
    def make(n_steps: int, iwad: str = "FREEDOOM1") -> dict:
        st = FrameStack(4, 84)
        obs = [st.reset(rng.integers(0, 256, (84, 84), dtype=np.uint8))]
        for _ in range(n_steps - 1):
            if rng.random() < 0.2:
                obs.append(st.get())                     # action dropped: same stack
            else:
                obs.append(st.push(rng.integers(0, 256, (84, 84), dtype=np.uint8)))
        rew = rng.normal(size=n_steps).tolist()
        return {"obs": obs, "priv": [rng.random(N_PRIV).astype(np.float32) for _ in range(n_steps)],
                "act": rng.integers(0, len(ACTIONS), n_steps).tolist(),
                "logp": (-rng.random(n_steps)).tolist(), "rew": rew,
                "tics": rng.integers(1, 40, n_steps).tolist(), "len": n_steps, "ret": float(sum(rew)),
                "rollout_tics": 500, "sectors_gained": int(rng.integers(0, 9)),
                "exit": bool(rng.random() < 0.3), "death": False, "terminated": True, "iwad": iwad}
    return make


@pytest.fixture
def make_batch(rng, make_rollout):
    """make_batch(batch_dir, actor, version, groups=2, G=3, run_id=None) ->
    path of a batch file written the way an eval.py worker writes it
    (run_id None: a batch from before batches carried one)."""
    from eval import write_batch

    def make(batch_dir: Path, actor: str, version: int, groups: int = 2, G: int = 3,
             run_id: str | None = None) -> Path:
        recs = [make_rollout(int(n)) for n in rng.integers(2, 20, groups * G)]
        arrays = pack_rollouts(recs, [i // G for i in range(groups * G)], [0] * (groups * G))
        meta = {"actor": actor, "version": version, "groups": groups, "group_size": G,
                "iwads": ["FREEDOOM1"], "group_labels": [], "rollout_s": 1.0, "created": 0}
        if run_id is not None:
            meta["run_id"] = run_id
        return write_batch(batch_dir, actor, arrays, meta)
    return make
