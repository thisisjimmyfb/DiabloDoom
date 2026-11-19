#!/usr/bin/env bash
# Evaluate a trained policy.
#
# By default, a training-style eval: eval.py plays every training IWAD/map
# pair (every IWAD under wad/ unless --iwads; --maps, default 1 1..1 4)
# --group-size times (8), each game in its own window (no sound, as fast as
# it runs; --headless: no windows), with training's env,
# rewards and rollout rules -- the --max-tics limit (525), ended early by
# the portal and key goals, from training's random group starts --
# --groups pairs at a time (the cores minus 2), and prints one line per
# pair and an overall one. Other arguments pass through to eval.py.
#
# With --iwad <path>, watch it play instead: launch DiabloDoom locally on
# that IWAD file, in a window with sound, with the policy driving it
# (eval.py --iwad; the game runs with -rldrive, in real time), and print the
# drive's stats at the end (sectors gained, exit, death, health, keys,
# distance, game time, action use). With the map's monsters since
# 2026-11-18 (--no-monsters: none, --eval, as before), practice items and, if asked
# for, practice monsters (--item-tics, --spawn-tics; the latter off by
# default since 2026-11-18; 0 turns them off), with a line counting
# sightings, items and uniques, and the CONSULTs on them. The drive
# stops at the exit (on death too with --on-death stop; by default it
# respawns with a new gun and plays on, 2026-11-18), after
# --max-decisions, or on Ctrl-C (a CONSULT shows "CONSULT" on screen and
# removes every practice monster, in sight or not: CONSULT's VLM isn't attached here); the game window then stays open for
# you (--quit-at-end closes it). Other arguments pass through to eval.py.
#
#   ./eval.sh                                   # training-style eval: the Hub checkpoint, every IWAD, maps 1 1..1 4, 525 tics
#   ./eval.sh --max-tics 2100 --maps all
#   ./eval.sh --headless                        # no game windows
#   ./eval.sh --temperature 0.5                 # sample more decisively, key by key (0: each key's most likely; default 1, as trained)
#   ./eval.sh --group-size 2 --groups 2         # 4 windows at a time instead of 64
#   ./eval.sh --iwad ../../wad/UltimateDoom/DOOM.WAD                  # watch it play Ultimate Doom E1M1
#   ./eval.sh --iwad ../../wad/FreedoomPhase1/FREEDOOM1.WAD --warp "1 2" --temperature 0.5 --delay 0.3
#
# The policy (both modes): --checkpoint if given; else the Hugging Face
# repo's doom-runner.pt (--hub-repo, default thisisjimmy/DoomRunner;
# downloaded into runs/hub-play/ with the hf CLI, no login needed for a
# public repo), falling back to the newest local checkpoint when it can't
# be downloaded; --no-hub skips the repo.
# Needs the setup train.sh does: run ./train.sh --smoke once on a fresh checkout.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
PY="$HERE/.venv/bin/python"

if [[ ! -x "$REPO/build/src/chocolate-doom" ]]; then
    echo "[eval.sh] game not built; building (Release)"
    "$REPO/scripts/build.sh" --release
fi
if [[ ! -x "$PY" ]] || ! "$PY" -c "import torch, env" 2>/dev/null; then
    echo "[eval.sh] missing .venv (torch, env); run ./train.sh --smoke once first" >&2
    exit 1
fi
# IWADs: every .wad under wad/ (any case, any depth) whose header says
# IWAD, sorted, as train.sh finds them.
WAD_DIR="$REPO/wad"
IWADS=()
if [[ -d "$WAD_DIR" ]]; then
    while IFS= read -r -d '' f; do
        [[ "$(head -c 4 "$f")" == IWAD ]] && IWADS+=("$f")
    done < <(find "$WAD_DIR" -type f -iname '*.wad' -print0 | sort -z)
fi
if (( ${#IWADS[@]} == 0 )); then
    echo "[eval.sh] no IWAD found under $WAD_DIR" >&2
    exit 1
fi
HUB_REPO=thisisjimmy/DoomRunner

# Pull out eval.sh's own arguments: --iwad <path> (watch mode) and --no-hub.
IWAD=""
HUB=1
args=()
while (( $# )); do
    case "$1" in
        --iwad) [[ $# -ge 2 ]] || { echo "[eval.sh] --iwad needs an IWAD path" >&2; exit 1; }
                IWAD="$2"; shift ;;
        --iwad=*) IWAD="${1#--iwad=}" ;;
        --no-hub) HUB=0 ;;
        --checkpoint|--checkpoint=*|--hub-repo|--hub-repo=*) HUB=0; args+=("$1") ;;
        *) args+=("$1") ;;
    esac
    shift
done
if [[ -n "$IWAD" ]]; then
    # A path, resolved here: eval.py runs from this script's directory.
    if [[ ! -f "$IWAD" ]]; then
        echo "[eval.sh] --iwad $IWAD: no such file (it takes an IWAD path); the IWADs under wad/:" >&2
        for f in "${IWADS[@]}"; do echo "  $f" >&2; done
        exit 1
    fi
    IWAD="$(cd "$(dirname "$IWAD")" && pwd)/$(basename "$IWAD")"
fi
pre=()
(( HUB )) && pre+=(--hub-repo "$HUB_REPO")
cd "$HERE"

if [[ -z "$IWAD" ]]; then
    # Training-style eval (eval.py): every IWAD unless --iwads is given.
    has_iwads=0
    for a in ${args[@]+"${args[@]}"}; do [[ "$a" == --iwads || "$a" == --iwads=* ]] && has_iwads=1; done
    if (( ! has_iwads )); then
        pre+=(--iwads "${IWADS[@]}")
        echo "[eval.sh] training-style eval on: ${IWADS[*]#"$WAD_DIR/"} (--iwad <path> watches one play instead)"
    fi
    exec "$PY" eval.py "${pre[@]}" ${args[@]+"${args[@]}"}
fi

echo "[eval.sh] watching it play $IWAD"
if [[ -z "${DISPLAY:-}" && -z "${WAYLAND_DISPLAY:-}" ]]; then
    echo "[eval.sh] warning: no DISPLAY or WAYLAND_DISPLAY; the game window can't open" >&2
fi
exec "$PY" eval.py --iwad "$IWAD" "${pre[@]}" ${args[@]+"${args[@]}"}
