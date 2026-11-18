# DiabloDoom RL environment

Gymnasium wrapper around DiabloDoom running under `-rlenv`. Design:
[`SPEC.md`](SPEC.md). Engine side: `src/diablo/d_rlenv.c`.

## Setup

```sh
scripts/build.sh                                  # from the repo root
cd agent/training
uv venv .venv && uv pip install --python .venv/bin/python -e .
```

The env finds the game at `build/src/chocolate-doom` and the IWAD at
`wad/FreedoomPhase1/FREEDOOM1.WAD` by default (`EngineConfig.game`,
`EngineConfig.iwad`).

## Use

```python
import gymnasium as gym
from env import EnvConfig, EngineConfig, standin_consult  # registers DiabloDoom-v0

game = gym.make("DiabloDoom-v0", config=EnvConfig(
    engine=EngineConfig(warp="1 1", skill=3),
    consult_handler=standin_consult,
))
obs, info = game.reset(options={"slot": 5, "seed": 17, "group": 0})
obs, reward, terminated, truncated, info = game.step(9)   # PRESS W: run forward for one tic
```

- **Observation:** `uint8 (4, 84, 84)`, the last 4 decision frames in
  grayscale, area-resampled from the 320x200 screen (status bar kept).
- **Actions:** `Discrete(29)`, one decision every game tic (2026-11-15),
  see `env.ACTIONS`: `PRESS <keys>`, the keys held for the next tic, in
  every combination of one key or none from each pair (W/S run
  forward/back, A/D strafe, L/R turn about 7° a tic; `PRESS` alone holds
  nothing), then `USE` and `CONSULT`. The engine's multi-tic commands
  (`WALK`, `GOTO`, the 45° `TURNL`/`TURNR`, ...) stay for the farm driver.
  There is no `MAP` action (removed 2026-11-14), and no forced map
  glances either (removed 2026-11-16: with 4 frames of memory the policy
  can't use a map it saw a moment ago), so training never shows the
  automap. In combat,
  every action except `CONSULT` is dropped: no command is sent, no game
  time passes, and the step is penalized until the policy consults.
- **CONSULT** calls `EnvConfig.consult_handler(game, state, kinds)`.
  `null_consult` does nothing; `standin_consult` (training stand-in)
  clears what was consulted about: the `CONSULT` driver command
  removes every practice monster, in sight or not (2026-11-17), and
  under `-rlenv` also the map monsters in sight and, in combat, those
  targeting the player, then `END` closes the round; on a unique pickup
  a full backpack is salvaged until it has room. The policy learns when
  to hand off, not how to fight. The triggers (2026-11-17): a monster in
  view, combat, and a unique picked up -- the engine holds a strong
  pickup flash for 3.5 s on a unique (the frame's mean brightness roughly
  doubles), and the trigger lasts 3 s (`unique_grace_tics`, 105, as a
  monster's `grace_tics`), inside the flash's full-strength part:
  CONSULT in that time pays `consult_hit`, letting it pass costs
  `trigger_miss`, charged step by step over the window (2026-11-17). It replaced the full-backpack trigger, whose only cue
  was a status message a few pixels tall in the policy's frame. Practice
  items roll a unique a quarter of the time so it comes up. A farm handler
  would signal Muse VM instead. Handlers send driver commands through
  `game.consult_command(line)` (`game` being the `DiabloDoomEnv` instance).
- **Rewards:** `RewardConfig` (SPEC.md §5.3). The values are placeholders.
  The portal goal (`--portal-goal`, default 1): a rollout ends once it
  enters a sector not yet visited (paid by `sector`, 15; there is no
  separate portal bonus);
  `--max-tics` is 525 (15 s), per map a starting budget: after a batch
  where no rollout on a map gets through a portal (or to the exit), that
  map's limit grows x`--tics-growth` (1.5) up to `--max-tics-cap` (2100),
  and any progress puts it back to 525 (`TicBudget`; not saved: a new
  run starts every map at 525). Progress still means finding the path, never just getting near a
  portal. Until it ends, time and distance cost one
  penalty: every game tic costs `time` x (1 + d / `far_scale`), d the
  distance (map units, the mean of the step's start and end) to the
  nearest portal of the player's current sector (2026-11-16: a passable
  two-sided line into any other sector, visited or not; doors and lifts
  count by their special; before, the nearer of the exit line and the
  nearest frontier portal). `time` is `--time-penalty` (1/3 per game
  second) and `far_scale` 256, so the clock runs at 1/3 point per second
  beside a way out of the room and twice that 256 units away: being far
  costs more the longer it lasts, and the faster crossing wins its group.
  A sector with no portal costs the base rate. It does little
  without the goal: rollouts that all run to the limit pay alike, and the
  group-relative advantage cancels most of it. The distance comes from
  the state line (`portal_dist`) and shapes the reward only:
  the policy acts from the frames alone. Facing a way on pays too
  (`look`, 2026-11-16): every tic, `look` x max(0, cos a), a the angle
  from the facing to the nearest portal of the current sector into an
  unvisited sector (`portal_rel`; none: nothing), at half the clock's
  base rate (`--time-penalty` / 70), so it softens the clock for facing
  the right way but never pays for standing still.
  Walking paths (2026-11-17): both now go by the walking path to the
  nearest unvisited room when there is one -- `path_dist`, the distance
  along it, and `path_rel`, the angle to the farthest of its next 4
  cells the player can walk to in a straight line, so the reward follows
  a corridor round a bend instead of pointing through the wall. The
  engine builds a 32-unit grid of where the player can stand and step
  (the `GOTO` pathfinder's rules) once per level load, with doors taken
  as open, and runs Dijkstra from every unvisited non-door sector when
  the visited set, the keys or a door changes: a closed door is on the
  path only if the player can open it (a manual door; a locked one with
  its key). About 30-80 ms per reset, nothing measurable per step.
  Lifts and raised floors are taken as they were at the load, and steps
  finer than the grid (a spiral stair of 16-unit steps) are a blur. No
  path (nothing reachable): the straight portal distances below.
  Doors first (2026-11-17): the engine finds the door and lift sectors
  from the line specials that move them (a manual door line's back
  sector, a remote door or lift trigger's tagged sectors), and both
  distances go by the current sector's portals through a door or onto a
  lift when it has one, by all its portals otherwise; a sector boundary
  is often just a step or a light change in mid-room. A door's own small
  sector never pays the sector reward (`door_sectors` on the state
  line), so a door crossing pays once, for the room behind it, and
  stepping into a doorway doesn't reach the portal goal.
  Turning that gets undone costs `turn_waste` (-0.005 per 45 degrees,
  2026-11-16): over the last 8 tics, the facing's total turning minus its
  net turning; a steady turn either way, or holding still, costs nothing.
  Reversing forward and back costs `move_flip` (-0.015): W within 4 tics
  of S, or S of W; strafing back and forth is free.
  A USE that sets nothing going (no engine `use` event: a plain wall,
  empty space) costs `use_miss` (-0.1; 2026-11-16, raised 2026-11-17);
  opening a door, even one used before, costs nothing, and so does a USE
  facing something usable that didn't move (the engine's `wall_use`: a
  pressed one-shot switch, or the face of a remote door, one only a
  switch or trigger opens). A locked door counts as usable only with its
  key (2026-11-17): without it, USE on it costs `use_miss`.
  Trying walls pays (`use_wall`, +0.25, 2026-11-17): a USE with a wall
  straight ahead within USE range (64 units; the state line's
  `wall_dist` and `wall_line`) pays once per wall line per rollout,
  whatever the wall -- the frames can't tell a switch or door from a
  plain wall, so the policy is paid for what it can see, trying a wall
  in reach, and a mechanism still pays its +10 on top. Such a USE never
  costs `use_miss`; only USE at nothing in reach does.
  Fast movement pays back the clock only going forward (`fast_forward`,
  2026-11-16): scaled by the cosine between the movement and the way on
  (`fast_path`, 2026-11-17: the path direction, else the nearest
  unvisited portal's; with neither, the facing), so running a circle or
  a detour, backing up or strafing off the way pays the whole clock,
  like standing still.
  A quick large drop -- freefall past 10 units a tic, 64 units or more in
  all -- costs `fall` (-3 on landing, whatever the height, 2026-11-16): it
  usually means the player left the intended path. Lifts (even the
  blazing one, 8 units a tic), stairs, going up and teleports never cost.
  Staring at a wall up close costs `wall_look` (the clock's base rate, per
  tic, 2026-11-16): the wall straight ahead nearer than 48 units, from the
  engine's `wall_dist`. Not while a switch or door is in reach
  (`wall_use`), so using mechanisms is free. The held-out eval keeps
  `--eval-max-tics` (2100) with no goal and no penalty, so its numbers
  compare across versions. Stage 1 has no map monsters, but passive
  damage (nukage, slime, crushers, barrels) still hurts: every HP lost
  costs `hp_loss` (-0.2; healing pays nothing), and a death ends the
  rollout with `death` (-100, as big as the exit) on top. There is no
  combat or stuck term (a long combat still ends the rollout).
  Pickups pay from the engine's `pickup` events: `loot` (5) per Diablo
  item or armor picked up, `health` (0.2) per HP a health pickup healed
  (a medikit 5, a +1 health bonus 0.2), the mirror of `hp_loss`.
  Positive reinforcement: `exit` (100, the top reward) for triggering the
  level complete (ends the rollout), `key` (25) per key colour, `sector`
  (15) per new sector (more than a mechanism: getting somewhere new is
  the goal, a door only the way there), `mechanism` (10) for each new
  mechanism set going. `consult_false` (-0.3) per CONSULT with nothing to
  consult about.
  `key` (25) pays each key colour (blue, yellow, red) the first time it is
  held in a rollout, from the state line's `keys` mask; keys held at the
  start pay nothing. In training a new key also ends the rollout (`end`
  "key", the key goal), so its end state competes with the batch's
  portal rollouts for the next start (see Frontier starts); the held-out
  eval doesn't stop at keys.
- **info:** `tics` for this step, `rollout_tics`, `mode`, `group`, `slot`,
  `seed`, `exit`, and `state` (the engine's STATE line, for logging and
  the critic; never part of the observation).
- **Resets:** `LOAD slot` then `SEED seed`. Slots come from
  `EnvConfig.start_slots` and are copied from `EngineConfig.savedir_template`.
  With neither, the warp start is saved to `bootstrap_slot` (4) when the
  game starts and reloaded later; an `install_state` save (training's
  progress) replaces it in that slot, and removing that save puts the map
  start back. For GRPO groups with a vector env, set
  each env's next reset with
  `envs.set_attr("next_reset", [{"slot": s, "seed": k, "group": g}, ...])`.
- **Episode end:** exit reached or death (terminated); `max_tics` (2100)
  game tics or `max_combat_steps` steps in combat (truncated).

Each instance runs in its own temp working directory with its own
`-savedir`, FIFOs and `/dev/shm` segment, all removed on `close()`.
`env.cleanup_stale()` clears leftovers from crashed runs.

## Training (`train.py`)

GRPO pretraining of the level runner (SPEC.md §6, §7.1, §7.4).

```sh
uv pip install --python .venv/bin/python torch --index-url https://download.pytorch.org/whl/cu128
.venv/bin/python train.py --run-dir runs/stage1          # stage 1: no monsters
.venv/bin/python train.py --run-dir runs/stage1          # same command resumes
.venv/bin/python train.py --stage 2 --run-dir runs/stage2 --init runs/stage1/doom-runner.pt
```

- **Length:** each run of `train.py` (and `./train.sh`) trains `--more`
  (100) updates past the newest checkpoint, so rerunning the same command
  keeps going 100 at a time. `--iterations N` instead sets the run's
  total: it stops at update N, and does nothing if the run already has N.

- **Policy:** the step-1 feedforward network: Nature CNN on the
  4 x H x W stack (4 x 80 x 128), A action logits (A = 29, sampled), and a value head on detached
  features plus HP and exit distance (straight-line distance to the exit
  line; 0 on a level without one). Parameters:
  79,011 + 513 * A + 32,768 * conv_out(W) * conv_out(H) (2,453,184), where
  conv_out(n) = ((n - 8) // 4 + 1 - 4) // 2 + 1 - 2 (`model.conv_out`;
  conv_out(128) = 12, conv_out(80) = 6). The value head trains from the start
  but never shapes the policy during GRPO.
- **Observation size:** `--obs-size WxH` (2026-11-17; default 128x80) sets
  the frame the policy sees, e.g. `160x100` (half the screen, its aspect
  kept: 4.8M parameters) or `320x200` (full size: 24.9M). One number is
  the width W, with the height that keeps the screen's 320x200 aspect,
  H = round(W * 200 / 320) (`--obs-size 128` is 128x80). It applies to a
  new run and is saved in the checkpoint: a resumed run keeps its own
  size (asking for another stops), and `eval.py` reads it from the
  checkpoint. A batch's frames take steps * 4 * W * H bytes (at 70,000
  steps: 2.9 GB at 128x80, 4.5 GB at 160x100, 18 GB at 320x200). On the GPU each `--minibatch`
  (8192) runs in `--micro-batch` pieces (2048) whose gradients add up to
  the minibatch's before its one optimizer step: the same update, less
  memory (a whole 8192 minibatch at 160x100 peaks at 10.2 GB and ran out
  of the RTX 5080's 16 GB in training; 4096 peaks at 5.2 GB, 2048 at
  2.6 GB).
- **Batch:** `--groups` x `--group-size` (8) instances; `--groups`
  defaults to one group per IWAD/map pair, at most the logical cores
  minus 2 (14 on a 16-thread CPU; the 2 stay free; `train.sh` passes
  every IWAD under `wad/`: 2 x E1M1-E1M4 = 8 groups). Groups go map by map
  with every IWAD on each: group g plays `--maps[(g // I) % M]` on
  `iwads[g % I]`, so E1M1 on Freedoom, E1M1 on The Ultimate Doom, then
  E1M2 on each, and the first group of every IWAD (the one `--capture`
  records) is on E1M1.
  **Map rotation:** with more IWAD/map pairs than groups, the groups take
  turns: every `--rotate-every` (10) batches they move on to the next
  block of pairs, wrapping around, so every pair gets trained without more
  instances (`--rotate-every 0`: no rotation, the extra pairs never play).
  A moved instance's game restarts on its new map (a few seconds); each
  map keeps its progress save and tic budget while it waits for its turn.
  A resume picks up its rotation from the iteration. Each session
  shuffles the pair order (and with it which group plays which pair) from
  a fresh map seed, printed on the `[train] run` line and saved in
  `config.json` as `map_seed`; `--map-seed N` repeats a session's choices.
  The training line's metrics carry `groups`
  (the maps played) while rotating. `--maps all` trains every map the
  IWADs share, minus `--eval-maps` and, in stage 1, the boss-exit maps
  (E1M8, E2M8, E3M8, E4M6, E4M8, Doom II's MAP07 and MAP30: their exits
  need a boss death, impossible with `--eval`): 29 maps per IWAD for
  Freedoom and The Ultimate Doom, 58 pairs, a full cycle in 5 rotations
  (50 batches) with 14 groups (16 logical cores). Eval runs every eval map on
  every IWAD, and metrics add `sectors_<iwad>`/`exit_rate_<iwad>`. Save
  slots are IWAD-specific, so `--slots` needs a single IWAD. All members of a
  group start from the same slot, seed and `--warmup-max` random warm-up
  actions (each held for 15 tics), so only action sampling separates them. Until prepared save
  slots exist (`--slots-dir`, `--slots`), the warm-up is what varies the
  start states.
- **Advantage:** group-normalized return (`--advantage outcome`) or the
  process variant (`process`): per-step rewards normalized across the
  group and summed from each step on, except the one-off terms
  (`RewardConfig.immediate`: consult, use_miss, use_wall, move_flip,
  turn_waste, wall_look; 2026-11-17), which count on their own step only,
  since they judge just that action and later steps don't follow from it. Zero-variance groups are dropped and
  reported as `collapsed_share`.
- **Loss:** clipped ratio (`--clip` 0.2), `--epochs` 4, entropy bonus
  `--ent-coef` 0.01, and an optional KL to a reference policy
  (`--ref`, `--beta`).
- **Frontier starts:** a rollout stops at `--max-tics` (or at the portal
  or key goal), so on its own training would only ever see the start of each
  map. Every group loads its start from slot 4 (the map start until there
  is progress), so all its members start from the identical state. After
  every batch, each map's highest-return rollout that ended alive,
  exploring, short of the exit and with progress (a new sector, or a new
  key) -- only
  that one -- saves the game where it stopped to slot 5 (`SAVE 5`, so the
  start in slot 4 is never overwritten mid-batch) along with its visited
  sectors (`VISITED`; replayed with `VISIT` after each load, since saves
  don't store them, so the portal it came in by is not new). The save is
  copied into slot 5 of every instance on that map, then slots 4 and 5
  swap there: the progress is the next batch's start in 4, the old start
  sits in 5. With the portal goal the agent moves on portal by portal,
  and key by key: the save holds the key, so the next start has it. A
  batch without progress keeps the current start; `--frontier-patience`
  (5) of them in a row at the full tic budget (a dead end) clear it, back to the map start. The
  saves live only in the running trainer and its games, never on disk: a
  new run (a restart, a resume on another machine) starts every map from
  its beginning, and only the map rotation resumes, from the
  checkpoint's iteration (a save only loads in the exact IWAD file it was
  made with, so it couldn't follow the run between machines anyway). A rollout that reaches the exit clears
  that map's frontier (the other rollouts of the batch still run to
  their own ends): slot 4 holds the map start again, and the cycle
  starts over. `front` in the training line is the share of rollouts that
  started from a frontier. `--no-frontier` turns it off (every start is
  the map start, still from slot 4). `--slots` can't include 4 or 5.
- **Stages:** `--stage 1` runs with `--eval` (vanilla's `-nomonsters`,
  renamed 2026-11-17; the spec's `-rlnocombat`), which also turns
  turn-based combat off entirely, passive damage included; `--stage 2`
  has the map's monsters. Both
  spawn practice monsters on screen while exploring (`--spawn-tics`,
  default 350 tics on average, 250 and 700 before 2026-11-17; 0 turns them off). Map monsters almost
  always see the player the moment the player sees them, so without the
  practice monsters a sighting would never come before combat. They face
  away and never start a fight: one that would acquire the player is
  removed instead. Each is removed 5 s after spawning (or when a save
  holding one is loaded), and CONSULT on one pays `consult_hit` and
  removes it. A sighting stays a trigger for its 3 s even if the policy
  turns away, so looking away still costs `trigger_miss`. The train
  line's `consult a/b` is the CONSULT rate with a monster or unique
  trigger pending / with none (`consult_trig` / `consult_idle` in
  metrics.jsonl): b near a means CONSULT ignores the cue. Practice items (2026-11-17; `--item-tics`, default 420, 300 before; 0
  turns them off): one random Diablo loot item at a time (a weapon,
  armor, jewelry, a potion) appears on the straight line to a portal of
  the current sector that is on screen (within 40 degrees of the
  facing), 64-256 units away, in sight, on a floor within a step of the
  player's: tried in the order unvisited doors and lifts, other
  unvisited portals, visited doors and lifts, other visited portals,
  nearest first in each. None fits, or a full backpack: no item that time. It is removed after 10 s. While it lies, each unit walked toward it
  pays `item_approach` (0.01) and each unit away costs it. Picking one up pays the usual `loot` reward;
  the train line's `items` is the share of spawned practice items
  picked up (`items_spawned` / `items_picked` in metrics.jsonl, and
  `items` [spawned, picked] per rollout in rollouts.jsonl).
- **Eval:** at the start and at the end of each run, and every
  `--eval-every N` iterations as well when N > 0 (default 0); the end
  prints the change per IWAD and map (`[eval delta]`). A run stopped
  with Ctrl-C (or SIGTERM) still evals at the end: the last saved
  checkpoint's weights (reloaded, since the update in progress is
  dropped); a second Ctrl-C skips it, and an interrupt during an eval
  or before the first update finished has none. `--eval-maps ''`
  turns evals off; nothing in training reads them. The default
  `--eval-maps random` draws `--eval-count` (2) held-out maps fresh each
  session (from the map seed): maps every IWAD has, minus the training
  maps and, in stage 1, the boss-exit maps. A comma-separated list fixes
  them instead. So the start and end evals of one session compare, but
  two sessions usually eval different maps. With `--maps all` the eval
  maps are drawn first and left out of training for that session only,
  so a later session may eval maps an earlier one trained on. Each eval plays
  `--eval-episodes` rollouts on each `--eval-maps` map from a fixed start (no progress saves), scored with the
  training rewards, time penalty included, but with no portal or key goal
  ending a rollout early, so sectors compare across versions. Reports
  sectors gained, exit rate and return.
- **Overlap:** `--overlap` runs each update on the GPU while the games
  play the next batch with the weights from before the update, so batches
  are one version old (`lag` 1 in `metrics.jsonl`; the clipped ratio
  allows for it). Measured 2026-11-13 at the default size on an idle box:
  about 6% more decisions per second (the update is about 2 s of a
  ~28 s iteration). After a restart, the first batch is played with the
  resumed weights (`lag` 0), so an overlapped run that was interrupted is
  not bit-identical to one that wasn't; its start states still are.
- **Health checks (2026-11-16):** every update is checked before it's
  saved, and an unhealthy one stops training with exit code 1: that
  update isn't saved, there's no end-of-run eval, and nothing more is
  uploaded (an upload already running, of an earlier checkpoint that
  passed, finishes). The metrics row says why (`stopped`). Two checks:
  ReLU collapse, when at least `--stop-dead-units` (0.5) of the trunk's
  last-layer units never fire on the whole batch (`dead_units`; a fresh
  network is about 0.03). A dead unit gets no gradient and never comes
  back; with all 512 dead the policy ignores the screen and only its
  biases learn, which is what the 2026-11-16 run did (KL near 0, 92% right
  turns). And an exploding gradient, when the gradient norm before
  clipping (`grad_norm_max`) passes `--stop-grad-ratio` (100) times the
  median of the last 50 healthy updates (checked once 20 exist; a resumed
  run starts the history again), or `--stop-grad-norm` if set (off by
  default: the fixed 1000 stopped a healthy run on a one-off 1,390, and
  clipping makes a finite spike harmless -- the worst one in 1,169
  updates was 25x the median), or any optimizer step has a non-finite
  loss or gradient (that step is skipped, so it never reaches the
  weights; this one always stops). 0 turns a threshold off. The `[train]`
  line shows both: `dead` and `gnorm`.
- **Advantage scale (2026-11-16):** `--norm-adv` (on; `--no-norm-adv`
  turns it off) divides each batch's advantages by their standard
  deviation, with no mean shift, so signs and proportions stay. Process
  advantages sum every later step's score, so their size follows the
  rollout length (525-2100 tics): unscaled, long batches drowned the
  entropy bonus (entropy fell to 0.4) and swung the gradient norm from
  30 to 1,390. The `[train]` line's `adv` is the spread before scaling.
- **Checkpoints:** one file, `runs/<name>/doom-runner.pt`, replaced
  after every update. It holds weights, optimizer, the iteration and
  other counters, and every RNG state, including the start-state sampler.
  Writes are atomic (a crash mid-write leaves the previous one); a
  checkpoint that can't be loaded stops the run rather than starting it
  fresh. Older versions live in the Hub repo's commits.
  SIGTERM exits cleanly; rollouts in progress are discarded.
- **Logs:** `runs/<name>/metrics.jsonl` (train and eval lines) and
  `config.json`, plus `runs/<name>/rollouts.jsonl` for debugging: one line
  per training rollout giving the env
  index, group and map, the start (`map` or `frontier`, slot, seed,
  warm-up length), why it ended (`end`: exit, death, key, portal,
  time, combat), its tics against that map's `max_tics`, the return split by
  reward term (`terms`), sectors gained, HP, position and sector at the
  start and end, goal distances, action counts, and what the frontier did
  for its map (`frontier`: saved, exit, stale or kept; `picked` on the
  rollout it saved or that exited). `wall_start`/`wall_end` are spelled
  like the replay file names, so a cut in a replay finds its rollout:
  `grep '"env": 8,' rollouts.jsonl`.
- **Hugging Face Hub (`--hub-repo`):** `./train.sh` (not `--smoke`)
  passes `--hub-repo thisisjimmy/DoomRunner`; `--no-hub` turns it off,
  another `--hub-repo user/name` replaces it. `--no-hub-download` skips
  the pull (to start over, with an empty `--run-dir`: the first push
  then replaces the repo's checkpoint, the old one stays in its
  history), and `--no-hub-upload` skips the pushes. Before training, the repo's
  `doom-runner.pt` is downloaded into the run directory, so training
  resumes from it (policy, optimizer, RNG and counters), unless the run
  directory's checkpoint is at that iteration or a newer one. While training, the newest checkpoint is
  uploaded as `doom-runner.pt` every `--hub-every` (100) updates and at the
  end (Ctrl-C included), in the background, one upload at a time, so the
  repo's commit history ("iteration N") is the checkpoint history. Only
  the checkpoint goes up: a run on a fresh machine (a recycled VM)
  resumes the policy and the map rotation (the episode and mission each
  group plays, from the iteration), each map from its beginning. A push
  also removes any `frontier/` an older version uploaded. Metrics and logs
  stay local. Uses the
  `hf` CLI (`hf auth login` once, with a write token), so the venv needs
  no extra package; its output goes to `<run-dir>/hub_upload.log`. When
  the Hub closes an upload's connection, `hf` never notices and waits
  forever, so an upload that prints no progress for 5 minutes, or runs
  over 30, is killed with everything it started (2026-11-18; logged as
  `killed` in hub_upload.log): during training the next save retries, and
  at the end the newest checkpoint gets up to 3 tries. A repo
  that can't be read (network, login) stops the run before training,
  rather than starting fresh and overwriting the repo's checkpoint.
- **Replays:** off by default. `./train.sh --capture` turns them on:
  the first `--capture-envs` training envs of each IWAD (train.sh sets 1;
  `train.py`'s own default is 0) launch with `--continuous-capture` and
  record continuous replay into `runs/<name>/replays/env<i>_<IWAD>/`
  (e.g. `env00_FREEDOOM1/`, `env08_DOOM/`), and
  `train.py --capture-eval` records the held-out eval envs into
  `replays/eval<i>/`. Each
  file is 15 s of game time at 35 fps (525 tics, about 2-3 MB), named
  `diablodoom_YYYYMMDD_HHMMSS_mmm.mp4` by the wall-clock time it was
  queued, so names are unique and sort chronologically. Segments run
  across episode resets (a reset shows as a cut), and the last partial
  segment is written when the run exits. Capture doesn't change the
  game: a captured env plays the same as an uncaptured one. A captured
  env produces roughly 5 game-minutes of footage per real minute, so
  watch disk use on long runs.
- **No blit (`--noblit`, train.sh's default):** the games launch with
  `-noblit`, which skips the SDL blit (`I_FinishUpdate`) that nobody sees
  in a headless game: about 4x more decisions per second for one env
  (measured 2026-11-14: 1208/s against 295/s). The frame the policy sees
  and the replays come from the game's 8-bit buffer, so they are
  byte-identical. `./train.sh --no-noblit` blits as before.
  `--nodraw` (opt-in, benchmarks only) skips rendering as well, about 6x,
  but then the frame is never drawn, so the policy is blind, and replay
  capture is off.

## Evaluating (`eval.sh`)

Renamed from `play.sh` 2026-11-16 (and `play.py` folded into `eval.py`,
2026-11-17). Both modes take the policy from the
same places: `--checkpoint` if given, else the Hugging Face repo's
`doom-runner.pt` (`--hub-repo`, default `thisisjimmy/DoomRunner`,
downloaded into `runs/hub-play/` with the `hf` CLI on PATH; a public repo
needs no login), else, when that can't be downloaded, the newest local
`runs/*/doom-runner.pt`. `--no-hub` skips the repo.

### Training-style eval (the default)

`eval.py` plays every training IWAD/map pair (every IWAD under `wad/`
unless `--iwads`; `--maps`, default `1 1`..`1 4`) `--group-size` times
(8), each game in its own window (2026-11-17: no sound, `-window` so a
fullscreen config can't take over, running as fast as it can; `--headless`
turns the windows off, and `--noblit` / `--nodraw` are ignored with
windows, which they would leave black), with training's env, rewards and
rollout rules: the `--max-tics` limit (525), ended early by the portal
and key goals, from training's random group starts (`--seed`, 0). With
more pairs than `--groups` (the cores minus 2) they take turns, like
training's map rotation. Nothing is trained, saved or uploaded.

```sh
./eval.sh                                     # the Hub checkpoint, every IWAD, maps 1 1..1 4, 525 tics
./eval.sh --no-hub --max-tics 2100 --maps all
./eval.sh --headless                          # no game windows
./eval.sh --group-size 2 --groups 2           # 4 windows at a time instead of 64
```

It prints one line per pair and an overall one, in the `[it]` line's
terms, then the most used actions (the Hub checkpoint, 2026-11-16):

```
  FREEDOOM1:1 1  ret   -8.79 ± 7.17  sectors  0.12  exit 0.00  death 0.00  steps    497  tics   497  (8 rollouts)
  DOOM:1 1       ret   14.73 ± 0.18  sectors  1.00  exit 0.00  death 0.00  steps     36  tics    36  (8 rollouts)
  ...
  all            ret    1.29 ±10.23  sectors  0.31  exit 0.00  death 0.00  steps    417  tics   417  (64 rollouts)
[eval] top actions: PRESS AR 7%, PRESS DR 7%, PRESS R 6%, PRESS WR 6%, PRESS WA 5%, PRESS WAL 5%
```

### Watching it play (`--iwad`)

`--iwad <path>` launches the game locally on that IWAD file, in a window
with sound, with the policy driving it. The game runs with `-rldrive`
(real time, normal play), and `eval.py` turns each decision's frame into
the same 4-frame stack the policy trained on (its `--obs-size`, from the checkpoint). Both modes always run
without map monsters or turn-based combat (2026-11-17: `--eval` here, stage 1 in the
training-style eval, where `--stage 2` is ignored). Both get training's
practice monsters and items (`--spawn-tics`, `--item-tics`; 2026-11-17
for watch mode, the engine's spawners now running under `-rldrive`
too), and watch mode's summary adds a `practice:` line: monsters seen
and how many got a CONSULT while in view, items picked up of those
spawned, uniques, and how many got a CONSULT within the 3 s window.

```sh
./eval.sh --iwad ../../wad/UltimateDoom/DOOM.WAD          # Ultimate Doom E1M1
./eval.sh --iwad ../../wad/FreedoomPhase1/FREEDOOM1.WAD --checkpoint runs/stage1/doom-runner.pt --warp "1 2"
./eval.sh --iwad ../../wad/UltimateDoom/DOOM.WAD --temperature 0.5 --delay 0.3   # more decisive actions, slowed down
```

The drive stops at the exit, on death, after `--max-decisions`, or on
Ctrl-C. The game window then stays open for you;
`--quit-at-end` closes it. A CONSULT while exploring is sent: the game
shows `CONSULT` as the status message, so you see the policy asked
for the hand-off, and removes every practice monster, in sight or not; the drive goes on (`--on-consult stop` stops it there).
That needs a game built after 2026-11-16; an older one logs `CONSULT
needs -rlenv` and shows `CMD CONSULT` instead. Each decision prints the
action, the mode, the position and the sectors gained, and the end of the
drive prints its stats:

```
[eval] runs/stage1/doom-runner.pt
[eval] level FREEDOOM1 1 1: max decisions
[eval]   decisions 400, game time 23.3 s (815 tics)
[eval]   sectors gained +5 (12.9 a minute), exit no, died no
[eval]   health 100 -> 100, keys none, distance 3054 units (131 a second)
[eval]   forward 36%  back 26%  strafe 72%  turn L 31%  turn R 39%  idle 2%  USE 0  CONSULT 1
[eval]   top actions: PRESS DR 8%, PRESS WA 7%, PRESS AR 6%, PRESS WAR 5%, PRESS WAL 5%
```

Differences from training: `-rldrive` keeps normal play's on-screen
`CMD` echo, and explore strafes do what they do in a normal game (queue a
combat move) instead of `-rlenv`'s sideways step.

## Tests

The tests use pytest (the `test` extra; `./test.sh` installs it when
missing). Tests that launch the game are marked `game`.

GitHub Actions (`.github/workflows/training-tests.yml`) runs
`pytest -m "not game"` on every push and pull request to `master` that
touches `agent/training/`, on CPU torch with no game build: the `game`
tests need an IWAD, and IWADs aren't in the repository. Run the full
suite locally before pushing.

```sh
./test.sh                          # everything, game runs included (~2.5 min)
./test.sh -m "not game"            # only the tests that need no game (~7 s)
./test.sh tests/test_learn.py      # one file
./test.sh -k resume -s             # by name, showing the tests' own output
```

`test.sh` passes its arguments to pytest; `.venv/bin/python -m pytest`
from this directory works the same. One file per module, plus shared
fixtures (fake rollouts) in `tests/conftest.py`:

- `test_env.py` (game): gymnasium's `check_env`, a 10,000-decision random
  agent that never wedges, determinism (same slot, seed and actions give
  identical states and frames), 8-instance `AsyncVectorEnv` throughput,
  and explore strafes.
- `test_model.py`: IWAD/group allocation, map rotation and moving groups,
  `--maps all`, progress saves (frontier), tic budgets, checkpoints (one
  file, corrupt stops), RNG restore, `--obs-size` and the
  Hub pull and push (with a fake `hf`).
- `test_rollout.py`: group starts and warm-up actions.
- `test_learn.py`: advantages, value targets and the update.
- `test_eval.py`: `eval.py` refuses a checkpoint from another action set;
  game: `--iwad` drives a headless game and quits, or leaves it running
  detached (no work directory or shared memory left for `cleanup_stale`),
  prints the drive's stats, and sends CONSULT (shown, the drive goes on;
  `--on-consult stop` stops); the training-style eval prints a line per
  map and an overall one within the tic limit.
- `test_train.py` (game): `test_resume` runs `train.py` for 2 iterations
  with evals (checking `rollouts.jsonl`), then resumes it
  to 3; `test_rotation` alternates two maps with `--rotate-every 1`; `test_iwads` splits
  groups over Freedoom and The Ultimate Doom (skipped without both);
  `test_capture` checks replay segments land in `replays/env00_<IWAD>/` with
  timestamped names.

Measured 2026-11-12 (16 cores, WSL2): about 135 steps/s for one env and
about 385 steps/s for 8 envs in `AsyncVectorEnv`, with rendering on every tic.

## Not yet in the engine

`monstervis` (the env
falls back to "visible targets while exploring"), `backpack_full`,
`-rlnocombat`, exit path distance, `ITEM` events, `STATEHASH`, and
rendering only at decisions. See SPEC.md §4.
