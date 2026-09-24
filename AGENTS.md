# AGENTS.md — DiabloDoom

> Working agreement for agentic contributors (human or AI) on this repo.
> The turn-based mode design lives in `SPEC.md` — read it before touching
> anything turn-related. This file is about how to work here, not what to
> build.

## What this is

DiabloDoom is a fork of **Chocolate Doom 3.1.1** (GPL) with a Diablo-style
loot and equipment system layered on top: tiered drops on every kill,
a grid backpack + paperdoll character screen, item icons, a stat screen,
and save/load persistence. Real-time Doom gameplay is untouched.

## Build & run

```sh
# from repo root
scripts/build.sh                      # cmake build into ./build
./build/src/chocolate-doom -iwad <path-to-iwad>
```

Test rig: Xvfb on `:96` (currently 1280x800). Capture the game window by
title with `~/workspace/doom/wshot.py` — it grabs the actual 800x600 game
window. Do **not** use the old `xshot.py` root capture: it grabs a fixed
640x400 region and will crop the status bar out of frame.

## Repo map

- `src/doom/d_diablo.c/.h` — item database, drop tables, player item state.
- `src/doom/d_diablo_ui.c/.h` — character screen (paperdoll, backpack,
  tooltips, drag-and-drop).
- `src/doom/d_diablo_icons.c/.h` — item icon art.
- `src/doom/st_stuff.c` — status bar. The classic 2–7 arms boxes were
  replaced with the equipped Diablo weapon panel (icon + damage range,
  `FISTS` when empty). See `ST_drawDiabloWeapon`. Planned: under the
  no-ammo rule, both ammo displays go away and the AMMO readout becomes
  a stats panel (character stats + live weapon state: attack speed,
  cooldown, charge/heat). Which stats TBD — see `SPEC.md`.
- `src/doom/p_inter.c` — pickup/drop, including Diablo loot drops.
- `DIABLO.md` — user-facing doc for the loot/equipment system (Phases 1–2
  era; update it when behavior changes).

## Standing rules

1. **Real-time mode is sacred.** Nothing in the turn-based work may change
   stock tic-path behavior. If a change risks real-time regression, it
   doesn't land.
2. **No scalar input, ever** (Jimmy's words: *"We shouldn't have actions
   that require scalar input because that will be too difficult to aim."*).
   Turn-mode actions are discrete: directions and actor IDs only. No
   aiming, no crosshair math, no ground targeting, no manual turning.
3. **Phase 0 needs Jimmy's explicit approval.** Do not start implementing
   turn-based mode until he says go.
4. **No ammo.** Every weapon effectively has infinite ammo; availability
   is governed by attack speed (TP cost), cooldowns, charge, and heat,
   League-style. No ammo pickups, counters, or reloading. "Ammo" survives
   only as a planned paperdoll inventory slot: equippable ammo items that
   are never consumed and passively grant stat bonuses to the equipped
   weapon (one shared slot vs per-weapon TBD).
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

W/S move, A/D strafe, arrows turn · C character screen · Tab
backpack/paperdoll · Enter/Space pick up/place · E equip · R/Backspace
cancel/use held consumable · Q unequip all · Esc close.
