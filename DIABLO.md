# DiabloDoom — Diablo-style loot in Chocolate Doom

A mod of Chocolate Doom 3.1.1 that adds Diablo-style loot, equipment,
and a character screen to Doom.

## Phase 1: Loot backend (done)

- Every monster kill has a 70% chance to drop loot.
- 5 rarity tiers: normal, magic, rare, set, unique (~6% unique).
- 42 data-driven item definitions with Diablo names (Stone of Jordan,
  Harlequin Crest, etc.), equipment slots, grid sizes, and stats.
- Stats: weapon damage, strength, armor, life steal, vitality,
  magic find, movement speed.
- E: equip/use most recent item. Q: unequip all.

## Phase 2: Character screen (done)

Press **C** to open the character screen (single-player only). The game
pauses while the screen is open.

### Layout

- **Paperdoll** (left): helm, armor, weapon, shield, 2 rings, amulet,
  boots, gloves, belt.
- **Backpack** (right): 10×4 grid. Items occupy their grid_w × grid_h
  footprint (e.g., weapons 2×3, potions 1×1).
- **Stats** (bottom): damage, armor, strength, dexterity, vitality,
  energy, resistances, life steal, magic find, movement speed,
  crit chance, crit damage.

### Mouse controls (two-click)

The UI uses click-to-pick-up / click-to-place (not hold-and-drag):

- **Left-click** an item: pick it up (cursor holds it).
- **Left-click** a valid equipment slot: equip it. If the slot is
  occupied, the items swap.
- **Left-click** a backpack cell: place the held item (top-left at the
  clicked cell). Rejected if out of bounds or overlapping.
- **Left-click** empty space: nothing.
- **Right-click**: if holding a consumable, use it. Else if holding an
  item, cancel (stash it back). Else close the screen.
- **Hover**: tooltip shows name, rarity, damage, armor, attributes,
  resistances, life steal, magic find, movement speed.
- **ESC** or **C**: close (stashes any held item).

### Rarity colors

- Normal: white/gray
- Magic: blue
- Rare: yellow
- Set: green
- Unique: gold/purple

### Fallbacks

- **E**: equip/use most recent backpack item (Phase 1 behavior).
- **Q**: unequip all to backpack.

### Limitations (Phase 2)

- Placeholder visuals: items are rarity-colored boxes with the item's
  initial letter. Custom 2D art is Phase 3.
- Two-click interaction, not drag-and-drop.
- Single-player only. The screen does not open in multiplayer.
- Equipment resets on save/load and new game (Phase 1 limitation).
- No boot/glove items in the loot pool yet.

## Phase 3 (planned)

- Original 2D art/icons for every loot item.
- Convert and pack artwork into Doom-compatible assets.

## Building

```sh
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc) chocolate-doom
```

Run with a Doom IWAD (e.g., FreeDoom):
```sh
./src/chocolate-doom -iwad freedoom1.wad
```
