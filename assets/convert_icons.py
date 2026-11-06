#!/usr/bin/env python3
"""Convert item icons to Doom palette-indexed C arrays.

Reads WebP icons from assets/items/ (repo-relative), resizes to 32x32,
quantizes to the Doom PLAYPAL palette, and writes
src/diablo/d_diablo_icons.c + src/diablo/d_diablo_icons.h.

Icon policy (AD/AP gun rework):
- The 8 gun bases share one icon each (variants reuse the base icon).
- The 4 FOCUS items share the ammo-box icon.
- Every other item keeps a unique icon.
Total: 49 icons. Order defines the D_ICON_* indices.
"""
import os
from PIL import Image

REPO_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS_DIR = os.path.join(REPO_DIR, "assets", "items")
OUTPUT_C = os.path.join(REPO_DIR, "src", "diablo", "d_diablo_icons.c")
OUTPUT_H = os.path.join(REPO_DIR, "src", "diablo", "d_diablo_icons.h")
PALETTE_RAW = "/tmp/doom_palette.raw"
ICON_SIZE = 32

# Icon name -> filename (without extension).
ICONS = [
    # Gun bases (8): all variants of a base share its icon.
    "gun_pistol",
    "gun_shotgun",
    "gun_chaingun",
    "gun_chainsaw",
    "gun_rocket",
    "gun_plasma",
    "gun_bfg",
    "gun_ssg",
    # Shared (1): the 4 FOCUS items (ammo-as-equipment).
    "ammo_box",
    # Normal non-gun (9)
    "armor_leather",
    "shield_buckler",
    "helm_cap",
    "belt",
    "potion_red",
    "potion_blue",
    "potion_green",
    "boot_leather",
    "glove_leather",
    # Magic non-gun (6)
    "m_vampiric_bone_shield",
    "m_prismatic_amulet",
    "m_lizards_ring",
    "m_soldiers_chain_mail",
    "boot_traveler",
    "glove_assault",
    # Rare non-gun (4)
    "r_nightmare_coil",
    "r_grimward",
    "boot_stormwalker",
    "glove_doom_grasp",
    # Set non-gun (8)
    "s_tal_rasha_crest",
    "s_ik_soul_cage",
    "s_trang_oul_guise",
    "s_mavina_sight",
    "s_natalya_shadow",
    "s_griswold_valor",
    "boot_ik_pillar",
    "glove_mavina_clutch",
    # Unique non-gun (11)
    "u_stone_of_jordan",
    "u_harlequin_crest",
    "u_arkaine_valor",
    "u_maras_kaleidoscope",
    "u_bulkathos_band",
    "u_lidless_wall",
    "u_vipermagi",
    "u_thundergod_vigor",
    "u_raven_frost",
    "boot_war_traveler",
    "glove_frostburn",
    # Generic slot icons (2): fallback for any item with no specific
    # icon, so it shows its slot's generic art instead of the pistol.
    "amulet",
    "ring",
]

def load_palette():
    with open(PALETTE_RAW, "rb") as f:
        data = f.read()
    # List of (r, g, b) tuples
    return [(data[i], data[i+1], data[i+2]) for i in range(0, 768, 3)]

def find_nearest(palette, r, g, b, exclude=None):
    """Find nearest palette index by Euclidean distance."""
    best_idx = 0
    best_dist = float('inf')
    for i, (pr, pg, pb) in enumerate(palette):
        if i == exclude:
            continue
        dr = r - pr
        dg = g - pg
        db = b - pb
        dist = dr*dr + dg*dg + db*db
        if dist < best_dist:
            best_dist = dist
            best_idx = i
            if dist == 0:
                break
    return best_idx

# Palette index reserved for transparent pixels. The renderer skips
# these, so the icon's black background disappears and the rarity
# tile behind it shows through. Real pixels are never allowed to
# quantize to this index (see convert_icon).
TRANSPARENT_IDX = 255
# A pixel counts as background (near-black) for the flood fill.
BG_THRESHOLD = 64

def convert_icon(path, palette):
    """Load image, resize to ICON_SIZE, quantize to palette. Returns bytes.

    The source art is a centered item on a black background. A flood
    fill from the border through near-black pixels finds that
    background (and only it -- dark parts inside the item are not
    connected to the border, so they stay opaque) and marks it with
    the transparent index.
    """
    img = Image.open(path).convert("RGB")
    img = img.resize((ICON_SIZE, ICON_SIZE), Image.LANCZOS)
    pixels = img.load()
    # Near-black grid for the flood fill.
    is_bg = [[max(pixels[x, y]) < BG_THRESHOLD
              for x in range(ICON_SIZE)] for y in range(ICON_SIZE)]
    # Flood fill from every border cell that is background.
    transparent = [[False] * ICON_SIZE for _ in range(ICON_SIZE)]
    stack = []
    for x in range(ICON_SIZE):
        for y in (0, ICON_SIZE - 1):
            if is_bg[y][x] and not transparent[y][x]:
                transparent[y][x] = True
                stack.append((x, y))
    for y in range(ICON_SIZE):
        for x in (0, ICON_SIZE - 1):
            if is_bg[y][x] and not transparent[y][x]:
                transparent[y][x] = True
                stack.append((x, y))
    while stack:
        x, y = stack.pop()
        for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
            if 0 <= nx < ICON_SIZE and 0 <= ny < ICON_SIZE \
                    and is_bg[ny][nx] and not transparent[ny][nx]:
                transparent[ny][nx] = True
                stack.append((nx, ny))
    result = bytearray()
    for y in range(ICON_SIZE):
        for x in range(ICON_SIZE):
            if transparent[y][x]:
                result.append(TRANSPARENT_IDX)
                continue
            r, g, b = pixels[x, y]
            idx = find_nearest(palette, r, g, b, exclude=TRANSPARENT_IDX)
            result.append(idx)
    return bytes(result)

def main():
    palette = load_palette()
    print(f"Loaded palette with {len(palette)} colors")

    icon_data = {}
    missing = []
    for name in ICONS:
        path = os.path.join(ASSETS_DIR, name + ".webp")
        if not os.path.exists(path):
            print(f"WARNING: Missing {path}")
            missing.append(name)
            continue
        data = convert_icon(path, palette)
        icon_data[name] = data
        print(f"Converted {name}: {len(data)} bytes")

    if missing:
        raise SystemExit(f"Missing icons: {missing}")

    # Write C file
    with open(OUTPUT_C, "w") as f:
        f.write("// DiabloDoom item icons - Doom palette-indexed 32x32 pixel data.\n")
        f.write("// Auto-generated by assets/convert_icons.py - DO NOT EDIT.\n\n")
        f.write('#include "d_diablo_icons.h"\n\n')
        for i, name in enumerate(ICONS):
            data = icon_data[name]
            f.write(f"static const unsigned char icon_{i}[] = {{\n")
            for y in range(ICON_SIZE):
                row = data[y*ICON_SIZE:(y+1)*ICON_SIZE]
                f.write("    " + ", ".join(str(b) for b in row) + ",\n")
            f.write("};\n\n")
        f.write("const unsigned char *d_item_icons[] = {\n")
        for i in range(len(ICONS)):
            f.write(f"    icon_{i},\n")
        f.write("};\n\n")
        f.write(f"const int d_num_item_icons = {len(ICONS)};\n")
        f.write(f"const int d_icon_size = {ICON_SIZE};\n")

    # Write header
    with open(OUTPUT_H, "w") as f:
        f.write("// DiabloDoom item icons - public interface.\n")
        f.write("// Auto-generated by assets/convert_icons.py - DO NOT EDIT.\n")
        f.write("#ifndef D_DIABLO_ICONS_H\n")
        f.write("#define D_DIABLO_ICONS_H\n\n")
        f.write("// 32x32 palette-indexed icon data, one per entry.\n")
        f.write("extern const unsigned char *d_item_icons[];\n")
        f.write("extern const int d_num_item_icons;\n")
        f.write("extern const int d_icon_size;\n\n")
        f.write("// Palette index for transparent pixels (the icon's black\n")
        f.write("// background). The renderer skips these. Real pixels are\n")
        f.write("// never quantized to this index.\n")
        f.write("#define D_ICON_TRANSPARENT 255\n\n")
        f.write("// Icon indices (must match order in d_diablo_icons.c).\n")
        for i, name in enumerate(ICONS):
            f.write(f"#define D_ICON_{name.upper()} {i}\n")
        f.write("\n#endif\n")

    print(f"Wrote {len(ICONS)} icons to {OUTPUT_C} and {OUTPUT_H}")

if __name__ == "__main__":
    main()
