#!/usr/bin/env bash
# Run the tests in tests/ with pytest, game runs included (about 3 minutes).
# Arguments pass through to pytest:
#
#   ./test.sh                       # everything
#   ./test.sh -m "not game"         # fast: only the tests that need no game
#   ./test.sh tests/test_learn.py   # one file
#   ./test.sh -k resume -s          # by name, with the tests' own output
#
# Installs pytest into .venv if it's missing. Needs the setup train.sh does
# (the game build and .venv): run ./train.sh --smoke once first on a fresh
# checkout.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PY="$HERE/.venv/bin/python"
GAME="$HERE/../../build/src/chocolate-doom"

if [[ ! -x "$PY" || ! -x "$GAME" ]]; then
    echo "[test.sh] missing .venv or the game build; run ./train.sh --smoke once first" >&2
    exit 1
fi
if ! "$PY" -c "import pytest" 2>/dev/null; then
    echo "[test.sh] installing pytest"
    if command -v uv >/dev/null; then
        uv pip install --python "$PY" pytest
    else
        "$PY" -m pip install pytest
    fi
fi

cd "$HERE"
exec "$PY" -m pytest "$@" -v
