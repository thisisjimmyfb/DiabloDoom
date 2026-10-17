#!/bin/bash
# DiabloDoom farm: Ralph-style vision-driven loop.
#
# On launch:
#   1. git pull --rebase from GitHub; if new changes, rebuild the game.
#   2. Launch the game.
#   3. Loop until timeout or game ends:
#      - Take screenshot via SCREENSHOT driver command
#      - Feed screenshot + PROMPT.md + game log to vision LLM
#      - Vision LLM decides action(s); send via FIFO
#      - Repeat
#
# The vision LLM integration is via the VISION_CMD env var: a command that
# takes (screenshot_path, prompt_path, log_path) and outputs the driver
# command(s) to send (e.g. "WALK\n", "ATTACK\n", "COLLECT\n").
# Default: a subagent worker (see PROMPT.md) provides vision.
#
# Override paths via environment variables.

set -e

# ==================== CONFIG ====================
REPO="${REPO:-~/workspace/DiabloDoom}"
GAME="${GAME:-$REPO/build/src/chocolate-doom}"
DOOM_WAD="${DOOM_WAD:-~/workspace/doom/freedoom1.wad}"
LOG="${LOG:-~/workspace/doom_hourly.log}"
GAME_LOG="${GAME_LOG:-/tmp/doom_hourly.log}"
SCREEN_DIR="${SCREEN_DIR:-~/workspace/screenshots}"
# Screenshots: /tmp on Muse VMs (ephemeral), $HOME/doomshots on local machines.
if [[ "${JARVIS_FQDN:-}" == *"metaaivm.com"* ]]; then
    SHOTDIR="${SHOTDIR:-/tmp/doomshots}"
else
    SHOTDIR="${SHOTDIR:-$HOME/doomshots}"
fi
CMDFILE="${CMDFILE:-/tmp/doom_cmd.txt}"
PROMPT="${PROMPT:-$REPO/scripts/PROMPT.md}"
FARM_SECONDS="${FARM_SECONDS:-300}"
WARP="${WARP:-1 1}"
GAME_WIDTH="${GAME_WIDTH:-960}"
GAME_HEIGHT="${GAME_HEIGHT:-600}"
# Vision command: takes screenshot, prompt, log; outputs driver commands.
# If unset, the loop pauses for manual/external vision input.
VISION_CMD="${VISION_CMD:-}"
# ================== END CONFIG ==================

mkdir -p "$SCREEN_DIR" "$SHOTDIR"
echo "[$(date)] Starting DiabloDoom Ralph loop" >> "$LOG"

# 1. Pull latest from GitHub; rebuild if changed.
cd "$REPO"
BEFORE=$(git rev-parse HEAD)
echo "[$(date)] Pulling from GitHub..." >> "$LOG"
if git pull --rebase 2>> "$LOG"; then
    AFTER=$(git rev-parse HEAD)
    if [ "$BEFORE" != "$AFTER" ]; then
        echo "[$(date)] New changes pulled ($BEFORE -> $AFTER), rebuilding..." >> "$LOG"
        # Backup live binary before rebuild (failed link can destroy it).
        cp "$GAME" ~/workspace/chocolate-doom.farm-backup 2>/dev/null || true
        cd "$REPO/build"
        # Re-run cmake (link.txt goes stale across VM recycles).
        cmake . >> "$LOG" 2>&1 || echo "cmake failed" >> "$LOG"
        if make -j2 chocolate-doom >> "$LOG" 2>&1; then
            echo "[$(date)] Rebuild successful" >> "$LOG"
            cp "$GAME" ~/workspace/chocolate-doom.farm-backup
        else
            echo "[$(date)] Rebuild FAILED, restoring backup" >> "$LOG"
            cp ~/workspace/chocolate-doom.farm-backup "$GAME" 2>/dev/null || true
        fi
        cd "$REPO"
    else
        echo "[$(date)] Already up to date ($AFTER)" >> "$LOG"
    fi
else
    echo "[$(date)] git pull failed, continuing with existing binary" >> "$LOG"
fi

# 2. Check binary exists.
if [ ! -f "$GAME" ]; then
    echo "Game binary missing at $GAME" >> "$LOG"
    exit 1
fi

launch_game() {
    cd "$SHOTDIR"
    # Clear stale FIFO (game recreates it; avoids worker-written regular file race).
    rm -f "$CMDFILE"
    SDL_VIDEODRIVER=x11 nohup "$GAME" -iwad "$DOOM_WAD" \
        -width "$GAME_WIDTH" -height "$GAME_HEIGHT" -warp $WARP \
        > "$GAME_LOG" 2>&1 &
    disown
    sleep 15
}

# 3. Kill old game, start fresh (KEEP progress.dat for continuous progress).
# Use bracket trick to avoid pkill matching our own shell.
pkill -9 -f "chocolate-doo[m]" 2>/dev/null || true
sleep 2
launch_game

# 4. Ralph loop: screenshot -> vision -> act -> repeat.
END_TIME=$(($(date +%s) + FARM_SECONDS))
ITER=0

send_cmd() {
    printf "%b" "$1" > "$CMDFILE"
    sleep 2
}

take_screenshot() {
    local ts=$(date +%Y%m%d_%H%M%S)
    local out="$SCREEN_DIR/doom_ralph_${ts}_${ITER}.png"
    send_cmd "SCREENSHOT\n"
    sleep 4
    local newest=$(ls -t "$SHOTDIR"/DOOM*.png 2>/dev/null | head -1)
    if [ -n "$newest" ]; then
        # Upscale 3x nearest-neighbor to match display size.
        python3 - "$newest" "$out" >> "$LOG" 2>&1 <<'PYEOF' || echo "screenshot failed" >> "$LOG"
import sys
from PIL import Image
src, dst = sys.argv[1], sys.argv[2]
Image.open(src).convert("RGB").resize((960, 600), Image.NEAREST).save(dst)
print("SHOT-OK", dst)
PYEOF
        rm -f "$SHOTDIR"/DOOM*.png
        echo "$out"
    else
        echo ""
    fi
}

while [ $(date +%s) -lt $END_TIME ]; do
    ITER=$((ITER + 1))
    echo "[$(date)] --- Iteration $ITER ---" >> "$LOG"

    # Check game alive.
    if ! pgrep -f "chocolate-doo[m]" > /dev/null; then
        echo "Game died, restarting..." >> "$LOG"
        launch_game
        continue
    fi

    # Check for BFG (goal reached).
    if grep -ai "bfg" "$GAME_LOG" 2>/dev/null | grep -ai "pickup\|got\|acquired" | grep -q .; then
        echo "BFG FOUND! Ending loop." >> "$LOG"
        break
    fi

    # Take screenshot for vision.
    SHOT=$(take_screenshot)
    if [ -z "$SHOT" ]; then
        echo "Screenshot failed, skipping iteration" >> "$LOG"
        sleep 5
        continue
    fi

    # Get recent log context (last 50 lines).
    LOGCTX=$(tail -50 "$GAME_LOG" 2>/dev/null)

    # Vision LLM decides action.
    if [ -n "$VISION_CMD" ]; then
        # VISION_CMD takes: screenshot_path prompt_path log_context
        # Outputs: driver command(s) to send (e.g. "WALK\nATTACK\n")
        ACTION=$($VISION_CMD "$SHOT" "$PROMPT" "$LOGCTX" 2>> "$LOG")
        if [ -n "$ACTION" ]; then
            echo "[$(date)] Vision action: $(echo "$ACTION" | tr '\n' ' ')" >> "$LOG"
            send_cmd "$ACTION"
        else
            echo "[$(date)] Vision returned empty, defaulting to WALK" >> "$LOG"
            send_cmd "WALK\n"
        fi
    else
        # No vision command configured: save screenshot + context for
        # external vision agent (e.g. cron worker subagent reads PROMPT.md).
        # Default to a safe exploratory action.
        echo "[$(date)] No VISION_CMD; screenshot $SHOT awaiting external vision" >> "$LOG"
        # Write state file for external agent pickup.
        cat > /tmp/doom_ralph_state.json <<EOF
{
    "iteration": $ITER,
    "screenshot": "$SHOT",
    "prompt": "$PROMPT",
    "log_tail": $(echo "$LOGCTX" | python3 -c "import sys,json; print(json.dumps(sys.stdin.read()))"),
    "timestamp": "$(date -Iseconds)"
}
EOF
        # Default action while waiting: cautious explore.
        send_cmd "WALK\n"
        sleep 3
    fi

    # Brief pause between iterations (vision needs time to process).
    sleep 5
done

KILLS=$(grep -ac "exec-attack-kill" "$GAME_LOG" 2>/dev/null || true)
echo "[$(date)] Ralph loop complete: $ITER iterations, $KILLS kills" >> "$LOG"

# Rotate screenshots: keep newest 500.
ls -t "$SCREEN_DIR"/doom_ralph_*.png 2>/dev/null | tail -n +501 | xargs -r rm -f
