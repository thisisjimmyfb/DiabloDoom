#!/usr/bin/env bash
# Set up the training environment and run the trainer (SPEC.md §7.1, §7.5).
#
# Full training on one box (train.py):
#   ./train.sh                       # 100 stage-1 updates in runs/stage1 (rerun: 100 more);
#                                    # the final checkpoint is copied to agent/
#   ./train.sh --smoke               # smallest run: 1 group of 2 per IWAD, 1 iteration, no eval
#   ./train.sh --run-dir runs/x ...  # any train.py arguments pass through
#   ./train.sh --no-noblit           # also blit (games run with -noblit by default)
#
# Split training (rollout workers feed one learner through a spool):
#   ./train.sh --learn               # learner only (learn.py): runs/stage1, spool runs/stage1/spool;
#                                    # the final checkpoint is copied to agent/
#   ./train.sh --actor               # rollout worker only (eval.py) on runs/stage1/spool
#   ./train.sh --actor --spool DIR --actor-id vm0 --device cpu   # e.g. on a Muse VM
#   ./train.sh --heldout             # held-out eval of each new version -> runs/stage1/eval.jsonl
# Arguments after the mode pass through to learn.py / eval.py.
#
# Setup is idempotent and only does what is missing:
#   - builds the game (Release) if build/src/chocolate-doom doesn't exist
#     (not for --learn: the learner runs no game)
#   - creates .venv (uv if available, else python3 -m venv)
#   - installs the env package (editable) and torch (CUDA 12.8 build when
#     an NVIDIA GPU is present, the RTX 5080 needs it; CPU build otherwise)
set -euo pipefail

MODE=train
case "${1:-}" in
    --smoke|--learn|--actor|--heldout) MODE="${1#--}"; shift ;;
esac

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
VENV="$HERE/.venv"
PY="$VENV/bin/python"
GAME="$REPO/build/src/chocolate-doom"
WAD_DIR="$REPO/wad"

log() { printf '[train.sh] %s\n' "$*"; }

# --- game binary and IWADs (everything but the learner plays the game) ----
IWADS=()
if [[ "$MODE" != learn ]]; then
    if [[ ! -x "$GAME" ]]; then
        log "game not built; building (Release)"
        "$REPO/scripts/build.sh" --release
    fi
    # Every .wad under wad/ (any case, any depth) whose header says IWAD.
    # PWADs (mods, the avatar faces) are skipped: they can't be passed as -iwad.
    while IFS= read -r -d '' f; do
        [[ "$(head -c 4 "$f")" == IWAD ]] && IWADS+=("$f")
    done < <(find "$WAD_DIR" -type f -iname '*.wad' -print0 | sort -z)
    if (( ${#IWADS[@]} == 0 )); then
        log "no IWAD found under $WAD_DIR"
        exit 1
    fi
fi

# --- python environment ---------------------------------------------------
if [[ ! -x "$PY" ]]; then
    log "creating $VENV"
    if command -v uv >/dev/null; then
        uv venv "$VENV"
    else
        python3 -m venv "$VENV"
    fi
fi

pip_install() {
    if command -v uv >/dev/null; then
        uv pip install --python "$PY" "$@"
    else
        "$PY" -m pip install "$@"
    fi
}

if ! "$PY" -c "import env, gymnasium, numpy" 2>/dev/null; then
    log "installing the env package"
    pip_install -e "$HERE"
fi

if ! "$PY" -c "import torch" 2>/dev/null; then
    if command -v nvidia-smi >/dev/null && nvidia-smi -L >/dev/null 2>&1; then
        log "installing torch (CUDA 12.8 build)"
        pip_install torch --index-url https://download.pytorch.org/whl/cu128
    else
        log "no NVIDIA GPU found; installing the CPU torch build"
        pip_install torch --index-url https://download.pytorch.org/whl/cpu
    fi
fi

"$PY" -c "import torch; print('[train.sh] torch', torch.__version__, '| cuda' if torch.cuda.is_available() else '| cpu', torch.cuda.get_device_name(0) if torch.cuda.is_available() else '')"

# Instances left behind by a crashed run (temp dirs, /dev/shm segments).
"$PY" -c "from env import cleanup_stale; cleanup_stale()"

# --- run --------------------------------------------------------------------
cd "$HERE"

# has_arg NAME: was --NAME (or --NAME=...) passed?
has_arg() {
    local a
    for a in "${ARGS[@]}"; do
        [[ "$a" == "--$1" || "$a" == "--$1="* ]] && return 0
    done
    return 1
}
ARGS=("$@")
# arg_value NAME: the value given for --NAME (either spelling), else empty.
arg_value() {
    local i
    for (( i = 0; i < ${#ARGS[@]}; i++ )); do
        case "${ARGS[i]}" in
            "--$1") echo "${ARGS[i + 1]:-}"; return ;;
            "--$1="*) echo "${ARGS[i]#--$1=}"; return ;;
        esac
    done
}

# Defaults, each added only when not given: --run-dir runs/stage1
# (checkpoints, metrics, replays, the spool; gitignored); the final
# checkpoint (full and resumable, ~20 MB) is copied to --outpath, the
# repo's agent/ directory, so it can be committed; --iwads is every IWAD
# found under wad/ (groups split evenly; --groups 8 trains all 4 maps on
# both Freedoom and Ultimate Doom). The rest are the scripts' own
# defaults (stage 1, 4 groups x 8, E1M1-E1M4, eval on E1M5/E1M6).
RUN_DIR="$(arg_value run-dir)"
RUN_DIR="${RUN_DIR:-runs/stage1}"     # the default spool follows a given --run-dir
pre=()
add_iwads() {
    if ! has_arg iwads; then
        pre+=(--iwads "${IWADS[@]}")
        log "IWADs: ${IWADS[*]#"$WAD_DIR/"}"
    fi
}
add_run_dir() { has_arg run-dir || pre+=(--run-dir "$RUN_DIR"); }
add_outpath() { has_arg outpath || pre+=(--outpath "$(dirname "$HERE")"); }
add_spool() { has_arg spool || pre+=(--spool "$RUN_DIR/spool"); }
# Games launch with -noblit (skip the SDL blit nobody sees headless: about
# 4x more decisions/s, frames and replays unchanged) unless --no-noblit
# (or --noblit) is given.
add_noblit() { has_arg noblit || has_arg no-noblit || pre+=(--noblit); }

case "$MODE" in
    smoke)
        # Smallest end-to-end check: one short rollout per IWAD (2 instances
        # each), one update, one checkpoint. Not enough to learn anything.
        n_smoke=${#IWADS[@]}
        has_arg iwads && n_smoke=1
        add_iwads; add_noblit
        exec "$PY" train.py --run-dir runs/smoke --groups "$n_smoke" --group-size 2 \
            --iterations 1 --max-tics 350 --eval-every 0 --capture-envs 0 "${pre[@]}" "$@" ;;
    train)
        add_run_dir; add_outpath; add_iwads; add_noblit
        exec "$PY" train.py "${pre[@]}" "$@" ;;
    learn)
        add_run_dir; add_outpath
        exec "$PY" learn.py "${pre[@]}" "$@" ;;
    actor)
        add_spool; add_iwads; add_noblit
        exec "$PY" eval.py "${pre[@]}" "$@" ;;
    heldout)
        add_spool; add_run_dir; add_iwads; add_noblit
        exec "$PY" eval.py --heldout "${pre[@]}" "$@" ;;
esac
