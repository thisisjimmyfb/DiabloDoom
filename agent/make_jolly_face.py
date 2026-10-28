#!/usr/bin/env python3
"""Build the default Jolly avatar face PWAD for DiabloDoom (v2).

v2 (2026-10-28) replaces the v1 duplication default, which made
50% health look exactly like 100% and the idle cycle invisible:
every one of the 42 slots now gets its OWN image. No byte is
shared across bands or straight variants (asserted below).

Source: the Jolly 24x29 expression sheet (~/workspace/
jolly_expressions_24x29.png in the author's workspace; a 3x2 grid
of 240x290 cells). Six states come from the sheet; evil grin and
rampage are synthesized at 24x29 from Normal (the sheet has no
grin/rampage frames), per agent/MUSE.md.

Fill: the STBAR face recess is ~30 wide, so v2 patches are 30x29
(offsets (-2,-2)), not 24x29. Each base face is cropped to its top
24 rows (dropping body-only rows) and NEAREST-scaled to 30x29, so
the head fills the frame and the features are ~25% larger. The
widget draws any size patch; vanilla faces themselves vary
(24-31 wide), so 30x29 is normal.

Grading (bands 1-4, progressively grimier; band 0 stays clean):
- Straight mouth per band: dot -> flat bar -> frown -> open
  small -> open grimace. (Straight is the idle face, so its
  progression is the one the player actually watches.)
- Marks per band (dark red-brown scratches, away from features):
  band 1: 2, band 2: 4, band 3: 6, band 4: 8. Applied to all six
  per-band expressions (straight, turns, ouch, grin, rampage).
- Eye narrowing for straight/turns at bands 3-4 (shortened, then
  slits). Ouch/grin/rampage keep their own eyes (their expression
  IS the eyes); marks carry their grading.
Straight variants per band: eyes center / 1px left / 1px right,
so the ~0.5s idle cycle visibly moves.
God/dead are singles (no bands); after the fill transform they
are forced mirror-symmetric (left half mirrored), per the
symmetry lesson.

Output: agent/jolly.wad — 42 lumps, the exact names
src/doom/st_stuff.c loads (ST_loadUnloadGraphics).
PLAYPAL comes from the IWAD (freedoom1.wad here).
Precedent for patch encoding: assets/make_pistol_sprite.py.
"""
import os
import struct

from PIL import Image, ImageDraw

REPO_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SHEET = os.path.expanduser("~/workspace/jolly_expressions_24x29.png")
DEAD_STANDALONE = os.path.expanduser("~/workspace/jolly_dead_24x29.png")
IWAD = os.path.expanduser("~/workspace/doom/freedoom1.wad")
OUTPUT = os.path.join(REPO_DIR, "agent", "jolly.wad")
PREVIEW_ALL = "/tmp/jolly_face_v2_preview_all.png"
PREVIEW_FOCUSED = "/tmp/jolly_face_v2_preview.png"

CELLS = {
    "Normal": (10, 10),
    "LookLeft": (260, 10),
    "LookRight": (510, 10),
    "Pain": (10, 330),
    "God": (260, 330),
    "Dead": (510, 330),
}
W, H = 30, 29
MOUTH_DARK = (56, 48, 37)   # Normal's mouth-dot color
EYE_DARK = (22, 24, 19)     # Normal's eye color
MARK = (96, 32, 32)         # scratch / battle wear
MARKS = {
    0: [],
    1: [(4, 14), (25, 14)],
    2: [(4, 14), (25, 14), (10, 6), (20, 6)],
    3: [(4, 14), (25, 14), (10, 6), (20, 6), (10, 21), (20, 21)],
    4: [(4, 14), (25, 14), (10, 6), (20, 6), (10, 21), (20, 21),
        (15, 5), (15, 24)],
}


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


def extract_faces_24():
    sheet = Image.open(SHEET).convert("RGB")
    faces = {}
    for name, (x, y) in CELLS.items():
        faces[name] = sheet.crop((x, y, x + 240, y + 290)).resize(
            (24, 29), Image.NEAREST
        )
    import numpy as np

    standalone = Image.open(DEAD_STANDALONE).convert("RGB")
    if not np.array_equal(np.asarray(faces["Dead"]), np.asarray(standalone)):
        print("WARN: sheet Dead != standalone dead; using standalone")
        faces["Dead"] = standalone
    return faces


def inpaint(img, pts):
    """Replace pts with the median of nearby non-dark pixels."""
    import numpy as np

    a = np.asarray(img).astype(int)
    lum = a.mean(axis=2)
    w, h = img.size
    for (x, y) in pts:
        vals = []
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                nx, ny = x + dx, y + dy
                if 0 <= nx < w and 0 <= ny < h and lum[ny, nx] >= 100:
                    vals.append(a[ny, nx])
        if vals:
            a[y, x] = np.median(np.array(vals), axis=0).astype(int)
    return Image.fromarray(a.astype("uint8"))


def synth_grin_24(normal):
    img = inpaint(normal, [(12, 12)])
    px = img.load()
    for (x, y) in [(10, 12), (11, 12), (12, 11), (13, 11), (14, 10)]:
        px[x, y] = MOUTH_DARK
    return img


def synth_rampage_24(normal):
    old_left = [(6, 8), (5, 9), (6, 9), (7, 9), (6, 10)]
    old_right = [(17, 8), (18, 8), (17, 9), (18, 9), (18, 10)]
    new_left = [(5, 8), (6, 9), (7, 10)]
    new_right = [(18, 8), (17, 9), (16, 10)]
    erase = [p for p in old_left + old_right if p not in new_left + new_right]
    img = inpaint(normal, erase)
    px = img.load()
    for (x, y) in new_left + new_right:
        px[x, y] = EYE_DARK
    for x in range(9, 15):
        px[x, 12] = MOUTH_DARK
    return img


def fill_transform(img24):
    """Crop body-only bottom rows, NEAREST-scale to the 30x29 box."""
    return img24.crop((0, 0, 24, 24)).resize((W, H), Image.NEAREST)


def symmetrize(img):
    """Force exact left-right mirror symmetry (god/dead only)."""
    px = img.load()
    for x in range(W // 2):
        for y in range(H):
            px[W - 1 - x, y] = px[x, y]
    return img


def dark_sets(img):
    """Split dark feature pixels into eyes (upper) and mouth (lower)."""
    import numpy as np

    a = np.asarray(img).astype(int)
    lum = a.mean(axis=2)
    eyes, mouth = [], []
    for y in range(H):
        for x in range(W):
            if lum[y, x] < 90:
                (eyes if y <= 12 else mouth).append((x, y))
    return eyes, mouth


def shift_eyes(img, dx):
    """Move eye pixels horizontally by dx (straight variants)."""
    import numpy as np

    eyes, _ = dark_sets(img)
    a = np.asarray(img)
    colors = {p: tuple(a[p[1], p[0]]) for p in eyes}
    img = inpaint(img, eyes)
    px = img.load()
    for (x, y), col in colors.items():
        nx = x + dx
        if 0 <= nx < W:
            px[nx, y] = col
    return img


def narrow_eyes(img, mode):
    """Band 3: shorten eyes (drop bottom row). Band 4: slits (middle row)."""
    eyes, _ = dark_sets(img)
    if not eyes:
        return img
    rows = sorted(set(y for _, y in eyes))
    if mode == "shorten":
        drop = [(x, y) for (x, y) in eyes if y == rows[-1]]
    else:  # slit: keep only the middle row
        keep = rows[len(rows) // 2]
        drop = [(x, y) for (x, y) in eyes if y != keep]
    return inpaint(img, drop)


def replace_mouth(img, pixels):
    """Replace the mouth with the given pixel set (straight bands 1-4)."""
    _, mouth = dark_sets(img)
    img = inpaint(img, mouth)
    px = img.load()
    for (x, y) in pixels:
        if 0 <= x < W and 0 <= y < H:
            px[x, y] = MOUTH_DARK
    return img


def add_marks(img, band):
    px = img.load()
    for (x, y) in MARKS[band]:
        px[x, y] = MARK
    return img


def straight_mouths(cx, cy):
    """Mouth shapes per band, centered on the base mouth centroid."""
    return {
        0: None,  # keep base mouth
        1: [(cx - 2, cy), (cx - 1, cy), (cx, cy), (cx + 1, cy), (cx + 2, cy)],
        2: [(cx - 2, cy + 1), (cx - 1, cy), (cx, cy), (cx + 1, cy),
            (cx + 2, cy + 1)],
        3: [(cx - 1, cy), (cx + 1, cy),
            (cx - 1, cy + 1), (cx, cy + 1), (cx + 1, cy + 1)],
        4: [(cx - 2, cy), (cx - 1, cy), (cx + 1, cy), (cx + 2, cy),
            (cx - 2, cy + 1), (cx - 1, cy + 1), (cx, cy + 1),
            (cx + 1, cy + 1), (cx + 2, cy + 1)],
    }


def build_faces():
    f24 = extract_faces_24()
    f24["Grin"] = synth_grin_24(f24["Normal"])
    f24["Rampage"] = synth_rampage_24(f24["Normal"])
    base = {k: fill_transform(v) for k, v in f24.items()}
    base["God"] = symmetrize(base["God"])
    base["Dead"] = symmetrize(base["Dead"])
    # Mouth centroid of the straight base (for band mouths).
    _, mouth = dark_sets(base["Normal"])
    cx = sum(x for x, _ in mouth) // len(mouth)
    cy = sum(y for _, y in mouth) // len(mouth)
    mouths = straight_mouths(cx, cy)

    faces = {}  # lump name -> image
    for band in range(5):
        # Straights: 3 eye-shift variants.
        for variant, dx in enumerate((0, -1, 1)):
            img = shift_eyes(base["Normal"], dx)
            if band == 3:
                img = narrow_eyes(img, "shorten")
            elif band == 4:
                img = narrow_eyes(img, "slit")
            if mouths[band] is not None:
                img = replace_mouth(img, mouths[band])
            img = add_marks(img, band)
            faces[f"STFST{band}{variant}"] = img
        # Turns.
        for name, src in (("STFTR", base["LookRight"]),
                          ("STFTL", base["LookLeft"])):
            img = src.copy()
            if band == 3:
                img = narrow_eyes(img, "shorten")
            elif band == 4:
                img = narrow_eyes(img, "slit")
            img = add_marks(img, band)
            faces[f"{name}{band}0"] = img
        # Ouch / grin / rampage: marks only (expression preserved).
        for name, src in (("STFOUCH", base["Pain"]),
                          ("STFEVL", base["Grin"]),
                          ("STFKILL", base["Rampage"])):
            faces[f"{name}{band}"] = add_marks(src.copy(), band)
    faces["STFGOD0"] = base["God"]
    faces["STFDEAD0"] = base["Dead"]
    return faces


def build_pwad(faces, palette):
    order = []
    for i in range(5):
        for j in range(3):
            order.append(f"STFST{i}{j}")
        order.append(f"STFTR{i}0")
        order.append(f"STFTL{i}0")
        order.append(f"STFOUCH{i}")
        order.append(f"STFEVL{i}")
        order.append(f"STFKILL{i}")
    order += ["STFGOD0", "STFDEAD0"]
    assert len(order) == 42
    blobs = []
    for name in order:
        blobs.append((name, encode_patch(quantize(faces[name], palette))))
    # Distinctness: no two slots may share bytes (the v1 bug).
    seen = {}
    for name, blob in blobs:
        assert blob not in seen, f"duplicate patch bytes: {name} == {seen[blob]}"
        seen[blob] = name
    header_size = 12
    dir_off = header_size + sum(len(b) for _, b in blobs)
    out = [struct.pack("<4sii", b"PWAD", len(blobs), dir_off)]
    pos, directory = header_size, []
    for name, blob in blobs:
        out.append(blob)
        directory.append(struct.pack("<ii8s", pos, len(blob),
                                     name.encode().ljust(8, b"\x00")))
        pos += len(blob)
    out.extend(directory)
    return b"".join(out), order


def encode_patch(index_grid):
    """Doom patch_t: 30x29, offsets (-2,-2), one full post per column."""
    header_size = 8 + 4 * W
    col_data, offsets, pos = [], [], header_size
    for x in range(W):
        offsets.append(pos)
        col = bytes(index_grid[y][x] for y in range(H))
        data = bytes([0, H, 0]) + col + bytes([0, 0xFF])
        col_data.append(data)
        pos += len(data)
    header = struct.pack("<hhhh", W, H, -2, -2)
    header += struct.pack("<" + "i" * W, *offsets)
    return header + b"".join(col_data)


def save_grid(faces, names, cols, scale, path):
    cw, chh = W * scale, H * scale
    rows = (len(names) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * (cw + 10) + 10,
                              rows * (chh + 26) + 10), (24, 24, 24))
    dr = ImageDraw.Draw(sheet)
    for k, name in enumerate(names):
        r, c = divmod(k, cols)
        x = 10 + c * (cw + 10)
        y = 10 + r * (chh + 26)
        sheet.paste(faces[name].resize((cw, chh), Image.NEAREST), (x, y))
        dr.text((x, y + chh + 6), name, fill=(255, 255, 255))
    sheet.save(path)


def main():
    palette = load_playpal(IWAD)
    faces = build_faces()
    pwad, names = build_pwad(faces, palette)
    os.makedirs(os.path.dirname(OUTPUT), exist_ok=True)
    with open(OUTPUT, "wb") as f:
        f.write(pwad)
    print(f"wrote {OUTPUT} ({len(pwad)} bytes, {len(names)} lumps, "
          "all distinct)")
    save_grid(faces, names, 7, 4, PREVIEW_ALL)
    focused = ([f"STFST{b}0" for b in range(5)]
               + [f"STFST2{v}" for v in range(3)]
               + ["STFTR20", "STFTL20", "STFOUCH2", "STFEVL2",
                  "STFKILL2", "STFGOD0", "STFDEAD0"])
    save_grid(faces, focused, 5, 6, PREVIEW_FOCUSED)
    print("previews:", PREVIEW_ALL, PREVIEW_FOCUSED)


if __name__ == "__main__":
    main()
