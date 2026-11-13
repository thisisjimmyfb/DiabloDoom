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
from env import EnvConfig, EngineConfig, scripted_combat_consult  # registers DiabloDoom-v0

game = gym.make("DiabloDoom-v0", config=EnvConfig(
    engine=EngineConfig(warp="1 1", skill=3),
    consult_handler=scripted_combat_consult,
))
obs, info = game.reset(options={"slot": 5, "seed": 17, "group": 0})
obs, reward, terminated, truncated, info = game.step(2)   # WALK 0
```

- **Observation:** `uint8 (4, 84, 84)`, the last 4 decision frames in
  grayscale, area-resampled from the 320x200 screen (status bar kept).
- **Actions:** `Discrete(12)`, see `env.ACTIONS`. In combat,
  every action except `CONSULT` is dropped: no command is sent, no game
  time passes, and the step is penalized until the policy consults.
- **CONSULT** calls `EnvConfig.consult_handler(game, state, kinds)`.
  `null_consult` does nothing; `scripted_combat_consult` fights with
  ATTACK/END until combat ends (training stand-in). A farm handler
  would signal Muse VM instead. Handlers send driver commands through
  `game.consult_command(line)` (`game` being the `DiabloDoomEnv` instance).
- **Rewards:** `RewardConfig` (SPEC.md §5.3). The values are placeholders.
- **info:** `tics` for this step, `rollout_tics`, `mode`, `group`, `slot`,
  `seed`, `exit`, and `state` (the engine's STATE line, for logging and
  the critic; never part of the observation).
- **Resets:** `LOAD slot` then `SEED seed`. Slots come from
  `EnvConfig.start_slots` and are copied from `EngineConfig.savedir_template`.
  With neither, the warp start is saved to `bootstrap_slot` (5) on the
  first reset and reloaded later. For GRPO groups with a vector env, set
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

- **Policy:** the step-1 feedforward network: Nature CNN on the 4x84x84
  stack, 12 action logits (sampled), and a value head on detached
  features plus HP and exit distance (exit distance is 0 until the engine
  provides it). 1.69M parameters. The value head trains from the start
  but never shapes the policy during GRPO.
- **Batch:** `--groups` (4) x `--group-size` (8) instances. Group g plays
  map `--maps[g % n]` (Freedoom E1M1-E1M4 by default). All members of a
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
- **Stages:** `--stage 1` runs with `-nomonsters` (vanilla's flag, which is
  the spec's `-rlnocombat`) and a no-op CONSULT. `--stage 2` has monsters,
  with the scripted combat stand-in answering CONSULT.
- **Eval:** every `--eval-every` iterations (and before the first update),
  `--eval-episodes` rollouts on each `--eval-maps` map (held out:
  E1M5, E1M6) from a fixed start. Reports sectors gained and exit rate.
- **Checkpoints:** `runs/<name>/ckpt_NNNNNN.pt` at every update. Each
  holds weights, optimizer, counters and every RNG state, including the
  start-state sampler. Writes are atomic, the last `--keep` (3) are kept,
  and loading falls back to an older checkpoint if the newest is corrupt.
  SIGTERM exits cleanly; rollouts in progress are discarded.
- **Logs:** `runs/<name>/metrics.jsonl` (train and eval lines) and
  `config.json`.
- **Replays:** the first `--capture-envs` (1) training envs record
  continuous replay into `runs/<name>/replays/env<i>/`, and
  `--capture-eval` records the eval envs into `replays/eval<i>/`. Each
  file is 15 s of game time at 35 fps (525 tics, about 2-3 MB), named
  `diablodoom_YYYYMMDD_HHMMSS_mmm.mp4` by the wall-clock time it was
  queued, so names are unique and sort chronologically. Segments run
  across episode resets (a reset shows as a cut), and the last partial
  segment is written when the run exits. Capture doesn't change the
  game: a captured env plays the same as an uncaptured one. A captured
  env produces roughly 5 game-minutes of footage per real minute, so
  watch disk use on long runs (`--capture-envs 0` turns it off).

Measured 2026-11-12 at the default size: about 240 decisions/s (about
5,500 game tics/s) and about 12 s per iteration. Envs step in lockstep, so
each round waits for the slowest action; asynchronous stepping is a
Phase 3 item.

## Tests

```sh
.venv/bin/python tests/test_env.py check            # gymnasium check_env
.venv/bin/python tests/test_env.py random --steps 10000
.venv/bin/python tests/test_env.py determ --steps 1500
.venv/bin/python tests/test_env.py vector --k 8 --steps 1000
.venv/bin/python tests/test_env.py strafe
```

Measured 2026-11-12 (16 cores, WSL2): about 135 steps/s for one env and
about 385 steps/s for 8 envs in `AsyncVectorEnv`, with rendering on every tic.

## Not yet in the engine

`MAP` (sent, but answered as an unknown command), `monstervis` (the env
falls back to "visible targets while exploring"), `backpack_full`,
`-rlnocombat`, exit path distance, `ITEM` events, `STATEHASH`, and
rendering only at decisions. See SPEC.md §4.
