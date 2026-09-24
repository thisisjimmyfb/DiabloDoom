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
    TS_REACTION,   // enemy reactions / overwatch resolution
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
} turnctrl_t;

extern turnctrl_t turnctrl;

// Enabled only when launched with -turnbased (single-player).
boolean T_Enabled(void);

// Called once at startup from D_DoomMain after parms are parsed.
// Must be a no-op unless -turnbased was given.
void T_Init(void);

// Called when a new game / level starts in turn mode.
void T_NewGame(void);

// Per-tic hook called from G_Ticker. No-op unless turn mode active.
void T_Ticker(void);

// Input adapter: translate a key event into a turn action while in
// PLANNING/TARGETING. Returns true if the event was consumed.
// (Implemented in phase 1; stub returns false for now.)
boolean T_Responder(event_t *ev);

// Bounded simulation pulse: advance the world exactly `tics' tics.
// If freeze_monsters is true, live monsters do not think (XCOM-style:
// they act on their own phase). Input is locked during a pulse.
void T_RunPulse(int tics, boolean freeze_monsters);

// Deterministic turn-mode RNG (independent of M_Random stream).
int T_Random(void);
void T_SeedRNG(unsigned int seed);

#endif
