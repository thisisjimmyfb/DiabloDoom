#!/usr/bin/env python3
"""Build the default Jolly avatar face PWAD for DiabloDoom.

Source: the Jolly 24x29 expression sheet (~/workspace/
jolly_expressions_24x29.png in the author's workspace; the sheet is
a 3x2 grid of 240x290 cells, 10px borders/gutters, labels below
each row). Six states come straight from the sheet; evil grin and
rampage are synthesized at 24x29 from Normal (the sheet has no
grin/rampage frames), per agent/MUSE.md.

Output: agent/jolly_face.wad — 42 lumps, the exact names
src/doom/st_stuff.c loads (ST_loadUnloadGraphics). Default
duplication: one image per expression, repeated across all 5 pain
bands and across the 3 straight variants. All patches are 24x29,
offsets (-5,-2) matching STFST00/STFGOD0, fully opaque (the Jolly
cell fills its box; no transparency posts needed).

PLAYPAL comes from the IWAD (freedoom1.wad here; any Doom IWAD
works — the palette is only used to quantize RGB to indices).
Precedent for patch encoding: assets/make_pistol_sprite.py.
"""
import os
import struct
import sys

from PIL import Image

REPO_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SHEET = os.path.expanduser("~/workspace/jolly_expressions_24x29.png")
DEAD_STANDALONE = os.path.expanduser("~/workspace/jolly_dead_24x29.png")
IWAD = os.path.expanduser("~/workspace/doom/freedoom1.wad")
OUTPUT = os.path.join(REPO_DIR, "agent", "jolly_face.wad")
PREVIEW = "/tmp/jolly_face_preview.png"

# Sheet cells: (x, y) of each 240x290 face, labels below each row.
CELLS = {
    "Normal": (10, 10),
    "LookLeft": (260, 10),
    "LookRight": (510, 10),
    "Pain": (10, 330),
    "God": (260, 330),
    "Dead": (510, 330),
}
DARK = (56, 48, 37)  # Normal's mouth-dot color, reused for synth mouths


def load_playpal(iwad_path):
    d = open(iwad_path, "rb").read()
    n = struct.unpack("<i", d[4:8])[0]
    off = struct.unpack("<i", d[8:12])[0]
    for i in range(n):
        fp, sz = struct.unpack("<ii", d[off + i * 16: off + i * 16 + 8])
        nm = d[off + i * 16 + 8: off + i * 16 + 16].split(b"\x00")[0].decode()
        if nm == "PLAYPAL":
            pal = d[fp: fp + 768]
            return [(pal[j], pal[j + 1], pal[j + 2]) for j in range(0, 768, 3)]
    raise SystemExit("PLAYPAL not found in " + iwad_path)


def quantize(rgb_img, palette):
    """Nearest PLAYPAL index per pixel (Euclidean RGB)."""
    px = rgb_img.load()
    w, h = rgb_img.size
    out = [[0] * w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            best, bestd = 0, 1 << 30
            for idx, (pr, pg, pb) in enumerate(palette):
                dd = (r - pr) ** 2 + (g - pg) ** 2 + (b - pb) ** 2
                if dd < bestd:
                    best, bestd = idx, dd
            out[y][x] = best
    return out


def extract_faces():
    sheet = Image.open(SHEET).convert("RGB")
    faces = {}
    for name, (x, y) in CELLS.items():
        faces[name] = sheet.crop((x, y, x + 240, y + 290)).resize(
            (24, 29), Image.NEAREST
        )
    # Dead: the standalone file is the approved symmetric dead; it
    # must match the sheet cell exactly (it did at authoring time).
    standalone = Image.open(DEAD_STANDALONE).convert("RGB")
    import numpy as np

    if not np.array_equal(
        np.asarray(faces["Dead"]), np.asarray(standalone)
    ):
        print("WARN: sheet Dead != standalone dead; using standalone")
        faces["Dead"] = standalone
    return faces


def inpaint(img, pts):
    """Replace pts with the median of nearby non-dark pixels."""
    import numpy as np

    a = np.asarray(img).astype(int)
    lum = a.mean(axis=2)
    for (x, y) in pts:
        vals = []
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                nx, ny = x + dx, y + dy
                if 0 <= nx < 24 and 0 <= ny < 29 and lum[ny, nx] >= 100:
                    vals.append(a[ny, nx])
        if vals:
            a[y, x] = np.median(np.array(vals), axis=0).astype(int)
    return Image.fromarray(a.astype("uint8"))


def synth_grin(normal):
    """Asymmetric smirk rising to the right (vs God's symmetric smile)."""
    img = normal.copy()
    px = img.load()
    # Erase the old center dot, then draw the smirk.
    img = inpaint(img, [(12, 12)])
    px = img.load()
    for (x, y) in [(10, 12), (11, 12), (12, 11), (13, 11), (14, 10)]:
        px[x, y] = DARK
    return img


def synth_rampage(normal):
    """Angry V eyes + flat gritted mouth (vs Pain's squeezed eyes +
    open mouth, vs Normal's dot)."""
    img = normal.copy()
    # Old eye pixels not covered by the new diagonals get inpainted.
    old_left = [(6, 8), (5, 9), (6, 9), (7, 9), (6, 10)]
    old_right = [(17, 8), (18, 8), (17, 9), (18, 9), (18, 10)]
    new_left = [(5, 8), (6, 9), (7, 10)]
    new_right = [(18, 8), (17, 9), (16, 10)]
    erase = [p for p in old_left + old_right if p not in new_left + new_right]
    img = inpaint(img, erase)
    px = img.load()
    dark_eye = (22, 24, 19)
    for (x, y) in new_left + new_right:
        px[x, y] = dark_eye
    for x in range(9, 15):  # flat gritted bar (old dot at (12,12) absorbed)
        px[x, 12] = DARK
    return img


def encode_patch(index_grid):
    """Doom patch_t: 24x29, offsets (-5,-2), one full post per column."""
    w, h = 24, 29
    header_size = 8 + 4 * w
    col_data = []
    offsets = []
    pos = header_size
    for x in range(w):
        offsets.append(pos)
        col = bytes(index_grid[y][x] for y in range(h))
        data = bytes([0, h, 0]) + col + bytes([0, 0xFF])
        col_data.append(data)
        pos += len(data)
    header = struct.pack("<hhhh", w, h, -5, -2)
    header += struct.pack("<" + "i" * w, *offsets)
    return header + b"".join(col_data)


def build_pwad(faces, palette):
    expr = {
        "straight": faces["Normal"],
        "turnright": faces["LookRight"],
        "turnleft": faces["LookLeft"],
        "ouch": faces["Pain"],
        "grin": faces["Grin"],
        "rampage": faces["Rampage"],
        "god": faces["God"],
        "dead": faces["Dead"],
    }
    lumps = []  # (name, patch bytes) in loader order
    for i in range(5):
        for j in range(3):
            lumps.append((f"STFST{i}{j}", expr["straight"]))
        lumps.append((f"STFTR{i}0", expr["turnright"]))
        lumps.append((f"STFTL{i}0", expr["turnleft"]))
        lumps.append((f"STFOUCH{i}", expr["ouch"]))
        lumps.append((f"STFEVL{i}", expr["grin"]))
        lumps.append((f"STFKILL{i}", expr["rampage"]))
    lumps.append(("STFGOD0", expr["god"]))
    lumps.append(("STFDEAD0", expr["dead"]))
    assert len(lumps) == 42, len(lumps)
    blobs = []
    for name, img in lumps:
        assert len(name) <= 8, name
        blobs.append((name, encode_patch(quantize(img, palette))))
    header_size = 12
    data_size = sum(len(b) for _, b in blobs)
    dir_off = header_size + data_size
    out = [struct.pack("<4sii", b"PWAD", len(blobs), dir_off)]
    pos = header_size
    directory = []
    for name, blob in blobs:
        out.append(blob)
        directory.append(struct.pack("<ii8s", pos, len(blob),
                                     name.encode().ljust(8, b"\x00")))
        pos += len(blob)
    out.extend(directory)
    return b"".join(out), [n for n, _ in blobs]


def main():
    palette = load_playpal(IWAD)
    faces = extract_faces()
    faces["Grin"] = synth_grin(faces["Normal"])
    faces["Rampage"] = synth_rampage(faces["Normal"])
    pwad, names = build_pwad(faces, palette)
    os.makedirs(os.path.dirname(OUTPUT), exist_ok=True)
    with open(OUTPUT, "wb") as f:
        f.write(pwad)
    print(f"wrote {OUTPUT} ({len(pwad)} bytes, {len(names)} lumps)")
    # Preview montage for visual verification (not shipped).
    order = ["Normal", "LookLeft", "LookRight", "Pain",
             "Grin", "Rampage", "God", "Dead"]
    sheet = Image.new("RGB", (8 * 24 * 6 + 70, 29 * 6 + 30), (24, 24, 24))
    from PIL import ImageDraw

    dr = ImageDraw.Draw(sheet)
    for k, name in enumerate(order):
        big = faces[name].resize((24 * 6, 29 * 6), Image.NEAREST)
        x = 10 + k * (24 * 6 + 10)
        sheet.paste(big, (x, 10))
        dr.text((x, 29 * 6 + 12), name, fill=(255, 255, 255))
    sheet.save(PREVIEW)
    print("preview:", PREVIEW)
    print("lumps:", " ".join(names))


if __name__ == "__main__":
    main()
