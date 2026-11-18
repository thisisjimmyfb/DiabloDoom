#!/usr/bin/env bash
# Set up the training environment and run the trainer (SPEC.md §7.1).
#
# Full training on one box (train.py):
#   ./train.sh                       # 100 stage-1 updates in runs/stage1 (rerun: 100 more);
#                                    # the checkpoint is pulled from / pushed to the Hub
#   ./train.sh --smoke               # smallest run: 1 group of 2 per IWAD, 1 iteration, no eval
#   ./train.sh --run-dir runs/x ...  # any train.py arguments pass through
#   ./train.sh --no-noblit           # also blit (games run with -noblit by default)
#   ./train.sh --capture             # record continuous replay (off by default): the first
#                                    # training env of each IWAD -> <run-dir>/replays/
#   ./train.sh --no-hub              # don't pull from / push to the Hugging Face repo
#   ./train.sh --no-hub-download     # don't pull (start over: with a new --run-dir); still push
#   ./train.sh --no-hub-upload       # pull, but never push
#   ./train.sh --hub-repo user/name  # another repo (default thisisjimmy/DoomRunner)
#
# Hugging Face Hub (train mode, not --smoke): the repo's doom-runner.pt is
# pulled into the run directory first (resumed from unless the run
# directory has a newer checkpoint) and pushed back every --hub-every (200)
# updates and at the end. Needs the hf CLI, logged in: hf auth login.
#
# Setup is idempotent and only does what is missing:
#   - builds the game (Release) if build/src/chocolate-doom doesn't exist
#   - creates .venv (uv if available, else python3 -m venv)
#   - installs the env package (editable) and torch (CUDA 12.8 build when
#     an NVIDIA GPU is present, the RTX 5080 needs it; CPU build otherwise)
set -euo pipefail

MODE=train
case "${1:-}" in
    --smoke) MODE=smoke; shift ;;
esac

# --capture (replay capture on) and --no-hub (no Hugging Face repo) are
# train.sh's own: taken out of the arguments passed through, wherever they
# appear.
HUB_REPO=thisisjimmy/DoomRunner
CAPTURE=0
HUB=1
rest=()
for a in "$@"; do
    case "$a" in
        --capture) CAPTURE=1 ;;
        --no-hub) HUB=0 ;;
        *) rest+=("$a") ;;
    esac
done
set -- ${rest[@]+"${rest[@]}"}

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
VENV="$HERE/.venv"
PY="$VENV/bin/python"
GAME="$REPO/build/src/chocolate-doom"
WAD_DIR="$REPO/wad"

log() { printf '[train.sh] %s\n' "$*"; }

# --- game binary and IWADs ---------------------------------------------------
IWADS=()
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
ARGS=(${1+"$@"})
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
# (checkpoints, metrics, rollout log, replays; gitignored; the checkpoint
# goes to the Hugging Face repo, see above); --iwads is every IWAD
# found under wad/ (groups split evenly; --groups 8 trains all 4 maps on
# both Freedoom and Ultimate Doom). The rest are the scripts' own
# defaults (stage 1, one group of 8 per IWAD/map pair up to the logical
# cores minus 2, E1M1-E1M4, eval on 2 random held-out maps each session; --maps all
# trains every map, the groups taking turns every --rotate-every batches).
RUN_DIR="$(arg_value run-dir)"
RUN_DIR="${RUN_DIR:-runs/stage1}"
pre=()
add_iwads() {
    if ! has_arg iwads; then
        pre+=(--iwads "${IWADS[@]}")
        log "IWADs: ${IWADS[*]#"$WAD_DIR/"}"
    fi
}
add_run_dir() { has_arg run-dir || pre+=(--run-dir "$RUN_DIR"); }
# Games launch with -noblit (skip the SDL blit nobody sees headless: about
# 4x more decisions/s, frames and replays unchanged) unless --no-noblit
# (or --noblit) is given.
add_noblit() { has_arg noblit || has_arg no-noblit || pre+=(--noblit); }
# Replay capture (--continuous-capture in the games) only with --capture:
# the first training env of each IWAD records (unless --capture-envs is
# given).
add_capture() {
    (( CAPTURE )) || return 0
    has_arg capture-envs || pre+=(--capture-envs 1)
}
# The Hugging Face repo, unless --no-hub or another --hub-repo is given.
add_hub() {
    (( HUB )) || return 0
    has_arg hub-repo && return 0
    if ! command -v hf >/dev/null; then
        log "the hf CLI isn't on PATH: install it and run 'hf auth login', or pass --no-hub"
        exit 1
    fi
    pre+=(--hub-repo "$HUB_REPO")
    log "checkpoints: pulled from and pushed to huggingface.co/$HUB_REPO"
}

case "$MODE" in
    smoke)
        # Smallest end-to-end check: one short rollout per IWAD (2 instances
        # each), one update, one checkpoint. Not enough to learn anything.
        n_smoke=${#IWADS[@]}
        has_arg iwads && n_smoke=1
        add_iwads; add_noblit
        exec "$PY" train.py --run-dir runs/smoke --groups "$n_smoke" --group-size 2 \
            --iterations 1 --max-tics 350 --eval-maps "" --capture-envs 0 "${pre[@]}" "$@" ;;
    train)
        add_run_dir; add_iwads; add_noblit; add_capture; add_hub
        exec "$PY" train.py "${pre[@]}" "$@" ;;
esac
