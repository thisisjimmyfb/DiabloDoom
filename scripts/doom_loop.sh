#!/bin/bash
# DiabloDoom farm: Ralph-style vision-driven loop.
#
# On launch:
#   1. git pull --rebase from GitHub; if new changes, rebuild the game.
#   2. Launch the game.
#   3. Loop until timeout or game ends:
#      - Capture replay video via REPLAY driver command (15s H.264 + AAC)
#      - Feed replay video + PROMPT.md + game log to vision LLM
#      - Vision LLM decides action(s); send via FIFO
#      - Repeat
#
# The vision LLM integration is via the VISION_CMD env var: a command that
# takes (replay_path, prompt_path, log_path) and outputs the driver
# command(s) to send (e.g. "WALK\n", "ATTACK\n", "COLLECT\n").
# Default: a subagent worker (see PROMPT.md) provides vision.
#
# Sub-agent mode: --subagent {setup|observe|act} [--file path].
# The workflow (scripts/workflows/diablo-doom-loop.js) drives the loop:
# the script only captures (observe) or sends (act) — it never decides.
# Subagent mode never pulls, rebuilds, or kills the game.
#
# Override paths via environment variables.

set -e

# ==================== CONFIG ====================
REPO="${REPO:-$HOME/workspace/DiabloDoom}"
GAME="${GAME:-$REPO/build/src/chocolate-doom}"
DOOM_WAD="${DOOM_WAD:-$HOME/workspace/doom/freedoom1.wad}"
LOG="${LOG:-$HOME/workspace/doom_hourly.log}"
GAME_LOG="${GAME_LOG:-/tmp/doom_hourly.log}"
SCREEN_DIR="${SCREEN_DIR:-$HOME/workspace/screenshots}"
# Screenshots: /tmp on Muse VMs (ephemeral), $HOME/doomshots on local machines.
if [[ "${JARVIS_FQDN:-}" == *"metaaivm.com"* ]]; then
    SHOTDIR="${SHOTDIR:-/tmp/doomshots}"
else
    SHOTDIR="${SHOTDIR:-$HOME/doomshots}"
fi
CMDFILE="${CMDFILE:-/tmp/doom_cmd.txt}"
PROMPT="${PROMPT:-$REPO/scripts/PROMPT.md}"
FARM_SECONDS="${FARM_SECONDS:-300}"

# Subagent mode: --subagent {setup|observe|act} [--file path]
# The workflow (scripts/workflows/diablo-doom-loop.js) orchestrates:
#   setup   - ensure game running (launch only if needed), LOAD/SAVE slot 1
#   observe - REPLAY capture + log tail -> JSON {replay_path, log_tail, kills}
#   act     - write driver commands from --file to the game FIFO -> JSON {ok}
# Subagent mode never pulls, rebuilds, or kills the game.
SUBAGENT=false
SUBCMD=""
ACT_FILE=""
for arg in "$@"; do
    case "$arg" in
        --subagent) SUBAGENT=true ;;
        setup|observe|act) SUBCMD="$arg" ;;
        --file=*) ACT_FILE="${arg#--file=}" ;;
    esac
done
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
# Skipped entirely in --subagent mode (never pull, never rebuild).
if [ "$SUBAGENT" != true ]; then
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
fi # end: skip pull/rebuild in --subagent mode

# 2. Check binary exists.
if [ ! -f "$GAME" ]; then
    echo "Game binary missing at $GAME" >> "$LOG"
    exit 1
fi

launch_game() {
    cd "$SHOTDIR"
    # Clear stale FIFO (game recreates it; avoids worker-written regular file race).
    rm -f "$CMDFILE"
    SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-dummy} nohup "$GAME" -iwad "$DOOM_WAD" \
        -width "$GAME_WIDTH" -height "$GAME_HEIGHT" -warp $WARP \
        -nosound > "$GAME_LOG" 2>&1 &
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

# Load autosave slot 1 if it exists (resume where the last cycle left off).
# Doom savegame slot 0 + progress_auto1.dat.
if [ -f ~/.local/share/chocolate-doom/doomsav0.dsg ]; then
    echo "[$(date)] Autosave found, loading slot 0" >> "$LOG"
    send_cmd "LOAD 0\n"
    sleep 5
else
    echo "[$(date)] No autosave found, starting new game with autosave slot 1" >> "$LOG"
    # Initialize autosave slot 1 immediately.
    send_cmd "SAVE 0\n"
    sleep 5
fi

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

capture_replay() {
    local ts=$(date +%Y%m%d_%H%M%S)
    local out="$SCREEN_DIR/doom_ralph_${ts}_${ITER}.mp4"
    send_cmd "REPLAY\n"
    sleep 18
    local newest=$(ls -t "$SHOTDIR"/*.mp4 2>/dev/null | head -1)
    if [ -n "$newest" ]; then
        mv "$newest" "$out"
        echo "$out"
    else
        echo ""
    fi
}

# ---------------- subagent subcommands ----------------
# setup: ensure the game is running (launch only if needed), then
# LOAD 0 if autosave slot 1 exists, else SAVE 0 to initialize it.
subagent_setup() {
    local game_count launched save_status
    game_count=$(pgrep -c -f "chocolate-doo[m]" 2>/dev/null || true); game_count=${game_count:-0}
    if [ "$game_count" -gt 0 ]; then
        # A game is already running (possibly stale: stuck pre-GS_LEVEL or
        # holding a deleted FIFO). Kill all and launch fresh for a clean
        # command channel.
        pkill -9 -f "chocolate-doo[m]" 2>/dev/null || true
        sleep 2
    fi
    launch_game
    launched=1
    save_status="fresh"
    if [ -f ~/.local/share/chocolate-doom/doomsav0.dsg ]; then
        send_cmd "LOAD 0\n"
        sleep 5
        save_status="loaded"
    else
        send_cmd "SAVE 0\n"
        sleep 5
        save_status="initialized"
    fi
    LAUNCHED="$launched" KILLED="$game_count" SAVE_STATUS="$save_status" python3 - <<'PYEOF'
import json, os
print(json.dumps({
    "ok": True,
    "launched": os.environ.get("LAUNCHED") == "1",
    "killed": int(os.environ.get("KILLED", 0) or 0),
    "save": os.environ.get("SAVE_STATUS", ""),
    "notes": "setup complete"
}))
PYEOF
}

# observe: REPLAY capture + recent log tail -> JSON.
subagent_observe() {
    local game_count ts out newest replay_path log_tail kills
    game_count=$(pgrep -c -f "chocolate-doo[m]" 2>/dev/null || true); game_count=${game_count:-0}
    if [ "$game_count" -eq 0 ]; then
        echo '{"error": "game not running", "game_count": 0}'
        exit 1
    fi
    ts=$(date +%Y%m%d_%H%M%S)
    out="$SCREEN_DIR/doom_ralph_${ts}_obs.mp4"
    # REPLAY dumps the 15s ring buffer synchronously; poll for the new file
    # instead of sleeping a fixed 18s.
    before=$(ls -t "$SHOTDIR"/*.mp4 2>/dev/null | head -1)
    send_cmd "REPLAY\n"
    newest=""
    for i in $(seq 1 16); do
        sleep 1
        newest=$(ls -t "$SHOTDIR"/*.mp4 2>/dev/null | head -1)
        if [ -n "$newest" ] && [ "$newest" != "$before" ]; then
            break
        fi
        newest=""
    done
    replay_path=""
    if [ -n "$newest" ]; then
        mv "$newest" "$out"
        replay_path="$out"
    fi
    log_tail=$(tail -50 "$GAME_LOG" 2>/dev/null)
    kills=$(grep -ac "exec-attack-kill" "$GAME_LOG" 2>/dev/null || true); kills=${kills:-0}
    REPLAY_PATH="$replay_path" LOG_TAIL="$log_tail" KILLS="$kills" python3 - <<'PYEOF'
import json, os
print(json.dumps({
    "replay_path": os.environ.get("REPLAY_PATH", ""),
    "log_tail": os.environ.get("LOG_TAIL", ""),
    "kills": int(os.environ.get("KILLS", 0) or 0),
}))
PYEOF
}

# act: write driver commands from --file to the game FIFO -> JSON.
subagent_act() {
    if [ -z "$ACT_FILE" ] || [ ! -f "$ACT_FILE" ]; then
        echo '{"ok": false, "error": "no command file"}'
        exit 1
    fi
    if ! pgrep -f "chocolate-doo[m]" > /dev/null; then
        echo '{"ok": false, "error": "game not running"}'
        exit 1
    fi
    cat "$ACT_FILE" > "$CMDFILE"
    sleep 2
    echo '{"ok": true}'
}

if [ "$SUBAGENT" = true ]; then
    case "$SUBCMD" in
        setup) subagent_setup ;;
        observe) subagent_observe ;;
        act) subagent_act ;;
        *)
            echo "usage: $0 --subagent {setup|observe|act} [--file path]" >&2
            exit 1
            ;;
    esac
    exit 0
fi

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

    # Capture replay video for vision.
    REPLAY=$(capture_replay)
    if [ -z "$REPLAY" ]; then
        echo "Replay capture failed, skipping iteration" >> "$LOG"
        sleep 5
        continue
    fi

    # Get recent log context (last 50 lines).
    LOGCTX=$(tail -50 "$GAME_LOG" 2>/dev/null)

    # Vision LLM decides action.
    if [ -n "$VISION_CMD" ]; then
        # VISION_CMD takes: replay_path prompt_path log_context
        # Outputs: driver command(s) to send (e.g. "WALK\nATTACK\n")
        ACTION=$($VISION_CMD "$REPLAY" "$PROMPT" "$LOGCTX" 2>> "$LOG")
        if [ -n "$ACTION" ]; then
            echo "[$(date)] Vision action: $(echo "$ACTION" | tr '\n' ' ')" >> "$LOG"
            send_cmd "$ACTION"
        else
            echo "[$(date)] Vision returned empty, defaulting to WALK" >> "$LOG"
            send_cmd "WALK\n"
        fi
    else
        # No vision command configured: save replay + context for
        # external vision agent (e.g. cron worker subagent reads PROMPT.md).
        # Default to a safe exploratory action.
        echo "[$(date)] No VISION_CMD; replay $REPLAY awaiting external vision" >> "$LOG"
        # Write state file for external agent pickup.
        cat > /tmp/doom_ralph_state.json <<EOF
{
    "iteration": $ITER,
    "replay": "$REPLAY",
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
ls -t "$SCREEN_DIR"/doom_ralph_*.mp4 2>/dev/null | tail -n +101 | xargs -r rm -f
