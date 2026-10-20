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

# ---- Stage timing log (2026-10-20) ----
# Every loop stage (setup / observe / vision / act) writes timestamped
# lines to $LOG so per-stage durations can be read off afterwards:
#   [STAGE] <stage> start|mark|end ... ms=<elapsed>
# The vision stage runs inside the worker agent (between observe and
# act), so the script measures it as the gap: act.start - observe.end.
# Inter-call state (last stage end, epoch ms) lives in a small file.
STAGE_STATE="${STAGE_STATE:-/tmp/doom_loop_stage.env}"
stage_log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S.%3N')] [STAGE] $*" >> "$LOG"
}
now_ms() { date +%s%3N; }
stage_state_set() { # key value
    local tmp="${STAGE_STATE}.tmp.$$"
    grep -v "^$1=" "$STAGE_STATE" 2>/dev/null > "$tmp" || true
    echo "$1=$2" >> "$tmp"
    mv "$tmp" "$STAGE_STATE"
}
stage_state_get() { # key -> value or empty
    grep "^$1=" "$STAGE_STATE" 2>/dev/null | tail -1 | cut -d= -f2
}

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
    # Append, never overwrite: commands the game hasn't consumed yet
    # must survive this write (before the game creates the FIFO the
    # path can be a plain file, where ">" would erase queued input).
    # The game reads the FIFO every tick (35 Hz), so a healthy write
    # drains immediately; a block means no reader right now. Retry
    # for a while (the game may be mid-relaunch) instead of dropping
    # the command -- only give up after ~30s of attempts.
    local attempt
    for attempt in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
        if timeout 2 bash -c 'printf "%b" "$1" >> "$2"' _ "$1" "$CMDFILE"; then
            sleep 2
            return 0
        fi
        echo "[$(date)] send_cmd attempt $attempt blocked (FIFO not draining), retrying: $1" >> "$LOG"
        sleep 1
    done
    echo "[$(date)] send_cmd FAILED after 15 attempts, command NOT delivered: $1" >> "$LOG"
    return 1
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
    local before=$(ls -t "$SHOTDIR"/DOOM*.png "$SHOTDIR"/doomshots/*.png 2>/dev/null | head -1)
    send_cmd "SCREENSHOT\n"
    # Screenshots render at dispatch (~1 frame); poll briefly for the
    # new file instead of the old fixed 4s sleep.
    local newest=""
    for i in $(seq 1 8); do
        sleep 0.5
        newest=$(ls -t "$SHOTDIR"/DOOM*.png "$SHOTDIR"/doomshots/*.png 2>/dev/null | head -1)
        if [ -n "$newest" ] && [ "$newest" != "$before" ]; then
            break
        fi
        newest=""
    done
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
    local before=$(ls -t "$SHOTDIR"/*.mp4 2>/dev/null | head -1)
    local newest=""
    send_cmd "REPLAY\n"
    # Async encode lands in ~2-9s; poll (exit on arrival) instead of
    # the fixed 18s sleep from the synchronous-encode era.
    for i in $(seq 1 12); do
        sleep 1
        newest=$(ls -t "$SHOTDIR"/*.mp4 2>/dev/null | head -1)
        if [ -n "$newest" ] && [ "$newest" != "$before" ]; then
            break
        fi
        newest=""
    done
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
    local t_setup_start=$(now_ms)
    stage_log "setup start"
    # Restore stub libs wiped by VM recycles before anything else.
    restore_stub_libs
    stage_log "setup mark libs-restored ms=$(( $(now_ms) - t_setup_start ))"
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
    stage_log "setup mark pull-rebuild-done ms=$(( $(now_ms) - t_setup_start ))"
    game_count=$(pgrep -c -f "chocolate-doo[m]" 2>/dev/null || true); game_count=${game_count:-0}
    if [ "$game_count" -gt 0 ]; then
        # A game is already running (possibly stale: stuck pre-GS_LEVEL or
        # holding a deleted FIFO). Try to save its state first if the FIFO
        # is live, then kill all and launch fresh for a clean command
        # channel. Poll until actually dead before touching the FIFO.
        if [ -p "$CMDFILE" ]; then
            send_cmd "SAVE 0\n" 2>/dev/null || true
            sleep 3
        fi
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
    stage_log "setup mark game-launched ms=$(( $(now_ms) - t_setup_start ))"
    # Don't touch the FIFO until the game is confirmed draining it.
    if ! wait_for_fifo 60; then
        stage_log "setup end ok=false error=fifo-never-live ms=$(( $(now_ms) - t_setup_start ))"
        echo '{"ok": false, "error": "game launched but FIFO never became live (stuck boot or ghost)"}'
        exit 1
    fi
    stage_log "setup mark fifo-live ms=$(( $(now_ms) - t_setup_start ))"
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
    stage_log "setup end ok=true save=$save_status ms=$(( $(now_ms) - t_setup_start ))"
}

# observe: REPLAY capture + recent log tail -> JSON.
subagent_observe() {
    local game_count ts out newest replay_path screenshot_path log_tail kills
    local t_obs_start=$(now_ms)
    local last_act_end=$(stage_state_get LAST_ACT_END_MS)
    if [ -n "$last_act_end" ]; then
        stage_log "observe start since_act_ms=$(( t_obs_start - last_act_end )) (game resolving + idle)"
    else
        stage_log "observe start"
    fi
    game_count=$(pgrep -c -f "chocolate-doo[m]" 2>/dev/null || true); game_count=${game_count:-0}
    if [ "$game_count" -eq 0 ]; then
        stage_log "observe end ok=false error=game-not-running ms=$(( $(now_ms) - t_obs_start ))"
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
    stage_log "observe mark capture-sent ms=$(( $(now_ms) - t_obs_start ))"
    newest=""
    # Async encode: the file lands when the encoder thread finishes
    # (~2-9s measured); poll exits on arrival, cap is failure-only.
    for i in $(seq 1 12); do
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
    stage_log "observe mark mp4-ready found=$([ -n "$replay_path" ] && echo 1 || echo 0) ms=$(( $(now_ms) - t_obs_start ))"
    # Screenshot lands after the video; poll for the new PNG.
    # (Taken at dispatch now that the encode is async: ~1s typical.)
    screenshot_path=""
    for i in $(seq 1 4); do
        sleep 1
        newest_png=$(ls -t "$SHOTDIR"/*.png "$SHOTDIR"/doomshots/*.png 2>/dev/null | head -1)
        if [ -n "$newest_png" ] && [ "$newest_png" != "$before_png" ]; then
            screenshot_path="$newest_png"
            break
        fi
    done
    log_tail=$(tail -50 "$GAME_LOG" 2>/dev/null)
    kills=$(grep -ac "exec-attack-kill" "$GAME_LOG" 2>/dev/null || true); kills=${kills:-0}
    stage_log "observe mark png-ready found=$([ -n "$screenshot_path" ] && echo 1 || echo 0) ms=$(( $(now_ms) - t_obs_start ))"
    REPLAY_PATH="$replay_path" SCREENSHOT_PATH="$screenshot_path" LOG_TAIL="$log_tail" KILLS="$kills" python3 - <<'PYEOF'
import json, os
print(json.dumps({
    "replay_path": os.environ.get("REPLAY_PATH", ""),
    "screenshot_path": os.environ.get("SCREENSHOT_PATH", ""),
    "log_tail": os.environ.get("LOG_TAIL", ""),
    "kills": int(os.environ.get("KILLS", 0) or 0),
}))
PYEOF
    local t_obs_end=$(now_ms)
    stage_state_set LAST_OBSERVE_END_MS "$t_obs_end"
    stage_log "observe end ok=true ms=$(( t_obs_end - t_obs_start ))"
}

# act: write driver commands from --file to the game FIFO -> JSON.
subagent_act() {
    local t_act_start=$(now_ms)
    local last_obs_end=$(stage_state_get LAST_OBSERVE_END_MS)
    if [ -n "$last_obs_end" ]; then
        # The gap between observe ending and act starting IS the
        # vision stage: worker watching the replay + deciding.
        stage_log "vision end ms=$(( t_act_start - last_obs_end )) (observe->act gap)"
    fi
    stage_log "act start"
    if [ -z "$ACT_FILE" ] || [ ! -f "$ACT_FILE" ]; then
        stage_log "act end ok=false error=no-command-file ms=$(( $(now_ms) - t_act_start ))"
        echo '{"ok": false, "error": "no command file"}'
        exit 1
    fi
    if ! pgrep -f "chocolate-doo[m]" > /dev/null; then
        stage_log "act end ok=false error=game-not-running ms=$(( $(now_ms) - t_act_start ))"
        echo '{"ok": false, "error": "game not running"}'
        exit 1
    fi
    # Log what's being sent (single line: commands separated by spaces).
    stage_log "act mark cmds: $(tr '\n' ' ' < "$ACT_FILE")"
    # Append (never overwrite unconsumed commands). The game drains
    # the FIFO every tick, so a block means no reader right now (e.g.
    # mid-relaunch): retry briefly so a transient gap delays the
    # commands instead of dropping them.
    local act_attempt act_written=0
    for act_attempt in 1 2 3 4 5; do
        if timeout 2 bash -c 'cat "$1" >> "$2"' _ "$ACT_FILE" "$CMDFILE"; then
            act_written=1
            break
        fi
        stage_log "act mark write-blocked attempt=$act_attempt, retrying"
        sleep 1
    done
    if [ "$act_written" != 1 ]; then
        stage_log "act end ok=false error=fifo-write-blocked ms=$(( $(now_ms) - t_act_start ))"
        echo '{"ok": false, "error": "FIFO write blocked (not draining)"}'
        exit 1
    fi
    sleep 2
    local t_act_end=$(now_ms)
    stage_state_set LAST_ACT_END_MS "$t_act_end"
    stage_log "act end ok=true ms=$(( t_act_end - t_act_start ))"
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
    ITER_START_MS=$(now_ms)
    stage_log "loop iteration=$ITER start"

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
    stage_log "loop iteration=$ITER observe start"
    REPLAY=$(capture_replay)
    stage_log "loop iteration=$ITER observe end ms=$(( $(now_ms) - ITER_START_MS ))"
    if [ -z "$REPLAY" ]; then
        echo "Replay capture failed, skipping iteration" >> "$LOG"
        sleep 5
        continue
    fi

    # Get recent log context (last 50 lines).
    LOGCTX=$(tail -50 "$GAME_LOG" 2>/dev/null)

    # Vision LLM decides action.
    stage_log "loop iteration=$ITER vision start"
    if [ -n "$VISION_CMD" ]; then
        # VISION_CMD takes: replay_path prompt_path log_context
        # Outputs: driver command(s) to send (e.g. "WALK\nATTACK\n")
        ACTION=$($VISION_CMD "$REPLAY" "$PROMPT" "$LOGCTX" 2>> "$LOG")
        stage_log "loop iteration=$ITER vision end"
        stage_log "loop iteration=$ITER act start"
        if [ -n "$ACTION" ]; then
            echo "[$(date)] Vision action: $(echo "$ACTION" | tr '\n' ' ')" >> "$LOG"
            send_cmd "$ACTION"
        else
            echo "[$(date)] Vision returned empty, defaulting to WALK" >> "$LOG"
            send_cmd "WALK\n"
        fi
        stage_log "loop iteration=$ITER act end total_ms=$(( $(now_ms) - ITER_START_MS ))"
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
