"""Shared pytest fixtures: fake rollouts for the trainer
tests (no game needed), and a one-time cleanup of game instances left by
a crashed run."""

from __future__ import annotations


import numpy as np
import pytest

from env import ACTIONS, cleanup_stale
from env.obs import FrameStack
from model import N_PRIV


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
