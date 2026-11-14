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

// -rldrive: control and output only, for driving a normal game (real
// time, its window, sound and everything else unchanged) with the
// trained policy. It turns on the driver anywhere on -cmdpipe (between
// levels too), the JSON lines on -statepipe, the frame in -shm, and the
// decision points -- published, never blocking: one "decision" line when
// the game becomes ready for input, then nothing until the next command
// or until the game changes kind while idle (combat starts, the player
// dies, the level ends). -rlenv implies it (rldrive is true under both)
// and adds the training-only parts: uncapped tics, blocking decisions,
// and no wipes, ENDOOM, CMD echo, idle timer or config save.
extern boolean rldrive;

// Parse -rlenv / -rldrive / -cmdpipe / -statepipe / -shm. Call once from D_DoomMain
// after M_LoadDefaults (it overrides config values such as
// show_endoom). No-op without -rlenv or -rldrive.
void RL_Init(void);

// Command FIFO path for the driver: -cmdpipe, else /tmp/doom_cmd.txt.
const char *RL_CmdPipePath(void);

// Write one JSON object as a line to the state output. printf-style;
// the newline is added. No-op unless rldrive (-rlenv or -rldrive).
void RL_Emit(const char *fmt, ...) PRINTF_ATTR(1, 2);

// Emit one STATE line (event = why, e.g. "decision" or "state").
// tics = game tics since the previous decision (-1 if not tracked).
void RL_EmitState(const char *why, int tics);

// Copy the current screen (I_VideoBuffer, 320x200 8-bit) and the
// palette into the -shm segment. No-op without -shm.
void RL_PublishFrame(void);

// A pickup failed because the backpack is full (D_BackpackFullMsg,
// called every tic the player stands on the item, before its message
// throttle). The STATE line's "backpack_full" -- one of the CONSULT
// triggers -- stays true for 4 s after the last one while the backpack
// is still full. Next to it, "monstervis" (a living monster on screen while
// exploring) and "bp_count" (items in the backpack).
void RL_NoteBackpackFull(void);

// CONSULT practice (-rlenv). On the map a monster almost always sees
// the player the moment the player sees it (detection has no range
// limit), so a sighting would never come before combat. With
//   -rlspawn <tics>      mean game tics between spawns while exploring
// RL_SpawnTick (each explore tic) spawns one monster at a time in line
// of sight: on screen (128-640 units ahead, within 30 degrees of the
// view) when there's room, else anywhere around the player (a player
// that WALKed until blocked faces a wall), so it comes on screen when
// the player turns. It faces away and deaf (MF_AMBUSH), so it stays frozen and never
// acquires the player -- a sighting the policy should CONSULT about. One
// that stays out of sight for 5 s is removed. Rolls use a private RNG
// seeded by SEED (RL_SpawnSeed), so episodes stay deterministic and
// P_Random's sequence is untouched. RL_LevelSetup forgets them (every
// level setup, including a LOAD).
//
// RL_Consult is the CONSULT driver command (-rlenv only): the training
// stand-in for the hand-off to Muse's VLM. It removes every monster in
// the player's sight and, in combat, every monster targeting the
// player: a spawned one disappears (no kill, no loot); a map monster is
// killed (kill count and Diablo drops, as if the VLM had fought it).
void RL_SpawnTick(void);
void RL_SpawnSeed(int n);
void RL_LevelSetup(void);
void RL_Consult(void);

// Mechanism use reporting (the env's mechanism reward, SPEC.md §5.3).
// P_UseLines brackets every player use with RL_UseBegin/RL_UseEnd, and
// P_ChangeSwitchTexture reports each switch line it flips in between
// (RL_NoteSwitch, a no-op outside a use). RL_UseEnd sends one
//   {"event":"use","sectors":[...],"lines":[...],"gametic":N}
// line when the use did something: "sectors" are the sectors that
// started moving (a door, lift, floor, ceiling, stairs... -- their
// specialdata went from empty to set), "lines" the switches flipped
// (this also covers switches that move nothing, such as lights or the
// exit). A use that did nothing -- a plain wall, a locked door without
// its key, a door already moving -- sends nothing. No-ops without -rlenv.
void RL_UseBegin(void);
void RL_NoteSwitch(int line);
void RL_UseEnd(void);

#endif
