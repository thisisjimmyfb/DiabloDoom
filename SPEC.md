# DiabloDoom — Turn-Based Mode Spec

> Distilled from the September 2026 build plan. This is the source of truth
> for turn-based design; `AGENTS.md` points here. Iterate on this file
> directly — it is meant to be edited.

## Pitch

**First-person XCOM.** A turn-based looter-shooter inside DiabloDoom with
persistent Diablo-style character growth and a public Threads progression
history — without manual aiming.

Select a visible enemy, preview the odds, confirm the action. Every input
is discrete; every drop develops one named hero across levels and sessions.

## What stays / what changes

**Keep:** Doom maps, first-person presentation, the Diablo
inventory/equipment system.

**Add:** numbered targets, Tempo turns, XP levels, persistent loadouts,
tactical reactions.

**Prove first:** one complete map, six weapons, one named hero, one public
character history.

## Status bar Diablo-fication

The classic status bar is being converted to Diablo character readouts,
one panel at a time. The bar stays; only the panels change.

- **ARMS → equipped weapon panel** (done, `ee1ee623`). The classic 2–7
  weapon ownership boxes are retired. The ARMS section now shows only the
  weapon equipped in the Diablo inventory: item icon + aggregated damage
  range, or `FISTS` when the slot is empty. See `ST_drawDiabloWeapon` in
  `src/doom/st_stuff.c`.
- **AMMO → mana + kit panel** (done). The left ammo readout is now the
  unified mana pool display (blue droplet, `MANA cur/max`, `ST_drawTurnMana`
  in `src/doom/st_stuff.c`): 100 max, starts at 50, backpack doubles the
  max to 200. The right-side per-type ammo counters are replaced by the
  kit status panel: LoL-style AD/AP stat block (gold sword icon + damage
  range, teal sparkle + ability power, cooldown below), `ST_drawTurnKits`.

## Five design pillars

1. **Selection, not aiming.** Choose a numbered visible enemy, inspect the
   outcome, confirm. No free-aim shooting, manual turning, or
   crosshair-derived hit chance.
2. **Weapon identity.** AD, AP, attack speed, charge, heat, and cooldowns
   explain how a gun plays.
3. **Loot changes tactics.** Affixes alter costs, scaling, reactions, or
   cooldown timing — not just damage.
4. **One persistent hero.** Name, XP, attributes, equipment, inventory, and
   lifetime feats survive every session.
5. **Doom remains Doom.** First-person camera, maps, monsters, and
   sounds stay intact.

## Explicit non-goals (v1)

- No ground-target cursor; offensive actions always name an enemy.
- No true anatomical hitboxes; aimed choices modify the resolved attack.
- No squad turns, netplay, demo compatibility, or multiplayer sync.
- No automatic Threads bio edits (no API endpoint exists).
- No campaign-wide rebalance before the core loop proves itself.

## Hard input rule

**No action may require scalar, analog, cursor, or crosshair input.**
Turn mode accepts discrete choices only: directions and actor IDs.
Mouse position, stick angle, turn duration, and crosshair placement never
affect an action's target or hit chance. If the player ever needs to steer
a reticle, rotate manually, or choose a ground point, the input contract
has failed.

---

## Core loop

### Tempo

Each round begins with **10 Tempo Points (TP)**. ("Tempo" avoids colliding
with the League-style AP stat.) The player commits actions until TP is
exhausted or chooses End Turn. Cooldowns and status effects advance once
per round; each committed action resolves through a bounded simulation
pulse.

### Baseline action costs

| Action              | TP                          |
|---------------------|-----------------------------|
| Select / cycle target | 0                         |
| Facing-relative step (WASD) | 1                  |
| Use / interact     | 2                           |
| Attack selected enemy | Derived from attack speed |
| Undo last queued action (Backspace) | 0 (refunds TP) |
| Clear queue (Z)     | 0 (refunds all TP)          |
| Inventory / character screen (C) | 0 (free; owns input while open) |
| End turn            | 0 (always legal; listed last as the exit row) |

`attack_cost = clamp(2, 8, ceil(base_cost / attack_speed))`

### Turn flow (queued actions, FIFO execution)

1. **Acquire** — every line-of-sight enemy gets a stable number + list entry.
2. **Select** — cycle or press a number; inspect hit chance, damage, cost,
   HP, cover.
3. **Queue** — planning actions (move, use, attack) ENQUEUE and
   reserve TP immediately. The queue shows each entry with its cost and
   the total reserved TP. Unaffordable actions are dimmed and cannot be
   queued. Backspace removes the last entry (refunds TP); Z clears the
   whole queue (refunds all TP).
4. **Execute** — END TURN drains the queue FIFO: each entry runs with its
   normal settle behavior, then the enemy phase runs. An empty queue
   skips straight to the enemy phase.

**Queue rules:**
- Movement entries snapshot their world-space direction at queue time
  (e.g. STEP E); turning the view afterward does not change them.
- Attack entries snapshot the target actor; at execution the target is
  revalidated — if it died or became invalid, the attack skips gracefully,
  refunds its TP, and the drain continues.
- Blocked movement retains partial-progress behavior.
- Input locks while the queue drains.

**Resolution rule:** An attack auto-faces its snapshotted target at
execution; steps are facing-relative (W/S forward/back, A/D strafe) and
never change facing. The left/right arrows rotate the view 45 degrees
for free — that is view control, not aiming: targeting is still numbered
selection, so the no-aiming contract holds. View turning is immediate
and never queued. Selection, cancellation, undo, and clear are free;
TP is reserved at queue time, so browsing targets never changes world
state.

**Inventory rule:** opening the character screen costs no TP. Equipping or
consuming an item does. Grid organization stays free.

---

## Combat language

### Stats

| Stat   | Meaning              | Effect                                                  |
|--------|----------------------|---------------------------------------------------------|
| AD     | Attack Damage        | Scales ballistic, explosive, melee. Strength feeds AD.  |
| AP     | Ability Power        | Scales plasma, BFG, status, charged effects. Energy feeds AP. |
| AS     | Attack Speed         | Reduces TP attack cost / shots per burst; hard floors prevent zero-cost attacks. |
| Haste  | Cooldown recovery    | Shortens cooldowns by a displayed, rounded turn count.  |
| Crit   | Chance + damage      | Gear-inherent (Diablo-style): weapons/items grant crit chance and bonus crit damage. Base crits deal x2; each crit-damage point adds 1%. Chance caps at 50%. Crit is rolled exactly once per attack inside `T_ResolveAttack` (deterministic turn RNG, once-per-attack pellet semantics); the legacy `P_DamageMobj` crit hook is bypassed in turn mode so crits can never double-roll. |
| Armor  | Physical defense     | Diminishing-returns reduction of AD-tagged damage.      |
| MR     | Magic resistance     | Reduces AP-tagged plasma, occult, status damage.        |
| Move   | Movement efficiency  | Boots/effects can reduce step cost, never below 1 TP.   |

Damage: `damage = base + (AD × ratio_AD) + (AP × ratio_AP)` — most weapons
have one dominant scaling path; hybrid is a special build, not the default.
The preview shows the final damage range before firing.

Defense: `reduction = defense / (defense + K)`, K tuned in balance testing.

Tooltip order (ground loot, backpack, equipped — all identical):
damage range; AD/AP ratios; TP cost; attack speed; charge/heat state;
cooldown; crit; special trait.

### Level-up attributes (the existing four)

- **Strength** — AD, knockback resistance, heavy-weapon handling.
- **Dexterity** — hit chance, attack-speed TP cost, dodge.
- **Energy** — AP, charge efficiency, potion and status power.
- **Vitality** — max health, physical resilience, recovery.

---

## Weapon kits (vertical slice: six)

| Weapon   | Scaling | Mechanic | Identity |
|----------|---------|----------|----------|
| Pistol   | AD      | Tap      | Quickdraw: low TP cost, no cooldown. Consecutive shots on the same target gain accuracy. |
| Shotgun  | AD      | Burst    | Pump cycle: wide close-range burst around the selected enemy, then one-round recovery. Player never places the spread. |
| Chaingun | AD      | Heat     | Spin up: each burst raises heat and improves attack speed. Overheat locks firing one round; ending early preserves control. |
| Rocket   | Hybrid  | Cooldown | Siege shot: select an enemy, splash around it, no ground cursor. Charged variant: bigger radius + damage, then two-round cooldown. |
| Plasma   | AP      | Charge   | Capacitor: charge 1–3 TP; each level raises damage and armor penetration. Carrying charge across a round adds heat/decay. Costs 5 mana per attack (the PULSE kit). |
| BFG      | AP      | Ultimate | Annihilation: select an enemy, preview splash, commit a long charge + four-round cooldown. |

Chainsaw and super shotgun come after the slice. Cooldowns tick at round
end (recommended) with a one-round minimum; Haste shortens them.

**Mana is ammo, renamed.** The engine ammo system is the unified mana
pool (`player->ammo[am_clip]`): 100 max, starts at 50, backpack doubles
the max to 200. Every ammo pickup funnels into mana while keeping its own
`clipammo` amount, so a shard (+10) and a crystal (+50) restore different
amounts; at full mana pickups are left on the ground. Mana Potions restore
50 mana, capped by the max. The PULSE kit (`wp_plasma`) costs 5 mana per
attack on top of its TP cost — validated when the attack is queued
(`NEED 5 MANA (HAVE n).` on failure), deducted when it executes. Other
kits cost no mana. Availability is still governed by attack speed (TP
cost), cooldowns, charge, and heat, League-style; mana is the one
spendable resource on top.

**FOCUS slot.** "Ammo" survives as a paperdoll inventory slot renamed
FOCUS: equippable focus items (Piercing Rounds, Incendiary Shells, Swift
Cartridges, Spotter Rounds) that are never consumed and passively grant
stat bonuses to the equipped weapon. One shared slot; layout unchanged,
no profile-version bump.

**Loot rule:** common affixes bend numbers; rare/unique traits bend
mechanics (e.g. a unique plasma rifle preserves charge between rounds).

---

## Selection combat

### Target acquisition

Every living enemy in line of sight gets a **stable number** for the
current planning state, a screen-space marker, and a list entry (name, hit
chance, distance, HP). If markers overlap or leave the viewport, the list
is authoritative. Sorting is deterministic: threat, then distance, then
actor ID. The selected target's marker is bracketed (`>[N]<`) and its list
row highlighted; markers are projected with the engine's own sprite math
so each number sits above its enemy's head.

### Visual language

Turn-system chrome renders in **gold**: the TURN MODE header, TP readout,
action list, and help line. **Red stays Diablo's color** — inventory
screens, item rarity accents, and Diablo stat readouts. Actions the player
cannot afford render **dimmed gold** with their TP cost shown and cannot be
selected. END TURN is always the final action-list row, visually separated
as the exit (`>>T END TURN 0TP<<`).

### Select → preview → confirm (enqueues)

- `Tab` / `]` next target, `[` previous, number keys select directly.
- ATTACK shows hit chance, damage, TP cost, range, cover, target HP.
- Confirm enqueues the attack and reserves its TP; it executes FIFO on
  END TURN, auto-facing the snapshotted target. Cancel returns to
  selection for free. Selection never advances time.
- If the enemy becomes invalid before commitment, the action cancels
  without reserving TP. If it becomes invalid before execution, the
  queued attack skips gracefully and refunds its TP.

### Enemy-targeted abilities

Charged rockets and the BFG ultimate use the same contract as ATTACK:
choose an enemy, preview the radius around that actor, confirm. There is
no world cursor or ground coordinate.

### Cover and telegraphs

- **Cover:** multi-point traces apply an accuracy penalty; full blockage
  removes the enemy from legal attack targets.

**Fairness:** melee, hitscan, and projectile enemies keep recognizable
behavior and visible telegraphs. Newly alerted enemies show a state change
before attacking; unseen enemies get no free shots.

---

## Engine plan

One rules layer owns time, input, and resolution. Do **not** scatter
`if (turn_based)` checks through every weapon and monster.

| Layer             | Owns                                              | Guard                    |
|-------------------|---------------------------------------------------|--------------------------|
| Input adapter     | Keyboard tokens → typed actions: MOVE(dir), SELECT(id), ATTACK(id, mode), ABILITY(id), USE, POTION, END | Discrete values only |
| Target service    | LOS set, stable numbers, markers, sorted list, actor-ID validation at commit | No crosshair query |
| Turn controller   | Phase, TP, round, selected actor, queued action, charge, cooldowns, "stable enough for next input" | Single-player only |
| Action resolver   | Cost/preview, derived facing, revalidation, bounded pulse | Deterministic seed path |
| Gameplay bridge   | Approved movement/attack commands into existing player, weapon, projectile, thinker systems | Real-time path unchanged |
| Presentation      | HUD markers, target list, preview, weapon state, turn log, audio — reads controller state | No rules in UI |
| Persistence       | Doom saves own map + turn state; separate versioned character profile owns identity/progression | Atomic write + clean migration |

**State machine:** PLANNING → TARGETING → CONFIRM → PULSE → (REACTION) →
ROUND END. Targeting and confirmation are reversible; TP is spent only
when a valid action enters PULSE.

**Compatibility:** real-time play bypasses the controller via the stock
tic path. Turn mode disables demo recording/playback and netplay with a
clear message.

**Core invariant:** no turn-mode action reads mouse position, analog
magnitude, crosshair overlap, or a manually chosen world coordinate.

---

## Persistent progression

### Standalone character profile

Separate from Doom saves, versioned, loaded at startup and saved
atomically on every level exit and normal quit. A failed write keeps the
previous valid profile and shows a clear error.

```
profile_version
name
level, current_xp, unspent_points
base_stats {str, dex, energy, vit}
allocated_points {str, dex, energy, vit}
equipment[10]
backpack[]
session_count, total_kills, uniques_found
```

Item instances reuse the Phase 4 equipment/backpack serialization
(unique IDs, grid positions, rolled affixes, durability/charges).

**Separation rule:** loading a Doom save may restore map position and
turn state but must never silently replace the newer standalone
character. Profile reconciliation is explicit, versioned, and tested.

### XP and levels

- Each monster definition supplies a tier and base XP; higher tiers award
  more.
- A monotonic, data-driven `level_thresholds[]` table defines cumulative
  XP requirements.
- Crossing multiple thresholds grants every missed level and its points in
  one transaction.
- Each level grants allocatable points for Strength / Dexterity / Energy /
  Vitality; derived AD, AP, accuracy, crit, health, and defenses recalculate
  immediately.

```
new_xp = current_xp + monster_base_xp[tier]
while new_xp >= level_thresholds[level + 1]:
    level_up()
```

---

## Public character sheet (Threads)

The Threads timeline becomes the hero's history. Posts are automatic;
profile-bio edits are not.

- **First session:** chat vote to name the shared hero. Start unnamed,
  collect allowlisted candidates, run one bounded vote, sanitize the
  winner, lock it before the first combat session is published. The name
  is stored in the profile and used in posts, screenshots, save
  diagnostics, and the generated bio. Renaming requires an explicit later
  vote.
- **Publish triggers:** session end, level-up, unique drop. Write a
  character-sheet snapshot and queue one post (hero name, level, equipped
  weapon, armor summary, total kills). Aggregate same-action events into
  one snapshot/post. Event IDs prevent duplicate publication.
- **Post payload:** `event_id, event_type, timestamp, name, level,
  current_xp, weapon, armor_summary, total_kills, uniques_found,
  session_number, screenshot_path`. The game exports the package; a
  separate publisher formats and posts it. Combat never waits on the
  network.
- **Bio:** after each session the game writes `threads_bio.txt` (UTF-8,
  ≤150 chars, deterministic field-shortening order). Updating the live
  bio is a manual paste — Threads exposes no bio-edit endpoint.

---

## Build sequence

Every phase ends in a playable gate. No phase begins by assuming the
previous one "mostly works."

### Foundation

| Phase | Work | Gate |
|-------|------|------|
| 0 | Baseline. Turn controller wired as the unconditional default — no flag, no real-time mode. Capture Doom save behavior and the existing rebirth gear-preservation path. | Inventory, save/load, death, level transitions unchanged. |
| 1 | Turn kernel + derived movement. PLANNING/PULSE states, bounded simulation, round counter, discrete facing-relative MOVE (W/S step, A/D strafe, arrows turn view free). | Navigate and interact via MOVE, USE, END — no analog input; view turning is a free arrow-key action, not aiming. |
| 2 | Tempo economy. 10 TP, action costs, free target browsing, legal-action checks, round refresh. | No action overspends TP; cycling/canceling spend zero; save/load restores decision state. |
| 3 | Target service + ATTACK contract. Visible-enemy enumeration, numbered markers, target list, Tab/`[`/`]`/number keys, auto-face, preview, confirm. | Every legal enemy has one stable number; ATTACK always resolves against the confirmed actor ID. |
| 4 | Combat stats + hit preview. AD, AP, AS, Haste, crit, Armor, MR, range, cover, derived effects from the four attributes. | Fixed-seed tests reproduce displayed hit, damage, TP, cooldown, health, defense. |

**Checkpoint after 4:** clear the same room by keyboard and by scripted
actions. If the player ever needs a reticle, manual rotation, or a ground
point, the input contract has failed.

### Tactics, identity, social

| Phase | Work | Gate |
|-------|------|------|
| 5 | Weapon kits + enemy-targeted abilities. All six kits; rockets/BFG select an enemy and preview splash around it. | All six kits distinct; no attack or ability accepts a ground point or crosshair target. |
| 6 | Cover and telegraphs. Multi-point cover traces, telegraphs. | Full cover blocks attacks; newly alerted enemies telegraph before acting. |
| 7 | Tactical loot pass. Affixes for AD/AP ratios, AS, Haste, charge, heat, move thresholds; selected unique traits. | Tooltips explain every change; equip/unequip reverses state exactly. |

**Combat checkpoint:** finish the complete one-map tactical loop before
persistence begins. The same fixed-seed encounter must support distinct
AD, AP, and defensive builds.

### Progression and publishing

| Phase | Work | Gate |
|-------|------|------|
| 8 | Persistent profile, XP, levels. Reuse Phase 4 item serialization; startup load; level-exit/quit saves; tier XP; thresholds; stat points; lifetime counters; naming state. | Relaunch preserves the full profile; death/map transitions preserve gear; threshold edges and multi-level awards pass. |
| 9 | Encounter balance. Target readability, telegraphs, projectile speed, melee threat, cooldown cadence, tier XP, drops, one full map. | Map supports multiple builds; crowded fights stay readable; XP pacing has no dead/runaway bands. |
| 10 | Social identity + publication. Name vote, structured action logs, snapshots, character-sheet posts, event dedup, screenshots, 150-char bio generator. | Session-end, level-up, unique events yield one correct post; bio always fits; live bio editing remains manual. |

### Required test harnesses

- **Fixed-seed arena** — same monsters, positions, items, RNG every run.
- **Action replay** — text action scripts drive the controller without
  mouse events.
- **State dump** — round, TP, health, effects, cooldowns, inventory after
  each action.

### Quality gates (every phase)

- No action requires scalar, analog, cursor, or crosshair input.
- Doom saves and the standalone profile round-trip their own state
  without overwriting each other.
- Displayed values match resolved values; no action leaves the controller
  stuck; fixed-seed replays stay deterministic.
- Post snapshots match the committed profile; bio output ≤150 chars.

---

## Release target (v1.0)

The smallest complete tactical loop: single-player turn-based
mode with Tab/`[`/`]`/number selection, preview, confirm, auto-facing; six
enemy-targeted weapon kits; 10-TP rounds with discrete controls only;
numbered markers + target list (name, hit, distance, HP); AD/AP/AS/Haste/
crit/Armor/MR + four allocatable attributes; cover,
tactical affixes, selected uniques; tier-scaled XP, thresholds,
stat points, one rebalanced map; standalone profile; first-session name
vote, character-sheet posts, manual-paste bio text.

**Success test:** a new player selects any visible enemy, understands the
weapon state, and predicts the cost and likely result before confirming —
never needing to aim, rotate, or place a cursor. After the session,
quit + relaunch restores the same named hero and loadout; the exported
character sheet matches the profile; the bio fits 150 characters.

**Three proof encounters:** hallway breach (cover, telegraphed hitscan) · projectile arena (movement pulses, rockets in
flight, charge timing, heat) · boss room (cooldown planning, BFG
commitment, build identity).

**Post-slice order:** chainsaw + super shotgun, wider unique pool, more
maps, then expanded command grammar and public-session cadence.

---

## Decisions to lock before implementation

| Decision | Recommendation / rule |
|----------|----------------------|
| Pulse boundary | Define exactly which weapon, projectile, monster, and world thinkers advance per action pulse. |
| Movement unit | Short fixed-distance, collision-aware step. WASD is facing-relative at commit (W/S step, A/D strafe); the view never auto-faces. |
| Round timing | Cooldowns tick at round end (recommended) for clarity. |
| Target identity | Display numbers stable while planning state is unchanged; resolve through actor IDs; rebuild + visibly renumber only after world state changes. |
| Failure handling | Action invalid after commitment → refund TP, return to planning, unless world state already changed. |
| Profile authority | Standalone profile owns identity, progression, loadout, backpack, counters; Doom saves own map + turn state. |
| Publishing boundary | Game exports immutable snapshots, never blocks on Threads; separate publisher dedups event IDs and records success/failure. |

Freeze these before content tuning. Any later change to profile ownership,
event identity, or threshold semantics requires a version bump + migration
test.

## Risks

| Risk | Containment |
|------|-------------|
| Tick coupling → freezes / double actions | Central controller, fixed-seed replays, explicit stable-state checks. |
| Too many stats obscure decisions | Progressive HUD, consistent tooltip order, inspectable math. |
| Original maps become unfair | Balance one map first; telegraph hitscan; tune activation. |
| Target numbers churn / markers collide | Stable actor IDs, deterministic renumbering, list-first fallback, overlap-aware marker offsets. |
| Profile corruption / save conflict | Version field, validation, atomic temp-file rename, previous-good backup, explicit ownership per save domain. |
| Duplicate / stale public updates | Immutable snapshots, unique event IDs, durable publish status, same-action aggregation. |
| Bio automation mistaken for supported | Generate text only; label manual paste clearly. |

**Recommended next move:** prototype Phases 0–3 on a branch. Go/no-go:
one room cleared through MOVE and ATTACK-style commands only, with stable
numbering, preview, confirmation, and derived facing. Continue if the
pulse is deterministic and every enemy is selectable without scalar
input; rework if target identity drifts, markers go ambiguous, camera
steering affects accuracy, saves resume wrong, or real-time regresses.
