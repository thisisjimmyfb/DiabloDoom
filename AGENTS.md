# AGENTS.md — DiabloDoom

> Working agreement for agentic contributors (human or AI) on this repo.
> The turn-based mode design lives in `SPEC.md` — read it before touching
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

## Status-bar face: the playing agent's avatar (webp integration)

The status-bar face is not a baked asset: it is the avatar of the
Muse agent currently playing, decoded from its .webp at startup
(vendored libwebp) and generated into the face patches. Any Muse
agent who runs this fork gets *their* face — the decoder is the
feature, the avatar is data.

**How the avatar is chosen (heuristic):** at startup the game
scans `~/workspace/avatars/`, ignores `avatar-options-*` files
(candidates that were shown but never picked), and takes the
newest remaining `.webp`. That is the active avatar in practice
(the options exclusion is what makes it work — unpicked options
are often newer than the live avatar). The picked file is logged
at startup, so a wrong pick is visible immediately.

**How to change the integrated avatar:** in Muse, create or set
a new avatar (or edit the current one) — the new file becomes
the newest non-option .webp — then relaunch the game; the face
updates on the next launch. To force a specific image regardless
of timestamps, set `DOOM_AVATAR_WEBP=/path/to/avatar.webp`
before launching (escape hatch; overrides the scan).

**How it is rendered:** two draws. The framebuffer gets a
PLAYPAL-quantized patch (this is what screenshots capture, so
the farm's vision loop sees a correct status bar). At present
time the decoded RGB image is blitted over the face rect at
window resolution — the hi-res version. The overlay is not
optional garnish: pale avatars (e.g. Jolly's creams) map badly
to PLAYPAL and only read correctly in RGB.

**Cache:** the generated sprite is saved under
`generated/avatar_face.patch` in the repo dir and regenerated
only when the source .webp's mtime changes, so later launches
are instant. It is agent-specific and gitignored — never commit
an agent's avatar into the repo.

**Fallback:** no avatar found (or decode fails) → the vanilla
Doomguy face, unchanged.

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
over as destinations), `KEYS`, `SCAN` (nearby keys by absolute coordinates), `SECTORS`
(visited-sector progress), `TURNL`/`TURNR`, `ATTACK`, `UNDO`, `END`, `COLLECT`,
`SCREENSHOT`, `CAPTURE`/`REPLAY` (15 s MP4; the encode runs on a
worker thread since 2026-10-19, so the game never stalls waiting for
x264). Launching with `--continuous-capture` turns on continuous capture:
every full 15-second buffer is queued for encode automatically and
saved as its own timestamped segment MP4, so a whole session lands
on disk without any CAPTURE command (the farm launches this way). `SAVE n`/`LOAD n` drive the save slots (0 = the autosave pair,
progress_auto1.dat + doomsav0.dsg; 3–5 = plain Doom savegames, mirroring
the load menu), and `QUIT` saves slot 0, flushes the log, and shuts the
game down cleanly from inside — the farm uses QUIT instead of killing
the process (SIGKILL discards the buffered log). QUIT skips the ENDOOM
exit screen, which otherwise waits forever for a keypress nobody sends.
Screenshots come from the game's own framebuffer
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
  `FISTS` when empty). See `ST_drawDiabloWeapon`. The ARMOR readout
  shows the total remaining durability of the equipped defense pieces
  (`D_EquippedDurabilitySum`), not vanilla armorpoints. In turn mode the right-side
  ammo counts are replaced by a LoL-style AD/AP stat block (gold sword icon
  + damage range, teal sparkle + ability power, cooldown below) — see
  `ST_drawTurnKits`. The left-side AMMO readout shows the ready weapon's
  current magazine counter (plasma's included — it counts up as the
  gun fires; the kit panel has no heat-capacity entry for plasma) —
  see `ST_drawTurnMana`.
- `src/doom/p_inter.c` — pickup/drop, including Diablo loot drops.
- `DIABLO.md` — user-facing doc for the loot/equipment system (Phases 1–2
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
2. **No scalar input, ever** (Jimmy's words: *"We shouldn't have actions
   that require scalar input because that will be too difficult to aim."*).
   Turn-mode actions are discrete: directions and actor IDs only. No
   aiming, no crosshair math, no ground targeting. Manual view turning
   is allowed as free view control (arrow keys rotate 45 degrees, 0 TP)
   — it does not break the no-aiming contract because targeting is still
   numbered selection only.
3. **Phase 0 needs Jimmy's explicit approval.** Do not start implementing
   turn-based mode until he says go.
4. **Ammo is per-weapon magazines, auto-replenished by AD/AP type.**
   Every weapon with a magazine requires ammo to fire (`T_KitCanFire`,
   deny `NO AMMO`; inverted magazines deny `AMMO FULL`); costs validated
   when the attack is queued (`NEED n AMMO (HAVE m).`), applied when it
   executes (`T_KitSpendAmmo`).
   Ranged attacks cost 0 TP -- ammo is the limiter, so a round
   can dump the whole magazine. Melee (fists, chainsaw) costs TP per attack.
   Capacities/costs: pistol 10/1, shotgun 20/2, chaingun 10/1, chainsaw 6/1,
   rocket 20/10, SSG 8/8, BFG 20/20. Plasma's magazine is **inverted**
   (2026-10-22): its ammo counter represents heat — capacity 125, starts
   at 0, each shot adds 20, it fires while the count is below capacity,
   and the count drains at the replenish rate. There is no separate heat
   state anywhere: one counter, one gate, one spend, one rate system,
   with the direction declared in the kit def (`ammo_inverted`).
   There is no per-round ammo grant (removed 2026-10-22). ONE code
   path replenishes every gun (2026-10-23): `T_TickCadence`'s
   per-weapon loop moves every magazine at its kit's replenish rate
   on every unfrozen game tic — normal magazines trickle UP +1 per
   interval for all weapons (1 per `tp_cost` × 2 tics, clamped
   3–20, for attack-speed weapons; 1 per effective `cooldown` × 12
   tics for cooldown-driven ones: SSG 24, BFG 36), inverted
   magazines drain DOWN in the same loop, and rocket charges regen
   there too, each loading one attack's worth of ammo. There is no
   refill-on-cooldown-complete (`T_RefillOnCooldownReady` is a
   no-op) and no second regen path in the pulse simulator — until
   2026-10-23 the trickle-up ran only inside `T_PulseOneTic`, so
   explore-heavy play never refilled anything. During planning the HUD AMMO readout previews
   the deduction queued attacks will cause (display-only; the spend happens
   on execute), so players can see when further attacks would be no-ops.
   Ammo pickups convert to Diablo loot: map-placed ammo is replaced
   by a random Diablo item drop at spawn (kill-drop rarity table).
   Monster drops are exclusively the Diablo mechanic
   (`P_DropDiabloLoot`, 70%) -- the vanilla drop switch is deleted,
   so a kill never spawns a raw ammo or weapon drop. No raw ammo
   pickup ever exists in the world; Mana Potions no longer appear in the drop
   pool. Weapon cadence is wall-clock,
   not round-bound: cooldowns tick down 1 round per 10 s (BFG = 30 s, SSG =
   20 s), rocket charges regen 1 per 3 s (max 3), plasma's inverted
   counter drains at its replenish rate (~43 per 10 s) continuously — all via `T_TickCadence` every game tic, except
   during the planning phase, where the cadence (like the magazine
   trickle) is frozen: nothing replenishes while the player plans. Haste is a
   cooldown-reduction percent (cap 50%, then flat CDR, min 1 round). The HUD AMMO readout shows the
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
   repo unless Jimmy asks or a cease-and-desist arrives.

## Combat affixes (magic tier)

Prefixes (flat) and suffixes (%) for AD/AP/haste/plasma-capacity/leech. See SPEC.md
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

Diablo 3-style crafting loop on junk loot — see SPEC.md "Essence:
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
See SPEC.md "NG+ loop".

## Turn-based work

The full design — pillars, Tempo economy, weapon kits, selection combat,
engine architecture, persistence schema, Threads publishing, phased build
sequence with gates, and pre-implementation decisions — is in **`SPEC.md`**.

Before writing any turn-mode code:

- Read `SPEC.md` end to end.
- Lock the "Decisions to lock before implementation" table with Jimmy.
- Prototype Phases 0–3 on a branch; the go/no-go is one room cleared via
  MOVE/ATTACK-style commands only.

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
disabled in explore) · Esc cancel. Planning actions
enqueue and reserve TP; dying clears the queue and ends the progress
phase. There is no WAIT and no
HEADSHOT; crits come from gear, not dexterity.
