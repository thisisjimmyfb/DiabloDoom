#!/bin/bash
# DiabloDoom farm cycle: play the existing binary (no pull, no build),
# farm ~5 minutes, screenshot.
# Runs on the Muse VM. All state under ~ (survives recycle), never /tmp.

set -e

GAME=~/workspace/DiabloDoom/build/src/chocolate-doom
# Modded DiabloDoom binary, built from ~/workspace/DiabloDoom
# (includes the SCREENSHOT driver command + PNG screenshots).
# No pull, no build — Jimmy handles those.
IWAD=~/workspace/doom/freedoom1.wad
LOG=~/workspace/doom_hourly.log
SCREEN_DIR=~/workspace/screenshots
# In-game screenshots (SCREENSHOT driver command -> DOOM00.png) land in
# the game's CWD, so launch from a dedicated dir to keep them contained.
SHOTDIR=~/workspace/doomshots

mkdir -p $SCREEN_DIR $SHOTDIR

echo "[$(date)] Starting hourly DiabloDoom run" >> $LOG

# 1. (No auto-pull, no auto-build: just play the existing binary.
# Jimmy handles pulls and builds manually.)

# 2. Check the binary exists
if [ ! -f $GAME ]; then
  echo "Game binary missing at $GAME" >> $LOG
  exit 1
fi

# 4. Start Xvfb if not running
if ! pgrep -x Xvfb > /dev/null; then
  Xvfb :96 -screen 0 1280x800x24 > /tmp/xvfb.log 2>&1 &
  sleep 3
fi

# 5. Kill old game, start fresh (but KEEP progress.dat for continuous progress)
pkill -9 -f "chocolate-doom" 2>/dev/null || true
sleep 2
cd $SHOTDIR
DISPLAY=:96 SDL_VIDEODRIVER=x11 nohup $GAME -iwad $IWAD -width 960 -height 600 -warp 1 1 > /tmp/doom_hourly.log 2>&1 &
disown
sleep 15

# 6. Play for ~5 minutes (farm loop) - fits comfortably inside the
# 10-minute cadence with room for startup, screenshots, and reporting
CMDFILE=/tmp/doom_cmd.txt
END_TIME=$(($(date +%s) + 300))
ROUNDS=0

send_cmd() {
  printf "%b" "$1" > "$CMDFILE"
  sleep 2
}

while [ $(date +%s) -lt $END_TIME ]; do
  ROUNDS=$((ROUNDS + 1))

  if ! pgrep -f "chocolate-doom" > /dev/null; then
    echo "Game died, restarting..." >> $LOG
    cd $SHOTDIR
    DISPLAY=:96 SDL_VIDEODRIVER=x11 nohup $GAME -iwad $IWAD -width 960 -height 600 -warp 1 1 > /tmp/doom_hourly.log 2>&1 &
    disown
    sleep 15
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
  TURNSTATE=$(grep -a "\[TURN\]" /tmp/doom_hourly.log 2>/dev/null | tail -1 | grep -ao "state=[A-Z_]*" | tail -1)

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
    NEWEST=$(ls -t $SHOTDIR/DOOM*.png 2>/dev/null | head -1)
    if [ -n "$NEWEST" ]; then
      python3 - "$NEWEST" "$SCREEN_DIR/doom_farm_${TS}.png" >> $LOG 2>&1 <<'PYEOF' || echo "screenshot upscale failed" >> $LOG
import sys
from PIL import Image
src, dst = sys.argv[1], sys.argv[2]
Image.open(src).convert("RGB").resize((960, 600), Image.NEAREST).save(dst)
print("SHOT-OK", dst)
PYEOF
      rm -f $SHOTDIR/DOOM*.png
    else
      echo "SCREENSHOT produced no PNG this round" >> $LOG
    fi
  fi

  # Check for BFG
  if grep -ai "bfg" /tmp/doom_hourly.log 2>/dev/null | grep -ai "pickup\|got\|acquired" | grep -q .; then
    echo "BFG FOUND!" >> $LOG
    break
  fi
done

KILLS=$(grep -ac "exec-attack-kill" /tmp/doom_hourly.log 2>/dev/null || true)
echo "[$(date)] Hourly run complete: $ROUNDS rounds, $KILLS kills" >> $LOG

# Rotate farm screenshots: keep the newest 500 (~25MB), drop older ones so
# the standing every-minute capture cadence can't bloat the disk over days.
ls -t $SCREEN_DIR/doom_farm_*.png 2>/dev/null | tail -n +501 | xargs -r rm -f
