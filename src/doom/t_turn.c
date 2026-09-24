//
// t_turn.c — Turn-based (XCOM) mode: central turn controller.
//
// Owns the state machine, the bounded simulation pulse, round/enemy
// phases, and the test harnesses. Real-time mode never reaches this
// code: G_Ticker calls T_Ticker instead of P_Ticker only when the
// -turnbased flag passed the startup guards.

#include <string.h>
#include <stdio.h>

#include "hu_stuff.h"
#include "t_turn.h"
#include "doomstat.h"
#include "doomdef.h"
#include "d_player.h"
#include "d_think.h"
#include "d_main.h"
#include "m_argv.h"
#include "i_swap.h"
#include "g_game.h"
#include "p_local.h"
#include "m_misc.h"
#include "s_sound.h"
#include "v_video.h"
#include "w_wad.h"
#include "z_zone.h"

turnctrl_t turnctrl;

static boolean t_ready = false;
static const char *t_script_path = NULL;
static boolean t_script_done = false;
static boolean t_arena_done = false;

// Enemy phase length: 70 tics = 2 seconds of monster action.
#define ENEMY_PHASE_TICS 70

boolean T_Ready(void)
{
    return t_ready;
}

boolean T_Active(void)
{
    return turnbased_mode && t_ready && gamestate == GS_LEVEL && !netgame;
}

// ------------------------------------------------------------------
// Init / guards
// ------------------------------------------------------------------

void T_Init(void)
{
    memset(&turnctrl, 0, sizeof(turnctrl));
    turnctrl.tp_max = 10;
    turnctrl.tp = turnctrl.tp_max;
    turnctrl.round = 1;
    turnctrl.selected_target = -1;
    turnctrl.rng_seed = 0xC0FFEEu;
    turnctrl.state = TS_OFF;

    if (turnbased_mode)
    {
        // Single-player only; no demos. Clear message, fall back to
        // real-time rather than erroring out.
        if (netgame
         || M_CheckParm("-playdemo") > 0
         || M_CheckParm("-timedemo") > 0
         || M_CheckParm("-record") > 0)
        {
            printf("Turn-based mode is single-player only: "
                   "netplay and demos are disabled with -turnbased.\n"
                   "Falling back to real-time mode.\n");
            turnbased_mode = false;
        }
        else
        {
            turnctrl.state = TS_PLANNING;
            t_script_path = NULL;
            {
                int p = M_CheckParmWithArgs("-tbscript", 1);
                if (p > 0)
                    t_script_path = myargv[p + 1];
            }
            if (M_CheckParm("-tbarena") > 0)
                t_arena_done = false; // spawn on first level tick
            else
                t_arena_done = true;
            printf("Turn-based mode enabled: discrete Tempo turns, "
                   "no aiming. WASD move, SPACE use, . wait, T end turn.\n");
        }
    }

    t_ready = true;
}

void T_NewGame(void)
{
    if (!turnbased_mode || !t_ready)
        return;
    memset(&turnctrl, 0, sizeof(turnctrl));
    turnctrl.tp_max = 10;
    turnctrl.tp = turnctrl.tp_max;
    turnctrl.round = 1;
    turnctrl.selected_target = -1;
    turnctrl.rng_seed = 0xC0FFEEu;
    turnctrl.state = TS_PLANNING;
    t_arena_done = (M_CheckParm("-tbarena") <= 0);
    t_script_done = false;
}

void T_OnLoad(void)
{
    // A save captures the map, not the controller. Land in PLANNING so
    // the player can continue the round; round/TP persist in memory.
    if (!T_Active())
        return;
    turnctrl.state = TS_PLANNING;
    turnctrl.pulse_left = 0;
    turnctrl.queued = TA_NONE;
}

// ------------------------------------------------------------------
// Bounded simulation pulse
// ------------------------------------------------------------------

// View maintenance during frozen planning.
//
// P_Ticker never runs in turn mode, so the per-tic player upkeep the
// renderer depends on (viewz via P_CalcHeight, weapon sprite animation,
// damage/bonus palette decay) must happen explicitly. This is view-only:
// no movement, no sector damage, no pickups, no powerup ticks, no
// monster AI. The ticcmd is zeroed so accumulated analog input (e.g.
// mouse motion) cannot leak through.
//
// NOTE (Phase 5): P_MovePsprites advances the weapon state machine.
// Attack resolution must leave the weapon in a non-firing state at end
// of pulse, otherwise planning-time animation would keep firing.
#define T_INVERSECOLORMAP 32 // == INVERSECOLORMAP in p_user.c

static void T_MaintainView(void)
{
    int i;
    for (i = 0; i < MAXPLAYERS; i++)
    {
        player_t *pl = &players[i];
        if (!playeringame[i] || !pl->mo)
            continue;
        memset(&pl->cmd, 0, sizeof(pl->cmd));
        P_CalcHeight(pl);
        P_MovePsprites(pl);
        if (pl->damagecount)
            pl->damagecount--;
        if (pl->bonuscount)
            pl->bonuscount--;
        // Colormap handling, mirrored from P_PlayerThink.
        if (pl->powers[pw_invulnerability])
        {
            if (pl->powers[pw_invulnerability] > 4 * 32
                || (pl->powers[pw_invulnerability] & 8))
                pl->fixedcolormap = T_INVERSECOLORMAP;
            else
                pl->fixedcolormap = 0;
        }
        else if (pl->powers[pw_infrared])
        {
            if (pl->powers[pw_infrared] > 4 * 32
                || (pl->powers[pw_infrared] & 8))
                pl->fixedcolormap = 1;
            else
                pl->fixedcolormap = 0;
        }
        else
            pl->fixedcolormap = 0;
    }
}

static void T_EndPulse(void);

// Phase 2: infinite-ammo invariant. Turn mode never tracks ammunition;
// availability is governed by TP cost, cooldowns, charge, and heat.
// Keep every ammo pool maxed while planning so the weapon state machine
// can never fail or auto-switch for lack of ammo. Real-time mode never
// reaches this code.
void T_TopUpAmmo(void)
{
    player_t *p;
    int i;
    if (!T_Active())
        return;
    p = &players[consoleplayer];
    for (i = 0; i < NUMAMMO; i++)
        p->ammo[i] = p->maxammo[i];
}

// One world tic with optional monster freeze. Mirrors P_Ticker's
// structure; live monsters (MF_COUNTKILL, alive, not the player) are
// skipped when freeze_monsters is set — XCOM-style, they act on their
// own phase.
static void T_PulseOneTic(boolean freeze_monsters)
{
    int i;
    thinker_t *currentthinker, *nextthinker;

    if (paused)
        return;

    for (i = 0; i < MAXPLAYERS; i++)
        if (playeringame[i])
        {
            // No analog input during pulses: the discrete action already
            // moved/faced the player explicitly.
            memset(&players[i].cmd, 0, sizeof(ticcmd_t));
            P_PlayerThink(&players[i]);
        }

    currentthinker = thinkercap.next;
    while (currentthinker != &thinkercap)
    {
        if (currentthinker->function.acv == (actionf_v)(-1))
        {
            nextthinker = currentthinker->next;
            currentthinker->next->prev = currentthinker->prev;
            currentthinker->prev->next = currentthinker->next;
            Z_Free(currentthinker);
        }
        else
        {
            if (currentthinker->function.acp1)
            {
                boolean skip = false;
                if (freeze_monsters)
                {
                    mobj_t *mo = (mobj_t *)currentthinker;
                    // Only mobj thinkers can be monsters; the cast is
                    // safe because we check the function first.
                    if (currentthinker->function.acp1 == (actionf_p1)P_MobjThinker
                     && mo->player == NULL
                     && (mo->flags & MF_COUNTKILL)
                     && mo->health > 0)
                        skip = true;
                }
                if (!skip)
                    currentthinker->function.acp1(currentthinker);
            }
            nextthinker = currentthinker->next;
        }
        currentthinker = nextthinker;
    }

    P_UpdateSpecials();
    P_RespawnSpecials();
    leveltime++;
}

void T_BeginPulse(int tics, boolean freeze_monsters, boolean fast)
{
    turnctrl.pulse_left = tics;
    turnctrl.pulse_freeze = freeze_monsters;
    turnctrl.pulse_fast = fast;
    // Do NOT clear pulse_enemy here: T_DoEndTurn sets it before starting
    // the enemy-phase pulse, and T_EndPulse clears it after consuming it.
    if (!turnctrl.sync)
        turnctrl.state = TS_PULSE;
    else
    {
        // Script harness: run to completion immediately, then normalize
        // state exactly as the async path would.
        while (turnctrl.pulse_left > 0)
        {
            T_PulseOneTic(freeze_monsters);
            turnctrl.pulse_left--;
        }
        T_EndPulse();
    }
}

boolean T_InPulse(void)
{
    return turnctrl.state == TS_PULSE || turnctrl.state == TS_REACTION;
}

void T_RunPulseSync(int tics, boolean freeze_monsters)
{
    boolean was_sync = turnctrl.sync;
    turnctrl.sync = true;
    T_BeginPulse(tics, freeze_monsters, true);
    turnctrl.sync = was_sync;
}

// Round bookkeeping when a pulse finishes.
static void T_EndPulse(void)
{
    if (turnctrl.pulse_enemy)
    {
        // Enemy phase done: tick cooldowns (phase 5), new round.
        turnctrl.pulse_enemy = false;
        turnctrl.round++;
        turnctrl.tp = turnctrl.tp_max; // TP economy lands in phase 2
        turnctrl.hunkered = 0;
        turnctrl.overwatch_tp = 0;
        turnctrl.state = TS_PLANNING;
        {
            static char msg[64];
            M_snprintf(msg, sizeof(msg), "Round %d - your move.",
                       turnctrl.round);
            players[consoleplayer].message = msg;
        }
    }
    else
    {
        turnctrl.state = TS_PLANNING;
    }
    turnctrl.queued = TA_NONE;
    T_DumpState("pulse-end");
}

void T_Ticker(void)
{
    int i, n;

    if (!T_Active())
        return;

    // Belt and braces: no real-time input may leak into pulses.
    for (i = 0; i < MAXPLAYERS; i++)
        if (playeringame[i])
            memset(&players[i].cmd, 0, sizeof(ticcmd_t));

    // Deferred test-harness startup: arena + script on first level tick.
    if (!t_arena_done)
    {
        t_arena_done = true;
        T_SpawnArena();
    }
    if (t_script_path && !t_script_done)
    {
        t_script_done = true;
        T_RunScript(t_script_path);
    }

    if (paused)
        return;

    // Planning/targeting/confirm: the world is frozen, but the player's
    // view (viewz, weapon sprite, palette) must be maintained or the
    // renderer shows a broken frame.
    if (turnctrl.state == TS_PLANNING || turnctrl.state == TS_TARGETING
        || turnctrl.state == TS_CONFIRM || turnctrl.state == TS_ROUND_END)
    {
        T_MaintainView();
        T_TopUpAmmo();
    }

    if (turnctrl.state == TS_PULSE || turnctrl.state == TS_REACTION)
    {
        n = turnctrl.pulse_fast ? 4 : 1;
        while (n-- > 0 && turnctrl.pulse_left > 0)
        {
            T_PulseOneTic(turnctrl.pulse_freeze);
            turnctrl.pulse_left--;
        }
        if (turnctrl.pulse_left <= 0)
            T_EndPulse();
    }
    // PLANNING / TARGETING / CONFIRM: the world is frozen. Nothing runs.
}

// ------------------------------------------------------------------
// HUD
// ------------------------------------------------------------------

// Minimal text drawer using the already-loaded HU font (STCFN033+).
static int T_TextWidth(const char *s)
{
    int w = 0;
    while (*s)
    {
        unsigned char c = (unsigned char)*s++;
        if (c == ' ')
        {
            w += 5;
            continue;
        }
        if (c < '!' || c > '_') // hu_font covers '!'..'_' only (no lowercase)
            continue;
        w += SHORT(hu_font[c - '!']->width) + 1;
    }
    return w;
}

static void T_DrawText(int x, int y, const char *s)
{
    while (*s)
    {
        unsigned char c = (unsigned char)*s++;
        if (c == ' ')
        {
            x += 5;
            continue;
        }
        if (c < '!' || c > '_') // hu_font covers '!'..'_' only (no lowercase)
            continue;
        {
            patch_t *p = hu_font[c - '!'];
            int w = SHORT(p->width);
            int h = SHORT(p->height);
            // Clamp into the framebuffer; never trip V_DrawPatch rangecheck.
            if (x < 0)
                x = 0;
            if (x + w <= 320 && y >= 0 && y + h <= 200)
                V_DrawPatch(x, y, p);
            x += w + 1;
        }
    }
}

static void T_DrawTextCentered(int y, const char *s)
{
    T_DrawText((320 - T_TextWidth(s)) / 2, y, s);
}

void T_DrawHUD(void)
{
    char line[96];

    if (!T_Active())
        return;

    M_snprintf(line, sizeof(line), "TURN MODE - ROUND %d - TP %d/%d",
               turnctrl.round, turnctrl.tp, turnctrl.tp_max);
    T_DrawTextCentered(2, line);

    if (turnctrl.state == TS_REACTION)
        T_DrawTextCentered(12, "ENEMY PHASE...");
    else if (turnctrl.state == TS_PULSE)
        T_DrawTextCentered(12, "RESOLVING");
    else
    {
        T_DrawTextCentered(12, "WASD MOVE  X SWAP  SPC USE  . WAIT");
        T_DrawTextCentered(22, "H HUNKER  O OVERWATCH  T END TURN");
    }
}

// ------------------------------------------------------------------
// State dump (test harness)
// ------------------------------------------------------------------

void T_DumpState(const char *why)
{
    player_t *p = &players[consoleplayer];
    static const char *names[] = {
        "OFF", "PLANNING", "TARGETING", "CONFIRM",
        "PULSE", "REACTION", "ROUND_END"
    };
    const char *st = (turnctrl.state >= 0 && turnctrl.state <= TS_ROUND_END)
                     ? names[turnctrl.state] : "?";

    printf("[TURN] %-10s round=%d tp=%d/%d state=%s sel=%d hp=%d pos=(%d,%d) angle=%u\n",
           why ? why : "-",
           turnctrl.round, turnctrl.tp, turnctrl.tp_max, st,
           turnctrl.selected_target,
           p->mo ? p->health : -1,
           p->mo ? p->mo->x >> FRACBITS : 0,
           p->mo ? p->mo->y >> FRACBITS : 0,
           p->mo ? (unsigned)(p->mo->angle >> 24) : 0);
    fflush(stdout);
}

// ------------------------------------------------------------------
// Deterministic RNG
// ------------------------------------------------------------------

int T_Random(void)
{
    turnctrl.rng_seed = turnctrl.rng_seed * 1664525u + 1013904223u;
    return (int)((turnctrl.rng_seed >> 16) & 0x7FFF);
}

void T_SeedRNG(unsigned int seed)
{
    turnctrl.rng_seed = seed;
}
