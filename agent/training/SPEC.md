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
| `WAYPOINT <x> <y>` / `WAYPOINT` (2026-11-18) | One point in map units, drawn in the scene until the player comes within 64 units (then a `waypoint_reached` event): a white, black-outlined pillar shown through walls (dashed behind one), or an arrow at the screen edge while it is out of view. The state line carries `waypoint`. The VLM sets it in production (from a `MAPSHOT`); the env in training. |
| `MAPSHOT` (2026-11-18) | Draws the automap and saves it as a PNG within one tic, then closes it: the policy's frame never shows the map. Zoomed to fit the player's sector (its lines within 1024 units, with a margin), with the waypoint drawn as a white square. The `mapshot` event gives the file and the pixel-to-map numbers (`x0`, `y0`, `upp`, `h`, in 320x200 frame pixels). |
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
- Downsampled to 84×84, last 4 frames stacked.
- Nothing from the state pipe.

### 5.2 Actions

Discrete, 29 outputs, one decision every game tic (2026-11-15): each action is the keys held for the next tic, the way a keyboard is played. The env rejects anything else.

| # | Action | Notes |
|---|---|---|
| 0–26 | `PRESS <keys>` | Every combination of one key or none from each pair: W/S run forward/back, A/D strafe left/right, L/R turn left/right (`PRESS`, `PRESS L`, ..., `PRESS WAL`, ..., `PRESS SDR`; 0 is `PRESS` alone, nothing held). `G_BuildTiccmd` adds the keys to the ticcmd, so the player moves exactly as with those keys held: run speed, strafe-running, the run turn rate (about 7° a tic). Explore only |
| 27 | `USE` | Doors and switches, including exits |
| 28 | `CONSULT` | Environment signal, not a driver command: it drives Muse VM to make a decision — switching to combat or inventory mode. The handler is swappable: on the farm it signals Muse VM (e.g. `doom_loop.sh` exits so Muse's VLM takes over); in training the env forwards every CONSULT to the game as the `-rlenv` `CONSULT` driver command (§4), false ones included, and a stand-in (§5.4) does the rest. |
Everything else stays with the LLM/VLM or the harness: the engine's multi-tic movement (`WALK` in every form, the 45° `TURNL`/`TURNR`, `STRAFE_L`/`STRAFE_R`), `WAIT`, `COLLECT`, `SNAP_WALL` (aims from engine geometry), bare `WALK` (stops on engine detection), `SURVEY`, `GOTO`, the combat commands (`MOVE_FWD`, `MOVE_BACK`, `ATTACK`, `UNDO`, `END`), gear (`EQUIP`, `UNEQUIP`, `SALVAGE`), read-outs (`INSPECT`, `INVENTORY`, `KEYS`, `SECTORS`, `SCREENSHOT`, `CAPTURE`, `REPLAY`, `VERSION`) and session control (`SAVE`, `LOAD`, `RESPAWN`, `QUIT`).

The policy plays per tic (2026-11-15): no multi-tic commands, and no angled walks (`WALK <angle>` snaps the facing in a single tic).

No per-state action masks (they would leak engine state). No mode mask either: the policy keeps acting in combat, where every action except CONSULT does nothing and just costs a step (the env drops movement actions outside EXPLORE so they never queue TP-costing combat moves). The policy has to learn from the screen that combat has started and choose CONSULT. While the VLM/stand-in is playing a CONSULT, no policy steps are taken.

### 5.3 Rewards

The reward is computed by the env (`env/env.py`, `DiabloDoomEnv.step`) from the engine's state line and events (§4); none of those fields is a policy input -- the policy acts from the frames alone. Every step returns the sum of the terms below and, in `info["terms"]`, each term's share (logged per rollout in `rollouts.jsonl`). The magnitudes live in `RewardConfig`; the clock-tied ones are set by `model.make_env` from `--time-penalty`. A step is one decision, one game tic under per-tic decisions (more for a CONSULT the stand-in plays out).

Notation: `T` is the time penalty's base rate per tic, `--time-penalty` / 35 (default 1/3 point per game second, so `T` = 1/105). "Rollout" means from the reset (after any warm-up) to the end; once-per-rollout memory is cleared at each reset.

#### Progress

| Term | Value | Rule |
|---|---|---|
| `sector` | +15 per room | Each sector visited for the first time this rollout, by the engine's visited-sector count -- except door sectors (2026-11-17): the env counts `sectors − door_sectors` (`rooms()`), so walking through a door pays once, for the room behind it. Sectors visited at the start (a progress save's visited list, the warm-up) never pay. A sector is any area with its own floor, ceiling or light, so a stair step or a lit patch counts too; each pays once. |
| `exit` | +100 | Level completion triggered (exit switch or line). Ends the rollout. |
| `key` | +25 per colour | A key colour (blue, yellow, red; card and skull alike) first held this rollout, from the state line's `keys` mask. Keys held at the start never pay. In training a new key also ends the rollout (the key goal); the held-out eval doesn't stop for it. |
| `mechanism` | +10 | A USE that sets a mechanism going -- a sector starts moving (door, lift, floor, stairs) or a switch flips -- from the engine's `use` event. Once per mechanism per rollout; ones used in the warm-up count as used. |
| `loot` | +5 per item | A Diablo item into the backpack, or armor (which grants one), from the `pickup` event. Practice items (`-rlitems`) pay the same. Something left on the ground (full backpack) pays nothing. |
| `item` | +0.01 per unit | While a practice item lies (from its `item` event, 10 s or until picked up), each map unit closed toward it; moving away costs the same, so circling nets nothing (`item_approach`, 2026-11-17). |
| `health` | +0.5 per HP (+0.2 before 2026-11-18) | The HP a health pickup actually gave (the `pickup` event's `hp`): a medikit at 75 HP pays 12.5. The mirror of `hp_loss`. |

#### Time, distance and direction

| Term | Value | Rule |
|---|---|---|
| `time` | −`T` × (1 + d / 256) per tic | The clock: every game tic of the rollout. d is the distance to the way on (`goal_dist()`), the mean of the step's start and end: the walking distance to the nearest unvisited room, the state line's `path_dist` (2026-11-17: Dijkstra on a 32-unit grid of where the player can stand and step, built per level load by the `GOTO` pathfinder's rules, through doors the player can open -- manual ones, locked ones with the key); with no path, the straight distance to the nearest portal of the current sector, `portal_dist` (doors and lifts first, 2026-11-17); with neither, 0. So the clock runs at `T` beside the way on and twice that 256 units off: being far costs more the longer it lasts. It only bites with a rollout-ending goal (the portal goal, below): rollouts that all run to the limit pay alike, and the group-relative advantage cancels that. |
| `fast` | +`T` × forward share per fast tic | Each game tic on which the player moved at least `fast_speed` (4) units (position, not momentum: pushing into a wall never counts; the engine's `fast_tics`, `-rlfast`), scaled by max(0, cos b), b the angle between the movement and the way on (`fast_forward`; `fast_path`, 2026-11-17: the step-start `path_rel`, else `portal_rel`; with neither, the facing): running along the way pays back the clock's base, running across it -- a circle's sides, a detour -- strafing off it or backing up pays nothing. Moving well is nearly free; standing still, bumping into walls or backing up pays the whole clock. |
| `look` | +`T`/2 × max(0, cos a) per tic | Facing the way on. a is `way_rel()`: the state line's `path_rel` (2026-11-17), the angle to the farthest of the next 4 path cells the player can walk to in a straight line, so it turns with a corridor and never points into a wall; with no path, `portal_rel`, the angle to the nearest portal of the current sector into an unvisited sector (doors and lifts first); with neither, nothing. Half the clock, so it never pays for standing still. |

#### Movement habits

| Term | Value | Rule |
|---|---|---|
| `wall_look` | −`T` per tic | Each tic the first line ahead the player can't pass is nearer than 48 units (`wall_dist`): a view of bare wall shows nothing. Not while a usable wall is in reach (`wall_use`: a switch, a pressed one-shot switch, a closed door -- a locked one only with its key -- or a remote door's face, within USE range ahead or 45° off it). |
| `turn_waste` | −0.005 per 45° | Turning that gets undone: over the last 8 tics, Σ\|Δθ\| − \|ΣΔθ\|, charged as it accrues. A steady turn either way, and holding still, cost nothing; a teleport resets the window. |
| `move_flip` | −0.015 | W within 4 tics of S, or S within 4 of W. Strafing back and forth is free. |
| `fall` | −10 per fall (−3 before 2026-11-18) | An unbroken descent that at some point drops faster than 10 units a tic (freefall; the fastest lift lowers 8) and totals at least 64 units, charged once on landing whatever the height. Going up, stairs and lifts never cost; a teleport is no fall. From the state line's `z`. |

#### USE

| Term | Value | Rule |
|---|---|---|

#### CONSULT

Triggers, while exploring: a **monster in view** that hasn't acquired the player (the state line's `monstervis`; practice monsters from `-rlspawn` make these common), a **unique item picked up** (2026-11-17: the `pickup` event's `tier` 4; the engine draws a gold border around the avatar (status-bar face) for 3.5 s, fading out over its last half second (2026-11-18; magic, rare and set pickups get one too, in blue, yellow and green, so in grayscale the unique's is told apart by brightness alone; a screen flash before), through the 3 s window, the cue the policy can see -- it replaced the full-backpack trigger, whose only cue was a few pixels of text), and **combat** (the world is frozen until CONSULT).

| Term | Value | Rule |
|---|---|---|
| `consult` (hit) | +5 | The first CONSULT on a live monster or unique trigger. A CONSULT in combat pays nothing itself but avoids the miss. |
| `consult` (miss) | −2.5 (−8, then −12, before 2026-11-18) | A trigger left unanswered through its grace: 105 game tics (3 s) from a monster's sighting or a unique's pickup (its border stays on through it), 5 steps in combat. Charged step by step while it waits, −12 over the whole window (2026-11-17; once at the end before), so each frame without a CONSULT gets its share; a CONSULT stops it. A monster trigger stays live through its grace after the monster leaves the view (2026-11-17: turning away used to end it with no penalty, so the miss almost never fired), and a CONSULT then still pays the hit. |
| `consult` (false) | −2.5 (−0.5 before 2026-11-18) | A CONSULT with no live, unanswered trigger (−1.5 before 2026-11-17: a CONSULT then only paid off with a trigger live more than 23% of the time, so the policy stopped consulting; −0.3 before 2026-11-18: the policy consulted more everywhere, never more on a trigger). |

#### Damage and death

| Term | Value | Rule |
|---|---|---|
| `hp_loss` | −0.5 per HP (−0.2 before 2026-11-18) | Any HP lost (stage 1 has no map monsters, but nukage, slime, crushers and barrels still hurt). Healing only moves the mark up. |
| `death` | −100 | Ends the rollout; as big as the exit, on top of the HP lost on the way. |

#### How a rollout ends

| End | Kind | When |
|---|---|---|
| `exit` | terminated | The level is completed (+100). |
| `death` | terminated | The player dies (−100). |
| `key` | terminated | Training only: a new key colour (+25). |
| `portal` | terminated | Training only, `--portal-goal n` (default 1): n new rooms entered (each +15). No bonus of its own; it is what makes the clock matter, and with frontier starts the agent advances room by room. |
| `time` | truncated | `--max-tics` game tics (default 525, 15 s); the held-out eval runs `--eval-max-tics` (4200, 120 s; 2100 before 2026-11-18) with no portal or key goal, so its sectors compare across versions. |
| `combat` | truncated | 20 steps in combat. |

The return is the plain sum over the rollout. GRPO compares rollouts within a group that started from the same state (§7.1, `learn.group_advantages`): with `--advantage outcome` (the default) every step of a rollout gets its return's z-score within the group; with `--advantage process` each step's reward is z-scored against all the group's step rewards and summed from that step to the end (reward-to-go); the one-off terms (`RewardConfig.immediate`: `consult`, `move_flip`, `turn_waste`, `wall_look`; the env's `info["reward_now"]`, 2026-11-17) judge only the step's own action, so they are left out of that sum and added, divided by the same group spread, to their own step's advantage alone, and the value targets leave them out (§7.1, "Process advantage"). A group whose rollouts all score alike carries no signal and is dropped ("collapsed"). `--norm-adv` (default on) then divides the batch's advantages by their standard deviation, so batches of different lengths update alike.

#### Design notes

- **Only what the policy can perceive is rewarded as a behaviour.** It sees 4 stacked grayscale frames (default 128×80), so a cue must survive that: hence the unique border rather than a status message.
- **The clock is the backbone.** `fast`, `look` and `wall_look` are tied to `T`, so the shaping terms only ever trade against time: none of them can pay for standing still.
- **Shaping state is reward-only.** `path_dist`, `path_rel`, `portal_dist`, `portal_rel`, `wall_dist`, `wall_use`, `door_sectors`, `monstervis` and the events are never in the observation.
- **No combat or stuck term:** stage 1 has no map monsters (a long combat ends the rollout instead), and the clock already charges standing still.
- Magnitudes are tuned from training curves; watch for a term growing in `rollouts.jsonl` without progress following (e.g. `loot` farming against `sector`).

### 5.4 Episodes, rollouts and resets

- **Rollout:** up to 2,100 game tics (60 s) from one start state, and with a decision every tic up to 2,100 decisions. Ends early on death or exit. Produces one score R_i.
- **Unconsulted-combat cap:** the world is frozen in PLANNING, so steps there spend no tics and the 2,100-tic budget never runs out. After M policy steps in PLANNING without CONSULT, the rollout ends (with the per-step penalties already accrued).
- **Reset:** `LOAD n` from prepared save slots spread across each level, then `SEED s` to set `prndindex` (§4). With a 256-entry table a seed is one of 256 start offsets; variety comes mostly from slots. Slots near the exit come first in training. Some slots start with the backpack one or two items from full.
- **Frontier starts:** a rollout ends at 2,100 tics, so from the map start alone training never sees past the first minute of a level. After each batch, per (IWAD, map), the highest-return rollout that ended alive, in EXPLORE, short of the exit and with progress (a new sector, or a new key: the key goal ends the rollout there) saves the game where it stopped to slot 5 (`SAVE 5`; slot 4, the start every group loaded, is never overwritten mid-batch) with its visited sectors (`VISITED`, replayed by `VISIT` after each load); only that highest-scoring rollout is saved. The save is copied into slot 5 of every instance on that map and slots 4 and 5 swap, so the progress is the next batch's start in slot 4, the same for every member of a group. A batch without progress keeps the start and grows that map's rollout limit (`TicBudget`: x`--tics-growth` 1.5 from `--max-tics` up to `--max-tics-cap` 2100; progress, an exit or a cleared frontier puts it back to `--max-tics`), so the agent gets time to find the path while a new sector or key stays the only progress; `--frontier-patience` (5) such batches in a row at the full budget clear it; the saves and tic budgets are never written to disk (2026-11-15): a new run starts every map from its beginning, and only the map rotation resumes, from the checkpoint's iteration (a save only loads in the exact IWAD file it was made with, and machines may have different releases). An exit clears the map's frontier (the batch's other rollouts still run to their own ends), so slot 4 holds the map start again and the level is replayed from the start. `--no-frontier` turns it off (map starts only, still slot 4).
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
  - the **one-off** part n_t: the terms that judge only the action taken on that step (`RewardConfig.immediate`: `consult`, `move_flip`, `turn_waste`, `wall_look`). The env reports it per step as `info["reward_now"]`, and the rollout keeps it as `rew_now`.
  - the **chained** part c_t = r_t − n_t: everything else (`sector`, `fast`, `look`, `time`, `item`, `loot`, `health`, `key`, `mechanism`, `exit`, `death`, `hp_loss`, `fall`), which comes from a run of decisions, so earlier steps share its credit.

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
  | `hp_loss` | chained | Damage comes from where earlier steps took the player (nukage, crushers). |
  | `fall` | chained | Charged on landing, for a drop the player walked off steps earlier. |
  | `consult` | one-off | A hit (+5) or a false CONSULT (−2.5) judges that CONSULT alone; the miss is charged on each step without a CONSULT while the trigger waits. Whether the trigger is on screen is not the policy's doing, so no earlier step earned it. |
  | `move_flip` | one-off | Forward right after back (or back after forward): it judges this step's key against the last one. |
  | `turn_waste` | one-off | Turning back against the recent turns: it judges this step's turn. |
  | `wall_look` | one-off | Staring at a wall up close, charged per tic of the step: the next step can turn away. |

  A new term must be put in one stream or the other: the env counts it as chained unless it is added to `RewardConfig.immediate`.

  ```
  σ   = std of every r_t in the group             (the collapse test, as before)
  μ_c = mean of every c_t in the group
  G_t = Σ_{u ≥ t} (c_u − μ_c) / (σ + ε)
  σ_G = std of every G_t in the group
  A_t = G_t / (σ_G + ε)  +  n_t / (σ + ε)
  ```

  The chained part is summed from each step on, as before, then scaled to unit std over the group (2026-11-18). Its spread grows with rollout length, since each G_t sums every step after it: at std ≈ 8, a CONSULT hit's +5 was only about 0.3 of it, and the policy's P(CONSULT) didn't depend on a monster being on screen. The one-off part counts on its own step only, on the same scale as the chained part and with no mean shift: most steps have none, and z-scoring such a sparse stream on its own would blow a single hit up. The value targets use only the chained part, since the one-off part is already credited where it happened. `--advantage outcome` is unchanged: it ranks whole returns, which include both.

  Why: summed into the reward-to-go, a one-off reward moved every earlier step's advantage too. A CONSULT's +5 also credited the steps just before it, including the frames where a monster was in view and the policy did something else, so consulting late, or spamming CONSULT, scored about the same as consulting on sight. Measured at iteration 592, before the split: the CONSULT rate was 2.4% with a monster in view and 2.5% without. That's also why the CONSULT miss penalty (§5.3) is charged step by step over the grace window, −12 in all, instead of once at its end: each frame without a CONSULT carries its own share, on that step alone.
- **Loss:**

  ```
  L = −(1/Σ|o_i|) Σ_i Σ_t min(ρ_i,t·A_i, clip(ρ_i,t, 1−clip_coef, 1+clip_coef)·A_i) + β·KL(π_θ ‖ π_ref)
  ρ_i,t = π_θ(a_t|s_t) / π_old(a_t|s_t),   clip_coef ≈ 0.2
  ```

  A few epochs per batch. β = 0 to start (no reference policy). If the farm LLM's explore commands (paired with its frames) are behavior-cloned first, that clone becomes π_ref with β > 0.
- **Group diversity:** `P_Random` is deterministic, so only action sampling separates rollouts. Use a small entropy bonus. Drop zero-std groups and log the collapsed share. Read the logged entropy against the uniform policy's, ln 29 ≈ 3.37 for the 29 actions (§5.2).
- **Trainer:** a PPO-style clipped-ratio trainer with the GAE replaced by group advantages and group resets added. It runs as one process on one box (`train.py`; the split rollout-worker/learner trainer was removed 2026-11-14, §7.5).
- **Data:** mostly the game's own levels (they stay fixed) across both IWADs, so the runner sees two texture sets and two layouts of each map slot, plus filtered community maps so it isn't brittle. Randomize textures, player speed and turn rate. Optional early stage: generated mazes with a known shortest path.

### 7.2 Training stages

1. `-rlnocombat` (no map monsters): exploration, plus CONSULT on `-rlspawn` practice sightings.
2. Map monsters on: CONSULT on sighting, and on combat started without one.
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