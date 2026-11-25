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

// WAYPOINT RANDOM: a random standable point of the player's current
// sector (the part it can walk to within it), min_dist or more away.
boolean RL_SectorPoint(unsigned seed, int min_dist, int *x, int *y);

// WAYPOINT VIEW (2026-11-24): a random point the player can walk to and
// see, min_dist..max_dist away, as near the centre of the view as there is one.
boolean RL_ViewPoint(unsigned seed, int min_dist, int max_dist, int *x, int *y);

// DOORSPOT (2026-11-24, the door drill): the player teleported in front of
// a random closed door it can open, facing it, the waypoint behind it.
boolean RL_DoorSpot(unsigned seed, int min_dist, int max_dist, int jitter);

// WAYPOINT DOOR (2026-11-24): the waypoint past a closed door in view the
// player can walk to and open, no teleport; *line the door line.
boolean RL_DoorPoint(unsigned seed, int min_dist, int max_dist, int *x, int *y, int *line);

// WAYPOINT FRONTIER (2026-11-25): on the walking path to the nearest
// unvisited room, past its first door (*door the door sector), else the room.
boolean RL_FrontierPoint(int *x, int *y, int *door, boolean *far);

// Copy the current screen (I_VideoBuffer, 320x200 8-bit), the
// palette and the depth buffer into the -shm segment. No-op without -shm.
void RL_PublishFrame(void);

// Depth (2026-11-22): each pixel of the 3D view's distance from the
// viewer along the view direction, in map units (1..65534), published
// after the palette; 65535 where nothing was drawn (sky), 0 outside the
// view (status bar) and on the player's own weapon. Filled while -shm is
// on: RL_DepthBegin at the start of R_RenderPlayerView, then each wall
// column (RL_DepthCol, from its scale), floor/ceiling span (RL_DepthSpan,
// from its distance) and sprite post as it is drawn -- the renderer draws
// sprites far to near over what they hide, so the last write is the
// visible one, and only a sprite's opaque pixels are written.
extern boolean rl_depth_on;
extern boolean rl_depth_psprite;    // R_DrawPlayerSprites: the weapon, depth 0
void RL_DepthBegin(void);
void RL_DepthCol(int x, int yl, int yh, int scale);
void RL_DepthSpan(int y, int x1, int x2, int dist);
void RL_DepthPixel(int x, int y, double units);   // 2026-11-24: the waypoint skull

// A pickup failed because the backpack is full (D_BackpackFullMsg,
// called every tic the player stands on the item, before its message
// throttle). The STATE line's "backpack_full" -- one of the CONSULT
// triggers -- stays true for 4 s after the last one while the backpack
// is still full. Next to it, "monstervis" (a living monster on screen while
// exploring) and "bp_count" (items in the backpack).
void RL_NoteBackpackFull(void);

// CONSULT practice (-rlenv, and since 2026-11-17 -rldrive too, so
// eval.py's watch mode shows them). On the map a monster almost always sees
// the player the moment the player sees it (detection has no range
// limit), so a sighting would never come before combat. With
//   -rlspawn <tics>      mean game tics between spawns while exploring
// RL_SpawnTick (each explore tic) spawns one monster at a time on
// screen: in line of sight, 128-640 units ahead, within 30 degrees of
// the view (no room there: no spawn that time). It faces away and deaf
// (MF_AMBUSH), so it stays frozen -- a sighting the policy should
// CONSULT about -- and is marked MF_RLPRACTICE. It is removed 5 s after
// spawning, or at once if it would acquire the player (the player walks
// into its front arc; RL_PracticeAcquired, called from explore
// detection, removes it). It never triggers combat: explore detection
// skips it, T_IsCombatant is false for it (so it never keeps a fight
// going either), and T_EnterCombat refuses it as a spotter. Rolls use a
// private RNG seeded by SEED (RL_SpawnSeed), so episodes stay
// deterministic and P_Random's sequence is untouched. RL_LevelSetup
// forgets them (every level setup, including a LOAD), and the next
// explore tic removes any practice monster a loaded save carried.
//
// RL_Consult is the CONSULT driver command under -rlenv (every CONSULT
// shows "CONSULT" as the status message; under -rldrive alone it removes
// only the practice monsters; anywhere else, the message is all): the training
// stand-in for the hand-off to Muse's VLM. It removes every practice
// monster, in sight or not, every monster in the player's sight and, in
// combat, every monster targeting the player: a spawned one disappears (no kill, no loot); a map monster is
// killed (kill count and Diablo drops, as if the VLM had fought it).
//
// -rlitems <tics> (2026-11-17): mean game tics between practice items,
// the same way: one Diablo loot item (a weapon, armor, jewelry or a
// potion; tier and item from the private RNG) at a time, on the
// straight line to a portal of the current sector that is on screen
// (within 40 degrees of the facing), 64-256 units away, in sight and on
// a floor within 24 units of the player's: the unvisited portals tried
// first, then the visited ones, nearest first; none fits, or a full
// backpack: no spawn. It is marked MF_RLPRACTICE, sends an "item" event (x, y),
// and is removed 10 s after spawning; its pickup event says
// "practice":true. A loaded save's practice items are removed like its
// monsters.
void RL_SpawnTick(void);
void RL_SpawnSeed(int n);

// Fast movement (-rlenv, for the env's velocity reward). With
//   -rlfast <units>      map units per tic
// RL_MoveTick (each explore tic) counts the tics on which the player
// actually moved at least <units> since the tic before -- position, not
// momentum, so pushing into a wall never counts -- and the state line
// carries the count since the last decision as "fast_tics" (absent
// when off). A jump of more than 64 units in one tic (a teleport, a
// LOAD) is not movement. RL_DecisionMade resets the count.
void RL_MoveTick(void);
void RL_DecisionMade(void);
void RL_LevelSetup(void);
void RL_Consult(void);

// The player hurt a monster (P_DamageMobj, 2026-11-18): counts it in
// rl_player_hits and sends a "damage" event (amount, max HP, kill).
struct mobj_s;
extern int rl_player_hits;
void RL_PlayerHit(struct mobj_s *target, int dealt);
// The player was hit (P_DamageMobj, 2026-11-19): a "hurt" event (the
// amount before god mode, block, dodge, resistance and armour; monster:
// a monster's attack), even when it takes no HP. kind (2026-11-20):
// proj (a missile), hitscan (a line attack: rl_hitscan, set around its
// P_DamageMobj), melee (another monster attack) or world (no monster).
extern boolean rl_hitscan;
void RL_PlayerHurt(struct mobj_s *inflictor, struct mobj_s *source, int damage);
// Player projectiles (2026-11-21, the env's proj reward): RL_ProjSpawn
// tracks a rocket, plasma bolt or BFG ball the player fired; RL_ProjHit
// notes each monster it damages (direct or splash, once each); when it is
// removed RL_ProjEnd sends a "proj_end" event with that count (hits) and
// the gametic it was fired on (fired, 2026-11-22: the shot event's).
void RL_ProjSpawn(struct mobj_s *mo);
void RL_ProjHit(struct mobj_s *inflictor, struct mobj_s *target);
void RL_ProjSelf(struct mobj_s *inflictor);   // it hurt its shooter: proj_end's self (2026-11-23)
void RL_ProjEnd(struct mobj_s *mo);

// MONSTERS HIDE / SHOW (2026-11-18): every living map monster out of
// the world or back in (MF_RLHIDDEN), for the env's no-combat rollouts.
void RL_Monsters(boolean hide);
// DRILL ON / OFF (-rlenv, 2026-11-20): the combat drill keeps one awake
// monster ahead of the player, a new one as soon as it dies; a LOAD ends it.
void RL_Drill(boolean on);
void RL_DrillAim(int pct);
boolean RL_NoAttack(struct mobj_s *actor);
int RL_DrillDamage(struct mobj_s *target, int damage);
void RL_DrillProj(int pct, int self);   // DRILL ON PROJ (2026-11-23): 5 monsters, far, any direction
int RL_DrillSelfDamage(int damage);     // the proj drill's: the player's damage to itself, self %
void RL_DrillDist(void);                // DRILLDIST (2026-11-23): the drill monsters' distances
void RL_OpenSpace(int samples);         // OPENSPACE (2026-11-23): the proj drill's map scan
// A monster begins an attack (A_FaceTarget, the first action of every
// attack state; 2026-11-20): on the player, the tic is kept for the state
// line's attack_age (practice and hidden monsters don't count).
void RL_MonsterAttack(struct mobj_s *actor);
// ITEMS <period> [unique %] (-rlenv, 2026-11-20): practice items for this
// rollout (mean tics between them, 0 none; uniques that share), in place of
// -rlitems; on the way to the waypoint when one is on screen; a LOAD ends it.
void RL_Items(int period, int unique);

// GIVEGUN <seed> [KIT] [RANGED] (2026-11-18): a random normal-tier gun
// equipped (KIT, 2026-11-20: with gear for it; RANGED, 2026-11-20: never
// fists); false when there is none the player can use.
boolean RL_GiveGun(unsigned seed, boolean kit, boolean ranged, boolean hitscan, boolean proj, boolean melee);
// Explore detection found mo acquiring the player: if it is a practice
// monster, remove it and return true (no combat); else false.
struct mobj_s;
boolean RL_PracticeAcquired(struct mobj_s *mo);

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

// Pickups (2026-11-14): P_TouchSpecialThing sends a
//   {"event":"pickup","kind":"...","hp":N,"gametic":N}
// line for each thing the player picks up: kind "loot" (a Diablo item
// into the backpack), "armor" (green/blue armor; green also grants an
// item), "health" (stimpack, medikit, health bonus, soulsphere,
// megasphere), "key", or "other"; hp is the health it gave. A thing
// left on the ground (full backpack, full health) sends nothing.
// practice: an -rlitems practice item (the event's "practice" field).
// tier: a Diablo item's rarity (0 normal .. 4 unique; the event's "tier"
// field, 2026-11-17), -1 for anything else (left out).
// No-ops without -rldrive.
void RL_NotePickup(const char *kind, int hp, boolean practice, int tier);

#endif
