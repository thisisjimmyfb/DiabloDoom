# Chocolate Doom

Chocolate Doom aims to accurately reproduce the original DOS version of
Doom and other games based on the Doom engine in a form that can be
run on modern computers.

Originally, Chocolate Doom was only a Doom source port. The project
now includes ports of Heretic and Hexen, and Strife.

Chocolate Doom’s aims are:

 * To always be 100% Free and Open Source software.
 * Portability to as many different operating systems as possible.
 * Accurate reproduction of the original DOS versions of the games,
   including bugs.
 * Compatibility with the DOS demo, configuration and savegame files.
 * To provide an accurate retro “feel” (display and input should
   behave the same).

More information about the philosophy and design behind Chocolate Doom
can be found in the PHILOSOPHY file distributed with the source code.

## Setting up gameplay

For instructions on how to set up Chocolate Doom for play, see the
INSTALL file.

## Configuration File

Chocolate Doom is compatible with the DOS Doom configuration file
(normally named `default.cfg`). Existing configuration files for DOS
Doom should therefore simply work out of the box. However, Chocolate
Doom also provides some extra settings. These are stored in a
separate file named `chocolate-doom.cfg`.

The configuration can be edited using the chocolate-setup tool.

## Command line options

Chocolate Doom supports a number of command line parameters, including
some extras that were not originally suported by the DOS versions. For
binary distributions, see the CMDLINE file included with your
download; more information is also available on the Chocolate Doom
website.

## Playing TCs

With Vanilla Doom there is no way to include sprites in PWAD files.
Chocolate Doom’s ‘-file’ command line option behaves exactly the same
as Vanilla Doom, and trying to play TCs by adding the WAD files using
‘-file’ will not work.

Many Total Conversions (TCs) are distributed as a PWAD file which must
be merged into the main IWAD. Typically a copy of DEUSF.EXE is
included which performs this merge. Chocolate Doom includes a new
option, ‘-merge’, which will simulate this merge. Essentially, the
WAD directory is merged in memory, removing the need to modify the
IWAD on disk.

To play TCs using Chocolate Doom, run like this:

```
chocolate-doom -merge thetc.wad
```

Here are some examples:

```
chocolate-doom -merge batman.wad -deh batman.deh vbatman.deh  (Batman Doom)
chocolate-doom -merge aoddoom1.wad -deh aoddoom1.deh  (Army of Darkness Doom)
```

## Other information

 * Chocolate Doom includes a number of different options for music
   playback. See the README.Music file for more details.

 * More information, including information about how to play various
   classic TCs, is available on the Chocolate Doom website:

     https://www.chocolate-doom.org/

   You are encouraged to sign up and contribute any useful information
   you may have regarding the port!

 * Chocolate Doom is not perfect. Although it aims to accurately
   emulate and reproduce the DOS executables, some behavior can be very
   difficult to reproduce. Because of the nature of the project, you
   may also encounter Vanilla Doom bugs; these are intentionally
   present; see the NOT-BUGS file for more information.

   New bug reports, feedback, questions or suggestions can be submitted
   to the issue tracker on Github:

     https://github.com/chocolate-doom/chocolate-doom/issues

 * Source code patches are welcome, but please follow the style
   guidelines - see the file named HACKING included with the source
   distribution.

 * Chocolate Doom is distributed under the GNU GPL. See the COPYING
   file for more information.

## DiabloDoom: Diablo-style loot and equipment

This fork adds a Diablo-inspired loot and equipment system to Chocolate Doom.

### Loot drops (Phase 0)

Every monster kill has a 70% chance to drop loot in 5 rarity tiers:
normal, magic (~40%), rare (~20%), set (~9%), unique (~6%).
Pick up drops by walking over them.

### Equipment backend (Phase 1)

All 42 loot items have data-driven definitions (src/doom/d_diablo.c) with
names, tiers, equipment slots, Diablo-style grid sizes, and stats.

Equipment slots: helm, armor, weapon, shield, two rings, amulet, boots,
gloves, belt.

The player has a 40-slot backpack. Picking up loot stashes the specific
item in the backpack (no more instant health/armor/ammo rewards). If the
backpack is full, the item stays on the ground and "Backpack full!" is
shown.

Temporary controls (Phase 1):
- **E**: Equip (or use, for potions) the most recently picked-up item.
- **Q**: Unequip everything.

Stat effects:
- **Weapon damage**: +random(dmg_min..dmg_max) of the equipped weapon,
  plus +1 per 4 strength, on every point of player damage.
- **Armor**: incoming damage reduced by `damage * armor / (armor + 50)`
  (50 armor halves hits), capped at 75% reduction.
- **Life steal**: heal a percentage of damage dealt to monsters.
- **Vitality**: +5 maximum health per point (soulsphere respects it).
- **Magic find**: rarity roll shifted down by `magic_find / 2`.
- **Move speed**: player thrust scaled by `(100 + move_speed) / 100`.
- Dexterity, energy, and elemental resists are aggregated for future use.

Limitations (Phase 1):
- No inventory grid UI, drag/drop, or item artwork yet.
- Equipment is not saved to savegames; it resets on load (kept on death).
- Potions are used from the backpack (E) rather than equipped.
