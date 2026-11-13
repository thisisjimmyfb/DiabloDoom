// d_rlenv.h - RL environment mode (-rlenv).
//
// Runs DiabloDoom as a Gymnasium environment instance (see
// agent/training/SPEC.md): the FIFO command driver is enabled without
// D_OnMuseVM(), the game runs fixed tics as fast as it can
// (singletics), and wipes, ENDOOM, replay capture, the on-screen CMD
// echo and the wall-clock idle Zug Zug timer are all off.
//
//   -rlenv               enable RL environment mode
//   -cmdpipe <path>      command FIFO for this instance
//                        (default /tmp/doom_cmd.txt)
//   -statepipe <path>    state output, one JSON line per event; a FIFO
//                        (opened blocking for write: the env must open
//                        the read end) or a regular file (truncated).
//                        Without it, lines go to stdout prefixed "[RL] ".

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

#endif
