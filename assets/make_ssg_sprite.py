#!/usr/bin/env python3
"""Build the built-in super shotgun sprite as a real Doom patch.

The super shotgun's pickup sprite SGN2A0 exists only in Doom II
WADs, so on Doom 1 / Freedoom 1 the inventory and the status-bar
weapon panel could only fall back to the square compiled icon -- an
opaque 32x32 palette bitmap whose background is palette black, drawn
with a plain blit that knows nothing about transparency.

This script regenerates the super shotgun art (assets/items/gun_ssg.webp)
as a genuine patch_t:

- beefier canvas than the WAD pickup family (shotgun SHOTA0
  is 38x13, super shotgun SGN2A0 is 43x14; this sprite is 60x21,
  aspect-matched to the double-barrel silhouette),
- the gun silhouette fitted inside, aspect preserved,
- transparency encoded the Doom way: background pixels simply have
  no posts in their columns, so the existing patch draw path
  (UIDrawPatchFit / STWepDrawPatch) composites it over whatever is
  beneath, exactly like the WAD pickup sprites.

The source art has no alpha channel (RGB on pure black), so the
background is recovered by flood-filling the near-black region
connected to the image borders; enclosed dark details (screw heads,
the trigger-guard opening) stay opaque, like a hand-made sprite.

Output: src/diablo/d_ssg_sprite.c (byte array + patch accessor).
The PLAYPAL palette is read from an IWAD (PLAYPAL lump); pass the
WAD path as an argument or set DOOMWADDIR.
"""
import os
import struct
import sys
from collections import deque

from PIL import Image

REPO_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_WEBP = os.path.join(REPO_DIR, "assets", "items", "gun_ssg.webp")
OUTPUT_C = os.path.join(REPO_DIR, "src", "diablo", "d_ssg_sprite.c")

SPRITE_W = 60   # one-handed family: SHOTA0 38x13, CSAWA0 47x19
SPRITE_H = 21
BG_THRESHOLD = 10       # max(r,g,b) at/below this counts as background
ALPHA_OPAQUE = 128      # downscaled alpha at/above this stays opaque


def load_palette():
    candidates = []
    if len(sys.argv) > 1:
        candidates.append(sys.argv[1])
    if os.environ.get("DOOMWADDIR"):
        candidates.append(os.path.join(os.environ["DOOMWADDIR"],
                                       "freedoom1.wad"))
    candidates += [
        os.path.expanduser("~/workspace/doom/freedoom1.wad"),
        os.path.expanduser("~/workspace/doom/doom.wad"),
    ]
    for path in candidates:
        if not path or not os.path.exists(path):
            continue
        with open(path, "rb") as f:
            data = f.read()
        assert data[:4] in (b"IWAD", b"PWAD"), path
        numlumps, dirofs = struct.unpack_from("<ii", data, 4)
        for i in range(numlumps):
            ofs, size, name = struct.unpack_from("<ii8s", data,
                                                 dirofs + i * 16)
            if name.rstrip(b"\0") == b"PLAYPAL" and size >= 768:
                raw = data[ofs:ofs + 768]
                return [tuple(raw[j * 3:j * 3 + 3]) for j in range(256)]
    raise SystemExit("PLAYPAL not found; pass an IWAD path as argument")


def background_mask(img):
    """True where a pixel is background: near-black and connected to
    a border through near-black pixels (flood fill)."""
    w, h = img.size
    px = img.load()
    bg = bytearray(w * h)
    queue = deque()

    def is_dark(x, y):
        r, g, b = px[x, y]
        return max(r, g, b) <= BG_THRESHOLD

    for x in range(w):
        for y in (0, h - 1):
            if is_dark(x, y) and not bg[y * w + x]:
                bg[y * w + x] = 1
                queue.append((x, y))
    for y in range(h):
        for x in (0, w - 1):
            if is_dark(x, y) and not bg[y * w + x]:
                bg[y * w + x] = 1
                queue.append((x, y))
    while queue:
        x, y = queue.popleft()
        for nx, ny in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
            if 0 <= nx < w and 0 <= ny < h and not bg[ny * w + nx] \
                    and is_dark(nx, ny):
                bg[ny * w + nx] = 1
                queue.append((nx, ny))
    return bg


def nearest_index(palette, r, g, b):
    best, bestd = 0, 1 << 62
    for i in range(255):    # index 255 stays unused, as in patches
        pr, pg, pb = palette[i]
        d = (r - pr) ** 2 + (g - pg) ** 2 + (b - pb) ** 2
        if d < bestd:
            best, bestd = i, d
    return best


def main():
    palette = load_palette()
    img = Image.open(SRC_WEBP).convert("RGB")
    w, h = img.size
    bg = background_mask(img)

    # Bounding box of the gun (non-background), small pad kept.
    xs = [x for y in range(h) for x in range(w) if not bg[y * w + x]]
    ys = [y for y in range(h) for x in range(w) if not bg[y * w + x]]
    x0, x1, y0, y1 = min(xs), max(xs), min(ys), max(ys)
    gun = img.crop((x0, y0, x1 + 1, y1 + 1))
    alpha = Image.new("L", gun.size, 255)
    apx = alpha.load()
    for gy in range(gun.size[1]):
        for gx in range(gun.size[0]):
            if bg[(y0 + gy) * w + (x0 + gx)]:
                apx[gx, gy] = 0

    # Fit the silhouette into the sprite canvas, aspect preserved.
    scale = min(SPRITE_W / gun.size[0], SPRITE_H / gun.size[1])
    tw = max(1, round(gun.size[0] * scale))
    th = max(1, round(gun.size[1] * scale))
    gun_s = gun.resize((tw, th), Image.LANCZOS)
    alpha_s = alpha.resize((tw, th), Image.BILINEAR)
    canvas = Image.new("RGB", (SPRITE_W, SPRITE_H), (0, 0, 0))
    calpha = Image.new("L", (SPRITE_W, SPRITE_H), 0)
    ox, oy = (SPRITE_W - tw) // 2, (SPRITE_H - th) // 2
    canvas.paste(gun_s, (ox, oy))
    calpha.paste(alpha_s, (ox, oy))

    # Quantize opaque pixels to PLAYPAL indices; -1 = transparent.
    cpx, apx2 = canvas.load(), calpha.load()
    grid = [[-1] * SPRITE_W for _ in range(SPRITE_H)]
    cache = {}
    for y in range(SPRITE_H):
        for x in range(SPRITE_W):
            if apx2[x, y] >= ALPHA_OPAQUE:
                rgb = cpx[x, y]
                if rgb not in cache:
                    cache[rgb] = nearest_index(palette, *rgb)
                grid[y][x] = cache[rgb]

    # Encode as a Doom patch: header, column offsets, posted columns.
    header_size = 8 + 4 * SPRITE_W
    columns = []
    offsets = []
    pos = header_size
    for x in range(SPRITE_W):
        col = bytearray()
        y = 0
        while y < SPRITE_H:
            while y < SPRITE_H and grid[y][x] < 0:
                y += 1
            if y >= SPRITE_H:
                break
            top = y
            run = []
            while y < SPRITE_H and grid[y][x] >= 0:
                run.append(grid[y][x])
                y += 1
            col += bytes([top, len(run), 0])
            col += bytes(run)
            col += b"\0"
        col += b"\xff"
        offsets.append(pos)
        columns.append(bytes(col))
        pos += len(col)

    patch = bytearray()
    patch += struct.pack("<hhhh", SPRITE_W, SPRITE_H,
                         SPRITE_W // 2, SPRITE_H)
    for ofs in offsets:
        patch += struct.pack("<i", ofs)
    for col in columns:
        patch += col
    while len(patch) % 4:
        patch += b"\0"

    words = struct.unpack("<%dI" % (len(patch) // 4), bytes(patch))
    lines = []
    for i in range(0, len(words), 6):
        lines.append("    " + ", ".join("0x%08x" % v
                                        for v in words[i:i + 6]) + ",")
    body = "\n".join(lines)
    with open(OUTPUT_C, "w") as f:
        f.write("""// DiabloDoom built-in super shotgun sprite, in Doom patch format.
// Auto-generated by assets/make_ssg_sprite.py - DO NOT EDIT.
//
// The super shotgun's pickup sprite SGN2A0 exists only in Doom II
// WADs, so this embedded patch stands in for one on Doom 1 /
// Freedoom 1: %dx%d (beefier than SHOTA0 38x13 and SGN2A0 43x14)
// with real patch transparency, drawn through the same patch path
// as the WAD sprites instead of the opaque compiled-icon blit.

#include "d_ssg_sprite.h"

static const unsigned int ssg_patch_words[] =
{
%s
};

patch_t *D_SsgSprite(void)
{
    return (patch_t *)ssg_patch_words;
}
""" % (SPRITE_W, SPRITE_H, body))
    opaque = sum(1 for row in grid for v in row if v >= 0)
    print("wrote %s: %dx%d patch, %d bytes, %d/%d opaque pixels"
          % (OUTPUT_C, SPRITE_W, SPRITE_H, len(patch), opaque,
             SPRITE_W * SPRITE_H))


if __name__ == "__main__":
    main()
