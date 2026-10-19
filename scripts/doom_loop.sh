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
SETUP_LOCK="${SETUP_LOCK:-/tmp/doom_setup.lock}"
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

pull_and_rebuild() {
    cd "$REPO"
    local before after last_built
    last_built=$(cat "$REPO/build/.built_head" 2>/dev/null || echo "none")
    before=$(git rev-parse HEAD)
    echo "[$(date)] Pulling from GitHub..." >> "$LOG"
    if git pull --rebase 2>> "$LOG"; then
        after=$(git rev-parse HEAD)
        # Rebuild if the pull moved HEAD, OR if local commits (pushed from this
        # VM) left the binary behind — pull never moves HEAD for those.
        if [ "$before" != "$after" ] || [ "$after" != "$last_built" ]; then
            echo "[$(date)] Binary stale (built=$last_built, HEAD=$after), rebuilding..." >> "$LOG"
            cp "$GAME" ~/workspace/chocolate-doom.farm-backup 2>/dev/null || true
            # Install vendored dev headers (third_party/dev-debs) if the system
            # lost them (VM recycle wipes /usr). SDL2 itself falls back to
            # third_party/ via CMakeLists.txt; FluidSynth/SampleRate need debs.
            if [ ! -f /usr/include/fluidsynth.h ]; then
                sudo dpkg -i "$REPO/third_party/dev-debs/"*.deb >> "$LOG" 2>&1 || true
            fi
            cd "$REPO/build"
            # Re-run cmake (link.txt goes stale across VM recycles).
            cmake . >> "$LOG" 2>&1 || echo "cmake failed" >> "$LOG"
            if make -j2 chocolate-doom >> "$LOG" 2>&1; then
                echo "[$(date)] Rebuild successful" >> "$LOG"
                echo "$after" > "$REPO/build/.built_head"
                cp "$GAME" ~/workspace/chocolate-doom.farm-backup
            else
                echo "[$(date)] Rebuild FAILED, restoring backup" >> "$LOG"
                cp ~/workspace/chocolate-doom.farm-backup "$GAME" 2>/dev/null || true
            fi
            cd "$REPO"
        else
            echo "[$(date)] Binary already current ($after)" >> "$LOG"
        fi
    else
        echo "[$(date)] git pull failed, continuing with existing binary" >> "$LOG"
    fi
}


mkdir -p "$SCREEN_DIR" "$SHOTDIR"
echo "[$(date)] Starting DiabloDoom Ralph loop" >> "$LOG"

# 1. Pull latest from GitHub; rebuild if changed.
# Skipped entirely in --subagent mode (never pull, never rebuild).
if [ "$SUBAGENT" != true ]; then
    pull_and_rebuild
fi # end: skip pull/rebuild in --subagent mode (except setup, see below)

# Pull from GitHub; rebuild the binary if anything changed.
# Backs up the live binary first (a failed link step can destroy it).
# 2. Check binary exists.
if [ ! -f "$GAME" ]; then
    echo "Game binary missing at $GAME" >> "$LOG"
    exit 1
fi

launch_game() {
    cd "$SHOTDIR"
    # Clear stale FIFO (game recreates it; avoids worker-written regular file race).
    rm -f "$CMDFILE"
    # Close the setup-lock fd in the game child so the lock releases when
    # this script exits (not when the game dies).
    SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-dummy} nohup "$GAME" -iwad "$DOOM_WAD" \
        -width "$GAME_WIDTH" -height "$GAME_HEIGHT" -warp $WARP \
        -nosound 9>&- > "$GAME_LOG" 2>&1 &
    disown
    sleep 15
}

# 3. Kill old game, start fresh (KEEP progress.dat for continuous progress).
# Full-loop mode only: --subagent dispatches below and must NOT kill here.
# Use bracket trick to avoid pkill matching our own shell.
if [ "$SUBAGENT" != true ]; then
pkill -9 -f "chocolate-doo[m]" 2>/dev/null || true
sleep 2
launch_game
fi

# 4. Ralph loop: screenshot -> vision -> act -> repeat.
END_TIME=$(($(date +%s) + FARM_SECONDS))
ITER=0

send_cmd() {
    # Never block forever: a readerless FIFO (ghost) would hang the caller.
    if ! timeout 10 bash -c 'printf "%b" "$1" > "$2"' _ "$1" "$CMDFILE"; then
        echo "[$(date)] send_cmd timed out (FIFO not draining): $1" >> "$LOG"
        return 1
    fi
    sleep 2
}

# Wait until the game is actually draining the command FIFO (up to ~60s).
# Checks the path is a FIFO AND a live game process holds that exact inode
# open — this also detects the deleted-inode ghost (path inode won't match).
wait_for_fifo() {
    local timeout_s=${1:-60}
    local tries=0
    while [ $tries -lt $timeout_s ]; do
        if [ -p "$CMDFILE" ]; then
            local ino=$(stat -c %i "$CMDFILE" 2>/dev/null)
            if [ -n "$ino" ]; then
                for pid in $(pgrep -f "chocolate-doo[m]" 2>/dev/null); do
                    # Named-FIFO fds show as "-> /tmp/doom_cmd.txt", not
                    # "pipe:[ino]" (that pattern only matches anonymous
                    # pipes), so compare each fd's target inode instead.
                    for fd in /proc/$pid/fd/*; do
                        if [ "$(stat -L -c %i "$fd" 2>/dev/null)" = "$ino" ]; then
                            return 0
                        fi
                    done
                done
            fi
        fi
        sleep 1
        tries=$((tries+1))
    done
    return 1
}

# Load autosave slot 1 if it exists (resume where the last cycle left off).
# Doom savegame slot 0 + progress_auto1.dat.
# Full-loop mode only: --subagent dispatches below and must NOT send here.
if [ "$SUBAGENT" != true ]; then
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
# Restore stub shared libraries wiped by VM recycles. The farm's vendored
# audio libs (fluidsynth, SDL_mixer) need libinstpatch, libopusfile, and
# libpipewire, which don't survive in /usr. Regenerate functional stubs
# from the vendored libs' undefined symbols (the game never calls these).
restore_stub_libs() {
    local bindir="$HOME/workspace/DiabloDoom/build/src"
    local vendored="$HOME/workspace/DiabloDoom/third_party/runtime-libs"
    local needed=0

    # Check if binary loads cleanly
    if ! ldd "$bindir/chocolate-doom" 2>/dev/null | grep -q "not found"; then
        # Also verify no symbol lookup errors on the vendored libs
        if "$bindir/chocolate-doom" --help 2>&1 | grep -q "symbol lookup error"; then
            needed=1
        fi
    else
        needed=1
    fi
    [ "$needed" -eq 0 ] && return 0

    # Generate libinstpatch stub (fluidsynth needs ipatch_* + pw_* symbols)
    if [ -f "$vendored/libfluidsynth.so.3" ]; then
        nm -D "$vendored/libfluidsynth.so.3" 2>/dev/null | grep " U " | \
            grep -E "ipatch|pw_" | awk '{print $2}' > /tmp/inst_syms.txt
        if [ -s /tmp/inst_syms.txt ]; then
            python3 -c "
import sys
with open('/tmp/inst_syms.txt') as f:
    syms = [l.strip() for l in f if l.strip()]
with open('/tmp/stub_inst.c', 'w') as out:
    for s in syms:
        out.write(f'void* {s}(void) {{ return 0; }}\n')
"
            gcc -shared -fPIC -o /usr/lib/x86_64-linux-gnu/libinstpatch-1.0.so.2 \
                /tmp/stub_inst.c 2>/dev/null || \
            sudo gcc -shared -fPIC -o /usr/lib/x86_64-linux-gnu/libinstpatch-1.0.so.2 \
                /tmp/stub_inst.c 2>/dev/null || true
        fi
    fi

    # Generate libopusfile stub (SDL_mixer needs op_* symbols)
    if [ -f "$vendored/libSDL2_mixer-2.0.so.0" ]; then
        nm -D "$vendored/libSDL2_mixer-2.0.so.0" 2>/dev/null | grep " U " | \
            grep "op_" | awk '{print $2}' > /tmp/opus_syms.txt
        if [ -s /tmp/opus_syms.txt ]; then
            python3 -c "
with open('/tmp/opus_syms.txt') as f:
    syms = [l.strip() for l in f if l.strip()]
with open('/tmp/stub_opus.c', 'w') as out:
    for s in syms:
        out.write(f'void* {s}(void) {{ return 0; }}\n')
"
            gcc -shared -fPIC -o /usr/lib/x86_64-linux-gnu/libopusfile.so.0 \
                /tmp/stub_opus.c 2>/dev/null || \
            sudo gcc -shared -fPIC -o /usr/lib/x86_64-linux-gnu/libopusfile.so.0 \
                /tmp/stub_opus.c 2>/dev/null || true
        fi
    fi

    # Empty libpipewire stub (fluidsynth links it, never used)
    echo '// empty' > /tmp/stub_pw.c
    gcc -shared -fPIC -o /usr/lib/x86_64-linux-gnu/libpipewire-0.3.so.0 \
        /tmp/stub_pw.c 2>/dev/null || \
    sudo gcc -shared -fPIC -o /usr/lib/x86_64-linux-gnu/libpipewire-0.3.so.0 \
        /tmp/stub_pw.c 2>/dev/null || true

    # Reinstall libmodplug from saved deb if missing
    if ! ldconfig -p 2>/dev/null | grep -q "libmodplug.so.1"; then
        sudo dpkg -i --force-all "$HOME/workspace/doom/modplug.deb" 2>/dev/null || true
    fi

    sudo ldconfig 2>/dev/null || true
}

# setup: ensure the game is running (launch only if needed), then
# LOAD 0 if autosave slot 1 exists, else SAVE 0 to initialize it.
subagent_setup() {
    local game_count launched save_status
    # Restore stub libs wiped by VM recycles before anything else.
    restore_stub_libs
    # Singleton: only one setup/launch at a time. A second concurrent setup
    # would interleave pkill/rm/launch and create FIFO ghosts + duplicate
    # games. Non-blocking: fail fast so the caller can retry or stand down.
    exec 9>"$SETUP_LOCK"
    if ! flock -n 9; then
        echo '{"ok": false, "error": "another setup is already in progress"}'
        exit 1
    fi
    # Pull latest and rebuild if anything changed, so the fresh game
    # always runs the newest binary.
    pull_and_rebuild
    game_count=$(pgrep -c -f "chocolate-doo[m]" 2>/dev/null || true); game_count=${game_count:-0}
    if [ "$game_count" -gt 0 ]; then
        # A game is already running (possibly stale: stuck pre-GS_LEVEL or
        # holding a deleted FIFO). Kill all and launch fresh for a clean
        # command channel. Poll until actually dead before touching the FIFO.
        pkill -9 -f "chocolate-doo[m]" 2>/dev/null || true
        local waits=0
        while pgrep -f "chocolate-doo[m]" > /dev/null && [ $waits -lt 10 ]; do
            sleep 1
            waits=$((waits+1))
        done
        if pgrep -f "chocolate-doo[m]" > /dev/null; then
            echo '{"ok": false, "error": "could not kill existing game"}'
            exit 1
        fi
    fi
    launch_game
    launched=1
    # Don't touch the FIFO until the game is confirmed draining it.
    if ! wait_for_fifo 60; then
        echo '{"ok": false, "error": "game launched but FIFO never became live (stuck boot or ghost)"}'
        exit 1
    fi
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
    local game_count ts out newest replay_path screenshot_path log_tail kills
    game_count=$(pgrep -c -f "chocolate-doo[m]" 2>/dev/null || true); game_count=${game_count:-0}
    if [ "$game_count" -eq 0 ]; then
        echo '{"error": "game not running", "game_count": 0}'
        exit 1
    fi
    ts=$(date +%Y%m%d_%H%M%S)
    out="$SCREEN_DIR/doom_ralph_${ts}_obs.mp4"
    # CAPTURE saves the 15s replay video first, then a screenshot.
    # Poll for both new files instead of sleeping a fixed time.
    before_mp4=$(ls -t "$SHOTDIR"/*.mp4 2>/dev/null | head -1)
    before_png=$(ls -t "$SHOTDIR"/*.png "$SHOTDIR"/doomshots/*.png 2>/dev/null | head -1)
    send_cmd "CAPTURE\n"
    newest=""
    for i in $(seq 1 16); do
        sleep 1
        newest=$(ls -t "$SHOTDIR"/*.mp4 2>/dev/null | head -1)
        if [ -n "$newest" ] && [ "$newest" != "$before_mp4" ]; then
            break
        fi
        newest=""
    done
    replay_path=""
    if [ -n "$newest" ]; then
        mv "$newest" "$out"
        replay_path="$out"
    fi
    # Screenshot lands after the video; poll for the new PNG.
    screenshot_path=""
    for i in $(seq 1 8); do
        sleep 1
        newest_png=$(ls -t "$SHOTDIR"/*.png "$SHOTDIR"/doomshots/*.png 2>/dev/null | head -1)
        if [ -n "$newest_png" ] && [ "$newest_png" != "$before_png" ]; then
            screenshot_path="$newest_png"
            break
        fi
    done
    log_tail=$(tail -50 "$GAME_LOG" 2>/dev/null)
    kills=$(grep -ac "exec-attack-kill" "$GAME_LOG" 2>/dev/null || true); kills=${kills:-0}
    REPLAY_PATH="$replay_path" SCREENSHOT_PATH="$screenshot_path" LOG_TAIL="$log_tail" KILLS="$kills" python3 - <<'PYEOF'
import json, os
print(json.dumps({
    "replay_path": os.environ.get("REPLAY_PATH", ""),
    "screenshot_path": os.environ.get("SCREENSHOT_PATH", ""),
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
    # timeout: never block forever on a readerless FIFO (ghost).
    if ! timeout 10 bash -c 'cat "$1" > "$2"' _ "$ACT_FILE" "$CMDFILE"; then
        echo '{"ok": false, "error": "FIFO write blocked (not draining)"}'
        exit 1
    fi
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

# Full-loop mode owns the game for its run: take the setup lock (blocking)
# so a concurrent --subagent setup fails fast instead of racing our launch.
exec 9>"$SETUP_LOCK"
flock 9

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
