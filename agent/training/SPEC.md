# DiabloDoom RL Environment — Spec

Status: draft, 2026-11-12. Source plan: "DiabloDoom Gymnasium Integration Plan" (Claude Doc). Codebase re-checked at commit `c0e8fedb` (2026-11-12).

## 1. Goal and scope

Wrap DiabloDoom as a Gymnasium environment that runs headless, many instances at once, faster than real time. A small policy learns to **run levels** (explore and find exits) from the screen alone. An LLM/VLM plays the turn-based combat and makes gear decisions.

| In scope for the policy | Out of scope for the policy |
|---|---|
| Moving, turning, strafing, using doors and switches | Combat turns (LLM/VLM) |
| Choosing the keys held every game tic (W/S, A/D, turn) | Gear: equip, unequip, salvage (LLM/VLM plus item cascade) |
| Deciding when to CONSULT the LLM/VLM | Reading logs, state JSON or map coordinates |

Standing rules:
- **Discrete commands only**, no scalar input (no `GOTO x y`).
- **Pixels only** for the policy. Engine state may drive rewards, episode ends, routing and the critic, never the policy's input.
- **Combat stays turn-based.** If combat ever moves to the policy, combat should become real-time.

## 2. What the codebase already has

| Piece | Where | Use |
|---|---|---|
| FIFO command driver, one command per tic | `cmd_drain` / `cmd_dispatch` in `src/diablo/t_turn.c`, called first in `T_Ticker` | Action channel. Gated by `D_OnMuseVM()` today. |
| Turn state machine | `turnctrl.state`: EXPLORE, PLANNING, TARGETING, CONFIRM, PULSE, REACTION, ROUND_END | Routing: EXPLORE → policy, PLANNING onward → LLM/VLM |
| State dump | `T_DumpState` (mode, TP, queue, HP, position, angle) | Basis for the JSON state line |
| Kill, damage, sector tracking | `T_CountKill`, `T_CountDamage`, `T_TrackTick` / `SECTORS` | Reward terms |
| Test arena and scripts | `-tbarena` (2 zombiemen + 2 imps), `-tbscript` | C-side regression tests for the combat handoff |
| Save slots | `SAVE n` / `LOAD n` | Resets and new start states |
| Headless launch | `SDL_VIDEODRIVER=dummy` in `agent/doom_loop.sh` | Proven on the farm |
| Fixed-tic option | `singletics` in `src/d_loop.c`, `D_RunFrame` in `src/doom/d_main.c` | Fast-forward switch |
| Item pickup choke point | `D_BackpackAdd` / `D_BackpackAddDur` in `src/diablo/d_diablo.c` | `ITEM` events |
| LLM player and logs | `agent/doom_loop.sh`, `SYSTEM_PROMPT.md`, `PROMPT.md` | VLM handoff; logs are item-model training data |

The replay system (`d_replay.c`) records 15-second MP4 video, not inputs, and `T_Init` disables turn mode under `-record` / `-playdemo`. There is no demo-replay determinism test; §10 replaces it.

## 3. Architecture

```
 Python (RTX 5080 box)                         DiabloDoom process × N  (-rlenv)
┌──────────────────────────┐                  ┌───────────────────────────────┐
│ Trainer (GRPO; PPO in P6)│                  │ FIFO driver (cmd_dispatch)    │
│ AsyncVectorEnv           │ ── commands ───▶ │ Turn controller, game tics    │
│ DiabloDoomEnv × N        │                  │   (T_Ticker under singletics) │
│ LLM/VLM handoff (CONSULT)│ ◀─ state JSON ── │ State emitter                 │
└──────────────────────────┘ ◀─ frame (shm) ─ └───────────────────────────────┘
```

- **One game per process.** Doom keeps all game state in globals, so `AsyncVectorEnv` scales by adding processes.
- **Two pipes plus shared memory per instance:** a command pipe in, a state pipe out (one JSON line per event), and the frame in shared memory.
- **Event-driven steps.** The engine answers only when a decision is due: every tic in EXPLORE (per-tic decisions, 2026-11-15), so one `env.step()` is one tic; only a multi-tic driver command (the farm's WALK, GOTO, ...) or a frozen PLANNING step differs.
- **CONSULT is the only route to the LLM/VLM.** The game cannot hand off by itself; CONSULT is an environment signal that drives Muse VM to make a decision (combat or inventory). The policy must choose CONSULT itself — nothing sends it automatically. That includes when combat starts before the policy consults (a monster acquired the player first): the game stays in PLANNING until the policy recognizes combat from the screen and chooses CONSULT. Control returns to the policy back in EXPLORE.
- **In-process library** (`libdiablodoom.so` + pybind11) if Phase 3 profiling shows the pipe round trip dominates. With a decision every tic that is likely: 32 instances mean 32 round trips and one batched forward pass per game tic.

## 4. Engine changes

| Change | Detail |
|---|---|
| `-rlenv` | Enables the FIFO driver without `D_OnMuseVM()`. Sets `singletics`; skips wipes, ENDOOM and continuous capture. Suppresses the on-screen `CMD <command>` echo (it would put the policy's last action into its own observation and churn the HUD message line) and the wall-clock idle Zug Zug timer (`D_UITickIdle`, `I_GetTimeMS`). |
| `SEED n` | Sets `prndindex` (the `P_Random` index, 0–255) right after `LOAD`. Required: savegames don't store `prndindex` and `G_DoLoadGame` doesn't clear it (`M_ClearRandom` runs only on a new game), so without it the RNG position after a load depends on the process's history. `P_Random` also rolls all Diablo loot, affixes and defense checks. |
| `-cmdpipe <path>`, `-statepipe <path>` | Per-instance pipes (today the FIFO is hardcoded to `/tmp/doom_cmd.txt`). |
| `STATE` command and auto-emit | One JSON line: mode, TP, queue, HP, position, angle, weapon kit and magazine, loot prompt, up to 9 targets (type, HP, distance, relative angle), kill/damage/sector counters. Emitted automatically when a decision is due: in EXPLORE every tic (2026-11-15) unless a multi-tic command (WAIT, a 45° turn, SURVEY, WALK, GOTO, COLLECT) still runs, then on the tic it ends, and on entering PLANNING (the policy must then choose CONSULT, §3). |
| Frame to shared memory | At each decision, copy `I_VideoBuffer` (320×200, 8-bit) and the palette. No PNG, no SDL surface. |
| Render only at decisions | Skip `D_Display` on tics between decisions. Moot since per-tic decisions (2026-11-15): every tic is a decision, so every tic renders. |
| `STATEHASH` command | Hash of player, mobj and turn-controller state, for determinism checks. |
| `WAYPOINT <x> <y>` / `WAYPOINT` (2026-11-18) | One point in map units, drawn in the scene until the player touches it as a pickup (2026-11-19: within 64 units before; then a `waypoint_reached` event): the menu skull (`M_SKULL1`/`2`, recoloured green, blinking, 1 map unit a pixel, about a pickup's size), shown through walls; nothing while it is out of view. The state line carries `waypoint`. The VLM sets it in production (from a `MAPSHOT`); the env in training. |
| `MAPSHOT` (2026-11-18) | Draws the automap and saves it as a PNG within one tic, then closes it: the policy's frame never shows the map. Zoomed to fit the player's sector (its lines within 1024 units, with a margin), with the waypoint drawn as a green square. The `mapshot` event gives the file and the pixel-to-map numbers (`x0`, `y0`, `upp`, `h`, in 320x200 frame pixels). |
| Explore-mode strafes | Today `STRAFE_L/R` queue a TP-costing turn move that only previews until END TURN. Under `-rlenv` in EXPLORE they take a fixed sideways step without turning, like `WALK <angle>`. |
| `-rlnocombat` | Removes every monster at level load. Reuse vanilla's monster-removal flag (`d_main.c`; the variable renamed `eval_mode` 2026-11-17) rather than new removal code: launched as `--eval` (vanilla's `-nomonsters`, renamed 2026-11-17). It also turns turn-based combat off entirely: no passive-damage planning, no explore mini-combat (Ctrl / `ATTACK` do nothing in explore), and no monster-sighting entry. |
| Mechanism use events | Under `-rlenv`, every player USE that sets something going sends `{"event":"use","sectors":[...],"lines":[...]}`: the sectors that gained an active special (doors, lifts, floors, ceilings, stairs) and the switch lines flipped. A use that does nothing (a wall, a locked door without its key, a door already moving) sends nothing. Feeds the mechanism reward (§5.3). |
| Pickup events | Each thing the player picks up sends `{"event":"pickup","kind":K,"hp":N,"practice":B,"gametic":N}` (a Diablo item also `"tier":T`, 0 normal .. 4 unique, 2026-11-17) from `P_TouchSpecialThing` (`RL_NotePickup`), K one of loot, armor, health, key, other: loot is a Diablo item into the backpack, `hp` the health the pickup gave. A thing left on the ground (full backpack, full health) sends nothing. |
| `-rldrive` | Control and output only, for driving a normal, real-time game with the policy (farm play, §7.3): the driver on `-cmdpipe`, the JSON lines on `-statepipe`, the frame in `-shm`, and decision points published without blocking (one per idle period, plus one when combat starts, the player dies or the level ends while idle). No uncapped tics and none of the `-rlenv` suppressions. `-rlenv` implies it. |
| `MONSTERVIS` (state field) | Whether the player can see a monster that has **not** acquired the player, while in EXPLORE. Precise test: `P_CheckSight` to the monster, inside the player's horizontal FOV, and the sprite at least K pixels tall on the 320×200 screen (so a speck on the horizon doesn't count). Rewards only (§5.3); never in the observation. Implemented as `monstervis`: a living monster among the visible targets within the 90° view (no sprite-size test yet). Next to it, `backpack_full` (a pickup failed on a full backpack in the last 4 s, and it is still full) and `bp_count`. |
| `-rlspawn <tics>` | CONSULT practice. Detection has no range limit, so on the map a monster almost always acquires the player the moment the player sees it, and a sighting before combat almost never happens. With `-rlspawn`, explore spawns one monster at a time on screen: in line of sight, 128–384 units ahead (640 before 2026-11-17), within 30° of the view (no room there, no spawn that time). It faces away and deaf, so it stays frozen: a sighting to CONSULT about. It never triggers combat: if it would acquire the player (the player walks into its front arc), it is removed instead, and it never counts as a combatant. Mean `<tics>` game tics between spawns; each is removed 5 s after spawning, and one in a loaded save is removed at once (practice monsters carry a flag the savegame keeps). Rolls use a private RNG seeded by `SEED`, so episodes stay deterministic. Works with `--eval`. |
| `CONSULT` (driver command) | Under `-rlenv`, the training stand-in's hand-off (§5.4): removes every practice monster, in sight or not (2026-11-17), every map monster in sight and, in combat, every monster targeting the player. A spawned practice monster disappears (no kill, no loot); a map monster is killed (kill count and drops, as if the VLM had fought it). Under `-rldrive` alone (eval.py --iwad; 2026-11-16) it removes only the practice monsters (2026-11-17), never a map monster. Every CONSULT shows `CONSULT` as the status message (under `-rlenv` too since 2026-11-17), so a watcher sees the policy asked for the hand-off. |
| Backpack-full status message | Shown on a failed pickup. Its own fixed screen spot, size and colour that survive an 84×84 downsample, fixed duration. Today's `D_BackpackFullMsg` → `D_Msg` uses the shared HUD message line in small text and is overwritten by later messages. |
| Exit path distance | Engine pathfinder distance from the player to the exit, locked doors blocked unless the key is held, capped when no path exists. Critic input only (§7.3). |
| `ITEM` event | Emitted from `D_BackpackAddDur`: tier, item index, durability, rolled stats, backpack index. |

## 5. Environment interface

### 5.1 Observation

- The screen as displayed: the scene, status bar kept.
- Downsampled (area-averaged) to 128×80 grayscale (84×84 at first), last 4 frames stacked.
- With each frame (2026-11-22, `EnvConfig.depth`) the engine's depth buffer: each pixel's distance along the view direction, rendered with the frame (`-shm` segment version 2, `RL_DepthBegin`), shrunk to 128×80 by taking the nearest depth of each output pixel's area (a monster wins over the wall behind it; an average blended them), and coded near-bright on a log2 scale (`env.obs.depth_code`: 254 at 16 map units .. 1 at 4096, 255 the status bar and the player's weapon, 0 the sky). 2 × 80 × 128 a frame, 8 × 80 × 128 stacked. For vision: a monster's silhouette stands apart from the background, which texture and Doom's dim lighting hide in grayscale, and projectiles show as well. It is the policy's first input from the engine beyond the screen (AGENTS.md: the no-oracle exception). A checkpoint records it (`obs_depth`; one without saw grayscale only), a resumed run keeps its own, and eval follows the checkpoint.
- Nothing from the state pipe.

### 5.2 Actions

Discrete, 29 outputs, one decision every game tic (2026-11-15): each action is the keys held for the next tic, the way a keyboard is played. The env rejects anything else.

| # | Action | Notes |
|---|---|---|
| 0–53 | `PRESS <keys>` | Every combination of one key or none from each pair: W/S run forward/back, A/D strafe left/right, L/R turn left/right, each without and with `CTRL`, the fire key (2026-11-18: the ready weapon, fists when none; 0–26 without fire before) (`PRESS`, `PRESS CTRL`, `PRESS L`, ..., `PRESS WAL CTRL`, ..., `PRESS SDR CTRL`; index 18·forward + 6·strafe + 2·turn + fire; 0 is `PRESS` alone, nothing held). `G_BuildTiccmd` adds the keys to the ticcmd, so the player moves exactly as with those keys held: run speed, strafe-running, the run turn rate (about 7° a tic). Explore only |
| 54 | `USE` | Doors and switches, including exits |
| 55 | `CONSULT` | Environment signal, not a driver command: it drives Muse VM to make a decision — switching to combat or inventory mode. The handler is swappable: on the farm it signals Muse VM (e.g. `doom_loop.sh` exits so Muse's VLM takes over); in training the env forwards every CONSULT to the game as the `-rlenv` `CONSULT` driver command (§4), false ones included, and a stand-in (§5.4) does the rest. |
Everything else stays with the LLM/VLM or the harness: the engine's multi-tic movement (`WALK` in every form, the 45° `TURNL`/`TURNR`, `STRAFE_L`/`STRAFE_R`), `WAIT`, `COLLECT`, `SNAP_WALL` (aims from engine geometry), bare `WALK` (stops on engine detection), `SURVEY`, `GOTO`, the combat commands (`MOVE_FWD`, `MOVE_BACK`, `ATTACK`, `UNDO`, `END`), gear (`EQUIP`, `UNEQUIP`, `SALVAGE`), read-outs (`INSPECT`, `INVENTORY`, `KEYS`, `SECTORS`, `SCREENSHOT`, `CAPTURE`, `REPLAY`, `VERSION`) and session control (`SAVE`, `LOAD`, `RESPAWN`, `QUIT`).

The policy plays per tic (2026-11-15): no multi-tic commands, and no angled walks (`WALK <angle>` snaps the facing in a single tic).

No per-state action masks (they would leak engine state). No mode mask either: the policy keeps acting in combat, where every action except CONSULT does nothing and just costs a step (the env drops movement actions outside EXPLORE so they never queue TP-costing combat moves). The policy has to learn from the screen that combat has started and choose CONSULT. While the VLM/stand-in is playing a CONSULT, no policy steps are taken.

### 5.3 Rewards

The reward is computed by the env (`env/env.py`, `DiabloDoomEnv.step`) from the engine's state line and events (§4); none of those fields is a policy input -- the policy acts from the frames alone. Every step returns the sum of the terms below and, in `info["terms"]`, each term's share (logged per rollout in `rollouts.jsonl`). The magnitudes live in `RewardConfig`; the clock-tied ones are set by `model.make_env` from `--time-penalty`. Every term has a row here with its purpose; a new term gets one too. A step is one decision, one game tic under per-tic decisions (more for a CONSULT the stand-in plays out).

Notation: `T` is the time penalty's base rate per tic, `--time-penalty` / 35 (default 1/3 point per game second, so `T` = 1/105). "Rollout" means from the reset (after any warm-up) to the end; once-per-rollout memory is cleared at each reset.

#### Progress

| Term | Value | Purpose | Rule |
|---|---|---|---|
| `sector` | +15 per room | Exploration: a new room is the main sign of progress through a map. | Each sector visited for the first time this rollout, by the engine's visited-sector count -- except door sectors (2026-11-17): the env counts `sectors − door_sectors` (`rooms()`), so walking through a door pays once, for the room behind it. Sectors visited at the start (a progress save's visited list, the warm-up) never pay. A sector is any area with its own floor, ceiling or light, so a stair step or a lit patch counts too; each pays once. |
| `exit` | +100 | The goal: finishing the level. | Level completion triggered (exit switch or line). Ends the rollout. |
| `key` | +25 per colour | Keys open the locked doors on the way to the exit. | A key colour (blue, yellow, red; card and skull alike) first held this rollout, from the state line's `keys` mask. Keys held at the start never pay. In training a new key also ends the rollout (the key goal); the held-out eval doesn't stop for it. |
| `mechanism` | +10 | Doors, lifts and switches gate progress; teaches using them. | A USE that sets a mechanism going -- a sector starts moving (door, lift, floor, stairs) or a switch flips -- from the engine's `use` event. Once per mechanism per rollout; ones used in the warm-up count as used. |
| `loot` | +5 per item | The Diablo loop: collecting items. | A Diablo item into the backpack, or armor (which grants one), from the `pickup` event. Practice items (`-rlitems`) pay the same. Something left on the ground (full backpack) pays nothing. |
| `item` | +0.01 per unit | A dense guide to `loot`: walk to an item in sight, so the pickup gets found. | While a practice item lies (from its `item` event, 10 s or until picked up), each map unit closed toward it; moving away costs the same, so circling nets nothing (`item_approach`, 2026-11-17). |
| `health` | +0.75 per HP (+0.2, then +0.5, before 2026-11-18) | Makes healing worth a detour when hurt; the mirror of the hit penalties. | The HP a health pickup actually gave (the `pickup` event's `hp`): a medikit at 75 HP pays 18.75. The mirror of the hit penalties. |

#### Time, distance and direction

| Term | Value | Purpose | Rule |
|---|---|---|---|
| `waypoint_time` | none since 2026-11-20 (−`T` per tic before) | Was the urgency while following a waypoint. Dropped: a group with waypoints has no clock at all (no `time` either, in the gaps or after the 5th), since 5 fixed `waypoint` rewards, the rollout limit and the discount already make sooner better, and a clock that switched rate around waypoints worked against `waypoint` (the conflict check stopped training at iteration 1070). | The clock while a WAYPOINT is set (`--waypoints`, on by default since 2026-11-18): the `time` row at its base rate (2026-11-19; before, the term `waypoint`, with d the straight distance to the waypoint). The env stands in for the VLM: it sets one at a random standable point of a sector next to the player's, one it can walk into from its part of its sector (door and small sectors lead on to the one behind; `WAYPOINT RANDOM`; `waypoint_place` "path": at the engine's `path_ahead`, 192 units along the walking path to the nearest unvisited room, 16-unit noise), never within 256 units of the player (any 96 or more when none is), keeps it until it is reached (replaced every 5 s before 2026-11-18), and sets the next 1 s after one is reached (the engine clears it when touched, as a pickup); 30% of groups get none. While one is set, `fast` points at it instead and `look` pays nothing (it pointed at it before 2026-11-20: paid per tic facing it, lingering paid); without one, `fast` uses the facing (not `fast_path`) and `look` pays nothing. `eval.py` sets them too, `--iwad` included. Chained. |
| `waypoint_look_approach` | +0.01 per unit × max(0, cos a) | A dense guide to `waypoint`: walk toward it while facing it, so it stays on screen. | (2026-11-20; `waypoint_approach`, unscaled, from 2026-11-19) The distance closed toward the waypoint set, times how squarely the player faced it at the step's start (a the angle from the facing to it); away costs the same way, so a strafe while facing elsewhere costs little (a jump over 64 units is not walking). No per-tic pay: standing facing it earns nothing. Chained. |
| `waypoint` | +15 | Follow the marker: in production the VLM steers the policy with waypoints. | (2026-11-19) A waypoint reached (`waypoint_reached`). At most `waypoint_count` (5) are set per rollout; a goal ending it early (exit, key, portal) pays this for each of the 5 not reached, so the new sector a waypoint leads into never costs the ones after it. Chained. |
| `time` | 0, off (2026-11-21: `make_env` passes `time=0`; −`T` × (1 + d / 256) per tic before) | Don't loiter: every tic costs, more the farther from the way on, so leave rooms and keep moving. | The clock: every game tic of the rollout, in groups without waypoints only (2026-11-20). d is the distance to the way on (`goal_dist()`), the mean of the step's start and end: the walking distance to the nearest unvisited room, the state line's `path_dist` (2026-11-17: Dijkstra on a 32-unit grid of where the player can stand and step, built per level load by the `GOTO` pathfinder's rules, through doors the player can open -- manual ones, locked ones with the key); with no path, the straight distance to the nearest portal of the current sector, `portal_dist` (doors and lifts first, 2026-11-17); with neither, 0. So the clock runs at `T` beside the way on and twice that 256 units off: being far costs more the longer it lasts. It only bites with a rollout-ending goal (the portal goal, below): rollouts that all run to the limit pay alike, and the group-relative advantage cancels that. |
| `fast` | 0 since 2026-11-20 (+`T` × forward share per fast tic before) | Off: with combat, moving for its own sake shouldn't pay. Was: pays back the clock for moving along the way on, so moving with purpose is nearly free and standing or bumping into walls costs. | Each game tic on which the player moved at least `fast_speed` (4) units (position, not momentum: pushing into a wall never counts; the engine's `fast_tics`, `-rlfast`), scaled by max(0, cos b), b the angle between the movement and the way on (`fast_forward`; `fast_path`, 2026-11-17: the step-start `path_rel`, else `portal_rel`; with neither, the facing): running along the way pays back the clock's base, running across it -- a circle's sides, a detour -- strafing off it or backing up pays nothing. Moving well is nearly free; standing still, bumping into walls or backing up pays the whole clock. |
| `look` | +`T`/2 × max(0, cos a) per tic | Face the way on, so the frames show where to go. | Facing the way on. a is `way_rel()`: the state line's `path_rel` (2026-11-17), the angle to the farthest of the next 4 path cells the player can walk to in a straight line, so it turns with a corridor and never points into a wall; with no path, `portal_rel`, the angle to the nearest portal of the current sector into an unvisited sector (doors and lifts first); with neither, nothing. Half the clock, so it never pays for standing still. |

#### Movement habits

| Term | Value | Purpose | Rule |
|---|---|---|---|
| `wall_look` | −0.05 per tic, chained (2026-11-21: half of `turn`, so staring at a wall may last twice as long as turning; −0.02 std one-off, then, earlier that day; −`T` points one-off before) | Don't stare at walls: a view of bare wall shows nothing. | Each tic the first line ahead the player can't pass is nearer than 48 units (`wall_dist`): a view of bare wall shows nothing. Not while a usable wall is in reach (`wall_use`: a switch, a pressed one-shot switch, a closed door -- a locked one only with its key -- or a remote door's face, within USE range ahead or 45° off it). |
| `turn_waste` | 0 since 2026-11-20 (−0.005 per 45° before) | Off: `turn` already charges every turning tic outside combat, so less turning means less jitter, and this charged the small corrections that track an enemy in combat. Was: No jittery turning back and forth. | Turning that gets undone: over the last 8 tics, Σ\|Δθ\| − \|ΣΔθ\|, charged as it accrues. A steady turn either way, and holding still, cost nothing; a teleport resets the window. |
| `turn` | −0.1 per tic, on its step (−0.01 at first, 2026-11-20) | Against circling: a steady turn one way, which `turn_waste` never charges (2026-11-20: 41% of tics turned left, 6% right). | (2026-11-20) Each tic a turn key (L or R, with anything else) was held, charged on that step, chained (2026-11-21; all paid when the rollout ended before, so every step's return carried the same cost and it ranked whole rollouts without saying which turns were wasted) -- except in combat: a monster began an attack on the player within `turn_combat_tics` (105, 3 s) before the step (the engine's `attack_age`, 2026-11-20). Turning is the only aim, and finding an attacker off screen is turning too, so circle-strafing and re-acquiring a target stay free. (At −0.1 everywhere `turn` conflicted with `damage` and `kill` and combat deaths rose from 0.16 to 0.40; then the exemption was a monster on screen, `foe_dist`, until the combat drill's `damage|turn` conflict stopped training at −0.611: turning back to an attacker out of view cost.) Turning to face a door costs the same as spinning; a 90° turn, about 13 tics, costs 1.3, a full spin about 5. |
| `move_flip` | −2 (2026-11-22; −3 late 2026-11-21; −0.1 std earlier that day; −0.015 before) | No dithering forward and back. | W within 4 tics of S, or S within 4 of W. Strafing back and forth is free. |
| `fall` | −10 per fall (−3 before 2026-11-18) | Avoid drops: often no way back up, and pits and nukage below. | An unbroken descent that at some point drops faster than 10 units a tic (freefall; the fastest lift lowers 8) and totals at least 64 units, charged once on landing whatever the height. Going up, stairs and lifts never cost; a teleport is no fall. From the state line's `z`. |

#### USE

| Term | Value | Purpose | Rule |
|---|---|---|---|

#### CONSULT

Triggers, while exploring (no **monster in view** trigger since 2026-11-18: the policy fights; training no longer launches `-rlspawn` practice monsters): a **unique item picked up** (2026-11-17: the `pickup` event's `tier` 4; the engine draws a gold border around the avatar (status-bar face) for 3.5 s, fading out over its last half second (2026-11-18; magic, rare and set pickups get one too, in blue, yellow and green, so in grayscale the unique's is told apart by brightness alone; a screen flash before), through the 3 s window, the cue the policy can see -- it replaced the full-backpack trigger, whose only cue was a few pixels of text), and **combat** (the world is frozen until CONSULT).

| Term | Value | Purpose | Rule |
|---|---|---|---|
| `consult` (hit) | +30 (late 2026-11-21; +1 std earlier that day; +5 before) | Hand off to the VLM when a trigger shows. | The first CONSULT on a live unique trigger. A CONSULT in combat pays nothing itself but avoids the miss. |
| `consult` (miss) | −15 (late 2026-11-21; −0.5 std earlier that day; −2.5 before; −8, then −12, before 2026-11-18) | Hand off promptly: ignoring a trigger costs. | A trigger left unanswered through its grace: 105 game tics (3 s) from a monster's sighting or a unique's pickup (its border stays on through it), 5 steps in combat. Charged step by step while it waits, −12 over the whole window (2026-11-17; once at the end before), so each frame without a CONSULT gets its share; a CONSULT stops it. A monster trigger stays live through its grace after the monster leaves the view (2026-11-17: turning away used to end it with no penalty, so the miss almost never fired), and a CONSULT then still pays the hit. |
| `consult` (false) | −15 (late 2026-11-21; −0.5 std earlier that day; −2.5 before; −0.5 before 2026-11-18) | Don't hand off without a trigger. | A CONSULT with no live, unanswered trigger (−1.5 before 2026-11-17: a CONSULT then only paid off with a trigger live more than 23% of the time, so the policy stopped consulting; −0.3 before 2026-11-18: the policy consulted more everywhere, never more on a trigger). |

#### Damage and death

| Term | Value | Purpose | Rule |
|---|---|---|---|
| `hit_unavoidable` / `hit_avoidable` | −1 / −15 per hit (2026-11-20; one `hp_loss` −7.5 per hit before, per HP before 2026-11-19: −0.2, −0.5, then −0.75) | `hit_avoidable`: dodge projectiles, melee and hazards. `hit_unavoidable`: kill hitscanners early. | Each tic the engine sends a `hurt` event (a blast's pellets once), whatever the amount and even when block, armour or resistance take no HP: gear scales the amount, not being hit is the skill. The event's `kind` splits it: `hitscan` (a line attack, which can't be dodged: a small pressure to kill early) pays `hit_unavoidable`; `proj`, `melee` and `world` (nukage, slime, crushers, barrels), which can be avoided, pay `hit_avoidable`. Healing pays nothing. |
| `death` | −200 (2026-11-20; −150 before, when a death also forfeited the rollout's combat pay; −100 before 2026-11-18) | Survive. | Ends the rollout; 2× the exit, on top of the HP lost on the way. |

#### Combat (2026-11-18)

A share of the groups fight (`--combat`, 0.5; stage 2: all): every game launches with its map monsters (no `--eval`) on skill 2 (`--skill`, 2026-11-20; 3 before: the easy set of monsters, without the medium set's ambush closets, at normal damage), and right after loading the start save the env sends `MONSTERS SHOW`, or `MONSTERS HIDE` to take every living map monster out of the world for a group that only explores (all members alike, from the group's seed). Every rollout also starts with a random gun equipped (`GIVEGUN`: any of the seven, or fists, evenly among the eight, the same for the whole group; `EnvConfig.give_gun`), else the player is on fists, with the strength gear the gun needs (strength comes only from gear: Immortal King's Soul Cage, and for the BFG Mara's Kaleidoscope too), and since 2026-11-20 (`EnvConfig.gun_kit`, `GIVEGUN <seed> KIT`) a kit for it, every other slot emptied first: fists the most armour (helm, armour, shield) and strength (elsewhere), an AD gun the most AD, an AP gun the most AP, never vitality, so a kill takes fewer hits and fighting beats running. Combat and survival weigh half again as much as the maze run: a monster's full HP 11.25 in `damage` (22.5 with `kill` until 2026-11-20) against a new sector's 15, and the hit penalties, `death` and `health` ×1.5.


Rollout types (2026-11-20, `--mix combat=3,waypoint=2,maze=1,mixed=2`, `combat=2,...,maze=2,...` at first that day, groups per type, adding up to `--groups`; `env.TYPES`). A type sets what the world holds, never how it is scored: **every type has the same reward function** (the tables here), so an issue a situation causes shows up as an issue of the reward rules, wherever it happens. **combat** (the combat drill): a drill map from its map start, map monsters hidden, the engine's `DRILL ON` keeping one awake monster (zombieman, shotgun guy, imp or demon) 320-768 units ahead (2026-11-20; 192-768 at first, then 192-512) within 20° of the facing, the next one 0-2 s after a kill (0-70 tics, random; at once before: an empty screen to hold fire at too) and a fresh one at once only when it has had no line of sight to the player for 10 s, stuck (2026-11-21; off screen for 1 s, then 3 s, before -- turning away from a monster closing in made it vanish and a fresh one spawn in view); never fists (`GIVEGUN ... RANGED`); no waypoints or practice items (`ITEMS 0`); every sector visited from the start (`VISIT`), so no new room can pay and fighting is its only payoff. All 56 actions, as everywhere (2026-11-20; from the drill's start that day it sampled only strafe and fire under an action mask, which needed its own entropy treatment: first left out of the floor, when the drill collapsed onto 97% `PRESS D`, then a per-sample floor -- removed with the mask, so a type differs only in its world). The entropy floor is `--ent-target` (0.62) times ln 56, 2.5 nats. **waypoint** (the waypoint drill): a drill map from its map start, map monsters hidden, endless waypoints (no `waypoint_count` limit, the next one at once) and frequent practice items (`ITEMS 175 50`: about every 5 s, half uniques; on the way to the waypoint when it is on screen), so walking to markers, picking up and CONSULT on the gold border are dense. **maze**: the rotating maps, map monsters hidden, no waypoints. **mixed**: the rotating maps, map monsters and waypoints (production). The drill maps (`model.DRILL_MAPS`, 2026-11-20; Ultimate Doom E1M1 alone before): Ultimate Doom E3M2 and E1M2, Freedoom E1M8 and E4M3 -- picked from a survey of every E1-E4 map for the most open area connected to the player start (wide openings, floors within a step), no damaging floor there, and at least ~400 units of sightline ahead and 20° to each side; the drill groups rotate over those of the `--iwads` in use, one map on every `--rotate-every` batches (drill maps can still be held-out eval maps: drills are training like any other). The drills always start at the map start and end only on the exit or `--drill-tics` (1050, 30 s); a death costs `death` and respawns at once (2026-11-22, the engine's `RESPAWN`: at the map start, no level reload, so the drill goes on; the same `GIVEGUN` again, since respawning empties the slots; it ended the rollout before): no portal or key goal (a waypoint in the next sector would end it at once). maze and mixed rotate over the IWAD/map pairs with the frontier and the tic budget. The train metrics count every rollout alike; each row of `rollouts.jsonl` carries its `type`. Held-out eval runs `maze`. A waypoint is reached within `EnvConfig.waypoint_reach` (96 units, 2026-11-20, every type; before, only the engine's touch as a pickup), the env clearing the engine's marker with the next action. (The same day, before the types: `--drill-groups` / `--drill`, combat drill groups beside the random split below.) `--combat` and `--stage 2` now only set untyped resets (eval.py).
| Term | Value | Purpose | Rule |
|---|---|---|---|
| `damage` | +11.25 per a monster's full HP | A dense combat signal: every hit on a monster is progress toward the kill. | The engine's `damage` event: the HP the player took off a monster (capped at its max), over its max HP, so every kill pays the same in all. Paid on the step of the hit since 2026-11-20 and kept whatever follows, a death included (being hit is the hit penalties' alone). From 2026-11-19 it was paid when the rollout ended, nothing after a death (times `hit_decay` 0.8 per hit taken until 2026-11-20): a hit was credited to the whole rollout while a miss's `fire_waste` (later `damage_miss`, removed 2026-11-21) landed on its own shot, and firing rose. Chained. |
| `kill` | 0 since 2026-11-20 (+11.25 before) | Off: `damage` alone pays for fighting (a kill is the last of a monster's HP). Was: finish monsters off, removing threats one at a time. | The `damage` event that killed it; paid with `damage` (on the hit's step since 2026-11-20; at the end from 2026-11-19). Chained. |
| `view` | +2.5 per monster in view per full HP of damage | Keep the fight in view: every monster still inside the view cone (round a corner too) makes each hit worth more, so cornering and strafing that keep enemies in frame pay; no hit, no pay. | (2026-11-20) Each hit pays `view` × its share of the monster's max HP (as `damage`) × the engine's `damage` event `inview`: the living monsters inside the player's 90° view cone at that moment, any distance, line of sight or not, woken or not (the one hit included). Paid on the hit's step like `damage`, kept after a death. Chained. |
| `damage_miss` (`fire_waste` before) | removed 2026-11-21 (−0.03 per point of the gun's average damage per shot earlier that day; −0.15 before that, −0.8 a shot from 2026-11-20, −0.3 before, −0.1 before 2026-11-19) | Was: aim before shooting. `aim` now judges each hitscan press, and `proj` each projectile's result, so a miss charge only duplicated them (and charged a projectile at fire time, hit or not). | Was: a shot that hurt no monster at once, times the shot event's `sdmg` (also removed). |
| `blind_fire` | removed 2026-11-22 (−1 a press earlier that day, when `aim_off` −1 made firing spam: 44% of steps with no monster on screen; 0, off, from 2026-11-21; −0.35, −0.5 and −1 std, −10 and −20 points, before) | Was: don't fire at nothing. `aim_off` alone now charges a press off target. | Was: a press while the state it acted on had no monster on screen (`monstervis`), one-off. The metrics `fire_seen` / `fire_blind` stay. |
| `aim` | ±10 per fire press (late 2026-11-22: symmetric again, `aim_off` −10; a press breaks even at 1/2 on target); +7 on target, −3 off earlier that day (`aim_off`: breaking even at 3/10; −2 before that; −1 before that, breaking even at 1/8, when firing spammed: about 45% of steps on and off target alike; ±7 before, about −5.7 a press for a fresh policy ~9% on target), one-off (2026-11-22; ±10 one-off before, when firing fell to 3% on and off target; before that chained, its own later presses drowned each press's credit and inflated the group scale; earlier that day chained at ±5, then ±2 before, when firing rose to 30% of steps, seen and blind alike; ±0.5 before that, about 0.006 of the group scale a press; late 2026-11-21: ±0.35 std, one-off, earlier that day; ±0.5 a press, chained, before that; ±0.05, then ±0.02, points per tic, fire or not, earlier that day: it stuck at −6 a rollout and didn't judge the press) | Fire when the crosshair is on the monster: every press is judged at once, not only when a bullet lands. | (2026-11-21) A step whose action presses fire (any `... CTRL`) while a living monster is in line of sight (the engine's `foe_dist`, any direction) in the state it acted on: +10 when a hitscan shot would be aimed at a monster (the engine's `aim_on`: `P_BulletSlope`'s autoaim probes -- the facing and 1<<26, about 5.6°, either side, 1024 units -- finding a living monster first, not a barrel or wall), −10 when not (`aim_off`; −3, −2, then −1, earlier 2026-11-22, −7 before); charged whether or not the gun was ready; only with a hitscan gun ready (pistol, shotgun, chaingun, super shotgun: a rocket, plasma or BFG shot has to lead a moving target, which this cone can't judge; fists reach only 64 units). The aimed cone widens as the monster comes closer (its body fills more of the view). Judged from the state the press acted on, so it needs no shot to come back. One-off since 2026-11-22 (one-off for part of 2026-11-21, chained in between). Metrics `fire_on` / `fire_off` (2026-11-22): the press rate in those states with the crosshair on a monster and off one (`rollouts.jsonl` `aim`: [steps on, fire; off, fire]); aim learned widens the gap, both falling is fire suppressed. |
| `track` | +2 per step that held the target, at most 10 steps (20) per press, one-off (late 2026-11-22; +1, at most 6, earlier that day) | Follow the target before firing: the steps that kept the crosshair on the monster, not only the press, earn credit, and only when an on-target press ends the streak, so tracking without firing earns nothing. | (2026-11-22) An on-target press (as `aim` judges it: hitscan gun ready, a monster in line of sight, the engine's `aim_on`) pays `track` back to each step before it that kept the crosshair on the same monster: the engine's `aim_streak`, the consecutive tics `aim_on`'s probes found that same living monster, restarted by anything else in the way (a wall, another monster), a skipped tic or a level load. At most `track_tics` (10; 6 earlier 2026-11-22) steps, and each step once: a press credits only the steps since the last press (a press off target spends its steps too), so rapid fire credits one step a press. Credited back like `proj` (`info["reward_back"]`). |
| `proj` | +6.25 per monster hit, −2.5 a miss (2026-11-21; +2.5 and −1 earlier that day) | Land projectiles: a rocket, plasma bolt or BFG ball has to lead a moving target, which `aim` can't judge, so the result itself pays. | (2026-11-21) When a projectile the player fired is gone (the engine's `proj_end` event, from `P_RemoveMobj`), `proj_hit` (+6.25) for each monster it damaged, direct or splash, each once (a rocket into 5 monsters pays 5×), or `proj_miss` (−2.5) when it damaged none. A hit also counts in the combat stats' hits (projectile hits counted nowhere before). `damage` still pays its hits too. One-off since 2026-11-22, credited back to the step that pulled the trigger: the engine's `proj_end` carries `fired`, the tic it was spawned (its `shot` event's), the env maps that shot to the step before it (the press acts a tic late) and reports `info["reward_back"]` [steps back, term, value], which `rollout.py` adds to that step's one-off reward (a plasma bolt lands 17–40 steps after its press). Chained, on the step it ended, before. |
| `range` | +0.015 per unit (0.01 before 2026-11-20) | Fight at the ready weapon's range: fists and chainsaw up close, the rocket launcher and BFG out of their own splash, hitscan guns at medium to long range. | (2026-11-20) Each map unit the nearest monster in line of sight, in any direction, behind the player too (2026-11-20; on screen only at first, which let turning away from a monster closing in stop the losses), woken or not (the engine's `foe_dist`) moves toward the weapon's band (`env.RANGE_BANDS`: fists and chainsaw 0–80, pistol and chaingun 256–768, shotgun 128–512, SSG 64–256, rocket and BFG 192–768, plasma 128–640); each unit away costs the same, inside the band nothing. A change of distance, never a per-tic pay, so holding the band or stalling a fight earns nothing. Only the nearest counts, so many enemies don't pay more; a jump over 64 units (another monster becomes the nearest) pays nothing. Chained. |

#### How a rollout ends

| End | Kind | When |
|---|---|---|
| `exit` | terminated | The level is completed (+100). |
| `death` | terminated | The player dies (−200); in a drill it respawns and plays on (2026-11-22). |
| `key` | terminated | Training only: a new key colour (+25). |
| `portal` | terminated | Training only, `--portal-goal n` (default 1): n new rooms entered (each +15). No bonus of its own; it is what makes the clock matter, and with frontier starts the agent advances room by room. |
| `time` | truncated | `--max-tics` game tics (default 525, 15 s); the held-out eval runs `--eval-max-tics` (4200, 120 s; 2100 before 2026-11-18) with no portal or key goal, so its sectors compare across versions. |
| `combat` | truncated | 20 steps in combat. |

The return is the plain sum over the rollout. GRPO compares rollouts within a group that started from the same state (§7.1, `learn.group_advantages`): with `--advantage outcome` (the default) every step of a rollout gets its return's z-score within the group; with `--advantage process` each step's reward, in points, less the group's mean step reward, is summed from that step to the end (reward-to-go); the one-off terms (`RewardConfig.immediate`: `consult`, `move_flip`, `turn_waste`, `aim` and `proj` (2026-11-22), `track` (2026-11-22); `wall_look` until 2026-11-21; the env's `info["reward_now"]`, 2026-11-17) judge only the step's own action, so they are left out of that sum and added, in points, to their own step's advantage alone; the group's advantages are then divided by one number, their std over the group (late 2026-11-21: only to balance groups of different lengths; logged as `adv_scale`), and the value targets leave them out (§7.1, "Process advantage"). A group whose rollouts all score alike carries no signal and is dropped ("collapsed"). `--norm-adv` (default on) then divides the batch's advantages by their standard deviation, so batches of different lengths update alike.

#### Design notes

- **Only what the policy can perceive is rewarded as a behaviour.** It sees 4 stacked grayscale frames (default 128×80), so a cue must survive that: hence the unique border rather than a status message.
- **The clock is the backbone.** `fast`, `look` and `wall_look` are tied to `T`, so the shaping terms only ever trade against time: none of them can pay for standing still.
- **Shaping state is reward-only.** `path_dist`, `path_rel`, `portal_dist`, `portal_rel`, `wall_dist`, `wall_use`, `door_sectors`, `monstervis` and the events are never in the observation.
- **No stuck term:** the clock already charges standing still. (Combat has its own terms since 2026-11-18, above.)
- Magnitudes are tuned from training curves; watch for a term growing in `rollouts.jsonl` without progress following (e.g. `loot` farming against `sector`).

### 5.4 Episodes, rollouts and resets

- **Rollout:** up to 2,100 game tics (60 s) from one start state, and with a decision every tic up to 2,100 decisions. Ends early on death or exit. Produces one score R_i.
- **Unconsulted-combat cap:** the world is frozen in PLANNING, so steps there spend no tics and the 2,100-tic budget never runs out. After M policy steps in PLANNING without CONSULT, the rollout ends (with the per-step penalties already accrued).
- **Reset:** `LOAD n` from prepared save slots spread across each level, then `SEED s` to set `prndindex` (§4). With a 256-entry table a seed is one of 256 start offsets; variety comes mostly from slots. Slots near the exit come first in training. Some slots start with the backpack one or two items from full.
- **Frontier starts:** a rollout ends at 2,100 tics, so from the map start alone training never sees past the first minute of a level. After each batch, per (IWAD, map), the highest-return rollout that ended alive, in EXPLORE, at 80% of max HP or more (`hp_max`) with no monster in its sector after the player (`hunted`; both 2026-11-19), short of the exit and with progress (a new sector, or a new key: the key goal ends the rollout there) saves the game where it stopped to slot 5 (`SAVE 5`; slot 4, the start every group loaded, is never overwritten mid-batch) with its visited sectors (`VISITED`, replayed by `VISIT` after each load); only that highest-scoring rollout is saved. The save is copied into slot 5 of every instance on that map and slots 4 and 5 swap, so the progress is the next batch's start in slot 4, the same for every member of a group. A batch without progress keeps the start and grows that map's rollout limit (`TicBudget`: x`--tics-growth` 1.5 from `--max-tics` up to `--max-tics-cap` 2100; progress, an exit or a cleared frontier puts it back to `--max-tics`), so the agent gets time to find the path while a new sector or key stays the only progress; `--frontier-patience` (5) such batches in a row at the full budget clear it; the saves and tic budgets are never written to disk (2026-11-15): a new run starts every map from its beginning, and only the map rotation resumes, from the checkpoint's iteration (a save only loads in the exact IWAD file it was made with, and machines may have different releases). An exit clears the map's frontier (the batch's other rollouts still run to their own ends), so slot 4 holds the map start again and the level is replayed from the start. `--no-frontier` turns it off (map starts only, still slot 4).
- **Group reset:** all G instances in a group load the same slot and seed.
- **IWAD:** each instance runs one IWAD (`-iwad`) for its whole life, so an instance's IWAD and map are fixed when it is spawned. Save slots belong to one IWAD (a save loaded under another IWAD's maps is garbage), so slots and multiple IWADs can't be mixed until slots are prepared per IWAD.
- **Training stand-ins:** a CONSULT is answered cheaply: the env sends every policy CONSULT to the game as the `CONSULT` driver command (2026-11-17), which shows the status message and removes every practice monster and the map monsters in sight; the stand-in (`standin_consult`) then closes a combat round with `END` (and CONSULT again), or salvages the backpack until it has room. Both stages run `-rlspawn` practice monsters (`--spawn-tics`, default 350 since 2026-11-17, 250 and 700 before), so CONSULT on sighting is trained from stage 1, and `-rlitems` practice loot (`--item-tics`, default 420, 300 before; 2026-11-17) to walk to and pick up.
- **`info`:** tics per step, mode, group id. (Mode is for logging and rewards only; it never masks actions.)

## 6. Policy architecture

```
screen (scene, 84×84, 4 stacked)
   → Nature CNN (3 conv) → 512 features
   → GRU core, 256 units   ──▶ action head: 29 logits, sampled
                            └▶ value head (+ HP, exit distance)  [critic, training only]
```

- **GRU** carries working memory: where the player has been and what is no longer on screen. GRU rather than LSTM because it is simpler (one state vector instead of two, fewer parameters); they perform about the same.
- **Actions are sampled**, not greedy, which is what makes a GRPO group's rollouts differ. (eval.py's `--temperature T` samples key by key instead -- move/USE/CONSULT, then the forward, strafe and turn keys, each conditional sharpened to p^(1/T) -- for watching or measuring a more decisive policy: 1 is the trained policy, and toward 0 each key takes its most likely value, so forward split over 9 PRESS combinations still beats one larger turn. Training always samples at 1.)
- **Value head** (one linear layer on the GRU output, plus HP and exit path distance as privileged inputs) trains from day one. GRPO ignores it; Phase 6 PPO needs it.
- **Hidden state** starts at zero for each training rollout. Phase 6 logs it at the start of each logged segment.
- **Size:** about 2 million parameters.
- **The GRU is required, not an optional second step.** With a decision every tic, the 4-frame stack holds about 0.1 s of play, so a feedforward policy is nearly memoryless: it can't remember a turn it made a second ago or a room it just left. (The current trainer still runs the feedforward network; adding the GRU, and carrying its state through rollouts and the update, is open work.)

## 7. Training

### 7.1 GRPO (pretraining)

- **Groups:** G = 8 rollouts from the same slot and seed; 4 groups per batch = 32 instances.
- **IWADs:** training runs on every IWAD at once: Freedoom Phase 1 and The Ultimate Doom (`train.sh` passes every file under `wad/` whose header is `IWAD`; PWADs are skipped). With I IWADs and M maps, group g plays map `maps[(g // I) % M]` on IWAD `iwads[g % I]`: map by map, every IWAD on each map (E1M1 on both, then E1M2 on both, ...). The default group count is I×M (8 for 2 IWADs × E1M1–E1M4), one group per IWAD/map pair, at most the logical CPU cores minus 2 (2 always left free). With more pairs than groups the groups rotate: every `--rotate-every` (10) batches they move on to the next block of pairs (pair `(r · groups + g) mod I·M` in rotation r), wrapping around; a moved instance's game restarts on its new map, and each map keeps its frontier save and tic budget for its next turn. Each session shuffles the pair order (which pairs come first, and which group plays which) with a fresh map seed, logged and saved as `map_seed` in `config.json` (`--map-seed` fixes it). `--rotate-every 0` turns rotation off (the extra pairs go untrained, with a warning). `--maps all` is every map the IWADs share, minus the eval maps and, in stage 1, the maps whose exit needs a boss death (E1M8, E2M8, E3M8, E4M6, E4M8; Doom II MAP07, MAP30). The held-out eval maps (`--eval-maps`, default `random`) are `--eval-count` (2) maps drawn each session with the same seed from the maps every IWAD has, minus the training maps and, in stage 1, the boss-exit maps; a list fixes them. Eval plays every held-out map on every IWAD, and the logs report sectors and exit rate per IWAD.
- **Advantage:**

  ```
  A_i = (R_i − mean(R_1..R_G)) / (std(R_1..R_G) + ε)        ε ≈ 1e-8
  ```

  Every decision in rollout i gets A_i. Fallback if learning is slow: the process variant (normalize per-step rewards across the group, sum from each step onward; the sums grow with the steps left, so `--norm-adv`, on by default since 2026-11-16, divides each batch's advantages by their standard deviation -- no mean shift -- to keep the entropy bonus and the gradient norm independent of the rollout length).
- **Process advantage: chained and one-off rewards (2026-11-17).** Each step's reward r_t is split in two:
  - the **one-off** part n_t: the terms that judge only the action taken on that step (`RewardConfig.immediate`: `consult`, `move_flip`, `turn_waste`, `aim` and `proj` (2026-11-22), `track` (2026-11-22); `wall_look` until 2026-11-21, `blind_fire` until 2026-11-22). The env reports it per step as `info["reward_now"]`, and the rollout keeps it as `rew_now`.
  - the **chained** part c_t = r_t − n_t: everything else (`sector`, `fast`, `look`, `time`, `item`, `loot`, `health`, `key`, `mechanism`, `exit`, `death`, `hit_unavoidable`, `hit_avoidable`, `fall`), which comes from a run of decisions, so earlier steps share its credit.

  The rule: a term is one-off when the step's own action fully decides it and no earlier decision set it up. It is chained when getting there took earlier steps: the way walked, the turns that lined the view up, the room entered.

  | Term | Stream | Why |
  |---|---|---|
  | `sector` | chained | A new room is reached by the whole walk that led there, not just the step that crossed its line. |
  | `fast` | chained | Running speed builds up over several tics of holding forward (4 units/tic on the third tic from rest), and lining up with the way on (`fast_path`) took the turns before. |
  | `look` | chained | Facing the way on is the result of the turns that got the view there. |
  | `time` | chained | The clock and its distance part (far from a way out costs more) follow from where earlier steps took the player. |
  | `item` | chained | Distance closed toward a practice item is the walk toward it, step after step. |
  | `loot` | chained | A pickup is the end of the walk to the item. |
  | `health` | chained | Same: a health pickup is reached by walking to it. |
  | `key` | chained | A key is picked up at the end of the route to it. |
  | `mechanism` | chained | The USE step starts the door or lift, but reaching the switch and facing it took the steps before. |
  | `exit` | chained | The level exit is the end of the whole route. |
  | `death` | chained | A death follows from the walk into the hazard (or a long stay in it), not only the last step. |
  | `hit_unavoidable`, `hit_avoidable` | chained | Damage comes from where earlier steps took the player (nukage, crushers). |
  | `fall` | chained | Charged on landing, for a drop the player walked off steps earlier. |
  | `consult` | one-off | A hit (+5) or a false CONSULT (−2.5) judges that CONSULT alone; the miss is charged on each step without a CONSULT while the trigger waits. Whether the trigger is on screen is not the policy's doing, so no earlier step earned it. |
  | `move_flip` | one-off | Forward right after back (or back after forward): it judges this step's key against the last one. |
  | `turn_waste` | one-off | Turning back against the recent turns: it judges this step's turn. |
  | `wall_look` | chained (2026-11-21; one-off before) | Staring at a wall up close, charged per tic: facing a wall follows from the steps that walked up to it, so they share the cost. |

  A new term must be put in one stream or the other: the env counts it as chained unless it is added to `RewardConfig.immediate`.

  ```
  σ   = std of every r_t in the group             (the collapse test, as before)
  μ_c = mean of every c_t in the group
  G_t = Σ_{u ≥ t} (c_u − μ_c)                      (points)
  s   = std of every G_t + n_t in the group        (the group's scale, metrics adv_scale)
  A_t = (G_t + n_t) / (s + ε)
  ```

  Since late 2026-11-21 every reward stays in points, the one-off ones too: the group's mean chained step reward is the baseline, and one number per group, the std of its advantages, divides them all. That number only balances groups against each other (a long maze group's sums swing more than a short drill's); within a group every term weighs by its points, so tuning is by plain ratios (a hit's 9 against a miss press's −0.5). The one-off values were converted from the std units below at about 30 points a std (an estimate from the drill's within-group return spread, about 55 points; `adv_scale` gives the real figure). `time` is off and `turn` is charged on its step since then, so no term pays only at the rollout's end. Before: the chained part was summed from each step on, divided by the std of every per-step reward, then scaled to unit std over the group (2026-11-18). Its spread grows with rollout length, since each G_t sums every step after it: at std ≈ 8, a CONSULT hit's +5 was only about 0.3 of it, and the policy's P(CONSULT) didn't depend on a monster being on screen. The one-off part was then added in standard deviations of advantage (earlier on 2026-11-21: scaled by its own std over the group, capped at ±5; before: by the std of every per-step reward).

  Why: summed into the reward-to-go, a one-off reward moved every earlier step's advantage too. A CONSULT's +5 also credited the steps just before it, including the frames where a monster was in view and the policy did something else, so consulting late, or spamming CONSULT, scored about the same as consulting on sight. Measured at iteration 592, before the split: the CONSULT rate was 2.4% with a monster in view and 2.5% without. That's also why the CONSULT miss penalty (§5.3) is charged step by step over the grace window, −12 in all, instead of once at its end: each frame without a CONSULT carries its own share, on that step alone.
- **Loss:**

  ```
  L = −(1/Σ|o_i|) Σ_i Σ_t min(ρ_i,t·A_i, clip(ρ_i,t, 1−clip_coef, 1+clip_coef)·A_i) + β·KL(π_θ ‖ π_ref)
  ρ_i,t = π_θ(a_t|s_t) / π_old(a_t|s_t),   clip_coef ≈ 0.2
  ```

  A few epochs per batch. β = 0 to start (no reference policy). If the farm LLM's explore commands (paired with its frames) are behavior-cloned first, that clone becomes π_ref with β > 0.
- **Group diversity:** `P_Random` is deterministic, so only action sampling separates rollouts. Use a small entropy bonus (or `--ent-target`'s per-state floor, which leaves the policy alone above it). Drop zero-std groups and log the collapsed share. Read the logged entropy against the uniform policy's, ln 29 ≈ 3.37 for the 29 actions (§5.2).
- **Trainer:** a PPO-style clipped-ratio trainer with the GAE replaced by group advantages and group resets added. It runs as one process on one box (`train.py`; the split rollout-worker/learner trainer was removed 2026-11-14, §7.5).
- **Data:** mostly the game's own levels (they stay fixed) across both IWADs, so the runner sees two texture sets and two layouts of each map slot, plus filtered community maps so it isn't brittle. Randomize textures, player speed and turn rate. Optional early stage: generated mazes with a known shortest path.

### 7.2 Training stages

1. Exploration and combat (2026-11-18): half the groups fight the map monsters with `PRESS ... CTRL`, the rest explore with them hidden (`MONSTERS HIDE`). No map monsters at all (`--eval`), plus CONSULT on `-rlspawn` practice sightings, before.
2. Every group fights (`--stage 2`).
3. Scripted combat stand-in with an HP-loss penalty: learning which fights to avoid. **Open; details later.**

### 7.3 Phase 6: session fine-tuning with PPO

- The runner plays live sessions with frozen weights and no search, logging every decision: the frame, the action and its probability, the reward, the game build (`VERSION`), and the GRU state at the start of each segment (about every 128 decisions). An hour of play is about 126,000 decisions (one per tic); pool sessions from several farm instances when one is too small.
- A session visits each state once, so there are no groups. PPO's critic scores each step:

  ```
  δ_t = r_t + γ·V(s_{t+1}) − V(s_t)
  A_t = δ_t + (γλ)·δ_{t+1} + (γλ)²·δ_{t+2} + …        γ ≈ 0.999, λ ≈ 0.95
  ```

  A step is one tic (per-tic decisions), so γ is a plain per-step discount; at one tic per step γ ≈ 0.999 gives a horizon of about 1,000 tics (≈ 30 s). Frozen PLANNING steps spend no tics and are rare. (Before per-tic decisions, steps spanned very different tic counts and the discount was raised to each step's tics.)
- **Critic:** before the first update on a session, fit the value head to that session's returns for a few epochs with the policy frozen. Normalize or clip returns, since CONSULT rewards are spiky. The last step bootstraps from V, because the game never really ends.
- **Update:** a few PPO epochs with a KL anchor to the shipped version. Data is discarded afterward (on-policy).
- **Eval gate:** run the fine-tuned runner on a fixed eval set (the game's own maps plus a few held-out ones). Ship only if it beats the shipped version. Run the same gate on each new game build.
- **Combat policy (future):** a separate network chosen by `turnctrl.state`, the one place continual training is planned. Gear stays with the LLM; the combat policy takes the equipped loadout's stats as input and trains across randomized loadouts.

### 7.4 Checkpointing

The execution environment can stop at any moment.
- Save at every update boundary: policy and value weights, reference policy, optimizer state, schedule positions, save-slot and seed sampler state, all RNG states, logger run id.
- Discard rollouts in progress on restart.
- Reward conflicts (2026-11-19): every `--conflict-every` (10) updates the learner takes each reward term's policy gradient alone (its advantages from that term's rewards only) on `--conflict-samples` (4096) samples and logs the cosine of every pair (`term_cos`; the console shows the lowest); a pair averaging below `--stop-conflict` (−0.5, −0.1 before 2026-11-20; −1 never) over `--conflict-window` (10 checks, 100 updates; 5, then 14, before) stops training like the health checks below. `waypoint_time` counts as `time` there (`learn.CONFLICT_MERGE`): a tic pays one or the other, so apart they are opposite by construction. `hit_avoidable` and `hit_unavoidable` are never compared (`learn.CONFLICT_SKIP`, 2026-11-21): a hit is one kind or the other, so they trade off within any fight. Redundancy (2026-11-20): from the same cosines, a pair averaging `--redundant-cos` (0.9) or more over `--redundant-window` (30 checks, 300 updates) is printed as a merge candidate when first named and logged in the metrics (`redundant`, pair -> mean) at each check while it lasts; a warning, never a stop (terms with different purposes can point the same way for a while). `damage` and `kill` averaged +0.93 the day it was added, and `kill` was turned off.
- Health checks before each save (2026-11-16): the update reports the share of the trunk's last-layer units that never fire on the batch (`dead_units`), the gradient norm before clipping (`grad_norm`, `grad_norm_max`) and the optimizer steps skipped for a non-finite loss or gradient (`nonfinite_steps`). ReLU collapse (`dead_units` ≥ `--stop-dead-units`, 0.5) or an exploding gradient (`grad_norm_max` > `--stop-grad-ratio`, 100, times the median of the last 50 healthy updates once 20 exist; or > `--stop-grad-norm`, off by default; or any non-finite step) stops training with exit code 1: that update is not saved, there is no end-of-run eval, and no further upload starts. The metrics row records the reason as `stopped`.
- One checkpoint file, `doom-runner.pt`, replaced after every update by an atomic write (temp file, `fsync`, `os.replace`), so a crash mid-write leaves the previous one; a checkpoint that fails to load stops the run rather than starting it fresh (2026-11-15: replaces keeping the last 3 `ckpt_NNNNNN.pt`; older versions live in the Hugging Face repo's commits, `--hub-repo`).
- On startup, delete stale FIFOs and `/dev/shm` segments and kill orphaned DiabloDoom processes. Only leftovers whose owner process is gone are touched, so several trainer processes can share one box.

### 7.5 Split trainer (removed)

Removed 2026-11-14. The trainer used to run optionally split, rollout workers (`eval.py`, now `rollout.py`; Muse VMs among them) feeding a learner (`learn.py`) through a spool of batch and weights files. Muse VMs didn't gain from the split, so only the single-box `train.py` remains: `eval.py` keeps the rollout, rollout-log and held-out eval pieces and `learn.py` the update, both used by `train.py`. `train.sh` has no `--learn`, `--actor` or `--heldout` modes.

## 8. LLM/VLM handoff and items

- **CONSULT** is the only way decisions reach the VLM (the game cannot hand off on its own); it covers both combat and gear. The policy chooses it on a sighting, a full backpack, or when combat has started without one (§3).
- **Items:** `ITEM` events are queued, not episode-ending. Queued items go to the VLM at the next CONSULT.
- **Cascade:** rules first (below level, wrong damage type for the equipped gun, backpack full), then a small item model, then the VLM. `PROMPT.md`'s rule of matching the gun's AD/AP type to the gear is a ready-made tier-1 rule.
- **Strategy input:** the VLM's build choice (glass cannon, tank, AD, AP) becomes a stat-weight vector for the item model and the VLM's combat prompt.
- **Training data:** farm logs (driver commands, `[INSPECT]` lines, loot events) parsed into `(item, character state, VLM decision)` rows for the item model.
- **Labels by branching:** `SAVE` before an item decision, `LOAD` and play out each choice with the runner driving, and compare outcomes.
- **During training,** stand-ins answer CONSULTs: rules plus the item model for gear, the scripted controller for combat. The VLM answers only in eval and real runs.

## 9. Phases and acceptance

| Phase | Work | Done when |
|---|---|---|
| 0 (½ day) | `-rlenv`, per-instance pipes, `singletics`, skip wipes/ENDOOM/capture | 8 headless instances answer `VERSION` on their own pipes, faster than 35 tics/s |
| 1 (1 day) | `STATE` line and auto-emit, frame to shared memory, `DiabloDoomEnv` | `check_env` passes; a random agent runs 10,000 decisions without wedging |
| 2 (1½ days) | Action allowlist, explore strafes, rewards, group resets, GRPO trainer | GRPO with the feedforward policy raises sectors visited on save slots it never trained on |
| 3 (1 day) | NVTX + `nsys` profiling of the per-tic loop | Time per tic split into the pipe round trip, the policy's forward pass and the update, for 1 vs 8 vs 32 instances |
| 4 (likely, with per-tic decisions) | `libdiablodoom.so` with `dd_create` / `dd_reset` / `dd_step` / `dd_destroy`, pybind11 | Pipe round trip gone from the profile |
| 5 | Item deferral (§8) | Items flow to the VLM at CONSULT; item model trained on farm logs |
| 6 | Session fine-tuning with PPO (§7.3) | A version fine-tuned on one session beats the shipped one on the eval set; one build change passes the eval gate end to end |

Phases 0–3 are the first trainable env, about 4½ days.

## 10. Validation

- **Determinism:** same slot, seed and command list, run twice: final `STATE` lines match byte for byte; `STATEHASH` matches mid-episode. GRPO depends on this.
- **Fast equals real time:** the same command list with and without `-rlenv` ends in the same state. Suspects if not: `T_TickCadence` cooldowns, the replay encoder, anything reading `I_GetTime` or `time(NULL)`. Known wall-clock reads as of `c0e8fedb`: the idle Zug Zug timer (`I_GetTimeMS`, sound only, suppressed under `-rlenv`), the Zug Zug date gate (`time(NULL)`, sound only), and `SURVEY` filename timestamps (not a policy action). None touch game state.
- **Env contract:** `check_env` passes; shapes and dtypes match the declared spaces; the observation contains nothing from the state pipe.
- **Throughput:** random-agent decisions (tics) per second for 1, 8 and 32 instances, and the share of each tic spent in the pipe round trip.
- **Policy checks:** report exit rate and time to exit on held-out maps.

## 11. Risks

| Risk | Why it matters | Mitigation |
|---|---|---|
| Hidden wall-clock dependencies | Fast runs diverge from real play | Fast-equals-real-time check; grep for `I_GetTime`, `time(NULL)` |
| Long rollouts in decisions | Up to 2,100 per-tic steps per rollout dilute one advantage over many near-identical steps | Process-reward variant; PPO with a critic; a larger minibatch keeps updates small (watch the KL) |
| Pipe round trip per tic | With a decision every tic, the pipe and IPC cost is paid 35 times per game second per instance | Measure in Phase 3; Phase 4's in-process library is likely needed |
| Exploration trained without combat | The runner may walk into fights the VLM then has to win | Third stage (open) |
| Sparse exit reward | Exits are rare | Dense sector reward; slots near exits first |
| `P_Random` is a 256-entry table | Variety comes from start states, not the RNG | Many save slots across maps and NG+ loops |
| GRPO groups collapse | Identical rollouts give zero gradient | Entropy bonus; skip zero-std groups; log the collapsed share |
| No critic during GRPO | One advantage per rollout, coarse credit | Process-reward variant; PPO baseline on the same env if curves stall |
| Pixels-only loops | The policy revisits rooms or spins | GRU memory, the time-and-distance penalty, the look reward |
| Rendering cost | The renderer runs every tic, every tic being a decision | `-noblit` (skip the SDL blit); Phase 3 measures what is left |
| HUD churn | Every HUD change shifts the policy's input — including world-space overlays (enemy health bars, floating damage numbers, depth scaling, added after `5fa94f8`) | Eval gate per build; randomize HUD details in training if churn continues |
| CONSULT reward farming | Large rewards get exploited | Grace window, first consult per trigger, stand-in clears the trigger, small cost without a trigger |
| CPU-bound on one box | 32 single-threaded instances want about 32 threads | Size instance count from the Phase 3 table; keep repo, pipes and `/dev/shm` on the Linux filesystem under WSL2 |

## 12. Decisions and open questions

Decided:
- Exploration trains with `-rlnocombat` (monsters removed), then a monsters-on stage for CONSULT.
- Training runs on the RTX 5080 box.
- G = 8, 2,100 tics per rollout.
- Training uses every IWAD under `wad/` (Freedoom Phase 1 and The Ultimate Doom), groups split evenly between them (§7.1).
- Training runs as one process on one box (`train.py`); the split worker/learner trainer was removed 2026-11-14 (§7.5).
- CONSULT rewards and penalties as in §5.3; the item trigger is a unique picked up, cued by a 3.5 s gold border around the avatar (2026-11-18; a screen flash from 2026-11-17; before, a backpack-full message on a failed pickup).
- CONSULT is an environment signal that drives Muse VM; the game cannot hand off by itself. The policy is taught to switch to combat or inventory mode with it, and the monster reward is paid only for sightings while exploring. The policy must choose CONSULT itself in every case — including after combat has started, where it earns no reward but each step without it is penalized. No automatic consult, no mode mask.
- The status bar stays in the policy's view.
- Phase 6 uses PPO on logged sessions, no live search. The critic gets HP and exit path distance.

Open:
- Third stage: fine-tuning with the scripted combat stand-in and an HP-loss penalty. Kept in the plan; details later.
- Reward magnitudes (§5.3).