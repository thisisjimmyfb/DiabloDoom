#!/bin/bash
# DiabloDoom farm cycle: play the existing binary (no pull, no build),
# farm ~5 minutes, screenshot.
#
# All paths and display settings live in the CONFIG section below.
# Override any of them via environment variables for local setups, e.g.:
#   REPO=~/github/DiabloDoom DOOM_WAD=~/wads/freedoom1.wad ./doom_loop.sh

set -e

# ==================== CONFIG (edit for local setups) ====================
# Repo root (contains build/src/chocolate-doom)
REPO="${REPO:-~/workspace/DiabloDoom}"
# Game binary (modded DiabloDoom build; no pull, no build — handled manually)
GAME="${GAME:-$REPO/build/src/chocolate-doom}"
# IWAD file
DOOM_WAD="${DOOM_WAD:-~/workspace/doom/freedoom1.wad}"
# Farm log (kill counts, run summaries)
LOG="${LOG:-~/workspace/doom_hourly.log}"
# Game stdout log (parsed for [TURN] state, kills, BFG)
GAME_LOG="${GAME_LOG:-/tmp/doom_hourly.log}"
# Where finished screenshots go
SCREEN_DIR="${SCREEN_DIR:-~/workspace/screenshots}"
# Working dir for in-game screenshots (SCREENSHOT driver cmd -> DOOM00.png
# lands in the game's CWD, so launch from a dedicated dir to contain them)
SHOTDIR="${SHOTDIR:-~/workspace/doomshots}"
# Driver command file (game polls this when D_OnMuseVM() is true)
CMDFILE="${CMDFILE:-/tmp/doom_cmd.txt}"
# Display settings. On headless VMs, Xvfb provides :96.
# On a local machine with a real display, set DISPLAY_USE_XVFB=0 and
# DISPLAY to your screen (usually :0).
DISPLAY_NUM="${DISPLAY_NUM:-96}"
DISPLAY_USE_XVFB="${DISPLAY_USE_XVFB:-1}"
export DISPLAY=":${DISPLAY_NUM}"
# Farm duration in seconds
FARM_SECONDS="${FARM_SECONDS:-300}"
# Warp target (episode map)
WARP="${WARP:-1 1}"
# Game window size
GAME_WIDTH="${GAME_WIDTH:-960}"
GAME_HEIGHT="${GAME_HEIGHT:-600}"
# ================== END CONFIG ==================

mkdir -p "$SCREEN_DIR" "$SHOTDIR"

echo "[$(date)] Starting hourly DiabloDoom run" >> "$LOG"

# 1. (No auto-pull, no auto-build: just play the existing binary.
# Jimmy handles pulls and builds manually.)

# 2. Check the binary exists
if [ ! -f "$GAME" ]; then
  echo "Game binary missing at $GAME" >> "$LOG"
  exit 1
fi

launch_game() {
  cd "$SHOTDIR"
  SDL_VIDEODRIVER=x11 nohup "$GAME" -iwad "$DOOM_WAD" \
    -width "$GAME_WIDTH" -height "$GAME_HEIGHT" -warp $WARP \
    > "$GAME_LOG" 2>&1 &
  disown
  sleep 15
}

# 4. Start Xvfb if needed and not running
if [ "$DISPLAY_USE_XVFB" = "1" ]; then
  if ! pgrep -x Xvfb > /dev/null; then
    Xvfb ":${DISPLAY_NUM}" -screen 0 1280x800x24 > /tmp/xvfb.log 2>&1 &
    sleep 3
  fi
fi

# 5. Kill old game, start fresh (but KEEP progress.dat for continuous progress)
pkill -9 -f "chocolate-doom" 2>/dev/null || true
sleep 2
launch_game

# 6. Play for ~5 minutes (farm loop) - fits comfortably inside the
# 10-minute cadence with room for startup, screenshots, and reporting
END_TIME=$(($(date +%s) + FARM_SECONDS))
ROUNDS=0

send_cmd() {
  printf "%b" "$1" > "$CMDFILE"
  sleep 2
}

while [ $(date +%s) -lt $END_TIME ]; do
  ROUNDS=$((ROUNDS + 1))

  if ! pgrep -f "chocolate-doom" > /dev/null; then
    echo "Game died, restarting..." >> "$LOG"
    launch_game
    continue
  fi

  # Player death is now handled in-game via Diablo-style respawn
  # (P_KillMobj respawns at level start, inventory intact).
  # No farm restart needed.

  # State detection: the game prints [TURN] lines only on explore/combat
  # transitions and at pulse end. Fresh boots land in explore SILENTLY
  # (T_NewGame sets TS_EXPLORE without dumping state), so "no [TURN]
  # line yet" means explore, not unknown. Parse state= from the last
  # [TURN] line; default to explore behavior when there isn't one.
  TURNSTATE=$(grep -a "\[TURN\]" "$GAME_LOG" 2>/dev/null | tail -1 | grep -ao "state=[A-Z_]*" | tail -1)

  if [ "$TURNSTATE" = "state=PLANNING" ]; then
    send_cmd "ATTACK\nATTACK\nEND\n"
    sleep 10
  else
    # Explore mode (or fresh boot, which starts in explore): walk, turn
    # occasionally to wander, and collect loot periodically. Driver
    # commands are no-ops outside explore, so this is safe in any state.
    send_cmd "WALK\n"
    sleep 2
    if [ $((ROUNDS % 3)) -eq 0 ]; then
      send_cmd "TURNL\n"
      sleep 1
    fi
    if [ $((ROUNDS % 10)) -eq 0 ]; then
      send_cmd "COLLECT\n"
      sleep 3
    fi
  fi

  # Screenshot every ~2-3 minutes via the game's in-game capture
  # (SCREENSHOT driver command -> DOOM00.png in $SHOTDIR: no tearing,
  # no Xlib dependency). Upscaled 3x nearest-neighbor to 960x600 to
  # match the display size. DOOM*.png are moved out each time so the
  # game's DOOM00..DOOM99 cycle never fills up.
  if [ $((ROUNDS % 10)) -eq 0 ]; then
    TS=$(date +%Y%m%d_%H%M%S)
    send_cmd "SCREENSHOT\n"
    sleep 4
    NEWEST=$(ls -t "$SHOTDIR"/DOOM*.png 2>/dev/null | head -1)
    if [ -n "$NEWEST" ]; then
      python3 - "$NEWEST" "$SCREEN_DIR/doom_farm_${TS}.png" >> "$LOG" 2>&1 <<'PYEOF' || echo "screenshot upscale failed" >> "$LOG"
import sys
from PIL import Image
src, dst = sys.argv[1], sys.argv[2]
Image.open(src).convert("RGB").resize((960, 600), Image.NEAREST).save(dst)
print("SHOT-OK", dst)
PYEOF
      rm -f "$SHOTDIR"/DOOM*.png
    else
      echo "SCREENSHOT produced no PNG this round" >> "$LOG"
    fi
  fi

  # Check for BFG
  if grep -ai "bfg" "$GAME_LOG" 2>/dev/null | grep -ai "pickup\|got\|acquired" | grep -q .; then
    echo "BFG FOUND!" >> "$LOG"
    break
  fi
done

KILLS=$(grep -ac "exec-attack-kill" "$GAME_LOG" 2>/dev/null || true)
echo "[$(date)] Hourly run complete: $ROUNDS rounds, $KILLS kills" >> "$LOG"

# Rotate farm screenshots: keep the newest 500 (~25MB), drop older ones so
# the standing every-minute capture cadence can't bloat the disk over days.
ls -t "$SCREEN_DIR"/doom_farm_*.png 2>/dev/null | tail -n +501 | xargs -r rm -f
