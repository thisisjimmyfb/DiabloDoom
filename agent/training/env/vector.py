"""Subset-stepping vector env for group rollouts (GRPO, SPEC.md §7.1).

Gymnasium's vector envs step every sub-env on every call and auto-reset
finished ones. A GRPO group needs the opposite: all G rollouts start
from the same state, each runs until it ends, and finished rollouts sit
idle until the whole batch is done. GroupVecEnv runs one worker process
per env (each worker owns one game process) and steps only the envs it
is asked to.
"""

from __future__ import annotations

import multiprocessing as mp
import traceback
from typing import Callable, Sequence

import numpy as np


def _worker(remote, env_fn):
    env = None
    try:
        env = env_fn()
        while True:
            cmd, data = remote.recv()
            if cmd == "step":
                obs, r, term, trunc, info = env.step(data)
                remote.send(("ok", (obs, r, term, trunc, info)))
            elif cmd == "reset":
                remote.send(("ok", env.reset(options=data)))
            elif cmd == "close":
                break
            else:
                remote.send(("error", f"unknown command {cmd}"))
    except (EOFError, KeyboardInterrupt):
        pass
    except Exception:
        try:
            remote.send(("error", traceback.format_exc()))
        except Exception:
            pass
    finally:
        if env is not None:
            env.close()
        remote.close()


class GroupVecEnv:
    def __init__(self, env_fns: Sequence[Callable], context: str = "spawn"):
        # spawn: workers never inherit the trainer's CUDA state.
        ctx = mp.get_context(context)
        self.n = len(env_fns)
        self._remotes, self._procs = [], []
        for fn in env_fns:
            parent, child = ctx.Pipe()
            p = ctx.Process(target=_worker, args=(child, fn), daemon=True)
            p.start()
            child.close()
            self._remotes.append(parent)
            self._procs.append(p)

    def _gather(self, idx: Sequence[int]):
        out = []
        for i in idx:
            status, payload = self._remotes[i].recv()
            if status != "ok":
                raise RuntimeError(f"env {i} failed:\n{payload}")
            out.append(payload)
        return out

    def reset(self, idx: Sequence[int], options: Sequence[dict | None]):
        for i, o in zip(idx, options):
            self._remotes[i].send(("reset", o))
        res = self._gather(idx)
        return np.stack([r[0] for r in res]), [r[1] for r in res]

    def step(self, idx: Sequence[int], actions: Sequence[int]):
        for i, a in zip(idx, actions):
            self._remotes[i].send(("step", int(a)))
        res = self._gather(idx)
        obs = np.stack([r[0] for r in res])
        rew = np.array([r[1] for r in res], dtype=np.float32)
        term = np.array([r[2] for r in res], dtype=bool)
        trunc = np.array([r[3] for r in res], dtype=bool)
        return obs, rew, term, trunc, [r[4] for r in res]

    def close(self):
        for r in self._remotes:
            try:
                r.send(("close", None))
            except Exception:
                pass
        for p in self._procs:
            # Long enough for a capturing env's QUIT to flush its
            # last replay segment (Engine.close waits up to 60 s).
            p.join(timeout=75)
            if p.is_alive():
                p.terminate()
