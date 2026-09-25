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
scripts/build.sh                      # cmake build into ./build
./build/src/chocolate-doom -iwad <path-to-iwad>
```

Test rig: Xvfb on `:96` (currently 1280x800). Capture the game window by
title with `~/workspace/doom/wshot.py` — with `window_width 1280` /
`window_height 800` in chocolate-doom.cfg the game window is 1066x800
(aspect-corrected). Do **not** use the old `xshot.py` root capture: it grabs a fixed
640x400 region and will crop the status bar out of frame.

## Repo map

- `src/doom/d_diablo.c/.h` — item database, drop tables, player item state.
- `src/doom/d_diablo_ui.c/.h` — character screen (paperdoll, backpack,
  tooltips, drag-and-drop, POWERS panel below the backpack listing active
  MECH_* special mechanics from equipped gear; stat boosts stay in STATS).
- `src/doom/d_diablo_icons.c/.h` — item icon art.
- `src/doom/st_stuff.c` — status bar. The classic 2–7 arms boxes were
  replaced with the equipped Diablo weapon panel (icon + damage range,
  `FISTS` when empty). See `ST_drawDiabloWeapon`. In turn mode the right-side
  ammo counts are replaced by a LoL-style AD/AP stat block (gold sword icon
  + damage range, teal sparkle + ability power, cooldown below) — see
  `ST_drawTurnKits`. The left-side AMMO readout becomes the unified mana
  pool display (blue droplet + `MANA cur/max`) — see `ST_drawTurnMana`.
- `src/doom/p_inter.c` — pickup/drop, including Diablo loot drops.
- `DIABLO.md` — user-facing doc for the loot/equipment system (Phases 1–2
  era; update it when behavior changes).

## Standing rules

1. **The fork is turn-based by default.** There is no `-turnbased` flag
   and no real-time mode; the turn controller is unconditional. Legacy
   real-time tic paths are unreachable and slated for cleanup — don't
   resurrect them.
2. **No scalar input, ever** (Jimmy's words: *"We shouldn't have actions
   that require scalar input because that will be too difficult to aim."*).
   Turn-mode actions are discrete: directions and actor IDs only. No
   aiming, no crosshair math, no ground targeting. Manual view turning
   is allowed as free view control (arrow keys rotate 45 degrees, 0 TP)
   — it does not break the no-aiming contract because targeting is still
   numbered selection only.
3. **Phase 0 needs Jimmy's explicit approval.** Do not start implementing
   turn-based mode until he says go.
4. **Mana is ammo, renamed.** The engine ammo system is the unified mana
   pool (`player->ammo[am_clip]`): 100 max, starts at 50, backpack doubles
   the max to 200. Every ammo pickup funnels into mana, keeping its own
   `clipammo` amount (shard +10, crystal +50, ember +1, cache +5,
   vial +20, flask +100, orbs +4, orb box +20); at full mana pickups are
   left on the ground. The PULSE kit (`wp_plasma`) costs 5 mana per attack
   on top of TP — validated when the attack is queued, deducted when it
   executes. Mana Potions restore 50 mana (capped). "Ammo" also survives
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
Enter/Space pick up/place · E equip · R/Backspace cancel/use held
consumable · Q unequip all · Esc close.

Turn mode (queued actions, FIFO): W/S step forward/back, A/D strafe
(1 TP, never change facing) · left/right arrows turn view 45° (free,
immediate, never queued) · SPACE use (2 TP) ·
F attack via preview-confirm (TP by attack speed) · T end turn (drains
queue FIFO, then enemy phase) · Backspace undo last queued action
(free, refunds TP) · Z clear queue (free, refunds all TP) · C inventory
(free, always available in rounds) · Tab / [
/ ] / 1-9 target selection (free) · Esc cancel. Planning actions
enqueue and reserve TP; END TURN executes. There is no WAIT and no
HEADSHOT; crits come from gear, not dexterity.
