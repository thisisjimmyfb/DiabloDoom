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
| Automap | `AM_Start` / `AM_Stop` / `AM_Drawer` in `src/doom/am_map.c` | `MAP` toggle |
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
| `MAP [tics]` command (new) | Opens the automap via `AM_Start`, as Tab does, for <tics> game tics (default 18, 0.5 s; clamped to 1–350), then closes it (`AM_Stop`); `MAP` while it is open closes it at once. Training never sends it (no `MAP` action, no forced glances). Follow mode, fixed zoom. Explore only (combat hides the map on entry; `MAP` in combat does nothing). Movement keeps working with the map open. |
| Automap rotation | Rotate the automap to the player's heading (vanilla points north). |
| Explore-mode strafes | Today `STRAFE_L/R` queue a TP-costing turn move that only previews until END TURN. Under `-rlenv` in EXPLORE they take a fixed sideways step without turning, like `WALK <angle>`. |
| `-rlnocombat` | Removes every monster at level load. Reuse the existing `nomonsters` flag (`d_main.c`) rather than new removal code. |
| Mechanism use events | Under `-rlenv`, every player USE that sets something going sends `{"event":"use","sectors":[...],"lines":[...]}`: the sectors that gained an active special (doors, lifts, floors, ceilings, stairs) and the switch lines flipped. A use that does nothing (a wall, a locked door without its key, a door already moving) sends nothing. Feeds the mechanism reward (§5.3). |
| Pickup events | Each thing the player picks up sends `{"event":"pickup","kind":K,"hp":N,"gametic":N}` from `P_TouchSpecialThing` (`RL_NotePickup`), K one of loot, armor, health, key, other: loot is a Diablo item into the backpack, `hp` the health the pickup gave. A thing left on the ground (full backpack, full health) sends nothing. |
| `-rldrive` | Control and output only, for driving a normal, real-time game with the policy (farm play, §7.3): the driver on `-cmdpipe`, the JSON lines on `-statepipe`, the frame in `-shm`, and decision points published without blocking (one per idle period, plus one when combat starts, the player dies or the level ends while idle). No uncapped tics and none of the `-rlenv` suppressions. `-rlenv` implies it. |
| `MONSTERVIS` (state field) | Whether the player can see a monster that has **not** acquired the player, while in EXPLORE. Precise test: `P_CheckSight` to the monster, inside the player's horizontal FOV, and the sprite at least K pixels tall on the 320×200 screen (so a speck on the horizon doesn't count). Rewards only (§5.3); never in the observation. Implemented as `monstervis`: a living monster among the visible targets within the 90° view (no sprite-size test yet). Next to it, `backpack_full` (a pickup failed on a full backpack in the last 4 s, and it is still full) and `bp_count`. |
| `-rlspawn <tics>` | CONSULT practice. Detection has no range limit, so on the map a monster almost always acquires the player the moment the player sees it, and a sighting before combat almost never happens. With `-rlspawn`, explore spawns one monster at a time on screen: in line of sight, 128–640 units ahead, within 30° of the view (no room there, no spawn that time). It faces away and deaf, so it stays frozen: a sighting to CONSULT about. It never triggers combat: if it would acquire the player (the player walks into its front arc), it is removed instead, and it never counts as a combatant. Mean `<tics>` game tics between spawns; each is removed 5 s after spawning, and one in a loaded save is removed at once (practice monsters carry a flag the savegame keeps). Rolls use a private RNG seeded by `SEED`, so episodes stay deterministic. Works with `-nomonsters`. |
| `CONSULT` (driver command, `-rlenv` only) | The training stand-in's hand-off (§5.4): removes every monster in sight and, in combat, every monster targeting the player. A spawned practice monster disappears (no kill, no loot); a map monster is killed (kill count and drops, as if the VLM had fought it). |
| Backpack-full status message | Shown on a failed pickup. Its own fixed screen spot, size and colour that survive an 84×84 downsample, fixed duration. Today's `D_BackpackFullMsg` → `D_Msg` uses the shared HUD message line in small text and is overwritten by later messages. |
| Exit path distance | Engine pathfinder distance from the player to the exit, locked doors blocked unless the key is held, capped when no path exists. Critic input only (§7.3). |
| `ITEM` event | Emitted from `D_BackpackAddDur`: tier, item index, durability, rolled stats, backpack index. |

## 5. Environment interface

### 5.1 Observation

- The screen as displayed: the scene, status bar kept. Training never shows the automap (no `MAP` action, no forced glances; §12).
- Downsampled to 84×84, last 4 frames stacked.
- Nothing from the state pipe.

### 5.2 Actions

Discrete, 29 outputs, one decision every game tic (2026-11-15): each action is the keys held for the next tic, the way a keyboard is played. The env rejects anything else.

| # | Action | Notes |
|---|---|---|
| 0–26 | `PRESS <keys>` | Every combination of one key or none from each pair: W/S run forward/back, A/D strafe left/right, L/R turn left/right (`PRESS`, `PRESS L`, ..., `PRESS WAL`, ..., `PRESS SDR`; 0 is `PRESS` alone, nothing held). `G_BuildTiccmd` adds the keys to the ticcmd, so the player moves exactly as with those keys held: run speed, strafe-running, the run turn rate (about 7° a tic). Explore only |
| 27 | `USE` | Doors and switches, including exits |
| 28 | `CONSULT` | Environment signal, not a driver command: it drives Muse VM to make a decision — switching to combat or inventory mode. The handler is swappable: on the farm it signals Muse VM (e.g. `doom_loop.sh` exits so Muse's VLM takes over); in training a stand-in answers (§5.4) with the `-rlenv` `CONSULT` driver command (§4). |

Older checkpoints are upgraded on load: each old output keeps its weights in the place of the action it carries over to (`WALK 0` → `PRESS W`, `TURNL`/`TURNR` → `PRESS L`/`R`, `STRAFE_L`/`STRAFE_R` → `PRESS A`/`D`, `USE`, `CONSULT`; `MAP` dropped), the new outputs start fresh.

Everything else stays with the LLM/VLM or the harness: the engine's multi-tic movement (`WALK` in every form, the 45° `TURNL`/`TURNR`, `STRAFE_L`/`STRAFE_R`), `WAIT`, `COLLECT`, `SNAP_WALL` (aims from engine geometry), bare `WALK` (stops on engine detection), `SURVEY`, `GOTO`, the combat commands (`MOVE_FWD`, `MOVE_BACK`, `ATTACK`, `UNDO`, `END`), gear (`EQUIP`, `UNEQUIP`, `SALVAGE`), read-outs (`INSPECT`, `INVENTORY`, `KEYS`, `SECTORS`, `SCREENSHOT`, `CAPTURE`, `REPLAY`, `VERSION`) and session control (`SAVE`, `LOAD`, `RESPAWN`, `QUIT`).

The policy plays per tic (2026-11-15): no multi-tic commands, and no angled walks (`WALK <angle>` snaps the facing in a single tic).

No per-state action masks (they would leak engine state). No mode mask either: the policy keeps acting in combat, where every action except CONSULT does nothing and just costs a step (the env drops movement actions outside EXPLORE so they never queue TP-costing combat moves). The policy has to learn from the screen that combat has started and choose CONSULT. While the VLM/stand-in is playing a CONSULT, no policy steps are taken.

### 5.3 Rewards

| Term | Sign | Notes |
|---|---|---|
| New sector discovered | +15 | Per sector not yet visited: the exploration signal, worth more than a mechanism (getting somewhere new is the goal, a door only the way there). With the portal goal (training) the first one also ends the rollout. |
| Exit reached | +100 | Level complete triggered (exit switch or line): the top reward. Ends the rollout. |
| Time and distance | − | One penalty, every game tic until the rollout ends: `time` × (1 + d / `far_scale`), d the distance (map units, the mean of the step's start and end) to the nearest portal of the player's current sector (2026-11-16): a passable two-sided line into any other sector, visited or not (a step up of at most 24 and room to stand; a line with a special, a door or lift, always counts). A sector with no portal costs the base rate. Before, it was the nearer of the exit line and the nearest frontier portal. `time` is `--time-penalty` (1/3 per second), `far_scale` 256: the clock runs twice as fast 256 units from a way out of the room, so being far costs more the longer it lasts and the faster portal crossing wins its group. Needs the portal goal (rollouts that all run to the limit pay alike, which the group-relative advantage cancels). From the state line's `portal_dist` (§4), reward only: the policy never sees it. |
| Portal goal | 0 | With `--portal-goal n` (default 1): the rollout ends once it has entered n sectors not yet visited (a start from a progress save counts that save's visited sectors as visited). No bonus of its own (the sector pays; merged 2026-11-15). Lets the time penalty work, and with frontier starts the agent advances portal by portal. |
| Key picked up | +25 | Each key colour (blue, yellow, red; a keycard and a skull key of one colour are the same colour) the first time it is held in a rollout; keys held at the start or picked up in the warm-up never pay. From the state line's `keys` mask (1 blue, 2 yellow, 4 red; §4). In training a new key also ends the rollout (the key goal, `EnvConfig.key_goal`; end "key"), and that end state is progress for the frontier; the held-out eval doesn't stop at keys. |
| Mechanism used | +10 | A USE that sets a mechanism going: a sector starts moving (door, lift, floor, ceiling, stairs) or a switch flips (including lights and the exit switch). Once per mechanism per rollout, so reusing a door or switch, or using a door from its other side, pays nothing; mechanisms used during the warm-up count as used. From the engine's `use` event (§4). |
| Wasted USE | −0.05 | A USE that sets nothing going: no engine `use` event (a plain wall, empty space). Opening a door or flipping a switch, even one used before, costs nothing. About 5 tics of the clock's base rate, so spamming USE costs more than standing still (2026-11-16). |
| CONSULT on trigger | + large | Paid only while exploring (EXPLORE). Trigger = a monster seen before it acquires the player (`MONSTERVIS`, §4), or the backpack-full message up. First consult per trigger only. |
| Trigger unanswered | −8 | Applied after a grace window: 150 game tics for a monster in view or a full backpack, 5 steps in combat (frozen: no tics pass) |
| CONSULT with no trigger | −1.5 | Discourages random consults |
| High velocity | +1/105 per tic | Every game tic on which the player actually moved at least `fast_speed` (4) map units: position, not momentum, so pushing into a wall or a barrel never counts. The engine counts the tics (`-rlfast <units>`, which the env adds; the state line's `fast_tics` since the last decision). It pays back the time penalty's base rate (`--time-penalty` / 35, tied to it), so during a fast tic only the distance part of the clock is charged: moving fast is nearly free, while standing still or pushing into a wall pays the full clock. Running (`PRESS W`) passes 4 units/tic on its third tic from rest. It replaced the per-step straight-movement reward (2026-11-15): a step is one tic now, and this pays unimpeded movement tic by tic. No separate wall penalty (removed 2026-11-15): a blocked move earns no fast tics, so it pays the whole clock. |
| Looking toward a way on | up to +1/210 per tic | Every tic pays `look` × max(0, cos a), a the angle between the player's facing and the closest point of the nearest portal of the current sector into an unvisited sector (the state line's `portal_rel`, §4): full credit facing it, nothing 90° or more away, nothing when the sector has none. Only unvisited ones, because right after a crossing the nearest portal is usually the one just used, behind the player. `look` is half the clock's base rate (`--time-penalty` / 70), so facing the right way never pays for standing still (2026-11-16). |
| Wasted rotation | −0.005 per 45° | Turning that gets undone, from the state line's facing: over the last `turn_waste_window` (8) tics, wasted = Σ\|Δθ\| − \|ΣΔθ\|. Each tic adds what it wastes: a turn against the window's net turn S undoes min(\|Δθ\|, \|S\|) and wastes twice that. A steady turn either way and holding still cost nothing, a wobble costs in proportion, a small correction after a long sweep costs little; a teleport resets the window (2026-11-16). It replaced a fixed −0.015, then −0.005, per L/R key reversal within 4 tics, under which always turning one way became the free habit (81% right turns). |
| Forward/back reversal | −0.015 | The same for W and S: W pressed at most `move_flip_window` (4) tics after S, or S after W. Strafing back and forth (A/D) is free (2026-11-16). |
| Fall | −1 per 64 units | A quick large drop usually means the player left the path the map intends. A fall is an unbroken run of tics going down that at some point drops faster than `fall_speed` (8) units a tic -- only freefall does: lifts lower at most 8 a tic, stairs drop 8-24 per step with flat treads between -- and totals at least `fall_min` (64) units; on landing it costs `fall` × drop / 64 (−2 for 128). Going up never costs; a teleport is no fall. From the state line's `z`, no engine change (2026-11-16). |
| Staring at a wall | −1/105 per tic | Each tic the first line the player can't pass straight ahead is nearer than `wall_look_dist` (48) units: a view of bare wall tells the policy nothing. From the state line's `wall_dist` (§4: a trace along the facing, up to 128). Never while a usable wall is in reach (`wall_use`: a switch or closed or half-open door within USE range, either within 45° of the facing or the wall straight ahead itself), so lining up on a switch or waiting at a door is free. Equals the clock's base rate (`--time-penalty` / 35), so staring at a wall costs the clock twice (2026-11-16). |
| Loot picked up | +5 per item | A Diablo item into the backpack, or armor (green armor also grants an item). From the engine's `pickup` event (§4); a thing left on the ground (full backpack) pays nothing. |
| Health picked up | +0.2 per HP healed | Stimpack, medikit, health bonus, soulsphere, megasphere: the HP it actually gave (the `pickup` event's `hp`), so a medikit pays 5 and a +1 bonus 0.2; the mirror of HP lost. |
| HP lost | −0.2 per HP | Any cause: stage 1 has no map monsters, but passive damage (nukage, slime, crushers, barrels) still hurts. Healing pays nothing (it only moves the mark up). |
| Death | −100 | Ends the rollout; as big as the exit reward, on top of the HP lost on the way. |

No combat-before-CONSULT or stuck term: stage 1 has no map monsters (a long combat still ends the rollout), and the time penalty already covers standing still (the velocity term rewards the opposite: unimpeded movement).

Magnitudes are tuned from training curves. Start with the consult reward near a few sectors' worth, the miss penalty somewhat larger.

### 5.4 Episodes, rollouts and resets

- **Rollout:** up to 2,100 game tics (60 s) from one start state, and with a decision every tic up to 2,100 decisions. Ends early on death or exit. Produces one score R_i.
- **Unconsulted-combat cap:** the world is frozen in PLANNING, so steps there spend no tics and the 2,100-tic budget never runs out. After M policy steps in PLANNING without CONSULT, the rollout ends (with the per-step penalties already accrued).
- **Reset:** `LOAD n` from prepared save slots spread across each level, then `SEED s` to set `prndindex` (§4). With a 256-entry table a seed is one of 256 start offsets; variety comes mostly from slots. Slots near the exit come first in training. Some slots start with the backpack one or two items from full.
- **Frontier starts:** a rollout ends at 2,100 tics, so from the map start alone training never sees past the first minute of a level. After each batch, per (IWAD, map), the highest-return rollout that ended alive, in EXPLORE, short of the exit and with progress (a new sector, or a new key: the key goal ends the rollout there) saves the game where it stopped to slot 5 (`SAVE 5`; slot 4, the start every group loaded, is never overwritten mid-batch) with its visited sectors (`VISITED`, replayed by `VISIT` after each load); only that highest-scoring rollout is saved. The save is copied into slot 5 of every instance on that map and slots 4 and 5 swap, so the progress is the next batch's start in slot 4, the same for every member of a group. A batch without progress keeps the start and grows that map's rollout limit (`TicBudget`: x`--tics-growth` 1.5 from `--max-tics` up to `--max-tics-cap` 2100; progress, an exit or a cleared frontier puts it back to `--max-tics`), so the agent gets time to find the path while a new sector or key stays the only progress; `--frontier-patience` (5) such batches in a row at the full budget clear it; the saves and tic budgets are never written to disk (2026-11-15): a new run starts every map from its beginning, and only the map rotation resumes, from the checkpoint's iteration (a save only loads in the exact IWAD file it was made with, and machines may have different releases). An exit clears the map's frontier (the batch's other rollouts still run to their own ends), so slot 4 holds the map start again and the level is replayed from the start. `--no-frontier` turns it off (map starts only, still slot 4).
- **Group reset:** all G instances in a group load the same slot and seed.
- **IWAD:** each instance runs one IWAD (`-iwad`) for its whole life, so an instance's IWAD and map are fixed when it is spawned. Save slots belong to one IWAD (a save loaded under another IWAD's maps is garbage), so slots and multiple IWADs can't be mixed until slots are prepared per IWAD.
- **Training stand-ins:** a CONSULT is answered cheaply: the stand-in (`standin_consult`) sends the `CONSULT` driver command, which removes the monster (then `END` closes a combat round), or salvages the backpack until it has room. Both stages run `-rlspawn` practice monsters (`--spawn-tics`, default 700), so CONSULT on sighting is trained from stage 1.
- **`info`:** tics per step, mode, group id. (Mode is for logging and rewards only; it never masks actions.)

## 6. Policy architecture

```
screen (scene, 84×84, 4 stacked)
   → Nature CNN (3 conv) → 512 features
   → GRU core, 256 units   ──▶ action head: 29 logits, sampled
                            └▶ value head (+ HP, exit distance)  [critic, training only]
```

- **GRU** carries working memory: where the player has been and what is no longer on screen. GRU rather than LSTM because it is simpler (one state vector instead of two, fewer parameters); they perform about the same.
- **Actions are sampled**, not greedy, which is what makes a GRPO group's rollouts differ.
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

  Every decision in rollout i gets A_i. Fallback if learning is slow: the process variant (normalize per-step rewards across the group, sum from each step onward).
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
- The automap counts as screen visual, but training never shows it: no `MAP` action, and no forced glances (removed 2026-11-16: the policy's 4 frames of memory can't hold a glance).
- Training runs on the RTX 5080 box.
- G = 8, 2,100 tics per rollout.
- Training uses every IWAD under `wad/` (Freedoom Phase 1 and The Ultimate Doom), groups split evenly between them (§7.1).
- Training runs as one process on one box (`train.py`); the split worker/learner trainer was removed 2026-11-14 (§7.5).
- CONSULT rewards and penalties as in §5.3; the backpack trigger is a dedicated backpack-full message on a failed pickup.
- CONSULT is an environment signal that drives Muse VM; the game cannot hand off by itself. The policy is taught to switch to combat or inventory mode with it, and the monster reward is paid only for sightings while exploring. The policy must choose CONSULT itself in every case — including after combat has started, where it earns no reward but each step without it is penalized. No automatic consult, no mode mask.
- The status bar stays in the policy's view.
- Phase 6 uses PPO on logged sessions, no live search. The critic gets HP and exit path distance.

Open:
- Third stage: fine-tuning with the scripted combat stand-in and an HP-loss penalty. Kept in the plan; details later.
- Reward magnitudes (§5.3).