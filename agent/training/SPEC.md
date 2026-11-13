# DiabloDoom RL Environment — Spec

Status: draft, 2026-11-12. Source plan: "DiabloDoom Gymnasium Integration Plan" (Claude Doc). Codebase re-checked at commit `c0e8fedb` (2026-11-12).

## 1. Goal and scope

Wrap DiabloDoom as a Gymnasium environment that runs headless, many instances at once, faster than real time. A small policy learns to **run levels** (explore and find exits) from the screen alone. An LLM/VLM plays the turn-based combat and makes gear decisions.

| In scope for the policy | Out of scope for the policy |
|---|---|
| Moving, turning, strafing, using doors and switches | Combat turns (LLM/VLM) |
| Toggling the automap | Gear: equip, unequip, salvage (LLM/VLM plus item cascade) |
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
- **Event-driven steps.** The engine answers only when a decision is due, so one `env.step()` can cover one tic or a long walk.
- **CONSULT is the only route to the LLM/VLM.** The game cannot hand off by itself; CONSULT is an environment signal that drives Muse VM to make a decision (combat or inventory). The policy must choose CONSULT itself — nothing sends it automatically. That includes when combat starts before the policy consults (a monster acquired the player first): the game stays in PLANNING until the policy recognizes combat from the screen and chooses CONSULT. Control returns to the policy back in EXPLORE.
- **In-process library** (`libdiablodoom.so` + pybind11) only if Phase 3 profiling shows pipe overhead matters.

## 4. Engine changes

| Change | Detail |
|---|---|
| `-rlenv` | Enables the FIFO driver without `D_OnMuseVM()`. Sets `singletics`; skips wipes, ENDOOM and continuous capture. Suppresses the on-screen `CMD <command>` echo (it would put the policy's last action into its own observation and churn the HUD message line) and the wall-clock idle Zug Zug timer (`D_UITickIdle`, `I_GetTimeMS`). |
| `SEED n` | Sets `prndindex` (the `P_Random` index, 0–255) right after `LOAD`. Required: savegames don't store `prndindex` and `G_DoLoadGame` doesn't clear it (`M_ClearRandom` runs only on a new game), so without it the RNG position after a load depends on the process's history. `P_Random` also rolls all Diablo loot, affixes and defense checks. |
| `-cmdpipe <path>`, `-statepipe <path>` | Per-instance pipes (today the FIFO is hardcoded to `/tmp/doom_cmd.txt`). |
| `STATE` command and auto-emit | One JSON line: mode, TP, queue, HP, position, angle, weapon kit and magazine, loot prompt, up to 9 targets (type, HP, distance, relative angle), kill/damage/sector counters. Emitted automatically when a decision is due: in EXPLORE with the driver idle, and on entering PLANNING (the policy must then choose CONSULT, §3). |
| Frame to shared memory | At each decision, copy `I_VideoBuffer` (320×200, 8-bit) and the palette. No PNG, no SDL surface. |
| Render only at decisions | Skip `D_Display` on tics between decisions. |
| `STATEHASH` command | Hash of player, mobj and turn-controller state, for determinism checks. |
| `MAP` command (new) | Toggles the automap via `AM_Start` / `AM_Stop`. Follow mode, fixed zoom. |
| Automap rotation | Rotate the automap to the player's heading (vanilla points north). |
| Explore-mode strafes | Today `STRAFE_L/R` queue a TP-costing turn move that only previews until END TURN. Under `-rlenv` in EXPLORE they take a fixed sideways step without turning, like `WALK <angle>`. |
| `-rlnocombat` | Removes every monster at level load. Reuse the existing `nomonsters` flag (`d_main.c`) rather than new removal code. |
| `MONSTERVIS` (state field) | Whether the player can see a monster that has **not** acquired the player, while in EXPLORE. Precise test: `P_CheckSight` to the monster, inside the player's horizontal FOV, and the sprite at least K pixels tall on the 320×200 screen (so a speck on the horizon doesn't count). Rewards only (§5.3); never in the observation. |
| Backpack-full status message | Shown on a failed pickup. Its own fixed screen spot, size and colour that survive an 84×84 downsample, fixed duration. Today's `D_BackpackFullMsg` → `D_Msg` uses the shared HUD message line in small text and is overwritten by later messages. |
| Exit path distance | Engine pathfinder distance from the player to the exit, locked doors blocked unless the key is held, capped when no path exists. Critic input only (§7.3). |
| `ITEM` event | Emitted from `D_BackpackAddDur`: tier, item index, durability, rolled stats, backpack index. |

## 5. Environment interface

### 5.1 Observation

- The screen as displayed: the scene (status bar kept) or, after `MAP`, the full-screen automap.
- Downsampled to 84×84, last 4 frames stacked.
- Nothing from the state pipe. The automap shows only lines the player has seen.

### 5.2 Actions

Discrete, 8 outputs. The env rejects anything else.

| # | Action | Notes |
|---|---|---|
| 0 | `WALK 0` | A fixed step forward, no turn |
| 1–2 | `TURNL`, `TURNR` | 45° turns, smooth (5° per tic) |
| 3–4 | `STRAFE_L`, `STRAFE_R` | Needs the explore-mode branch (§4) |
| 5 | `USE` | Doors and switches, including exits |
| 6 | `MAP` | Toggle the automap |
| 7 | `CONSULT` | Environment signal, not a driver command: it drives Muse VM to make a decision — switching to combat or inventory mode. The handler is swappable: on the farm it signals Muse VM (e.g. `doom_loop.sh` exits so Muse's VLM takes over); in training a stand-in answers (§5.4). |

Everything else stays with the LLM/VLM or the harness: `WAIT`, `COLLECT`, `SNAP_WALL` (aims from engine geometry), bare `WALK` (stops on engine detection), `SURVEY`, `GOTO`, the combat commands (`MOVE_FWD`, `MOVE_BACK`, `ATTACK`, `UNDO`, `END`), gear (`EQUIP`, `UNEQUIP`, `SALVAGE`), read-outs (`INSPECT`, `INVENTORY`, `KEYS`, `SECTORS`, `SCREENSHOT`, `CAPTURE`, `REPLAY`, `VERSION`) and session control (`SAVE`, `LOAD`, `RESPAWN`, `QUIT`).

No angled walks (`WALK -90` … `WALK 90`, decided 2026-11-13): `WALK <angle>` snaps the facing in a single tic, so the policy turns only with `TURNL`/`TURNR` and always walks with `WALK 0`.

No per-state action masks (they would leak engine state). No mode mask either: the policy keeps acting in combat, where every action except CONSULT does nothing and just costs a step (the env drops movement actions outside EXPLORE so they never queue TP-costing combat moves). The policy has to learn from the screen that combat has started and choose CONSULT. While the VLM/stand-in is playing a CONSULT, no policy steps are taken.

### 5.3 Rewards

| Term | Sign | Notes |
|---|---|---|
| New sector discovered | + | Dense exploration signal |
| Exit reached | + large | Ends the rollout |
| Death | − | Ends the rollout |
| Stuck | − | No position change over N tics |
| CONSULT on trigger | + large | Paid only while exploring (EXPLORE). Trigger = a monster seen before it acquires the player (`MONSTERVIS`, §4), or the backpack-full message up. First consult per trigger only. |
| Combat started unconsulted | − per step | No positive reward for consulting once combat has started (the sighting was missed). Every step in PLANNING without CONSULT costs a penalty, so the policy learns to consult quickly; combat also counts as an unanswered trigger for the grace-window penalty below. |
| Trigger unanswered | − large | Applied after a grace window of a few decisions |
| CONSULT with no trigger | − small | Discourages random consults |
| HP loss | − | Third training stage only (open, §12) |
| `MAP` toggle | − small | Only if map flicker appears (§11) |

Magnitudes are tuned from training curves. Start with the consult reward near a few sectors' worth, the miss penalty somewhat larger.

### 5.4 Episodes, rollouts and resets

- **Rollout:** 2,100 game tics (60 s) from one start state, about 50–200 decisions. Ends early on death or exit. Produces one score R_i.
- **Unconsulted-combat cap:** the world is frozen in PLANNING, so steps there spend no tics and the 2,100-tic budget never runs out. After M policy steps in PLANNING without CONSULT, the rollout ends (with the per-step penalties already accrued).
- **Reset:** `LOAD n` from prepared save slots spread across each level, then `SEED s` to set `prndindex` (§4). With a 256-entry table a seed is one of 256 start offsets; variety comes mostly from slots. Slots near the exit come first in training. Some slots start with the backpack one or two items from full.
- **Group reset:** all G instances in a group load the same slot and seed.
- **IWAD:** each instance runs one IWAD (`-iwad`) for its whole life, so an instance's IWAD and map are fixed when it is spawned. Save slots belong to one IWAD (a save loaded under another IWAD's maps is garbage), so slots and multiple IWADs can't be mixed until slots are prepared per IWAD.
- **Training stand-ins:** with `-rlnocombat` there are no fights. In the monsters-on stage, a CONSULT is answered cheaply: the stand-in removes the monster, or empties the backpack.
- **`info`:** tics per step, mode, group id. (Mode is for logging and rewards only; it never masks actions.)

## 6. Policy architecture

```
screen (scene or automap, 84×84, 4 stacked)
   → Nature CNN (3 conv) → 512 features
   → GRU core, 256 units   ──▶ action head: 8 logits, sampled
                            └▶ value head (+ HP, exit distance)  [critic, training only]
```

- **One encoder** for both views; the network tells them apart from pixels.
- **GRU** carries working memory and remembers the view that isn't showing. GRU rather than LSTM because it is simpler (one state vector instead of two, fewer parameters); they perform about the same.
- **Actions are sampled**, not greedy, which is what makes a GRPO group's rollouts differ.
- **Value head** (one linear layer on the GRU output, plus HP and exit path distance as privileged inputs) trains from day one. GRPO ignores it; Phase 6 PPO needs it.
- **Hidden state** starts at zero for each training rollout. Phase 6 logs it at the start of each logged segment.
- **Size:** about 2 million parameters.
- **Two-step build:** step 1 is feedforward, using the map only while it is in the 4-frame stack. Step 2 adds the GRU; keep it only if it loops less or finds exits faster on held-out maps.

## 7. Training

### 7.1 GRPO (pretraining)

- **Groups:** G = 8 rollouts from the same slot and seed; 4 groups per batch = 32 instances.
- **IWADs:** training runs on every IWAD at once: Freedoom Phase 1 and The Ultimate Doom (`train.sh` passes every file under `wad/` whose header is `IWAD`; PWADs are skipped). With I IWADs and M maps, group g plays map `maps[g % M]` on IWAD `iwads[(g % M + g // M) % I]`. Every map is trained at any group count of at least M, the IWADs split the groups evenly, and I×M groups (8 for 2 IWADs × E1M1–E1M4) cover every IWAD/map pair; with fewer, the trainer warns which pairs go untrained. Eval plays every held-out map on every IWAD, and the logs report sectors and exit rate per IWAD.
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
- **Group diversity:** `P_Random` is deterministic, so only action sampling separates rollouts. Use a small entropy bonus. Drop zero-std groups and log the collapsed share.
- **Trainer:** a PPO-style clipped-ratio trainer with the GAE replaced by group advantages and group resets added. It runs either as one process on one box (`train.py`) or split into rollout workers and a learner across machines (§7.5).
- **Data:** mostly the game's own levels (they stay fixed) across both IWADs, so the runner sees two texture sets and two layouts of each map slot, plus filtered community maps so it isn't brittle. Randomize textures, player speed and turn rate. Optional early stage: generated mazes with a known shortest path.

### 7.2 Training stages

1. `-rlnocombat` (no monsters): exploration.
2. Monsters on: CONSULT on sighting.
3. Scripted combat stand-in with an HP-loss penalty: learning which fights to avoid. **Open; details later.**

### 7.3 Phase 6: session fine-tuning with PPO

- The runner plays live sessions with frozen weights and no search, logging every decision: the frame, the action and its probability, the reward, the game build (`VERSION`), and the GRU state at the start of each segment (about every 128 decisions). An hour of play is about 5,000 decisions; pool sessions from several farm instances when one is too small.
- A session visits each state once, so there are no groups. PPO's critic scores each step:

  ```
  δ_t = r_t + γ^(tics_t)·V(s_{t+1}) − V(s_t)
  A_t = δ_t + (γλ)·δ_{t+1} + (γλ)²·δ_{t+2} + …        γ ≈ 0.99, λ ≈ 0.95
  ```

  The discount is raised to the tics a step took, so a long walk counts as more time than a quick turn.
- **Critic:** before the first update on a session, fit the value head to that session's returns for a few epochs with the policy frozen. Normalize or clip returns, since CONSULT rewards are spiky. The last step bootstraps from V, because the game never really ends.
- **Update:** a few PPO epochs with a KL anchor to the shipped version. Data is discarded afterward (on-policy).
- **Eval gate:** run the fine-tuned runner on a fixed eval set (the game's own maps plus a few held-out ones). Ship only if it beats the shipped version. Run the same gate on each new game build.
- **Combat policy (future):** a separate network chosen by `turnctrl.state`, the one place continual training is planned. Gear stays with the LLM; the combat policy takes the equipped loadout's stats as input and trains across randomized loadouts.

### 7.4 Checkpointing

The execution environment can stop at any moment.
- Save at every update boundary: policy and value weights, reference policy, optimizer state, schedule positions, save-slot and seed sampler state, all RNG states, logger run id.
- Discard rollouts in progress on restart.
- Atomic writes (temp file, `fsync`, `os.replace`), keep the last 3 checkpoints, fall back if the newest fails to load.
- On startup, delete stale FIFOs and `/dev/shm` segments and kill orphaned DiabloDoom processes. Only leftovers whose owner process is gone are touched, so several trainer processes can share one box.
- In the split trainer (§7.5) only the learner checkpoints. Rollout workers are stateless: on restart they pull the newest weights and start new groups. Exact bit-for-bit resume of a multi-machine run is given up; the learner's weights, optimizer and version still resume exactly.

### 7.5 Split trainer: rollout workers and learner

The game instances are the cost, and they are CPU-bound on every machine; the 1.7M-parameter policy is cheap to run on a CPU, and only the update wants the GPU. So the trainer splits in two, and the Muse VM (CPU only) contributes rollouts while the RTX 5080 box learns.

- **`eval.py` (rollout worker, any machine, CPU or GPU):** spawns the env instances, samples group starts (seed, slot, warm-up), plays G×groups rollouts with the newest weights it has, and writes each finished set of groups to the spool as one batch file. Several workers can run at once, on one box or several, each with its own `--actor-id`.
- **`eval.py --heldout` (held-out evaluation):** the same machinery on the held-out maps from fixed starts (seed 0, no warm-up), once per new weights version; writes metrics only, never training data.
- **`learn.py` (learner, the 5080):** waits until the spool holds enough fresh groups, computes group advantages and value targets, runs the update, checkpoints (§7.4), and publishes the new weights.
- **`model.py`:** shared by all three: the policy network, the env factory, IWAD/group allocation and checkpoint I/O.
- **`train.py`:** the single-box driver, kept: rollouts and update in one process, strictly alternating, no spool. It's the reference for the split's math and the cheapest local run.

**Spool** (a directory; the only interface between the two sides):
- `weights.pt`: `{version, policy}`, written atomically by the learner at startup (version of the resumed or initial weights) and after every update (version = updates done). Workers reload it when the version changes.
- `batches/<actor>_<time_ns>.npz`: one file per worker iteration, written atomically (temp file, `fsync`, `os.replace`), deleted by the learner once consumed.
- `DONE`: written by the learner when it finishes its iterations; workers exit when they see it.

**Batch contents** (per decision; §5.4 for the fields):
- Frames deduplicated: each new decision pushes one frame onto the 4-frame stack, so a batch stores every distinct 84×84 frame once plus a 4-index stack per decision, and the learner rebuilds the stacks exactly. About 7 KB per decision instead of 28 KB, before compression.
- `priv` (HP, exit distance), `act`, `logp` (under the acting weights, for the clipped ratio), `rew`, `tics`.
- Per rollout: length, group id, IWAD, rollout tics, sectors gained, exit, death, terminated (metrics only; the learner recomputes everything it trains on).
- Meta: actor id, weights version the rollouts were played with, group labels (IWAD:map), timing.

**Staleness and flow control:**
- The clipped ratio uses the acting policy's log-probs, so rollouts a version or two old are still valid. Batches more than `--max-lag` versions (2) behind the learner are dropped and counted.
- The learner updates once it holds at least `--min-groups` groups (4), taking every fresh batch in the spool at that moment, so the batch size grows with the number of workers.
- A worker pauses while `--max-pending` (2) of its own batches are unconsumed, so a fast worker can't fill the disk or train the learner on its own starts only.
- Groups are normalized within themselves, so groups from different workers, IWADs and maps mix freely in one update.

**Across machines:** the spool is just files. The learner's spool is the source of truth; batches go from a worker's machine to it and `weights.pt` goes back (rsync/scp in a loop, or a shared mount). The transport is open (§12). A worker with only Freedoom contributes Freedoom groups; the learner doesn't care which IWADs a batch covers.

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
| 2 (1½ days) | Action allowlist, explore strafes, `MAP`, rewards, group resets, GRPO trainer | GRPO with the step-1 policy raises sectors visited on save slots it never trained on |
| 3 (1 day) | Render only at decisions; NVTX + `nsys` profiling | SPS table: rendering every tic vs only at decisions, 1 vs 8 vs 32 instances |
| 4 (if Phase 3 says so) | `libdiablodoom.so` with `dd_create` / `dd_reset` / `dd_step` / `dd_destroy`, pybind11 | Pipe overhead gone from the profile |
| 5 | Item deferral (§8) | Items flow to the VLM at CONSULT; item model trained on farm logs |
| 6 | Session fine-tuning with PPO (§7.3) | A version fine-tuned on one session beats the shipped one on the eval set; one build change passes the eval gate end to end |

Phases 0–3 are the first trainable env, about 4½ days.

## 10. Validation

- **Determinism:** same slot, seed and command list, run twice: final `STATE` lines match byte for byte; `STATEHASH` matches mid-episode. GRPO depends on this.
- **Fast equals real time:** the same command list with and without `-rlenv` ends in the same state. Suspects if not: `T_TickCadence` cooldowns, the replay encoder, anything reading `I_GetTime` or `time(NULL)`. Known wall-clock reads as of `c0e8fedb`: the idle Zug Zug timer (`I_GetTimeMS`, sound only, suppressed under `-rlenv`), the Zug Zug date gate (`time(NULL)`, sound only), and `SURVEY` filename timestamps (not a policy action). None touch game state.
- **Env contract:** `check_env` passes; shapes and dtypes match the declared spaces; the observation contains nothing from the state pipe.
- **Throughput:** random-agent decisions per second for 1, 8 and 32 instances, rendering every tic vs only at decisions.
- **Policy checks:** evaluate with `MAP` disabled to confirm the map is used; report exit rate and time to exit on held-out maps.

## 11. Risks

| Risk | Why it matters | Mitigation |
|---|---|---|
| Hidden wall-clock dependencies | Fast runs diverge from real play | Fast-equals-real-time check; grep for `I_GetTime`, `time(NULL)` |
| Decisions span very different tic counts | Noisy credit assignment | Fixed tic budget per rollout; tics per step in `info`; tic-based discount in PPO |
| Pipe parsing overhead | CPU cost at high step rates | Measure in Phase 3; Phase 4 only if needed |
| Exploration trained without combat | The runner may walk into fights the VLM then has to win | Third stage (open) |
| Sparse exit reward | Exits are rare | Dense sector reward; slots near exits first |
| `P_Random` is a 256-entry table | Variety comes from start states, not the RNG | Many save slots across maps and NG+ loops |
| GRPO groups collapse | Identical rollouts give zero gradient | Entropy bonus; skip zero-std groups; log the collapsed share |
| No critic during GRPO | One advantage per rollout, coarse credit | Process-reward variant; PPO baseline on the same env if curves stall |
| Pixels-only loops | The policy revisits rooms or spins | `MAP`, GRU memory, stuck penalty |
| Map flicker | More decisions per rollout, flickering replays | Log toggle rate; small toggle cost if needed |
| Rendering cost | The renderer runs every decision | Render only at decisions; Phase 3 measures it |
| HUD churn | Every HUD change shifts the policy's input — including world-space overlays (enemy health bars, floating damage numbers, depth scaling, added after `5fa94f8`) | Eval gate per build; randomize HUD details in training if churn continues |
| CONSULT reward farming | Large rewards get exploited | Grace window, first consult per trigger, stand-in clears the trigger, small cost without a trigger |
| CPU-bound on one box | 32 single-threaded instances want about 32 threads | Size instance count from the Phase 3 table; keep repo, pipes and `/dev/shm` on the Linux filesystem under WSL2 |

## 12. Decisions and open questions

Decided:
- Exploration trains with `-rlnocombat` (monsters removed), then a monsters-on stage for CONSULT.
- The automap counts as screen visual, reached through the policy's `MAP` toggle.
- Training runs on the RTX 5080 box.
- G = 8, 2,100 tics per rollout.
- Training uses every IWAD under `wad/` (Freedoom Phase 1 and The Ultimate Doom), groups split evenly between them (§7.1).
- Training can be split: rollout workers (`eval.py`, including CPU-only Muse VMs) feed a learner (`learn.py`, the RTX 5080) through a spool directory (§7.5). `train.py` stays as the single-box driver.
- CONSULT rewards and penalties as in §5.3; the backpack trigger is a dedicated backpack-full message on a failed pickup.
- CONSULT is an environment signal that drives Muse VM; the game cannot hand off by itself. The policy is taught to switch to combat or inventory mode with it, and the monster reward is paid only for sightings while exploring. The policy must choose CONSULT itself in every case — including after combat has started, where it earns no reward but each step without it is penalized. No automatic consult, no mode mask.
- The status bar stays in the policy's view.
- Phase 6 uses PPO on logged sessions, no live search. The critic gets HP and exit path distance.

Open:
- Third stage: fine-tuning with the scripted combat stand-in and an HP-loss penalty. Kept in the plan; details later.
- Reward magnitudes (§5.3).
- Spool transport between a Muse VM and the 5080 box (§7.5): SSH/rsync, HTTP, or a shared mount, depending on what can reach what.