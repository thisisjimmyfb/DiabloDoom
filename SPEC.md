# DiabloDoom — Turn Based Combat Mode Spec

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

**Add:** numbered targets, Tempo turns, equipment-based progression,
persistent loadouts, tactical reactions.

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
- **AMMO → classic restored** (done). The left ammo readout is back to
  classic Doom: baked-in `AMMO` label with big red numbers showing the
  unified ammo pool (`player->ammo[am_clip]`), via the `w_ready` widget
  in `src/doom/st_stuff.c`. No custom text rendering.
- **Right ammo counts → AD/AP/CD panel** (done). The four per-type ammo
  counters (and their `BULL`/`SHELL`/`RCKT`/`CELL` labels, painted over)
  are replaced by a stat block, `ST_drawTurnKits`: damage (`AD x-y` for
  AD guns, `AP x` for AP guns — never both), cooldown (`CD x`), and the
  weapon's gate (`HEAT x` for plasma, `CHG x/y` for rocket). Text is
  single-pass (no shadow) for crispness.

## Five design pillars

1. **Selection, not aiming.** Choose a numbered visible enemy, inspect the
   outcome, confirm. No free-aim shooting, manual turning, or
   crosshair-derived hit chance.
2. **Weapon identity.** AD, AP, attack speed, charge, ammo, and cooldowns
   explain how a gun plays.
3. **Loot changes tactics.** Affixes alter costs, scaling, reactions, or
   cooldown timing — not just damage.
4. **One persistent hero.** Name, equipment, inventory, and
   lifetime feats survive every session.
5. **Doom remains Doom.** First-person camera, maps, monsters, and
   sounds stay intact.

## Explicit non-goals (v1)

- No ground-target cursor; offensive actions always name an enemy.
- No true anatomical hitboxes; aimed choices modify the resolved attack.
- No squad turns, netplay, demo compatibility, or multiplayer sync.
- No automatic Threads bio edits (no API endpoint exists).
- No campaign-wide rebalance before the core loop proves itself.

## Difficulties

Three Diablo-style difficulties, replacing Doom's five skill levels:

| Menu | Internal skill | Behavior |
|------|---------------|----------|
| NORMAL | `sk_medium` | Standard. |
| NIGHTMARE | `sk_hard` | Harder monsters. |
| HELL | `sk_nightmare` | Fast, respawning monsters (keeps the confirm prompt). |

The New Game menu shows text labels (`M_DrawNewGame` in
`src/doom/m_menu.c`); `-skill 1/2/3` maps to the same three levels.

## Hard input rule

**No action may require scalar, analog, cursor, or crosshair input.**
Turn mode accepts discrete choices only: directions and actor IDs.
Mouse position, stick angle, turn duration, and crosshair placement never
affect an action's target or hit chance. If the player ever needs to steer
a reticle, rotate manually, or choose a ground point, the input contract
has failed.

---

## Explore mode / turn based combat mode split

The game is **not** always turn-based. Two modes:

**explore mode** — free real-time movement (WASD + arrows, Chocolate Doom's
default bindings). The normal Doom ticker runs; enemies are frozen solid
(`P_MobjThinker` skips non-player `MF_COUNTKILL` mobjs). Barrels, loot,
doors, and the world tick normally. Walking over loot picks it up via the
standard touch path. The HUD shows a minimal `EXPLORE` header; no TP, no
queue, no action list. G (auto-loot pathing) is disabled in explore mode —
walk to the loot instead. F/Tab targeting is disabled: there is nothing
to fight.

**turn based combat mode** — exactly as described in Core loop below.
Triggered the moment an enemy AI **acquires the player as its target via
line of sight** (`T_ExploreDetect()`): sight within the monster's front
180-degree arc, or an already-acquired monster regaining LOS. The player
merely *seeing* an enemy only drives the HUD target markers; it never
starts combat. The HUD flips to `TURN MODE - ROUND 1` with a "X spots
you!" callout, and the player plans with 10 TP. Enemies that acquired
the player but are not visible get an "X is hunting you!" telegraph
before the first progress phase. When the queue drains with no
combatants left, the game returns to EXPLORE — and every monster
forgets the player (targets and stale gunshot noise are cleared), so
the next encounter starts from a clean detection state.

**Hearing alerts but never starts combat** (changed 2026-10-19): a
monster that hears the player (sector `soundtarget`, vanilla `A_Look`
hearing — no sight check) is alerted exactly as before — it takes the
player as its target and plays its wake sound (`[TURN] detect: X
heard the player`). Explore-mode monsters stay frozen, so it does not
move until a fight starts — but it *begins* that fight already
targeting the player. Combat itself only begins when acquisition
happens through the LOS path: the player comes into the alerted
monster's view and the already-acquired branch triggers the fight
with a target the player can actually see. Before this change,
hearing alone started combat through walls, and the player faced an
empty target list while the fight re-triggered in a loop.

**What counts as a combatant:** `T_IsCombatant()` — the mobj's type has a
melee or missile attack state in `mobjinfo`. Barrels and other shootable
props with no attack are **not** combatants: they never trigger combat,
never get numbered markers, never telegraph. They remain in the Tab
target list so the player can still shoot them deliberately.

**Voluntary fights:** hold Ctrl in explore mode to enter mini-combat
targeting — a single-turn attack on a barrel, prop, or enemy without
starting a full fight. Releasing Ctrl with no target fires in the facing
direction (for shootable walls/secrets that aren't targetable mobjs).

**Design intent:** the Diablo farm loop (walk room to room, grab loot,
build the character) stays frictionless; the XCOM tactical layer appears
only when an enemy spots the player. Combat entry is on acquisition,
combat exit is on clear (no combatants left when the queue drains).

---

## Core loop

### Tempo

Each round begins with **10 Tempo Points (TP)**. ("Tempo" avoids colliding
with the League-style AP stat.) The player commits actions until TP is
exhausted or chooses End Turn. Status effects advance once per round;
weapon cooldowns, charges, and inverted-ammo drain tick on wall-clock time instead —
every game tic via `T_TickCadence`, independent of round boundaries
(1 round = 10 seconds = 350 tics). Each committed action resolves
through a bounded simulation pulse.

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
4. **Progress** — Enter (or T) unfreezes the world into the *progress
   phase*: the queue drains FIFO in real time while the game runs —
   enemies move, shoot, and die around the player. There is no enemy
   phase; the player's queued commands simply play out inside the
   running game. The queue panel stays visible, entries popping off as
   they execute. When the queue is empty, a fixed 0.5 s (18-tic) settle
   runs, then the game returns to planning — or to explore, if no
   combatants remain. END TURN still teleports the camera back to the
   turn start first. T with an empty queue still runs a brief
   progress/settle phase, so it always advances the game. Dying clears
   the queue immediately and ends the phase; the death flow takes over.
5. **Preview** — during planning, the first-person camera follows queued
   moves (no game logic, just the view), so the player sees where the
   queue goes before committing. UNDO steps the camera back; Z clears
   the queue and resets the camera to the turn start.

**Queue rules:**
- Movement entries snapshot their world-space direction at queue time
  (e.g. STEP E); turning the view afterward does not change them.
- Attack entries snapshot the target actor; at execution the target is
  revalidated — if it died or became invalid, the attack skips gracefully,
  refunds its TP, and the drain continues.
- Movement uses `P_XYMovement` (the normal game movement code) with wall
  sliding (`P_SlideMove`), matching explore-mode behavior. Blocked moves
  slide along walls instead of stopping dead. `P_ZMovement` runs after
  each turn-based step so stairs climb the same as explore mode.
- **Step distance is 24 units × the movespeed multiplier**, and steps
  **animate smoothly** over the pulse at keyboard run speed instead of
  teleporting: the preview
  landing spot and the executed landing spot are the same computed
  target. Preview z-snaps at most one step (24 units); larger drops
  fall via gravity. (Movement rework, 2026-10-19.)
- Every movement path — turn steps, GOTO, WALK, and keyboard thrust —
  takes its displacement from the one shared scaling function
  `D_ScaleDisplacement` (d_diablo.c), so the movespeed modifier means
  the same thing everywhere; step duration scales by the same factor
  inverted (`D_ScaleMoveTics`). (WALK ignored the modifier until
  2026-10-22, when the scaling was unified.)
- GOTO movement is momentum-based (2026-10-22): the walker thrusts
  with the keyboard's own `P_Thrust` (run increment
  `forwardmove[1] * 2048`, scaled inside P_Thrust) and ground
  friction shapes the velocity — it spools up over ~10 tics, cruises
  at the keyboard's ~16.7 u/tic terminal, and brakes by thrust
  reversal past a waypoint, exactly like manual play. (It previously
  assigned a flat 16 u/tic velocity, which read as unnaturally fast
  on the short hops G-to-loot makes.)
- Input locks while the queue drains.
- Dying clears the queue immediately and ends the progress phase.

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

**Auto-loot rule:** the HUD shows "Loot nearby, press G to loot"
when loot is in reach — or "Loot nearby, but backpack full" when
`D_BackpackFull` says the pack can take no item of any size, since
inviting a COLLECT that can only refuse the loot would be a lie. **COLLECT -- the contract.** COLLECT is an autonomous walk to loot,
never an instant move or a teleport. Explore mode only.

1. **Reachability first.** `T_DoCollect` (setup) gathers up to 32
   candidates -- Diablo loot drops and player corpses -- nearest
   first, and runs the grid pathfinder (`T_FindPathTo`) on each in
   that order (an unreachable candidate is skipped: "[COLLECT] loot
   N unreachable, trying next"). Candidates are limited by a
   **distance threshold of 512 units** (16 grid cells, cartesian;
   `T_LOOT_MAX_DIST`): loot beyond it is not a COLLECT target, and
   the "Loot nearby, press G to loot" prompt applies the same
   threshold (shared scan rule), so the two never disagree about
   range. If no Diablo candidate is
   reachable, it tries the nearest vanilla pickup within 512 units
   the same way -- except an ammo cache when no magazine has room
   for it: `P_GiveAmmo` would refuse it on arrival (magazines
   auto-replenish in explore, so full is the steady state), so a
   useless cache is not a target and COLLECT returns false. If the
   magazines fill during the walk itself, the walk ends on arrival
   with "MAGAZINES FULL." instead of the stuck watchdog. **Every
   candidate with a path is queued**, nearest first; COLLECT hands
   them to the GOTO walker one at a time (point 2). **If none has
   a path, `T_DoCollect` returns false** and
   nothing else happens: no walk starts, the HUD shows "NO
   REACHABLE LOOT.", and the caller ignores the request. (The loot
   prompt shares this scan -- it lights only for loot COLLECT could
   actually path to.)
2. **COLLECT delegates the walk to GOTO** (2026-10-22). COLLECT
   owns target selection only; all movement belongs to the GOTO
   walker. Setup hands the first candidate's position to
   `T_GotoStart` as a destination: GOTO pathfinds once, stores the
   waypoints (the last one is the exact destination, not its cell
   centre, so the walk ends standing on the item), and glides
   through them on momentum. `T_CollectTick` runs once per game
   tick (from `T_ExploreTick`, after the world tick) as a mission
   monitor, reading how each leg ended via `T_GotoResult`
   (arrived / blocked / no path / cancelled). A blocked leg is
   re-planned once from where the player stands; a leg that fails
   again -- or finds no path -- advances COLLECT to the next
   queued candidate.
3. **The walk owns the player.** While a walk is active, movement
   input is blocked on both channels: the local player's movement
   keys do nothing (`P_MovePlayer` returns early while a walk is
   active), so held keys can't fight the walk or keep resetting its
   stuck watchdog, and queued driver commands wait -- only
   diagnostic commands (SCREENSHOT, CAPTURE, VERSION, ...) pass
   through. COLLECT cancels GOTO and WALK when it starts; only one
   autonomous walk runs at a time.
4. **The walk ends.** On pickup (the target leaves the world:
   "LOOT COLLECTED."); on arrival with the target still in the
   world (the pickup sweep refused it -- backpack full, armor not
   an upgrade, or a filled ammo cache: "MAGAZINES FULL."); when
   every queued candidate's leg has failed ("CAN'T REACH LOOT." --
   a leg physics refuses aborts after ~1 second without progress:
   the BFS said walkable, physics disagrees); on a 20-second hard
   cap ("COLLECT TIMED OUT."); and **immediately when combat is
   encountered** -- combat entry cancels the walk, as do leaving
   explore mode or dying. Cancelling zeroes the player's velocity:
   no coasting.

**Pathfinding rule** (2026-10-21): the BFS tests what movement
tests. A step between cells requires the segment between the two
centres to be standable at its quarter points as well as at both
centres, with the 24-unit step rule chained across consecutive
samples, and every sampled position must fit the player's full
height between floor and ceiling -- the fit rule `P_TryMove`
enforces but `P_CheckPosition` alone ignores, whose absence let
the BFS route through closed doors (zero ceiling gap) that the
walk then wedged against. (Quarter points, not just the midpoint:
a face sitting exactly on the shared cell boundary is only touched
by the centre and midpoint boxes, never overlapped, so the checks
passed while the swept body wedged on it.) The walk steers to cell
*centres* -- the same points the BFS samples; an earlier aim
expression overflowed to the cell corner and wedged walks against
wall junctions (fixed 2026-10-21; GOTO waypoints had the same bug).
World-to-grid conversion is floor division: C's truncating `/`
registered negative coordinates one cell too high, so every grid
consumer reasoned about the wrong cell in negative territory.

**Stall recovery** (2026-10-21): if a GOTO or COLLECT walk makes
no progress for ~0.4s, it briefly steers to the centre of the
player's current cell before resuming. A player standing off-centre
across a face line straddles it with his box, so every destination
check inherits the line's (possibly zero) opening and physics
refuses steps in all directions -- even though the route from his
centre is legal (E1M1's window slit at (2351,-16): 1 unit off-centre
wedged both walks at 0 units moved). Physics judges only the
destination, and his own centre is a clean one, so re-centring
frees the walk; the usual stuck watchdog still gives up if even
that fails.

**Pickup rule** (2026-10-21): items are picked up by presence, in
the game loop — `P_TouchNearbySpecials` runs once per tic from
`P_Ticker`, after movement resolves, and collects every special
within a *circle* around where the player actually is (reach =
player radius + item radius; `P_TouchSpecialThing`'s vertical gate
still applies). When several items are in reach on the same tic
they are picked up in **ascending cartesian distance** order —
nearest first — so backpack space goes to the closest loot, not
to thinker-list order (Jimmy, 2026-10-21). `PIT_CheckThing` no longer touches specials: the
old test fired at the *candidate* position inside movement/probe
checks, so it collected from spots physics never let the player
reach, reached ~51 units on the diagonal against 36 head-on (a
square test), and the COLLECT pathfinder's probes picked items up
instantly, before the walk moved. **The loot alert uses the same reachability rule** (changed
2026-10-19): the "Loot nearby, press G to loot" prompt fires only
when the nearest loot in distance order has a BFS path — loot
elsewhere on the map no
longer lights the prompt. The prompt also shares COLLECT's
512-unit distance threshold (`T_LOOT_MAX_DIST`), so distant loot
stays dark. Player corpses are candidates under the
same rule: a corpse lights the prompt only when COLLECT could path
to it (an earlier existence-only corpse clause bypassed the path
test; removed 2026-10-21 at Jimmy's order). The prompt itself is delivered through
the vanilla message system (set the player message; re-issued
every 3 seconds while loot remains, since a message lives 4) —
changed 2026-10-21 after the original hand-drawn prompt turned
out to be invisible: it was drawn from the status bar at rows
160-170, and the 3D view, rendered after the bar, painted over
it every frame.

**Corpse recovery** (2026-10-21): death stores the whole inventory
(equipped, backpack, essence) on the corpse; respawn clears the
player. Touching the corpse (walk-over, or COLLECT) recovers it
**one item at a time, like Diablo** -- never a wholesale overwrite
(the old code replaced the player's inventory with the corpse
snapshot, deleting anything looted on the corpse run). Order:
essence first (it always transfers), then equipped pieces slot by
slot -- a piece re-equips if its slot is empty, otherwise it goes
through the normal backpack add -- then the corpse's packed items,
each re-placed on the player's own grid by the normal add.
Whatever doesn't fit **stays on the corpse**, which remains in the
world ("Backpack full -- corpse keeps the rest.") so the player
can junk something and come back; only a fully emptied corpse is
removed.

**Ammo conversion & monster drops** (Jimmy's rules, 2026-10-21):
there is no raw ammo pickup in the world, and no vanilla monster
drop. Map-placed ammo (clips, shell and rocket boxes, cells)
converts at spawn into a roll: a random item from the kill-drop
rarity table (loop magic find) spawns in its place, logged as
`[LOOT] map-spawn ... (was <ammo>)`. Monster drops are exclusively
the Diablo mechanic -- `P_DropDiabloLoot` (70% chance, killer's
magic find) is the only thing a death can spawn. The vanilla drop
switch (zombiemen -> clip, shotgun guy -> shotgun, chaingun guy
-> chaingun) is **deleted, not converted**: a kill drops a Diablo
item or nothing. Rationale: magazines replenish on their own, so
raw ammo is redundant -- and as a magazine cache it was worse
than redundant, since `P_GiveAmmo` refuses a cache when every
magazine is full, which in explore is the steady state: the clip
Jimmy stood on could never be picked up at all. A map-placed
*weapon* is placement, not a drop: it stays a weapon and converts
at pickup via `P_GiveWeapon`'s Diablo item grant. (The
magazine-cache path in `P_GiveAmmo` remains only as a backstop
for ammo mobjs in pre-conversion saves, and COLLECT still refuses
to target a cache no magazine has room for.)

**Navigation awareness (2026-10-21):** the agent drives from
screenshots and the log, so the engine reports where movement
actually went. WALK and GOTO results are position-stamped (start
position, end position, net units moved). **GOTO takes absolute map
coordinates** (2026-10-22, Jimmy's order — the facing-relative form
is removed), and SCAN reports keys by absolute coordinates; the
coordinates an agent may navigate to are the ones the game itself
stamps — its own position trail and stamped event positions (corpse,
loot, keys) — never unearned map data. A movement-history ring
buffer samples the player's position at command start/end and every
~48 units of travel; the `HISTORY` diagnostic command dumps it,
flagging when the newest points cluster in a small box (the trap
signature). Visited sectors are tracked continuously (every tick,
from the player's real position — the old `SECTORS` count only
marked the sector occupied when polled), so the visited count is
a truthful scoreboard for preferring new ground over re-treading
old ground. Both trackers reset when the level's sector array changes.
(A `FRONTIER` diagnostic that reported the nearest unvisited
sectors with bearings shipped and was removed the same day,
2026-10-21 — Jimmy ruled it cheating. The agent finds new ground
the honest way: a 360° survey spin followed by a replay CAPTURE,
whose rolling buffer holds the whole panorama for review against
its own movement history.)

---

## Combat language

### Stats

| Stat   | Meaning              | Effect                                                  |
|--------|----------------------|---------------------------------------------------------|
| AD     | Attack Damage        | Scales ballistic, explosive, melee. Strength feeds AD.  |
| AP     | Ability Power        | Scales plasma, BFG, status, charged effects. Energy feeds AP. |
| AS     | Attack Speed         | Reduces melee TP attack cost; ranged attacks are TP-free (ammo-gated). |
| Haste  | Cooldown recovery    | Cooldown-reduction percent (cap 50%): applied when a cooldown is set, then flat reduction, minimum 1 round (1 round = 10 s). |
| Crit   | Chance + damage      | Gear-inherent (Diablo-style): weapons/items grant crit chance and bonus crit damage. Base crits deal x2; each crit-damage point adds 1%. Chance caps at 50%. Crit is rolled exactly once per attack inside `T_DiabloDamageCallback` (once-per-attack pellet semantics); the legacy `P_DamageMobj` crit hook is bypassed in turn mode so crits can never double-roll. |
| Defense | Diablo 2-style rating | Sum of the five durability slots' armor values (rescaled into the thousands). Lowers a monster's chance to hit; no damage reduction. |
| MR     | Magic resistance     | Reduces AP-tagged plasma, occult, status damage.        |
| Move   | Movement efficiency  | Boots/effects can reduce step cost, never below 1 TP.   |

Damage: `damage = base + (AD × ratio_AD) + (AP × ratio_AP)` — most weapons
have one dominant scaling path; hybrid is a special build, not the default.
The preview shows the final damage range before firing.

Defense (Diablo 2-style, since 2026-10-21): a monster's chance to hit is
`CTH = 200 * AR / (AR + Defense)`, clamped to [5, 95], where the
attacker's rating is `AR = 60 + spawn health`. A landed hit deals full
damage (elemental resists and vanilla Doom armor absorption still apply
afterwards). Only the helm, armor, boots, gloves and belt slots produce
Defense; shields and other slots do not.

Tooltip order (ground loot, backpack, equipped — all identical):
damage range; AD/AP ratios; TP cost; attack speed; charge/ammo state;
cooldown; crit; special trait.

### Hero attributes (the existing four)

- **Strength** — AD, knockback resistance, heavy-weapon handling.
- **Dexterity** — hit chance, attack-speed TP cost, dodge.
- **Energy** — AP, charge efficiency, potion and status power.
- **Vitality** — max health, physical resilience, recovery.

Level-ups are gone (see XP and levels below): the four attributes keep
their base values and shift only through gear.

---

## Gun base types (AD/AP)

The Diablo weapon loot is 8 Doom gun base types. Equipping a gun syncs the
Doom `readyweapon`, so the first-person sprite, ground pickup sprite, and
turn-mode kit all follow the gun. Fist is the unarmed fallback (no gun
equipped), not loot.

| Gun | Type | LoL mechanic | Replenish | Identity |
|-----|------|--------------|-----------|----------|
| Pistol | AD | **Magazine 10** (1/shot) | AS | Reliable fallback sidearm. |
| Shotgun | AD | **Magazine 20** (2/shot) | AS | Positioning reward, pellet spread. |
| Chaingun | AD | **Magazine 10** (1/shot) | AS | TP-hungry DPS hose. |
| Chainsaw | AD | **Magazine 6** (1/shot) + **30% inherent leech** | AS | High risk, constant uptime; leech sustains close-range fighting. |
| Plasma Rifle | AP | **Ammo** (inverted) | — (ammo) | The ammo counter represents heat: starts at 0, +20 per shot, fires while the count is below the 125 capacity, and drains at the replenish rate continuously — the drain is the reload period. No separate heat resource (2026-10-22). |
| Rocket Launcher | AP | **Charges** (3, +1 per 2 s) + **Magazine 20** (10/shot) | AS | Punctuated splash; firing consumes 1 charge. Each regenerated charge also loads one attack's worth of ammo. |
| Super Shotgun | AP | **Magazine 8** (8/shot) + **knockback** | CD | Breach tool: shoves target 1 tile. Ammo replenishes on cooldown rate (CD=2). |
| BFG9000 | AP | **Magazine 20** (20/shot) | CD | Ultimate: highest damage. Ammo replenishes on cooldown rate (CD=3). |

**Cooldown is not a blocking resource.** The `cooldown` value in a kit
definition only drives the ammo replenish rate for CD weapons
(`REPLENISH_CD`). Firing never sets a cooldown and never blocks — ammo
is the limiter. Attack speed is the command execution delay
(`attack_tics` in the kit): pistol 15, shotgun 40, chaingun 16, rocket 80,
plasma 16, BFG 80, chainsaw 12, SSG 50 tics (35 tics = 1 second).

**Continuous fire** (chainsaw/chaingun/plasma, 2026-10-22): committing
an attack with a continuous weapon arms a lock on the target via
`T_SetupContinuousFire` — called from BOTH commit paths (F-key confirm
and Ctrl-release; the Ctrl path previously never armed it, so
continuous weapons fired exactly one shot per round). While game time
runs, `T_TickContinuousFire` (called per tic from `T_PulseOneTic`)
performs the attack directly at `T_AttackInterval` — the ONE
firing-rate system (kit `attack_tics` scaled by the derived
attack-speed stat: interval = attack_tics × attack_tp / 4), shared
with queued attacks, whose executor pulses for the same interval
per shot. Damage likewise resolves through the single executor
(`T_ExecAttack`) for both — the held-trigger behavior. The
progress phase stays alive while the lock is armed; the lock breaks
on target death/removal, weapon switch, a full or empty magazine,
target switch, combat exit, or line of sight lost — the tick checks
`P_CheckSight` every tic and clears the lock the tic sight breaks
(Jimmy's design, 2026-10-22: continuous fire holds only while the
target stays in sight, so continuous weapons are strong in open
ground and a poor choice for corner peeking).

**No vanilla weapon auto-switch** (2026-10-19): Doom's `P_CheckAmmo`
no longer picks a new weapon when the current one runs dry, and the
Berserk pack no longer forces a switch to fists — the ready weapon
changes only through the Diablo equip paths. (The vanilla auto-switch
set `pendingweapon` directly, silently swapping the weapon and its
Diablo stats with no equip log line.)

**Base kits are deliberately weak.** Kit damage is ~1/3 of the original
values (e.g. BFG 10–18 instead of 30–50, Pistol 2–4 instead of 6–13), so
the looted gun's Diablo stats are the real damage source. Hunting better
gear is the progression; a naked gun does chip damage.

**Two-handed weapons.** The BFG9000, rocket launcher, chaingun, and
super shotgun occupy both hands: no shield slot while one is equipped
(`D_IsTwoHandedWeapon`, `src/doom/d_diablo.c`). Equipping a two-handed
gun unequips the shield (stashed in the backpack on direct equip, or
held on the cursor in the inventory UI); equipping a shield while a
two-handed gun is equipped unequips the gun the same way.

Damage model:

- **AD gun:** `dmg = kit_base + gun_dmg_range + STR/2..STR`, then × (1 + AD%).
  Gated by TP and its magazine (auto-replenished; see below).
- **AP gun:** `dmg = kit_base + AP/4..AP/2`, then × (1 + AP%).
  STR does not scale AP guns; ENE does.
- Kit damage **stacks with** the gun's Diablo stats instead of overwriting
  them, so the looted gun and its affixes move real damage.

Cadence plumbing:

- Cooldowns, charges, and the inverted-ammo drain tick on wall-clock
  time, every game tic (`T_TickCadence`, called from `T_Ticker`):
  cooldowns count down 1 round per 10 seconds (350 tics), rocket
  charges regenerate 1 per 2 seconds (70 tics), and inverted
  magazines (plasma) drain toward 0 at the weapon's replenish rate
  (`T_KitReplenishTics` — the same rate normal magazines trickle up
  at, run in reverse) with fractional accumulation — all independent
  of round boundaries. `T_KitTickCooldowns` is kept as a no-op for
  save compatibility. **Frozen during the planning phase**
  (2026-10-22): like the magazine trickle, the cadence does not run
  in TS_PLANNING / TS_TARGETING / TS_CONFIRM — no charge regen,
  cooldown progress, or inverted-ammo drain while the player plans.
  It runs in explore and during the progress phase.
- `T_KitCanFire` is THE gate for every weapon: charges > 0 **and**
  the magazine rule — normal magazines need ammo_cost in the
  counter, inverted magazines need the counter below capacity.
  There is no per-weapon gate code anywhere else: queue validation,
  the attack executor, and the continuous-fire tick all ask this
  one function (via `T_HasAmmoForKit` for the magazine half).
- Fire-denied messages name the gate:
  `AMMO FULL` (inverted at capacity) / `NO CHARGES` / `NO AMMO`.
- The rounds overlay kit line shows gate state: charge count or
  cooldown count. Plasma has no gate entry of its own — its counter
  is the AMMO readout.
- Inventory tooltips show AD or AP typing and the mechanic
  (e.g. "AP weapon — Ammo: +20/shot, inverted").

**Ammo: per-weapon magazines, auto-replenished by AD/AP type.** Every
weapon with a magazine requires ammo to fire (`T_KitCanFire` checks it;
`NO AMMO` deny). Costs are validated when the attack is queued
(`NEED n AMMO (HAVE m).` on failure) and deducted when it executes
(`T_KitSpendAmmo`); `T_HasAmmoForKit` is the affordability check. During
planning, the AMMO display previews the deduction queued attacks will
cause (display-only; the actual spend happens on execute), so the player
can see when further attacks would be no-ops — inverted magazines
preview the counter rising toward capacity instead.

- **AD weapons** replenish only via the **trickle** (below) while game
  time runs — there is no per-round ammo grant (the round-boundary
  `T_KitRegenAmmo` top-up was removed 2026-10-22). Ranged attacks cost
  0 TP -- ammo is the limiter,
  so a round can dump the whole magazine; the magazine is a burst
  buffer (e.g. Reaper TP refunds). Melee (fists, chainsaw) still costs
  TP per attack, reduced by attack speed. Ammo **trickles visibly
  during the progress phase** (`T_KitTrickleAmmo`, per-tic): +1 to the
  ready weapon's magazine at a weapon-tied interval — cooldown-gated
  weapons trickle 1 per `cooldown` × 12 tics, attack-speed weapons 1 per
  `tp_cost` × 2 tics (clamped 3–20 tics: pistol 8, shotgun 4, chaingun
  3). The trickle applies to magazine weapons (AD); AP weapons still
  refill only via cooldown completion / charge regen, as above.
- **AP weapons** replenish from **cooldown**: SSG/BFG refill to full
  the moment their cooldown completes (`T_RefillOnCooldownReady`,
  hooked in the wall-clock `T_TickCadence`); the rocket loads one attack's
  worth of ammo per regenerated charge. Plasma's magazine is **inverted**
  (2026-10-22): the ammo counter represents heat — it starts at 0,
  each shot adds 20, the weapon fires while the count is below its
  125 capacity, and the count drains at the replenish rate
  (~43 per 10 s), which is its reload period. One counter, one gate,
  one spend, one rate system — the direction is kit data
  (`ammo_inverted`), not a plasma code path. The old separate heat
  value survives only as a save-format slot that aliases plasma's
  counter, so pre-change saves load their plasma state.
- Map-placed ammo pickups are replaced by random Diablo item drops
  (same rarity table as kill drops); Mana Potions no longer appear in
  the drop pool (magazines replenish on their own).
- The status bar's left AMMO readout shows the ready weapon's current
  magazine counter — plasma's included (it counts up as the gun
  fires). The kit attribute panel shows no heat-capacity entry for
  plasma (removed 2026-10-22 — heat capacity is not an attribute;
  plasma's capacity is its max ammo). The panel always shows both
  AS and CD for every gun, AP and AD alike (2026-10-22) — both
  attributes affect all guns: attack speed paces the fire interval
  and cooldown drives the ammo replenish rate. The FOCUS paperdoll slot
  (equippable ammo items granting passive weapon stats) is unchanged.
- Magazines persist in saves (TUR3 format; older saves load full).

**FOCUS slot.** "Ammo" survives as a paperdoll inventory slot renamed
FOCUS: equippable focus items (Piercing Rounds, Incendiary Shells, Swift
Cartridges, Spotter Rounds) that are never consumed and passively grant
stat bonuses to the equipped weapon. One shared slot; layout unchanged,
no profile-version bump.

### Rare affixes grant mechanics

Hand-authored mechanic flags on `diablo_itemdef_t` — no dynamic affix
composer (deferred; revisit only if hand-authored affixes feel flat).
~14 rare guns, two signature affixes per gun. Every affix is a flag plus
one small hook; no new subsystems except where noted.

**Plasma Rifle (Inverted Ammo)**

| Affix | Effect |
|-------|--------|
| of the Phoenix | Kills vent 50 ammo (kill-reset). |
| Overclocked | +25 ammo dissipation per 10 seconds. |
| Caldera | Reaching max ammo detonates a fire nova around you instead of locking the weapon, then vents to 0. |

**Rocket Launcher (Charges)**

| Affix | Effect |
|-------|--------|
| Voltaic | Explosions chain 50% damage to a nearby enemy. |
| Bandolier | +1 max charge (4 total). |
| Siege | Direct hits knock enemies back 1 tile. |

**BFG9000 (Cooldown + Mana)**

| Affix | Effect |
|-------|--------|
| Hungry | Kills reduce remaining cooldown by 1 round. |
| Event Horizon | The blast drags enemies 1 tile toward its center. |

**Super Shotgun (Cooldown — reload)**

| Affix | Effect |
|-------|--------|
| Breacher | Kills refund the reload cooldown instantly. |
| of the Bull | Firing shoves *you* 1 tile backward (recoil repositioning). |

**AD guns**

| Affix | Effect |
|-------|--------|
| Splitting | Shotgun/Chaingun fire +2 pellets. |
| of the Reaper | Kills refund 2 TP (signature on the Chainsaw). |
| Lucky | Every 3rd pistol shot is a guaranteed crit. |
| of the Glacier | Hits slow enemies: they act 1 round later. **Stretch goal** — needs an enemy-phase hook. |

**Loot rule:** common affixes bend numbers; rare/unique traits bend
mechanics.

**Visibility:** active mechanics from equipped gear are listed in the
POWERS panel below the backpack on the character screen (C). Stat boosts
stay in STATS; POWERS shows only special mechanics (and future granted
skills). `D_ActiveMechs()` ORs the `MECH_*` bits across all equipped

### Combat affix families (magic tier)

Diablo-style prefixes (flat/constant boosts) and suffixes (percentage
boosts) for the core combat stats. These spawn on magic weapons and can
also appear on rings, amulets, gloves, and focus items.

**Suffixes — percentage boosts:**

| Suffix | Effect | Guns |
|--------|--------|------|
| of Slaying | +15% AD | Pistol, Shotgun, Chaingun, Chainsaw |
| of the Magus | +15% AP | Plasma Rifle, Rocket Launcher, SSG, BFG9000 |
| of Haste | +20% haste (cooldown reduction) | SSG, BFG9000 |
| of the Furnace | +30 plasma max ammo | Plasma Rifle |

**Prefixes — flat/constant boosts:**

| Prefix | Effect | Guns |
|--------|--------|------|
| Soldier's | +4–5 flat AD | Pistol, Shotgun, Chaingun, Chainsaw |
| Wizard's | +8–12 flat AP | Plasma Rifle, Rocket Launcher, SSG, BFG9000 |
| Quick | −1 cooldown round | SSG, BFG9000 |
| Vampiric | +5% lifesteal (10% on Chainsaw) | All 8 guns |

Flat AD/AP are added *before* the percentage multiplier, so they scale
with % boosts. Flat cooldown applies after haste % (min 1 round).

**Non-weapon affixes:** combat stats also spawn on jewelry and gloves:
Ring/Amulet of Slaying (+10% AD), of the Magus (+10% AP), of Haste (+10%
haste), Vampiric Ring (+5% leech); Gloves of Slaying (+3 flat AD), Gloves
of Haste; Cooling Rounds focus (+20 plasma max ammo).

**Affix difficulty tiers.** Higher difficulties drop stronger affix
variants (`diff_tier` field, filtered by `D_DifficultyTier()`):

| Tier | Difficulty | Example upgrades |
|------|-----------|------------------|
| 0 | Normal | Base affixes (above) |
| 1 | Nightmare / NG+1 | of Slaughter (+25% AD), of the Archmagus (+25% AP), of Speed (+30% haste), Veteran's (+8 flat AD), Sorcerer's (+14 flat AP), Bloodthirsty (+8% leech), of the Inferno (+50 plasma max ammo) |
| 2 | Hell / NG+2 | of Carnage (+40% AD), of the Godslayer (+40% AP), of Lightning (+45% haste), Champion's (+12 flat AD), Archmage's (+20 flat AP), Soulrender (+12% leech), of the Sun (+80 plasma max ammo) |
slots; `D_MechDesc()` provides the one-line display text.

### Essence: salvage and repair

Junk loot feeds a Diablo 3-style crafting loop instead of cluttering the
backpack. Essence is a per-player currency (`diablo_essence` on
`player_t`, persisted in savegames), shown in the inventory header
("Essence: N" next to BACKPACK; SOUL_CAGE icon art is the planned
display, text for now).

- **S (salvage):** destroys the held item — or the keyboard-selected
  backpack item — and grants Essence by tier: Normal 1, Magic 3, Rare 8,
  Set 15, Unique 25.
- **R (repair):** restores the held (or keyboard-selected backpack)
  item to full durability for Essence, pro-rated by tier and missing
  durability (`D_RepairCost`: full-repair base Normal 2, Magic 4,
  Rare 8, Set 12, Unique 20).

**Design intent:** every drop is either an upgrade or salvage —
the farm loop never dead-ends in a full backpack.

### Durability

The five Defense slots (helm, armor, boots, gloves, belt) carry
per-instance durability, `20 + Defense/20` points per piece
(`diablo_equipped_dur[]` / `diablo_backpack_dur[]` on `player_t`). Each
landed monster hit wears one random worn piece by 1
(`D_WearRandomPiece`, called from `P_DamageMobj` after the defense and
dodge rolls pass). At 0 the piece stays equipped and keeps its other
stats, but contributes no Defense until repaired. Durability travels
with the item through the backpack, equip swaps, the corpse
snapshot/recovery and savegames (format DIB3; DIB2 saves load with
every piece at full; a corpse in a loaded world also returns at full —
durability is not stored in the mobj stream). Picking up a vanilla
Doom armor suit (green/blue) or a megasphere restores every equipped
piece to full, free (`D_RepairAllEquipped`).

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

**Kill moment:** a kill triggers a ~2 s death beat — living monsters freeze
while the corpse's death animation plays out under a screenshot-able kill
banner (title + sub, drawn last over everything, ~2.5 s real time),
plus a brief kill flash. The queue drain resumes when the beat ends.

### Select → preview → confirm (enqueues)

- `Tab` / `]` next target, `[` previous, number keys select directly.
- ATTACK shows hit chance, damage, TP cost, range, cover, target HP.
- Confirm enqueues the attack and reserves its TP; it executes FIFO when
  the progress phase starts (Enter/T), auto-facing the snapshotted target.
  Cancel returns to selection for free. Selection never advances time.
  In CONFIRM state, Enter still confirms the attack.
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

### Hit resolution

Vanilla Doom has no miss chance; the hit formula is a Diablo-style
addition.

- **Hit chance** = accuracy − distance penalty − cover penalty, clamped
  5–95%. Distance bands: −10 / −20 / −30 beyond 192 / 384 / 576 map
  units — pistols are exempt from the distance penalty. Cover: −10 for
  one blocked trace, −25 for two; three blocked traces is full blockage
  and the target is not legal.
- A d100 roll decides each hit; every shotgun pellet rolls separately.
- A miss shows a floating `MISS` above the enemy for 2 s (long enough to
  survive the queue drain). Floating target numbers on enemy heads were
  removed.

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

**State machine:** PLANNING → TARGETING → CONFIRM → PULSE → PLANNING
(or EXPLORE when no combatants remain). Targeting and confirmation are
reversible; TP is reserved when an action is queued. There is no enemy
phase — monsters act in real time while the queue drains in PULSE.

**Compatibility:** real-time play bypasses the controller via the stock
tic path. Turn mode disables demo recording/playback and netplay with a
clear message.

**Core invariant:** no turn-mode action reads mouse position, analog
magnitude, crosshair overlap, or a manually chosen world coordinate.

---

## Persistent progression

### NG+ loop (New Game+)

Beating the final map (MAP30) loops back to MAP01 with stronger enemies —
the Diablo endgame. The player's gear and stats are kept; only the
world resets.

- `d_loop` counter: 0 = first playthrough, 1 = NG+, 2 = NG++, etc.
- Enemy HP: +50% per loop (`D_LoopHpMult`). Enemy damage: +25% per loop
  (`D_LoopDmgMult`).
- Loot: +10% magic find per loop (`D_LoopMagicFind`).
- Affix tiers: loop 1 unlocks Nightmare affixes, loop 2+ unlocks Hell
  affixes (see Combat affix families).
- Trigger: completing MAP30 sets `d_loop_warp`, warps to MAP01 via
  `G_DoWorldDone`, increments `d_loop` via `D_BeatGame()`.
- Message: "LOOP N - enemies grow stronger!" announces the new loop.

### Standalone character profile

Separate from Doom saves, versioned, loaded at startup and saved
atomically on every level exit and normal quit. A failed write keeps the
previous valid profile and shows a clear error.

```
profile_version
name
level, current_xp, unspent_points   (kept for save compatibility; unused —
                                    progression is equipment-only)
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

### Diablo progress file (as built 2026-10-17)

The Diablo state — gear, inventory, Essence, progression — persists in a
separate mmap'd file, never inside the Doom savegame:

- `progress.dat` — farm/legacy default (slot 0).
- `progress_slotN.dat` — the slot the player picks at New Game
  (`CHOOSE SAVE SLOT` → `D_ProgressInitSlot(slot)`).
- `progress_auto1/2/3.dat` — auto-save rotation, independent of the
  manual slot.

Paths resolve from the working directory (`./progress.dat`), so they
work on any checkout — the old `$HOME/workspace/DiabloDoom/` assumption
silently disabled persistence wherever the repo lived elsewhere. The
file holds a header (magic `DDPG`, version, active slot, checksums) plus
two 4MB slots; a failed write keeps the previous valid slot.

**Auto-save is event-driven, not periodic** (`D_ProgressTick` is a
no-op). `D_AutoSaveGame()` snapshots the whole progress file into the
next rotation slot on:

- combat entry (`T_EnterCombat`),
- loot drop (`P_DropDiabloLoot`),
- inventory change — equip, unequip, or moving gear between slots
  (character-screen inventory actions),
- level completion (`G_DoCompleted`),
- level start (`G_DoLoadLevel`).

Auto-save never fires from a demo or a netgame (2026-10-22):
`D_AutoSaveGame` returns immediately while `demoplayback`, `netdemo`,
or `netgame` is set, and `G_DoCompleted` likewise skips its profile
save. The trigger events all occur during demo playback, and the
Freedoom IWAD's attract demos DEMO2/DEMO3 were recorded as netgames —
an unattended title screen overwrote the player's real slot-1 pair
with demo state before this gate existed.

**Load Game menu:** auto-save slots list first (positions 1–3), then
manual slots (positions 4–6 = Doom slots 3–5). An auto-save is a
**pair**: `progress_autoN.dat` (Diablo state) plus `doomsav(N-1).dsg`
(full world state), written together by `D_AutoSaveGame` (with
`SAVEGAMESIZE` at 4MB the old buffer-overrun rationale is gone).
Loading an auto slot restores both halves
(`D_ProgressLoadAutoSlot` → `G_LoadGame`). The auto-save keeps a
**single slot** (slot 1), so slots 2–3 have no pair and **refuse to
load** — "auto slot N has no saved world." There is no
progress-only fallback: the old warp fallback (load the gear file
alone, start a fresh level, teleport the player in) silently
resurrected every monster in the level and was removed 2026-10-21
after Jimmy's "saved games are reviving dead monsters" report —
slots 2/3 always took it, since their `.dsg` partners are never
written, while stale `progress_auto2/3.dat` files from the
rotating era made the fake load look real.

### XP and levels (removed 2026-10-16)

The XP/leveling system was scrapped: progression is now purely
equipment-based — the Diablo loot grind is the character growth.

- `T_GainXP` is a no-op: no XP accumulation, no level-ups.
- `T_AddStatPoint` is disabled (prints that progression is
  equipment-based).
- `T_CountKill` only increments the kill counter; kills no longer
  award XP.
- The `xp`, `level`, `stat_points` fields remain in the save struct for
  compatibility but are unused.

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
- **Publish triggers:** session end, unique drop. Write a
  character-sheet snapshot and queue one post (hero name, equipped
  weapon, armor summary, total kills). Aggregate same-action events into
  one snapshot/post. Event IDs prevent duplicate publication.
- **Post payload:** `event_id, event_type, timestamp, name,
  weapon, armor_summary, total_kills, uniques_found,
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
| 7 | Tactical loot pass. Affixes for AD/AP ratios, AS, Haste, charge, inverted ammo, move thresholds; selected unique traits. | Tooltips explain every change; equip/unequip reverses state exactly. |

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
flight, charge timing, plasma drain) · boss room (cooldown planning, BFG
commitment, build identity).

**Post-slice order:** chainsaw + super shotgun, wider unique pool, more
maps, then expanded command grammar and public-session cadence.

---

## Decisions to lock before implementation

| Decision | Recommendation / rule |
|----------|----------------------|
| Pulse boundary | Define exactly which weapon, projectile, monster, and world thinkers advance per action pulse. |
| Movement unit | Short fixed-distance, collision-aware step. WASD is facing-relative at commit (W/S step, A/D strafe); the view never auto-faces. |
| Round timing | **Decided Oct 6:** weapon cooldowns, charges, and heat tick on wall-clock time (1 round = 10 s = 350 tics, via `T_TickCadence`, independent of round boundaries); status effects still advance once per round. |
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
