#!/usr/bin/env bash
# Launch DiabloDoom locally, in a window with sound, with a trained policy
# driving it (play.py; the game runs with -rldrive, in real time).
#
#   ./play.sh                                   # newest agent/ckpt_*.pt, first IWAD under wad/, E1M1, monsters on
#   IWAD_INDEX=2 ./play.sh                      # the second IWAD in the printed list
#   ./play.sh --checkpoint agent/ckpt_000129.pt --warp "1 2"
#   ./play.sh --greedy --delay 0.3              # most likely actions, slowed down for watching
#   ./play.sh --no-monster                      # monsters off, as in stage-1 training
#
# Monsters are on by default (--monster; --no-monster turns them off, and
# the plural spellings work too); the drive ends when combat
# starts, since CONSULT's VLM isn't attached here. The drive stops at the
# exit, on death, in combat, after --max-decisions, or on Ctrl-C; the game
# window then stays open for you (--quit-at-end closes it). Arguments pass
# through to play.py (--help lists them).
# Needs the setup train.sh does: run ./train.sh --smoke once on a fresh checkout.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
PY="$HERE/.venv/bin/python"

if [[ ! -x "$REPO/build/src/chocolate-doom" ]]; then
    echo "[play.sh] game not built; building (Release)"
    "$REPO/scripts/build.sh" --release
fi
if [[ ! -x "$PY" ]] || ! "$PY" -c "import torch, env" 2>/dev/null; then
    echo "[play.sh] missing .venv (torch, env); run ./train.sh --smoke once first" >&2
    exit 1
fi
if [[ -z "${DISPLAY:-}" && -z "${WAYLAND_DISPLAY:-}" ]]; then
    echo "[play.sh] warning: no DISPLAY or WAYLAND_DISPLAY; the game window can't open" >&2
fi

# IWADs: every .wad under wad/ (any case, any depth) whose header says
# IWAD, sorted, as train.sh finds them. --iwad defaults to the first;
# IWAD_INDEX=n picks the n-th of the listed ones (1-based). An --iwad
# given on the command line wins over both.
WAD_DIR="$REPO/wad"
IWADS=()
if [[ -d "$WAD_DIR" ]]; then
    while IFS= read -r -d '' f; do
        [[ "$(head -c 4 "$f")" == IWAD ]] && IWADS+=("$f")
    done < <(find "$WAD_DIR" -type f -iname '*.wad' -print0 | sort -z)
fi
if (( ${#IWADS[@]} == 0 )); then
    echo "[play.sh] no IWAD found under $WAD_DIR" >&2
    exit 1
fi
IWAD_INDEX="${IWAD_INDEX:-1}"
echo "[play.sh] IWADs (IWAD_INDEX=n picks one):"
for i in "${!IWADS[@]}"; do
    mark=" "; (( i + 1 == IWAD_INDEX )) && mark="*"
    printf '  %s %d  %s\n' "$mark" "$((i + 1))" "${IWADS[$i]#"$WAD_DIR/"}"
done
if ! [[ "$IWAD_INDEX" =~ ^[0-9]+$ ]] || (( IWAD_INDEX < 1 || IWAD_INDEX > ${#IWADS[@]} )); then
    echo "[play.sh] IWAD_INDEX=$IWAD_INDEX out of range (1..${#IWADS[@]})" >&2
    exit 1
fi
IWAD="${IWADS[$((IWAD_INDEX - 1))]}"

# --monster / --no-monster: play.py's --monsters / --no-monsters. The
# defaults go first, so one given on the command line wins.
args=()
for a in "$@"; do
    case "$a" in
        --monster) args+=(--monsters) ;;
        --no-monster) args+=(--no-monsters) ;;
        *) args+=("$a") ;;
    esac
done

cd "$HERE"
exec "$PY" play.py --monsters --iwad "$IWAD" ${args[@]+"${args[@]}"}
