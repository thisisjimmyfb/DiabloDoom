# DiabloDoom — Diablo-style loot in Chocolate Doom

A mod of Chocolate Doom 3.1.1 that adds Diablo-style loot, equipment,
and a character screen to Doom.

## Phase 1: Loot backend (done)

- Every monster kill has a 70% chance to drop loot.
- 5 rarity tiers: normal, magic, rare, set, unique (~6% unique).
- 42 data-driven item definitions with Diablo names (Stone of Jordan,
  Harlequin Crest, etc.), equipment slots, grid sizes, and stats.
- Stats: weapon damage, strength, defense, life steal, vitality,
  magic find, movement speed.
- E: equip/use most recent item. Q: unequip all.

## Phase 2: Character screen (done)

Press **C** to open the character screen (single-player only). The game
pauses while the screen is open.

### Layout

- **Paperdoll** (left): helm, armor, weapon, shield, 2 rings, amulet,
  boots, gloves, belt, plus a **FOCUS** slot — passive, non-consumable
  focus items (Piercing Rounds, Incendiary Shells, …) that grant stat
  bonuses to the equipped weapon.
- **Backpack** (right): 10×4 grid. Items occupy their grid_w × grid_h
  footprint (e.g., weapons 2×3, potions 1×1).
- **Stats** (bottom): damage, defense, strength, dexterity, vitality,
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
- **Hover**: tooltip shows name, rarity, damage, defense, durability,
  attributes, resistances, life steal, magic find, movement speed.
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

## Ammo

Every weapon carries its own magazine and needs ammo to fire: pistol 10
(1/shot), shotgun 20 (2/shot), chaingun 10 (1/shot), chainsaw 6 (1/shot),
rocket 20 (10/shot), Super Shotgun 8 (8/shot), BFG 20 (20/shot). Plasma
has no magazine — its heat *is* the ammo. Magazines refill automatically
by weapon type: AD weapons feed off your **attack speed** each round, AP
weapons reload when their **cooldown** finishes (rocket reloads as
charges regenerate). Ammo pickups are gone entirely: every map-placed
ammo pickup now spawns a random Diablo item instead, and Mana Potions
no longer drop from kills (any you already carry still works). The status bar's left AMMO area shows your
ready weapon's current magazine (remaining heat capacity for plasma).

## Weapon pickups

Walking over a Doom weapon (map-placed or dropped by an enemy)
grants a Diablo item **of that same weapon type**, rolled on the
usual rarity table — Normal through Unique, with your magic find
applied — first acquisition or duplicate alike. So a shotgun on
the ground might become a plain Shotgun, a magic Vampiric Shotgun,
or a rare Splitting Shotgun. The tier tables have gaps: not every
weapon type has a Set or Unique variant yet (there is no unique
pistol), and a roll that lands on a missing tier is re-rolled, so
those weapons simply can't produce that tier for now. If the
backpack has no room for the item, the weapon stays on the ground.
This matters because magazines auto-replenish: the classic "walk
over a duplicate for its ammo" almost never applies here, so
without the item grant a repeat pickup would do nothing at all.

Weapon item names follow the WAD being played: when a WAD renames
a Doom weapon in its DEHACKED strings, item names built around
that weapon inherit the new name — under Freedoom the chaingun is
the "minigun" and the chainsaw the "ripsaw", so you get Miniguns
and Vampiric Ripsaws. Two of Freedoom's names are shortened for
display because they're too long to sit inside an item name:
"SKAG 1337" shows as just SKAG (so, a Wizard's SKAG),
"Pump-Action Shotgun" as just Shotgun, "Double-Barrelled
Shotgun" as Dbl Shotgun, "Polaric Energy Weapon" (and "Polaric
Energy Cannon" in WADs that call it that) as Nrg Cannon, and
"Angle Grinder" as Grinder. Bespoke names
(Unmaker, Old Painless) and all non-weapon items (helms, boots,
rings…) keep their Diablo names, and the pistol keeps its name
too — it has no pickup string to inherit from.

## Explore / combat

The game is not always turn-based. **Explore** freely with WASD + arrows
(real-time movement); walking over loot picks it up. When an enemy
**spots you**, the game switches to **turn-based combat**: queue actions
(W/S/A/D move, F attack, Space use), then press **T** to execute them
in order, then enemies act. Clear all visible enemies to return to
exploring. Barrels never start combat but can still be targeted with
Tab. **G** auto-paths to the nearest loot during combat (walk to loot
while exploring).

## Essence (salvage / reroll / repair)

Junk loot is reroll fuel. **S** in the character screen salvages the
held (or selected backpack) item into **Essence** — Normal 1, Magic 3,
Rare 8, Set 15, Unique 25. **R** rerolls a Magic-or-better item into a
new random item of the same tier: Magic 5, Rare 15, Set 25, Unique 40
Essence (Normal items can't be rerolled). **F** repairs the held (or
selected backpack) item to full durability, pro-rated by tier and how
much durability is missing. Your Essence total shows in the inventory
header and is saved with your game.

## Defense & durability

Armor works like Diablo 2. Your helm, armor, boots, gloves and belt
each carry a **Defense** value (hundreds to thousands on good gear);
the total is your Defense rating. It doesn't reduce damage — it makes
monsters **miss**: the tougher the monster and the lower your Defense,
the more often it hits, and a landed hit deals full damage. Shields
don't add Defense.

Those five pieces also wear out. Every hit that lands on you knocks
1 point off a random worn piece. At 0 durability a piece is broken:
it keeps its other stats (+Strength, resists, ...) but adds no
Defense until repaired. Repair with **F** for Essence — or pick up a
vanilla Doom armor suit (green or blue) or a megasphere, which
restores every equipped piece to full for free. The +1 armor bonus
shards don't repair. Hover any piece in the character screen to see
its durability.

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
