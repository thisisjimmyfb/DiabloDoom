"""Observation preprocessing: 320x200 palette frame -> W x H grayscale
(EnvConfig.width x EnvConfig.height, 128x80), area-averaged, and (2026-11-22,
EnvConfig.depth) the engine's depth buffer beside it, the nearest depth of
each output pixel's area, log-encoded (depth_code): 2 x H x W a frame.
Stacked over the last N decisions (SPEC.md §5.1): an N*2 x H x W stack
(8 x 80 x 128; N x H x W, 4 x 80 x 128, without depth)."""

from __future__ import annotations

from collections import deque

import numpy as np

# ITU-R BT.601 luma weights.
_LUMA = np.array([0.299, 0.587, 0.114], dtype=np.float32)


# Depth code (2026-11-22): near bright. 255 outside the 3D view (status bar)
# and on the player's weapon (engine depth 0), 0 where nothing was drawn
# (sky, 65535), and 1..254 for 4096..16 map units, log2-spaced (about 32
# codes per doubling: 128 units is 158, 512 units 95).
DEPTH_NEAR, DEPTH_FAR = 16.0, 4096.0


def _depth_lut() -> np.ndarray:
    d = np.arange(65536, dtype=np.float64)
    near = 1.0 - (np.log2(np.clip(d, DEPTH_NEAR, DEPTH_FAR)) - np.log2(DEPTH_NEAR)) / np.log2(DEPTH_FAR / DEPTH_NEAR)
    lut = np.clip(np.round(1 + near * 253), 1, 254).astype(np.uint8)
    lut[0], lut[65535] = 255, 0
    return lut


_DEPTH_LUT = _depth_lut()


def depth_code(depth: np.ndarray) -> np.ndarray:
    """Engine depth (uint16 map units) -> the observation's uint8 code."""
    return _DEPTH_LUT[depth]


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


def _support(m: np.ndarray) -> np.ndarray:
    """Area matrix -> dst x k indices of the source cells each output cell
    touches, a row's last repeated to pad it (for a min, unlike _sparse's
    zero-weight padding)."""
    idx = [np.nonzero(r)[0] for r in m]
    k = max(len(i) for i in idx)
    return np.array([np.concatenate([i, np.full(k - len(i), i[-1])]) for i in idx], dtype=np.intp)


def _min_320x200_to_128x80(d: np.ndarray) -> np.ndarray:
    """The nearest-depth resize at the default size: 2.5 source pixels per
    output pixel both ways, so each pair of output pixels covers 5 source
    ones, the middle one shared (2026-11-22)."""
    v = d.reshape(40, 5, 320)
    rows = np.empty((40, 2, 320), d.dtype)
    rows[:, 0] = np.minimum(np.minimum(v[:, 0], v[:, 1]), v[:, 2])
    rows[:, 1] = np.minimum(np.minimum(v[:, 2], v[:, 3]), v[:, 4])
    h = rows.reshape(80, 64, 5)
    out = np.empty((80, 64, 2), d.dtype)
    out[..., 0] = np.minimum(np.minimum(h[..., 0], h[..., 1]), h[..., 2])
    out[..., 1] = np.minimum(np.minimum(h[..., 2], h[..., 3]), h[..., 4])
    return out.reshape(80, 128)


class Preprocessor:
    """Gather-based area resize, deliberately not a matmul: BLAS spawns a
    thread pool per process, and with many env workers those pools
    oversubscribe the CPU (measured: 8 workers fell from 391 to 54
    steps/s). The gather is exact and single-threaded.

    depth (2026-11-22): __call__ takes the engine's depth buffer too and
    returns 2 x height x width, [grayscale, depth code], the depth being
    the nearest of each output pixel's area (a monster wins over the wall
    behind it; an average would blend them)."""

    def __init__(self, src_w: int = 320, src_h: int = 200, width: int = 84, height: int | None = None,
                 depth: bool = False):
        self.width, self.height = width, height or width
        self.depth = depth
        self._iy, self._wy = _sparse(_area_matrix(src_h, self.height))   # height x ky
        self._ix, self._wx = _sparse(_area_matrix(src_w, self.width))    # width x kx
        self._sy = _support(_area_matrix(src_h, self.height))
        self._sx = _support(_area_matrix(src_w, self.width))

    def __call__(self, pix: np.ndarray, pal: np.ndarray, depth: np.ndarray | None = None) -> np.ndarray:
        # Palette -> luma per index (256 entries), then look up per pixel.
        luma_pal = (pal.astype(np.float32) * _LUMA).sum(axis=1)
        gray = luma_pal[pix]                                          # H x W
        rows = (gray[self._iy] * self._wy[:, :, None]).sum(axis=1)   # height x W
        small = (rows[:, self._ix] * self._wx[None]).sum(axis=2)     # height x width
        small = np.clip(small + 0.5, 0, 255).astype(np.uint8)
        if not self.depth:
            return small
        if depth is None:
            raise ValueError("depth observation without a depth buffer (an engine from before 2026-11-22: "
                             "rebuild it)")
        return np.stack([small, depth_code(self.nearest(depth))])

    def nearest(self, depth: np.ndarray) -> np.ndarray:
        """The nearest (smallest) depth of each output pixel's area."""
        if depth.shape == (200, 320) and (self.width, self.height) == (128, 80):
            return _min_320x200_to_128x80(depth)
        return depth[self._sy].min(axis=1)[:, self._sx].min(axis=2)


class FrameStack:
    def __init__(self, n: int, height: int = 84, width: int | None = None):
        self.n = n
        self.height, self.width = height, width or height
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
        if self._frames[0].ndim == 3:                                 # C x H x W each (depth: 2)
            return np.concatenate(self._frames, axis=0)
        return np.stack(self._frames, axis=0)
