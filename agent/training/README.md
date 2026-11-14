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
obs, reward, terminated, truncated, info = game.step(0)   # WALK 0
```

- **Observation:** `uint8 (4, 84, 84)`, the last 4 decision frames in
  grayscale, area-resampled from the 320x200 screen (status bar kept).
- **Actions:** `Discrete(8)`, see `env.ACTIONS`: `WALK 0` (a fixed step
  forward), `TURNL`/`TURNR` (45°), `STRAFE_L`/`STRAFE_R`, `USE`, `MAP`,
  `CONSULT`. In combat,
  every action except `CONSULT` is dropped: no command is sent, no game
  time passes, and the step is penalized until the policy consults.
- **CONSULT** calls `EnvConfig.consult_handler(game, state, kinds)`.
  `null_consult` does nothing; `standin_consult` (training stand-in)
  clears what was consulted about: the `CONSULT` driver command
  (`-rlenv` only) removes the monsters in sight, and in combat those
  targeting the player, then `END` closes the round; a full backpack is
  salvaged until it has room. The policy learns when to hand off, not how to
  fight. A farm handler
  would signal Muse VM instead. Handlers send driver commands through
  `game.consult_command(line)` (`game` being the `DiabloDoomEnv` instance).
- **Rewards:** `RewardConfig` (SPEC.md §5.3). The values are placeholders.
  `approach` (1/256 per map unit) pays progress toward the nearest goal:
  the exit line, or a frontier portal (a passable two-sided line from a
  visited sector into an unvisited one; doors and lifts count by their
  special). It pays the change in distance, so a rollout's total is the
  start distance minus the end one and hovering earns nothing; a step
  that visits a new sector re-measures without pay. The distances come
  from the state line (`exit_dist`, `frontier_dist`) and shape the reward
  only: the policy acts from the frames alone.
  `key` (10) pays each key colour (blue, yellow, red) the first time it is
  held in a rollout, from the state line's `keys` mask; keys held at the
  start pay nothing.
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
.venv/bin/python train.py --stage 2 --run-dir runs/stage2 --init runs/stage1/ckpt_000500.pt
```

- **Length:** each run of `train.py` (and `./train.sh`) trains `--more`
  (100) updates past the newest checkpoint, so rerunning the same command
  keeps going 100 at a time. `--iterations N` instead sets the run's
  total: it stops at update N, and does nothing if the run already has N.
  (`learn.py` keeps `--iterations` 1000, the total, since it writes the
  DONE marker that stops the workers.)

- **Policy:** the step-1 feedforward network: Nature CNN on the 4x84x84
  stack, 8 action logits (sampled), and a value head on detached
  features plus HP and exit distance (straight-line distance to the exit
  line; 0 on a level without one). 1.69M parameters. The value head trains from the start
  but never shapes the policy during GRPO.
- **Batch:** `--groups` (4) x `--group-size` (8) instances. Group g plays
  map `--maps[g % M]` (E1M1-E1M4 by default). With several `--iwads`
  (`train.sh` passes every IWAD under `wad/`), group g runs on
  `iwads[(g % M + g // M) % I]`: the IWADs split the groups evenly, and
  `--groups I*M` covers every IWAD/map pair. Eval runs every eval map on
  every IWAD, and metrics add `sectors_<iwad>`/`exit_rate_<iwad>`. Save
  slots are IWAD-specific, so `--slots` needs a single IWAD. All members of a
  group start from the same slot, seed and `--warmup-max` random warm-up
  actions, so only action sampling separates them. Until prepared save
  slots exist (`--slots-dir`, `--slots`), the warm-up is what varies the
  start states.
- **Advantage:** group-normalized return (`--advantage outcome`) or the
  process variant (`process`). Zero-variance groups are dropped and
  reported as `collapsed_share`.
- **Loss:** clipped ratio (`--clip` 0.2), `--epochs` 4, entropy bonus
  `--ent-coef` 0.01, and an optional KL to a reference policy
  (`--ref`, `--beta`).
- **Frontier starts:** a rollout stops at `--max-tics`, so on its own
  training would only ever see the first 60 s of each map. After every
  batch, each map's highest-return rollout that ended alive, exploring and
  short of the exit saves the game where it stopped (`SAVE 4`). The save
  goes to `<run-dir>/frontier/<iwad>_<map>.dsg` (or
  `<spool>/frontier/<actor-id>/` for an `eval.py` actor), so it survives a
  resume, and is copied into slot 4 of every instance on that map, where
  it replaces the map start. Every group starts from slot 4, so training
  always saves and loads its progress there. A rollout that reaches the
  exit clears that map's frontier: slot 4 holds the map start again, and
  the cycle starts over. `front` in the training line is the share of
  rollouts that started from a frontier. `--no-frontier` turns it off
  (every start is the map start, still from slot 4). `--slots` can't
  include 4.
- **Stages:** `--stage 1` runs with `-nomonsters` (vanilla's flag, which is
  the spec's `-rlnocombat`); `--stage 2` has the map's monsters. Both
  spawn practice monsters on screen while exploring (`--spawn-tics`,
  default 700 tics on average; 0 turns them off). Map monsters almost
  always see the player the moment the player sees them, so without the
  practice monsters a sighting would never come before combat. They face
  away and never start a fight: one that would acquire the player is
  removed instead. Each is removed 5 s after spawning (or when a save
  holding one is loaded), and CONSULT on one pays `consult_hit` and
  removes it.
- **Eval:** every `--eval-every` iterations (and before the first update),
  `--eval-episodes` rollouts on each `--eval-maps` map (held out:
  E1M5, E1M6) from a fixed start. Reports sectors gained and exit rate.
- **Overlap:** `--overlap` runs each update on the GPU while the games
  play the next batch with the weights from before the update, so batches
  are one version old (`lag` 1 in `metrics.jsonl`; the clipped ratio
  allows for it). Measured 2026-11-13 at the default size on an idle box:
  about 6% more decisions per second (the update is about 2 s of a
  ~28 s iteration). After a restart, the first batch is played with the
  resumed weights (`lag` 0), so an overlapped run that was interrupted is
  not bit-identical to one that wasn't; its start states still are.
- **Checkpoints:** `runs/<name>/ckpt_NNNNNN.pt` at every update. Each
  holds weights, optimizer, counters and every RNG state, including the
  start-state sampler. Writes are atomic, the last `--keep` (3) are kept,
  and loading falls back to an older checkpoint if the newest is corrupt.
  SIGTERM exits cleanly; rollouts in progress are discarded.
- **Logs:** `runs/<name>/metrics.jsonl` (train and eval lines) and
  `config.json`.
- **Replays:** off by default. `./train.sh --capture` turns them on (for
  training, actors and the held-out eval; the learner plays no game):
  the first `--capture-envs` training envs of each IWAD (train.sh sets 1;
  `train.py`'s own default is 0) launch with `--continuous-capture` and
  record continuous replay into `runs/<name>/replays/env<i>_<IWAD>/`
  (e.g. `env00_FREEDOOM1/`, `env08_DOOM/`), and
  `--capture-eval` (what `./train.sh --heldout --capture` passes) records
  the eval envs into `replays/eval<i>/`. Each
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
- **Spool** (`<run-dir>/spool` by default): the learner publishes
  `weights.pt` (with its version and run id) at startup and after every
  update. A worker that finds no `weights.pt` creates random version-0
  weights there itself and plays with them, so a worker can start before
  (or without) a learner; a fresh learner (no checkpoint, no `--init`)
  adopts them, so those batches still count;
  workers write `batches/<actor>_<time>.npz` (each distinct frame once,
  about 5-7 KB per decision); the learner deletes what it consumes and
  writes `DONE` when it finishes, and the workers exit.
- **Learner:** updates once it holds `--min-groups` (4) groups, taking
  every fresh batch; drops batches more than `--max-lag` (2) versions
  old, and batches played with another run's weights. Checkpoints, `--keep`, `--outpath` and resuming work as in
  `train.py`; `metrics.jsonl` adds the version, the actors, the lag and
  per-IWAD numbers.
- **Workers** are stateless: they take the env arguments (`--iwads`,
  `--maps`, `--groups`, `--stage`, …), pause while `--max-pending` (2) of
  their own batches are unconsumed, reload `weights.pt` when its version
  or run changes, and need a unique `--actor-id` (no `_`). Replay capture is off unless `--capture-envs` is set (`./train.sh --actor --capture` sets 1).
- **Across machines:** sync batches to the learner's spool and
  `weights.pt` back (rsync/scp, or a shared mount).

## Playing (`play.sh`)

Launches the game locally, in a window with sound, with a trained policy
driving it. The game runs with `-rldrive` (real time, normal play), and
`play.py` turns each decision's frame into the same 84x84 4-frame stack
the policy trained on.

```sh
./play.sh                                    # newest agent/ckpt_*.pt, Freedoom E1M1, monsters on
./play.sh --checkpoint agent/ckpt_000129.pt --warp "1 2"
./play.sh --greedy --delay 0.3               # most likely actions, slowed down for watching
./play.sh --no-monster                       # monsters off, as in stage-1 training (--no-monsters works too)
```

The drive stops at the exit, on death, when combat starts (CONSULT is the
hand-off to Muse's VLM, which isn't attached here; a CONSULT while
exploring is skipped unless `--on-consult stop`), after
`--max-decisions`, or on Ctrl-C. The game window then stays open for you;
`--quit-at-end` closes it. Each decision prints the action, the mode, the
position and the sectors gained.

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
fixtures (fake rollouts and batch files) in `tests/conftest.py`:

- `test_env.py` (game): gymnasium's `check_env`, a 10,000-decision random
  agent that never wedges, determinism (same slot, seed and actions give
  identical states and frames), 8-instance `AsyncVectorEnv` throughput,
  and explore strafes.
- `test_model.py`: batch format and frame dedup, IWAD/group allocation,
  checkpoints (keep N, corrupt fallback, export), RNG restore and the
  spool's weights file.
- `test_eval.py`: group starts, warm-up actions, atomic batch writes and
  the `--actor-id` check. Game: `test_worker` and `test_heldout` run
  `eval.py` against a fake learner (the test publishes `weights.pt` and
  `DONE`), checking whole groups from one start, the `--max-pending`
  pause, new weights, one eval per version and the exit on `DONE`.
- `test_learn.py`: advantages, value targets, the update and spool intake
  (stale, waiting, half-written batches); `test_learner` runs `learn.py`
  on hand-written batches without the game (updates, checkpoints, `DONE`,
  `--outpath`, resume, stale drop). Game: `test_split` runs `learn.py`
  with real `eval.py` workers until `DONE`.
- `test_play.py`: a checkpoint from another action set is refused; game:
  `play.py` drives a headless game and quits, or leaves it running
  detached (no work directory or shared memory left for `cleanup_stale`).
- `test_train.py` (game): `test_resume` runs `train.py` for 2 iterations
  with evals and `--outpath`, then resumes it to 3; `test_iwads` splits
  groups over Freedoom and The Ultimate Doom (skipped without both);
  `test_capture` checks replay segments land in `replays/env00_<IWAD>/` with
  timestamped names.

Measured 2026-11-12 (16 cores, WSL2): about 135 steps/s for one env and
about 385 steps/s for 8 envs in `AsyncVectorEnv`, with rendering on every tic.

## Not yet in the engine

`MAP` (sent, but answered as an unknown command), `monstervis` (the env
falls back to "visible targets while exploring"), `backpack_full`,
`-rlnocombat`, exit path distance, `ITEM` events, `STATEHASH`, and
rendering only at decisions. See SPEC.md §4.
