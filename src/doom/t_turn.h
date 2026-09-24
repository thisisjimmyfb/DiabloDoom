//
// t_turn.h — Turn-based (XCOM) mode: central turn controller.
//
// One rules layer owns time, input, and resolution. Do NOT scatter
// `if (turnbased_mode)` checks through every weapon and monster; route
// turn-mode behavior through this controller and the gameplay bridge.
//
// State machine: PLANNING -> TARGETING -> CONFIRM -> PULSE -> (REACTION)
// -> ROUND_END. Targeting and confirmation are reversible; TP is spent
// only when a valid action enters PULSE.
//
// Core invariant: no turn-mode action reads mouse position, analog
// magnitude, crosshair overlap, or a manually chosen world coordinate.

#ifndef __T_TURN__
#define __T_TURN__

#include "doomdef.h"
#include "d_event.h"

// Turn controller phases.
typedef enum
{
    TS_OFF,        // turn mode not active (real-time path)
    TS_PLANNING,   // world frozen; player issues discrete actions
    TS_TARGETING,  // a target is selected; preview shown
    TS_CONFIRM,    // action confirmed, awaiting pulse
    TS_PULSE,      // bounded simulation pulse resolving (input locked)
    TS_REACTION,   // enemy phase: monsters act, input locked
    TS_ROUND_END,  // cooldowns tick, round increments
} turnstate_t;

// Discrete turn-mode actions (input adapter output).
typedef enum
{
    TA_NONE,
    TA_MOVE_N, TA_MOVE_S, TA_MOVE_E, TA_MOVE_W,  // screen-relative steps
    TA_SELECT_NEXT, TA_SELECT_PREV, TA_SELECT_NUM,
    TA_ATTACK, TA_HEADSHOT,
    TA_ABILITY,        // weapon kit ability (phase 5)
    TA_SWAP_WEAPON,
    TA_USE,
    TA_HUNKER,
    TA_OVERWATCH,
    TA_POTION,
    TA_WAIT,
    TA_END_TURN,
    TA_CANCEL,
} turnaction_t;

// Central controller state. Owned by t_turn.c; read via accessors.
typedef struct
{
    turnstate_t state;
    int round;              // current round number (1-based)
    int tp;                 // tempo points remaining this round
    int tp_max;             // 10
    int selected_target;    // index into target list, -1 = none
    int selected_num;       // display number chosen via number keys
    turnaction_t queued;    // action awaiting pulse
    boolean headshot_mod;   // headshot modifier armed for next attack
    int hunkered;           // rounds of hunker defense remaining
    int overwatch_tp;       // TP reserved for overwatch (0 = none)
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

// Turn-mode actions (t_action.c). Phase 1: move/use/wait/end.
// Phase 2: TP costs, legal-action checks, swap/hunker/overwatch.
void T_DoMove(int dir);   // 0=N(fwd) 1=E(right) 2=S(back) 3=W(left)
void T_DoUse(void);
void T_DoWait(void);
void T_DoEndTurn(void);
void T_DoSwapWeapon(void); // 2 TP: cycle to next owned weapon
void T_DoHunker(void);     // 2 TP: defense until next round
void T_DoHeadshot(void);    // free: arm headshot modifier for next attack
void T_SnapshotOverwatch(void); // Phase 6: snapshot targets at enemy phase start
void T_TelegraphEnemies(void);  // Phase 6: warn of newly alerted enemies
void T_ResolveOverwatch(void);  // Phase 6: reaction attack at enemy phase end
void T_DoOverwatch(void);  // all remaining TP (min 3): reserve reaction

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

// Deterministic turn-mode RNG (independent of M_Random stream).
int T_Random(void);
void T_SeedRNG(unsigned int seed);

#endif
