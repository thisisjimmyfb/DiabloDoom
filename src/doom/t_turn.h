//
// t_turn.h — Turn-based (XCOM) mode: central turn controller.
//
// One rules layer owns time, input, and resolution. Do NOT scatter
// turn-mode conditionals through every weapon and monster; route
// turn-mode behavior through this controller and the gameplay bridge.
//
// State machine: PLANNING -> TARGETING -> CONFIRM -> (queue) ->
// END TURN -> drain FIFO (PULSE per entry) -> REACTION -> ROUND_END.
// Targeting and confirmation are reversible; TP is reserved when an
// action is enqueued and refunded on undo/clear/skip.
//
// Core invariant: no turn-mode action reads mouse position, analog
// magnitude, crosshair overlap, or a manually chosen world coordinate.

#ifndef __T_TURN__
#define __T_TURN__

#include "doomdef.h"
#include "d_event.h"
#include "tables.h" // angle_t for the queued-action snapshot
// Forward declare mobj_t for profile kill counter.
struct mobj_s;
typedef struct mobj_s mobj_t;

// Turn controller phases.
//
// Queue model: PLANNING enqueues actions (TP reserved immediately);
// END TURN drains the queue FIFO (each entry runs its own pulse),
// then the enemy phase runs, then round bookkeeping.
// TARGETING/CONFIRM build an attack for the queue; they never execute.
typedef enum
{
    TS_OFF,        // turn mode not active (real-time path)
    TS_PLANNING,   // world frozen; player queues discrete actions
    TS_TARGETING,  // a target is selected; preview shown
    TS_CONFIRM,    // attack previewed, awaiting enqueue
    TS_PULSE,      // bounded simulation pulse resolving (input locked)
    TS_REACTION,   // enemy phase: monsters act, input locked
    TS_ROUND_END,  // cooldowns tick, round increments
} turnstate_t;

// Discrete turn-mode actions (input adapter output).
typedef enum
{
    TA_NONE,
    TA_MOVE_N, TA_MOVE_S, TA_MOVE_E, TA_MOVE_W,  // facing-relative steps
    TA_TURN_L, TA_TURN_R,  // free view turn (arrow keys), 0 TP
    TA_SELECT_NEXT, TA_SELECT_PREV, TA_SELECT_NUM,
    TA_ATTACK,
    TA_ABILITY,        // weapon kit ability (phase 5)
    TA_USE,
    TA_POTION,
    TA_COLLECT,        // auto-path to nearest loot and pick it up
    TA_END_TURN,
    TA_CANCEL,
    TA_UNDO,           // undo last queued action (Backspace)
    TA_CLEAR_QUEUE,    // clear the whole queue (Z)
} turnaction_t;

// Queued action entry: planning snapshots everything the entry needs,
// so the queue reads concretely ("STEP EAST") and execution is FIFO.
#define T_QUEUE_MAX 16
typedef struct
{
    turnaction_t action;   // TA_MOVE_*, TA_ATTACK, TA_USE
    int cost;              // TP reserved at enqueue time
    angle_t moveangle;     // TA_MOVE_*: world-space step angle snapshot
    mobj_t *target;        // TA_ATTACK: actor snapshot (validated live)
    char target_name[16];  // TA_ATTACK: display-name snapshot
} t_queueentry_t;

// Central controller state. Owned by t_turn.c; read via accessors.
typedef struct
{
    turnstate_t state;
    int round;              // current round number (1-based)
    int tp;                 // tempo points remaining this round
    int tp_max;             // 10
    int selected_target;    // index into target list, -1 = none
    int selected_num;       // display number chosen via number keys
    // Queued-action model: PLANNING enqueues, END TURN drains FIFO.
    t_queueentry_t queue[T_QUEUE_MAX]; // ordered actions
    int queue_len;          // entries currently queued
    int queue_tp;           // TP reserved by queued actions
    boolean executing;      // draining the queue (input locked)
    int heat;               // chaingun heat 0-100 (phase 5)
    int cooldowns[8];       // per-weapon-kit cooldowns in rounds (phase 5)
    int charge_tp;          // plasma charge banked in TP (phase 5)
    unsigned int rng_seed;  // turn-mode deterministic RNG seed
    // Pulse runner state (phase 1).
    int pulse_left;         // sim tics remaining in the current pulse
    boolean pulse_freeze;   // freeze live monsters during this pulse
    boolean pulse_fast;     // run 4 sim tics per real tic
    boolean pulse_enemy;    // this pulse is the enemy phase
    boolean sync;           // run pulses synchronously (script harness)
    boolean firing;         // hold BT_ATTACK during the current pulse
} turnctrl_t;

extern turnctrl_t turnctrl;

// ------------------------------------------------------------------
// Target service (phase 3): visible-enemy enumeration with stable
// numbers. The list is rebuilt on demand; numbers are 1-based and
// stable while the world is frozen (thinker order = spawn order).
// ------------------------------------------------------------------
#define T_MAXTARGETS 32

// Rebuild the target list from live, visible enemies.
void T_RefreshTargets(void);
// Number of current targets (0 = none).
int T_NumTargets(void);
mobj_t *T_SelectedMobj(void);
// Target mobj by 0-based index, or NULL.
struct mobj_s *T_TargetMobj(int idx);
// Validate the selection after the world changed (death, etc.).
void T_ValidateSelection(void);

// Target selection (free, reversible).
void T_DoSelectNext(void);
void T_DoSelectPrev(void);
void T_DoSelectNum(int num);
void T_DoCancel(void);
// Attack: TARGETING -> CONFIRM -> auto-face + pulse.
void T_DoAttack(void);

// True once T_Init ran and the mode guards passed.
boolean T_Ready(void);

// Fully active: flag on, init done, in-level, single-player.
boolean T_Active(void);

// Called once at startup from D_DoomMain after parms are parsed.
void T_Init(void);

// Called on new game and level load in turn mode: full reset.
void T_NewGame(void);

// Called after loading a savegame: reset the state machine only
// (round/TP are the player's ongoing progress).
void T_OnLoad(void);
// Test harness: re-assert the in-script context after LOAD (which runs
// G_DoLoadLevel -> T_NewGame and would otherwise clear it).
void T_ScriptLoadFixup(void);
// Per-tic hook called from G_Ticker instead of P_Ticker.
void T_Ticker(void);

// Input adapter (t_input.c): translate a key event into a turn action
// while planning/targeting. Returns true if the event was consumed.
boolean T_Responder(event_t *ev);

// Pulse control.
void T_BeginPulse(int tics, boolean freeze_monsters, boolean fast);
boolean T_InPulse(void);
// Synchronous pulse (test harness): run to completion immediately.
void T_RunPulseSync(int tics, boolean freeze_monsters);
// True while a target selection is open (TARGETING/CONFIRM): the menu
// lets ESC fall through to the turn responder (cancel) in that case.
boolean T_InSelection(void);

// Turn-mode actions (t_action.c). Phase 1: move/use/end.
// Phase 2: TP costs, legal-action checks.
// Queue model: T_DoMove/T_DoUse/T_DoAttack ENQUEUE
// (TP reserved); T_DoEndTurn drains the queue FIFO, then the enemy
// phase runs. T_DoTurn stays immediate (free view control).
void T_DoMove(int dir);   // 0=N(fwd) 1=E(right) 2=S(back) 3=W(left)
void T_DoTurn(int dir);   // -1=left, +1=right; free 45-degree view turn
void T_DoUse(void);
void T_DoCollect(void);   // G: auto-path to nearest loot and pick it up
void T_DoEndTurn(void);
void T_DoUndoQueue(void);  // Backspace: undo last queued action, refund TP
void T_DoClearQueue(void); // Z: clear the whole queue, refund all TP
void T_ExecuteNext(void);  // run the next queued entry (queue drain)
void T_BeginEnemyPhase(void);
void T_QueueEntryName(const t_queueentry_t *e, char *buf, size_t buflen);
const char *T_TargetName(mobj_t *mo);
void T_TelegraphEnemies(void);  // Phase 6: warn of newly alerted enemies

// Phase 2: Tempo economy.
int T_CostFor(turnaction_t action);   // TP cost; selection/cancel = 0
boolean T_CanAfford(turnaction_t action);
int T_AttackCost(void);              // derived from attack speed (ph4/5)
void T_SpendTP(int cost);
// Infinite-ammo invariant: turn mode never tracks ammo.
void T_TopUpAmmo(void);

// HUD: mode indicator + contextual help (called from HU_Drawer).
void T_DrawHUD(void);

// Test harnesses (SPEC: fixed-seed arena, action replay, state dump).
void T_DumpState(const char *why);
void T_SpawnArena(void);
void T_RunScript(const char *path);

// Save/load: turn decision state (round, TP, phase-5 fields). Written as
// a trailing block by G_DoSaveGame; restored by G_DoLoadGame. Old saves
// without the block load fine (defaults kept). Implemented in p_saveg.c.
void P_ArchiveTurn(void);
boolean P_UnArchiveTurn(void);

// ------------------------------------------------------------------
// Phase 8: versioned standalone turn-based profile.
// Separate from level savegames; persists XP, levels, stat points,
// lifetime counters, and the hero name across sessions.
// Stored atomically (write temp + rename) on level exit and quit.
// Loaded at startup.
// ------------------------------------------------------------------
#define T_PROFILE_VERSION 3
#define T_PROFILE_NAME_LEN 32

typedef struct
{
    int version;                            // T_PROFILE_VERSION
    char name[T_PROFILE_NAME_LEN];          // hero name (NUL-terminated)
    int xp;                                 // accumulated experience
    int level;                              // current level (starts at 1)
    int stat_points;                        // unspent attribute points
    int lifetime_kills;                     // enemies killed (turn mode)
    int lifetime_damage;                    // damage dealt (turn mode)
    int lifetime_rounds;                    // rounds played (turn mode)
} t_profile_t;

extern t_profile_t t_profile;

// Profile lifecycle.
void T_ProfileInit(void);                   // defaults (call at startup)
void T_ProfileLoad(void);                   // load from disk (if exists)
void T_ProfileSave(void);                   // atomic save to disk
const char *T_ProfilePath(void);            // filesystem path

// XP and levels.
void T_GainXP(int amount);                  // add XP, handle level-ups
int T_XPForLevel(int level);                // XP threshold for level
void T_AddStatPoint(const char *stat);      // spend 1 point on str/dex/vit/ene

// Lifetime counters (call from combat code).
void T_CountKill(mobj_t *target);
void T_CountDamage(int dmg);
void T_CountRound(void);

// Kill banner: latched on the first kill of an attack, drawn big over the
// game view for ~2.5s so the kill moment is screenshot-able.
void T_KillBanner(const char *title, const char *sub);

// Deterministic turn-mode RNG (independent of M_Random stream).
int T_Random(void);
void T_SeedRNG(unsigned int seed);

#endif
