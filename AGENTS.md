# AGENTS.md — DiabloDoom

> Working agreement for agentic contributors (human or AI) on this repo.
> The turn-based mode design lives in `DIABLODOOM.md` — read it before touching
> anything turn-related. This file is about how to work here, not what to
> build.

## What this is

DiabloDoom is a fork of **Chocolate Doom 3.1.1** (GPL) with a Diablo-style
loot and equipment system layered on top: tiered drops on every kill,
a grid backpack + paperdoll character screen, item icons, a stat screen,
and save/load persistence. The fork is turn-based by default: discrete
Tempo turns, numbered targets, preview-confirm, no aiming — there is no
real-time mode and no `-turnbased` flag.

## Build & run

```sh
# from repo root
scripts/build.sh                      # cmake build into ./build (Debug by default)
scripts/build.sh --release            # optimized Release build
./build/src/chocolate-doom -iwad <path-to-iwad>
```

Test rig: Xvfb on `:96` (currently 1280x800). Capture the game window by
title with `~/workspace/doom/wshot.py` — with `window_width 1280` /
`window_height 800` in chocolate-doom.cfg the game window is 1066x800
(aspect-corrected). Do **not** use the old `xshot.py` root capture: it grabs a fixed
640x400 region and will crop the status bar out of frame.

## Status-bar face: the playing agent's avatar (reference image + generated assets)

SUPERSEDED 2026-10-23: the earlier design (decode the agent's .webp
at startup via vendored libwebp) is DEAD — the user killed it. The
.webp is a single static frame; it cannot generate expressions on
its own, and no expression variants exist in the avatar asset (its
animated variants are activity states — idle, working, making
something, milestone — not faces). So the engine does NOT read
.webp files. Instead:

- **Reference image:** the generating agent works from a
  front-facing head image of the user's avatar (default example:
  Jolly's face, the default Muse avatar). The repo does not
  currently carry the Jolly reference image itself — the shipped
  artifact is the built PWAD below.
- **Agent instructions:** `agent/MUSE.md` tells a Muse how to
  generate the face-state asset set from a reference image and wire
  it into the Doomguy status-bar face system (42 lumps: STFST look
  frames, STFST pain frames, STFGOD god mode, STFDEAD dead) — at
  35x31, the full face window, as PLAYPAL-quantized patches packed
  into a PWAD loaded with `-file`. The hi-res RGB overlay is
  future engine work, not a present mechanism (see MUSE.md). The
  shipped default is `agent/jolly.wad` (Jolly, smoke-tested in-game); first-time
  setup copies it to the gitignored `avatar/` slot and launches
  with `-file avatar/jolly.wad`.
- **Fallback:** no generated assets present → the vanilla Doomguy
  face, unchanged.

Expression work done 2026-10-23 (previews, not shipped assets):
Jolly at 24x29 in all six states (normal / look left / look right /
pain / god / dead), god and dead made exactly mirror-symmetric,
mouths drawn at final resolution (thin features get lost in the
downscale otherwise). Files: `~/workspace/jolly_expressions_24x29.png`,
`~/workspace/jolly_dead_24x29.png`. The 42-lump Jolly PWAD built
from these previews shipped 2026-10-28 (`agent/jolly.wad`,
guide `agent/MUSE.md`).

## VM automation driver

On Muse VMs (`D_OnMuseVM()` — `JARVIS_FQDN` contains `metaaivm.com`),
`T_Ticker()` polls `/tmp/doom_cmd.txt` every tick, executes one command per
line, then deletes the file. Commands include: `WALK` (persistent — keeps
walking until enemy, *reachable* loot (once per item: later walks ignore
an item WALK already stopped for, until it's gone), or blocked;
`WALK <angle>` turns by that relative angle first (-90..+90 degrees;
negative turns left, positive turns right) and walks forward a small
fixed distance (64 units) instead), `USE`
(use the door/switch in front of the player; explore mode), `WAIT`
(hold every later queued command — diagnostics included — for one
fixed 2-second beat while the world keeps ticking; stacks, capped at
10 s; exists so a door crossing can be one batch: `USE`, `WAIT`,
`GOTO`, with no vision round-trip inside the door's open window), `GOTO <x> <y>`
(absolute map coordinates — grid-BFS pathfind, Z-aware: a cell step up of more
than 24 units doesn't exist to the pathfinder, matching the engine's
own step limit, so targets on a drastically different height are
unreachable by construction; target clamped to nearest walkable
cell; grid conversion is floor division, so negative coordinates
register in the right cell; GOTO walks re-centre onto
the current cell when stalled off-centre across a face line, and
COLLECT rides on GOTO — it filters loot positions and hands them
over as destinations), `KEYS`, `SECTORS`
(visited-sector progress), `TURNL`/`TURNR`, `ATTACK`, `UNDO`, `END`, `COLLECT`,
`SCREENSHOT`, `CAPTURE`/`REPLAY` (15 s MP4; the encode runs on a
worker thread since 2026-10-19, so the game never stalls waiting for
x264). Launching with `--continuous-capture` turns on continuous capture:
every full 15-second buffer is queued for encode automatically and
saved as its own timestamped segment MP4, so a whole session lands
on disk without any CAPTURE command (the farm launches this way). A
driver-command LOAD restarts the capture: the in-progress segment is
dropped, so no segment ever straddles a load cut (2026-10-24) — only
driver loads need this, because a driver load shows no UI, so the cut
looks abrupt in the video; menu loads show the menu. `SAVE n`/`LOAD n` drive the save slots (0 = the autosave pair,
progress_auto1.dat + doomsav0.dsg; 3–5 = plain Doom savegames, mirroring
the load menu), and `QUIT` saves slot 0, flushes the log, and shuts the
game down cleanly from inside — the farm uses QUIT instead of killing
the process (SIGKILL discards the buffered log). QUIT skips the ENDOOM
exit screen, which otherwise waits forever for a keypress nobody sends.
Every executed driver command is also shown as an on-screen status message (`CMD <command>`, vanilla message pipeline, 4 s) so screenshots and replay segments show what the agent sent, not just the log. Screenshots come from the game's own framebuffer
(`SCREENSHOT` driver command), so no tearing — files land
timestamped as `diablodoom_<ts>_NN.png` under `/tmp/doomshots/doomshots/`
(that nested dir must exist; the game only creates `/tmp/doomshots`).
Replay MP4s land under `/tmp/doomshots/`. The farm driver is
`agent/doom_loop.sh`; it reads its instructions from
`agent/SYSTEM_PROMPT.md` (agent rules) and `agent/PROMPT.md`
(strategy).

## Repo map

- `src/diablo/` — the whole Diablo system, moved out of `src/doom/` 2026-10-23: the item/equipment backend, character screen, turn engine, progress saves, and replay capture. Vanilla Doom files with Diablo hooks stay in `src/doom/`.
- `src/diablo/d_diablo.c/.h` — item database, drop tables, player item state.
- `src/diablo/d_diablo_ui.c/.h` — character screen (paperdoll, backpack,
  tooltips, drag-and-drop, POWERS panel below the backpack listing active
  MECH_* special mechanics from equipped gear; stat boosts stay in STATS).
- `src/diablo/d_diablo_icons.c/.h` — item icon art.
- `src/diablo/d_pistol_sprite.c/.h` — the pistol's embedded patch (48x18, real transparency; the pistol has no WAD pickup sprite), generated by `assets/make_pistol_sprite.py`; drawn via `D_ItemBuiltinPatch` through the same patch path as WAD pickup sprites.
- `src/doom/st_stuff.c` — status bar. The classic 2–7 arms boxes were
  replaced with the equipped Diablo weapon panel (icon + damage range,
  `FISTS` when empty). See `ST_drawDiabloWeapon`. The panel background
  (STARMS) is scaled into the panel box (ST_WEPW x ST_HEIGHT) from the
  loaded WAD's lump size — never drawn at native width, since STARMS
  width varies by IWAD (Freedoom 38, Ultimate 40) and a native draw
  overflows into the face box. The ARMOR readout
  shows the total remaining durability of the equipped defense pieces
  (`D_EquippedDurabilitySum`), not vanilla armorpoints. In turn mode the right-side
  ammo counts are replaced by a LoL-style AD/AP stat block (gold sword icon
  + damage range, teal sparkle + ability power, cooldown below) — see
  `ST_drawTurnKits`. The left-side AMMO readout shows the ready weapon's
  current magazine counter (plasma's included — it counts up as the
  gun fires; the kit panel has no heat-capacity entry for plasma) —
  see `ST_drawTurnMana`.
- `src/doom/p_inter.c` — pickup/drop, including Diablo loot drops.
- `DIABLODOOM.md` — user-facing doc for the loot/equipment system (Phases 1–2
  era; update it when behavior changes).

## Standing rules

1. **Explore mode / turn based combat mode split.** The fork is NOT always turn-based.
   explore mode is free real-time movement (WASD + arrows via the normal
   Doom input path, `P_Ticker`); enemies are frozen (`P_MobjThinker`
   skips non-player `MF_COUNTKILL`). turn based combat mode triggers when an enemy AI
   **acquires the player as its target via line of sight** (`T_ExploreDetect()`, throttled
   to every 7 explore tics): sight within the monster's front 180-degree
   arc, or an already-acquired monster regaining LOS. **Hearing the player
   alerts but never starts combat** (2026-10-19): a monster that hears
   (sector `soundtarget`, vanilla `A_Look` hearing) takes the player as target and
   wakes, but combat waits for the LOS path — so a fight never starts through a wall
   with no visible target. The player merely seeing an enemy only drives the HUD
   markers. Leaving combat clears every monster's target and stale
   sector noise, so the next encounter starts clean. No visible combatants at round
   end returns to explore mode. Barrels never trigger combat but stay in the Tab
   target list. There is no `-turnbased` flag.
   Passive damage is a separate planning entry (2026-10-29): a health
   drop in explore mode enters mini-combat planning with no attack
   queued, no autosave, and no spotter message, damage-first after
   `P_Ticker()`; it pauses (does not cancel) a running GOTO/WALK,
   and cannot fire during planning or the progress phase.
2. **No scalar input, ever** (the user's words: *"We shouldn't have actions
   that require scalar input because that will be too difficult to aim."*).
   Turn-mode actions are discrete: directions and actor IDs only. No
   aiming, no crosshair math, no ground targeting. Manual view turning
   is allowed as free view control (arrow keys rotate 45 degrees, 0 TP)
   — it does not break the no-aiming contract because targeting is still
   numbered selection only.
3. **Phase 0 needs the user's explicit approval.** Do not start implementing
   turn-based mode until he says go.
4. **Ammo is per-weapon magazines; ammo is infinite (2026-11-07).**
   Every weapon with a magazine requires ammo to fire (`T_KitCanFire`,
   deny `NO AMMO`; inverted magazines deny `AMMO FULL`); costs validated
   when the attack is queued (`NEED n AMMO (HAVE m).`), applied when it
   executes (`T_KitSpendAmmo`).
   Ranged attacks cost 0 TP -- ammo is the limiter, so a round
   can dump the whole magazine. Melee (fists, chainsaw) costs TP per attack.
   Capacities/costs: pistol 10/1, shotgun 12/2, chaingun 10/1, chainsaw 6/1,
   rocket 20/10, SSG 8/8, BFG 20/20. Plasma's magazine is **inverted**
   (2026-10-22): its ammo counter represents heat — capacity 125, starts
   at 0, each shot adds 20, it fires while the count is below capacity,
   and the count drains at the replenish rate. There is no separate heat
   state anywhere: one counter, one gate, one spend, one rate system,
   with the direction declared in the kit def (`ammo_inverted`).
   There is no per-round ammo grant (removed 2026-10-22), and no
   over-time trickle (removed 2026-11-07): a normal magazine refills
   to full at once when its reload finishes — and the reload happens
   only when the magazine is empty (2026-11-07). Energy scales the
   capacity (base +1 per point of gear Energy, `T_KitMaxAmmo`), so
   Energy sets how many shots fit before the reload: `T_KitRefillAmmo`
   is called when the reload animation reaches the ready state
   (`P_MovePsprites` / `A_WeaponReady`), or immediately on the
   emptying shot for 0-cooldown weapons (whose reload animation is
   skipped). The reload animation's duration IS the weapon's cooldown
   in tics (2026-11-07: the kit's `cooldown` field is the number of
   tics, so the cooldown is the reload-speed knob -- tune the field
   to tune the reload). Attack/reload split (2026-11-07, weapon by
   weapon): the chain is split at the fire state -- attack frames
   (atkstate through the fire state) play at vanilla speed for now
   (attack speed is NOT Dexterity-derived; its driver is undefined
   until specified), reload frames (after the fire state) scale with
   the cooldown, so a reload shot's reload segment lasts exactly
   the cooldown while a normal shot's plays at vanilla speed.
   Shotgun exception (2026-11-07 rescind): the pump IS the reload,
   so it scales with cd on every shot, not just the emptying one.
   Current ladder (tics, 2026-11-07, vanilla reload-segment totals
   so every gun's reload plays at vanilla speed, 1:1 -- attack +
   reload equals the old chain total at base stats): chainsaw 9,
   chaingun 9, pistol 9, rocket 21, plasma 20, shotgun 34, BFG 20,
   SSG 52.
   Rocket charges are gone (2026-11-07): no charge gate; the
   magazine is the limiter (ammo_max 10, ammo_cost 10 = 1 shot
   per magazine).
   Death exits combat (2026-11-07): when the player dies, the turn
   controller calls T_EnterExplore() at the end of the progress phase
   and at the end of the enemy phase, so a death mid-pulse never
   leaves the game wedged in PLANNING at hp 0 with no respawn path.
   Dexterity tightens the spread (2026-11-07, option B): the shotgun
   cone (all 7 pellets), the chaingun cone (refire shots), and the
   pistol's refire spread scale by `100 / (100 + 2*Dex)` (effective
   Dex = 10 + gear), so the spread is quartered -- the sniper point
   -- at Dex 150. The pistol's first shot is accurate (no spread)
   and the SSG's own inline spread are untouched.
   Inverted magazines (plasma) still drain in `T_TickCadence`'s
   per-weapon loop on every unfrozen game tic, and rocket charges
   regen there too, each loading one attack's worth of ammo. Firing sets
   a weapon's cooldown counter (unified, 2026-11-01); the counter
   gates the plasma heat drain while it runs, but never blocks a
   shot. During planning the HUD AMMO readout previews
   the deduction queued attacks will cause (display-only; the spend happens
   on execute), so players can see when further attacks would be no-ops.
   Ammo pickups convert to Diablo loot: map-placed ammo is replaced
   by a random Diablo item drop at spawn (kill-drop rarity table).
   Monster drops are exclusively the Diablo mechanic
   (`P_DropDiabloLoot`, 70%) -- the vanilla drop switch is deleted,
   so a kill never spawns a raw ammo or weapon drop. No raw ammo
   pickup ever exists in the world; Mana Potions no longer appear in the drop
   pool. Weapon cadence is wall-clock,
   not round-bound: firing sets a weapon's cooldown counter to its
   effective cooldown converted to rounds (12 tics per round, rounded
   up), and cooldowns tick down 1 round per 10 s (BFG = 30 s, SSG =
   20 s, plasma = 10 s), rocket charges regen 1 per 3 s (max 3), plasma's inverted
   counter drains at its replenish rate (~43 per 10 s) continuously — all via `T_TickCadence` every game tic, except
   during the planning phase, where the cadence (like the magazine
   trickle) is frozen: nothing replenishes while the player plans. Haste is a
   cooldown-reduction percent (cap 50%, then flat CDR, min 1 tic). The HUD AMMO readout shows the
   ready weapon's magazine counter. \"Ammo\" also survives
   as the passive, non-consumable FOCUS paperdoll slot: equippable focus
   items (Piercing Rounds, Incendiary Shells, …) that passively grant stat
   bonuses to the equipped weapon.
5. **Screenshots use fresh timestamped filenames** — the client caches
   repeated names.
5. **One logical change per commit**, pushed to `master` on
   `github.com/thisisjimmyfb/DiabloDoom` only after it builds clean and is
   smoke-tested in-game.
6. **IP contingency is a backup plan only.** Do not rename items or the
   repo unless the user asks or a cease-and-desist arrives.
7. **Custom resources must degrade gracefully without their PWAD.**
   Any resource that does not exist in vanilla Doom (a custom sound
   slot, sprite, patch, flat, or any other lump) must be checked with
   `W_CheckNumForName` before use, and skipped or fallen back to a
   vanilla resource when absent. Never call `W_GetNumForName`,
   `S_StartSound`, or any other must-exist lookup on a possibly-absent
   custom lump: at startup a missing lump only precaches as -1, but at
   play time the lookup `I_Error`s out of the game. This applies to
   every custom resource, not just sounds. (Rule from the user,
   2026-11-04, after the DSZUGZUG turn-end sound shipped unguarded.)

## Combat affixes (magic tier)

Prefixes (flat) and suffixes (%) for AD/AP/haste/plasma-capacity/leech. See DIABLODOOM.md
"Combat affix families" for the full table.

- Suffixes: of Slaying (+15% AD), of the Magus (+15% AP), of Haste (+20%),
  of the Furnace (+30 plasma max ammo)
- Prefixes: Soldier's (+flat AD), Wizard's (+flat AP), Quick (−1 round),
  Vampiric (+lifesteal, all 8 guns)
- Also on rings/amulets/gloves/focus (e.g. Ring of Slaying, Cooling Rounds)
- Difficulty tiers: Nightmare and Hell drop stronger variants
  (`diff_tier` field; `D_DifficultyTier()` from gameskill + `d_loop`)
- Chainsaw: 30% inherent leech. SSG: natural 1-tile knockback.

## Essence (salvage / repair)

Diablo 3-style crafting loop on junk loot — see DIABLODOOM.md "Essence:
salvage and repair". Per-player currency `diablo_essence` on
`player_t`, persisted in savegames; the count shows in the inventory
header ("Essence: N" next to BACKPACK — SOUL_CAGE icon art is planned,
text display for now).

- **S**: salvage the held (or keyboard-selected backpack) item →
  Essence. Normal 1, Magic 3, Rare 8, Set 15, Unique 25.
- **R**: repair the held (or keyboard-selected) item to full
  durability for Essence, pro-rated by tier and missing durability
  (full-repair base: Normal 2, Magic 4, Rare 8, Set 12, Unique 20).
  (Use-held-consumable/cancel is Backspace or right-click.)

## NG+ loop

Beating MAP30 warps to MAP01 keeping gear. `d_loop` increments;
enemies get +50% HP / +25% damage per loop; +10% magic find per loop.
See DIABLODOOM.md "NG+ loop".

## Turn-based work

The full design — pillars, Tempo economy, weapon kits, selection combat,
engine architecture, persistence schema, Threads publishing, phased build
sequence with gates, and pre-implementation decisions — is in **`DIABLODOOM.md`** (the former SPEC.md, merged 2026-11-03).

Before changing turn-mode behavior:

- Read `DIABLODOOM.md`'s turn-based section end to end.
- The "Decisions to lock before implementation" table and the Phases 0–3
  prototype gates below were for the initial implementation, which has
  shipped; they are historical, not pending gates.

Key invariants from the spec: a central turn controller owns time, input,
and resolution (no scattered `if (turn_based)` checks); the standalone
character profile owns identity/progression while Doom saves own map/turn
state; the game exports immutable snapshots and never blocks on the
network.

## Controls (current, keyboard-first)

Diablo inventory: C character screen · Tab backpack/paperdoll ·
Enter/Space pick up/place · E equip · S salvage item into Essence ·
R repair item (Essence cost) · Backspace use held consumable / cancel
hold · Q unequip all · Esc close.

turn based combat mode (queued actions, FIFO — combat only, triggered by an enemy
AI acquiring the player): W/S step forward/back, A/D strafe
(1 TP, never change facing) · left/right arrows turn view 45° (free,
immediate, never queued) · SPACE use (free) ·
Hold CTRL to target (arrows navigate, camera snaps to target, release to fire; 0 TP ranged, TP by attack speed melee; in explore mode Ctrl starts mini-combat targeting for voluntary attacks on barrels/props/enemies) · Enter/T end turn (unfreeze into the progress phase: the queue drains FIFO in real time while the game runs — no enemy phase — then a fixed 18-tic settle, back to planning or explore; Enter still confirms the attack in CONFIRM state; T with an empty queue still runs a brief progress/settle phase) · Backspace undo last queued action
(free, refunds TP) · C inventory
(free, always available in rounds) · Tab / [
/ ] / 1-9 target selection (free) · G auto-loot nearest (turn mode only;
disabled in explore) · Esc cancel. Automap: Tab toggles the map in
explore mode; it hides the moment turn-based combat starts
(2026-10-31, inverted from the old behavior). Planning actions
enqueue and reserve TP; dying clears the queue and ends the progress
phase. There is no WAIT and no
HEADSHOT; crits come from gear, not dexterity.
