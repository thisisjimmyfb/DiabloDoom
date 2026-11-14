#!/usr/bin/env bash
# Launch DiabloDoom locally, in a window with sound, with a trained policy
# driving it (play.py; the game runs with -rldrive, in real time).
#
#   ./play.sh                                   # newest agent/ckpt_*.pt, Freedoom E1M1, monsters on
#   ./play.sh --checkpoint agent/ckpt_000129.pt --warp "1 2"
#   ./play.sh --greedy --delay 0.3              # most likely actions, slowed down for watching
#   ./play.sh --no-monsters                     # monsters off, as in stage-1 training
#
# Monsters are on (play.sh passes --monsters); the drive ends when combat
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

cd "$HERE"
exec "$PY" play.py --monsters "$@"
