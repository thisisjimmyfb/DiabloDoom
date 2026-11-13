// d_rlenv.h - RL environment mode (-rlenv).
//
// Runs DiabloDoom as a Gymnasium environment instance (see
// agent/training/SPEC.md): the FIFO command driver is enabled without
// D_OnMuseVM(), the game runs fixed tics as fast as it can
// (singletics), and wipes, ENDOOM, the on-screen CMD echo and the
// wall-clock idle Zug Zug timer are all off. Replay capture runs only
// with --continuous-capture (game-time segments, see d_replay.c).
//
//   -rlenv               enable RL environment mode
//   -cmdpipe <path>      command FIFO for this instance
//                        (default /tmp/doom_cmd.txt)
//   -statepipe <path>    state output, one JSON line per event; a FIFO
//                        (opened blocking for write: the env must open
//                        the read end) or a regular file (truncated).
//                        Without it, lines go to stdout prefixed "[RL] ".
//   -shm <name>          POSIX shared memory segment (shm_open name,
//                        e.g. /dd0) that receives the frame at every
//                        decision; layout in RL_PublishFrame.
//
// Step protocol: the engine sends a "decision" STATE line whenever a
// decision is due and then blocks on the command pipe until the next
// line arrives. Every command is eventually followed by exactly one
// "decision" line, so the env sends a command and reads events up to
// and including the next decision.

#ifndef D_RLENV_H
#define D_RLENV_H

#include "doomtype.h"

extern boolean rlenv;

// Parse -rlenv / -cmdpipe / -statepipe. Call once from D_DoomMain
// after M_LoadDefaults (it overrides config values such as
// show_endoom). No-op without -rlenv.
void RL_Init(void);

// Command FIFO path for the driver: -cmdpipe, else /tmp/doom_cmd.txt.
const char *RL_CmdPipePath(void);

// Write one JSON object as a line to the state output. printf-style;
// the newline is added. No-op unless rlenv.
void RL_Emit(const char *fmt, ...) PRINTF_ATTR(1, 2);

// Emit one STATE line (event = why, e.g. "decision" or "state").
// tics = game tics since the previous decision (-1 if not tracked).
void RL_EmitState(const char *why, int tics);

// Copy the current screen (I_VideoBuffer, 320x200 8-bit) and the
// palette into the -shm segment. No-op without -shm.
void RL_PublishFrame(void);

#endif
