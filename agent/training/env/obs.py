"""Observation preprocessing: 320x200 palette frame -> 84x84 grayscale,
area-averaged, stacked over the last N decisions (SPEC.md §5.1)."""

from __future__ import annotations

from collections import deque

import numpy as np

# ITU-R BT.601 luma weights.
_LUMA = np.array([0.299, 0.587, 0.114], dtype=np.float32)


def _area_matrix(src: int, dst: int) -> np.ndarray:
    """dst x src matrix whose rows average the source cells each output
    cell covers (exact area resampling, fractional overlaps weighted)."""
    m = np.zeros((dst, src), dtype=np.float32)
    scale = src / dst
    for i in range(dst):
        lo, hi = i * scale, (i + 1) * scale
        j0, j1 = int(np.floor(lo)), int(np.ceil(hi))
        for j in range(j0, min(j1, src)):
            m[i, j] = min(hi, j + 1) - max(lo, j)
        m[i] /= m[i].sum()
    return m


def _sparse(m: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Area matrix -> (indices, weights), each dst x k: every output cell
    touches at most k source cells."""
    idx = [np.nonzero(r)[0] for r in m]
    k = max(len(i) for i in idx)
    ind = np.zeros((m.shape[0], k), dtype=np.intp)
    w = np.zeros((m.shape[0], k), dtype=np.float32)
    for o, i in enumerate(idx):
        ind[o, :len(i)] = i
        w[o, :len(i)] = m[o, i]
    return ind, w


class Preprocessor:
    """Gather-based area resize, deliberately not a matmul: BLAS spawns a
    thread pool per process, and with many env workers those pools
    oversubscribe the CPU (measured: 8 workers fell from 391 to 54
    steps/s). The gather is exact and single-threaded."""

    def __init__(self, src_w: int = 320, src_h: int = 200, size: int = 84):
        self.size = size
        self._iy, self._wy = _sparse(_area_matrix(src_h, size))   # size x ky
        self._ix, self._wx = _sparse(_area_matrix(src_w, size))   # size x kx

    def __call__(self, pix: np.ndarray, pal: np.ndarray) -> np.ndarray:
        # Palette -> luma per index (256 entries), then look up per pixel.
        luma_pal = (pal.astype(np.float32) * _LUMA).sum(axis=1)
        gray = luma_pal[pix]                                          # H x W
        rows = (gray[self._iy] * self._wy[:, :, None]).sum(axis=1)   # size x W
        small = (rows[:, self._ix] * self._wx[None]).sum(axis=2)     # size x size
        return np.clip(small + 0.5, 0, 255).astype(np.uint8)


class FrameStack:
    def __init__(self, n: int, size: int):
        self.n = n
        self.size = size
        self._frames: deque[np.ndarray] = deque(maxlen=n)

    def reset(self, frame: np.ndarray) -> np.ndarray:
        self._frames.clear()
        for _ in range(self.n):
            self._frames.append(frame)
        return self.get()

    def push(self, frame: np.ndarray) -> np.ndarray:
        self._frames.append(frame)
        return self.get()

    def get(self) -> np.ndarray:
        return np.stack(self._frames, axis=0)
