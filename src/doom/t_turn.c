//
// t_turn.c — Turn-based (XCOM) mode: central turn controller.
//
// Phase 0: startup plumbing only. T_Init/T_Ticker/T_Responder are safe
// no-ops unless the game was launched with -turnbased, so real-time
// play, inventory, save/load, death, and level transitions are 100%
// unchanged.

#include "t_turn.h"
#include "doomstat.h"
#include "d_player.h"
#include "g_game.h"

turnctrl_t turnctrl;

boolean T_Enabled(void)
{
    // Single-player only: never in netgames or demo playback.
    // (netgame/demo guards land with the controller in phase 1.)
    return turnbased_mode;
}

void T_Init(void)
{
    memset(&turnctrl, 0, sizeof(turnctrl));
    turnctrl.state = T_Enabled() ? TS_PLANNING : TS_OFF;
    turnctrl.tp_max = 10;
    turnctrl.tp = turnctrl.tp_max;
    turnctrl.round = 1;
    turnctrl.selected_target = -1;
    turnctrl.rng_seed = 0xC0FFEEu;
}

void T_NewGame(void)
{
    if (!T_Enabled())
        return;
    // Full reset lands with the controller in phase 1.
    T_Init();
}

void T_Ticker(void)
{
    if (!T_Enabled())
        return;
    // Controller tick lands in phase 1.
}

boolean T_Responder(event_t *ev)
{
    if (!T_Enabled())
        return false;
    (void)ev;
    // Input adapter lands in phase 1.
    return false;
}

void T_RunPulse(int tics, boolean freeze_monsters)
{
    (void)tics;
    (void)freeze_monsters;
    // Bounded simulation pulse lands in phase 1.
}

int T_Random(void)
{
    // Simple LCG; deterministic and independent of the engine RNG.
    turnctrl.rng_seed = turnctrl.rng_seed * 1664525u + 1013904223u;
    return (int)((turnctrl.rng_seed >> 16) & 0x7FFF);
}

void T_SeedRNG(unsigned int seed)
{
    turnctrl.rng_seed = seed;
}
