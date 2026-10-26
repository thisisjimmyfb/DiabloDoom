//
// t_turn.c — Turn-based (XCOM) mode: central turn controller.
//
// Owns the state machine, the bounded simulation pulse, round/enemy
// phases, and the test harnesses. This fork is turn-based by default:
// G_Ticker calls T_Ticker instead of P_Ticker unconditionally.

#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include "hu_stuff.h"
#include "t_turn.h"
#include "t_combat.h"
#include "d_diablo.h"
#include "doomstat.h"
#include "doomdef.h"
#include "doomkeys.h"
#include "d_player.h"
#include "d_think.h"
#include "d_main.h"
#include "m_argv.h"
#include "p_tick.h"
#include "i_swap.h"
#include "g_game.h"
#include "d_replay.h"
#include "p_local.h"
#include "r_main.h"
#include "m_misc.h"
#include "s_sound.h"
#include "m_random.h"
#include "r_state.h"
#include "v_video.h"
#include "w_wad.h"
#include "z_zone.h"

// Kill banner (defined near the kill counters below): forward declarations
// so T_Ticker and T_DrawHUD can use them before the definitions.
static void T_TickKillBanner(void);
static void T_DrawKillBanner(void);
// T_IsCombatant is defined with the target-list API below; the HUD marker
// loop uses it earlier.
boolean T_IsCombatant(mobj_t *mo);

// ------------------------------------------------------------------
// Explore-mode detection: combat triggers when an AI enemy acquires
// the player as its target -- not when the player merely sees an
// enemy. Monsters are frozen in explore mode (P_MobjThinker skips
// them), so this pass mirrors what A_Look would do if it ran: sight
// within the monster's front 180-degree arc, or hearing a shot for
// non-deaf monsters. That enables limited stealth: approaching from
// behind or outside line of sight never alerts them, while firing a
// weapon (P_NoiseAlert) wakes everything in earshot.
// ------------------------------------------------------------------

// Short display names for the "X spots you!" / "X is hunting you!"
// messages.
static const char *T_MobjShortName(mobj_t *mo)
{
    if (mo == NULL)
        return "Enemy";
    switch (mo->type)
    {
      case MT_POSSESSED: return "Zombieman";
      case MT_SHOTGUY:   return "Shotgunner";
      case MT_CHAINGUY:  return "Chaingunner";
      case MT_TROOP:     return "Imp";
      case MT_SERGEANT:  return "Demon";
      default:           return "Enemy";
    }
}

// Mirror of A_Look's "seeyou" block for one monster: record the
// acquisition, play the wake-up sound, and park the monster in its see
// state. P_SetMobjState cannot be used here: it would invoke the chase
// action immediately, but the explore world is frozen, so the state
// fields are assigned directly and the action runs on the first
// enemy-phase tick instead.
static void T_SpotPlayer(mobj_t *mo, boolean by_sight)
{
    player_t *player = &players[consoleplayer];
    int sound;

    mo->target = player->mo;
    mo->threshold = 0;

    // Wake-up sound, with vanilla's randomization for zombie grunts.
    sound = mo->info->seesound;
    if (sound == sfx_posit1 || sound == sfx_posit2 || sound == sfx_posit3)
        sound = sfx_posit1 + P_Random() % 3;
    else if (sound == sfx_bgsit1 || sound == sfx_bgsit2)
        sound = sfx_bgsit1 + P_Random() % 2;
    if (sound != sfx_None)
    {
        if (mo->type == MT_SPIDER || mo->type == MT_CYBORG)
            S_StartSound(NULL, sound); // full volume
        else
            S_StartSound(mo, sound);
    }
    printf("[TURN] detect: %s %s the player\n", T_MobjShortName(mo),
           by_sight ? "saw" : "heard");

    mo->state = &states[mo->info->seestate];
    mo->tics = mo->state->tics;
    mo->sprite = mo->state->sprite;
    mo->frame = mo->state->frame;
}

// Explore-mode detection pass. Returns the first monster that acquired
// the player this tick (NULL if none); every detector records the
// acquisition. A monster that already holds the player as its target
// (shot, heard, or saw the player earlier) re-triggers: being acquired
// IS the combat trigger.
static mobj_t *T_ExploreDetect(void)
{
    player_t *player = &players[consoleplayer];
    thinker_t *th;
    mobj_t *spotter = NULL;

    if (!T_Active() || player->mo == NULL || player->mo->health <= 0)
        return NULL;

    for (th = thinkercap.next; th != &thinkercap; th = th->next)
    {
        mobj_t *mo;
        angle_t an;

        if (th->function.acp1 != (actionf_p1)P_MobjThinker)
            continue;
        mo = (mobj_t *)th;
        if (mo == player->mo || mo->player != NULL)
            continue;
        if (!(mo->flags & MF_SHOOTABLE) || mo->health <= 0)
            continue;
        if (!T_IsCombatant(mo)) // barrels etc. never trigger combat
            continue;

        // Already acquired (damaged, heard, or saw the player): the
        // acquisition itself is the trigger, but only if there's
        // current line of sight. Prevents entering combat through a
        // closed/half-open door when the monster can't actually see you.
        if (mo->target == player->mo)
        {
            if (!P_CheckSight(mo, player->mo))
                continue;
            if (spotter == NULL)
                spotter = mo;
            continue;
        }

        // Hearing (vanilla A_Look): a shot in this sector wakes
        // non-deaf monsters with no sight check. Hearing ALERTS the
        // monster (it gets the player as target and hunts in explore
        // mode) but does NOT trigger combat -- combat requires
        // acquisition via the LOS path. A heard-only monster that
        // rounds a corner and gains LOS enters combat through the
        // already-acquired branch above.
        if (!(mo->flags & MF_AMBUSH)
            && mo->subsector != NULL
            && mo->subsector->sector->soundtarget == player->mo)
        {
            T_SpotPlayer(mo, false);
            continue;
        }

        // Sight: front 180-degree arc + line of sight, exactly like
        // P_LookForPlayers(actor, false). Behind the monster's back
        // stays hidden.
        an = R_PointToAngle2(mo->x, mo->y,
                             player->mo->x, player->mo->y) - mo->angle;
        if (an > ANG90 && an < ANG270)
            continue;
        if (!P_CheckSight(mo, player->mo))
            continue;

        T_SpotPlayer(mo, true);
        if (spotter == NULL)
            spotter = mo;
    }
    return spotter;
}

// One explore-mode tick: run the world, then run throttled detection.
// Shared by T_Ticker and the test harness.
void T_ExploreTick(void)
{
    static int explore_sight_tic = 0;
    mobj_t *spotter;

    P_Ticker();
    // GOTO path stepping (explore mode only; cancelled on combat entry).
    T_GotoTick();
    // WALK persistent movement (explore mode only; stops on enemy/loot/blocked).
    T_WalkTick();
    // COLLECT autonomous walk to loot (explore mode only; cancelled
    // on combat entry; blocks FIFO input while active).
    T_CollectTick();
    if (++explore_sight_tic >= 7)
    {
        explore_sight_tic = 0;
        T_RefreshTargets();      // HUD markers: what the PLAYER sees
        spotter = T_ExploreDetect(); // what SEES the player
        if (spotter != NULL)
            T_EnterCombat(spotter);
    }
}

turnctrl_t turnctrl;

// Forward declaration (definition lives with the targeting code below).
static int t_numtargets;

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
    return t_ready && gamestate == GS_LEVEL && !netgame;
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
    turnctrl.state = TS_OFF;

    // Single-player only; no demos. This fork has no real-time mode to
    // fall back to, so netplay/demos are simply unsupported: warn and
    // keep turn mode armed (T_Active stays false while netgame, so the
    // legacy ticker runs instead).
    if (netgame
     || M_CheckParm("-playdemo") > 0
     || M_CheckParm("-timedemo") > 0
     || M_CheckParm("-record") > 0)
    {
        printf("Turn-based mode is single-player only: "
               "netplay and demos are not supported in this fork.\n");
    }
    else
    {
        turnctrl.state = TS_EXPLORE;
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
        printf("DiabloDoom: explore freely (WASD); turn-based combat "
               "starts when an enemy spots you.\n");
        // Phase 8: load the standalone profile.
        T_ProfileLoad();
        // Gun cadence (cooldowns/heat/charges) starts fresh every game.
        T_KitResetCadence();
    }

    t_ready = true;
}

void T_NewGame(void)
{
    if (!t_ready)
        return;
    memset(&turnctrl, 0, sizeof(turnctrl));
    turnctrl.tp_max = 10;
    turnctrl.tp = turnctrl.tp_max;
    turnctrl.round = 1;
    turnctrl.selected_target = -1;
    turnctrl.state = TS_EXPLORE;
    t_arena_done = (M_CheckParm("-tbarena") <= 0);
    t_script_done = false;
    // Fresh gun cadence for the new run (loaded games restore theirs).
    T_KitResetCadence();
}

void T_OnLoad(void)
{
    // A save captures the map, not the controller. Land in EXPLORE so
    // the player can move freely; combat triggers on sight.
    if (!T_Active())
        return;
    turnctrl.state = TS_EXPLORE;
    turnctrl.pulse_left = 0;
    turnctrl.queue_len = 0;
    turnctrl.queue_tp = 0;
    turnctrl.executing = false;
}

// ------------------------------------------------------------------
// Explore/combat mode split.
// ------------------------------------------------------------------

boolean T_InExplore(void)
{
    return T_Active() && turnctrl.state == TS_EXPLORE;
}

void T_EnterExplore(void)
{
    thinker_t *th;
    int i;

    turnctrl.state = TS_EXPLORE;
    turnctrl.queue_len = 0;
    turnctrl.queue_tp = 0;
    turnctrl.selected_target = -1;
    turnctrl.executing = false;
    turnctrl.pulse_left = 0;
    // The fight is over: the continuous-fire lock stands down too.
    T_ClearContinuousFire();
    // Stand down: the fight is over, so every monster forgets the
    // player -- otherwise the acquire==trigger rule would instantly
    // re-trigger combat on the next detection tick. Stale gunshot
    // noise is cleared for the same reason.
    for (th = thinkercap.next; th != &thinkercap; th = th->next)
    {
        mobj_t *mo;
        if (th->function.acp1 != (actionf_p1)P_MobjThinker)
            continue;
        mo = (mobj_t *)th;
        if (mo->player == NULL)
        {
            mo->target = NULL;
            mo->threshold = 0;
        }
    }
    for (i = 0; i < numsectors; i++)
        sectors[i].soundtarget = NULL;
    players[consoleplayer].message = "Explore: WASD to move.";
    // Clear stale movement keys so we don't drift on combat exit.
    {
        extern void G_ClearMovementKeys(void);
        G_ClearMovementKeys();
    }
    T_DumpState("enter-explore");
}

void T_EnterCombat(mobj_t *spotter)
{
    static char msg[96];
    // GOTO paths, WALK and the COLLECT walk are explore-mode only;
    // cancel on combat entry.
    T_GotoCancel();
    T_WalkCancel();
    T_CollectCancel();
    // Auto-save before combat: don't lose the pre-fight state.
    // Saves both the Doom savegame (slots 7-9) and progress.dat.
    {
        extern void D_AutoSaveGame(void);
        D_AutoSaveGame();
    }
    // Movement keys that might be held from explore mode. Their key
    // repeats are stale -- the player must release and re-press to act.
    static const int move_keys[] = {
        'w', 'a', 's', 'd', 'W', 'A', 'S', 'D',
        KEY_UPARROW, KEY_DOWNARROW, KEY_LEFTARROW, KEY_RIGHTARROW
    };
    unsigned int i;

    turnctrl.state = TS_PLANNING;
    turnctrl.round = 1;
    turnctrl.tp = turnctrl.tp_max;
    turnctrl.queue_len = 0;
    turnctrl.queue_tp = 0;
    turnctrl.selected_target = -1;
    turnctrl.executing = false;
    // Debug: why did combat trigger, and can the player see the spotter?
    // (Investigation 2026-10-19: combat re-triggering with no visible target.)
    if (spotter != NULL && players[consoleplayer].mo != NULL)
    {
        mobj_t *pmo = players[consoleplayer].mo;
        T_RefreshTargets();
        printf("[COMBAT] enter: spotter type=%d at (%d,%d), player at (%d,%d), dist=%d, player_sees_spotter=%d, visible_targets=%d\n",
               spotter->type, spotter->x >> 16, spotter->y >> 16,
               pmo->x >> 16, pmo->y >> 16,
               P_AproxDistance(spotter->x - pmo->x, spotter->y - pmo->y) >> 16,
               P_CheckSight(pmo, spotter) ? 1 : 0,
               t_numtargets);
    }
    // Anchor the preview at combat entry.
    if (players[consoleplayer].mo != NULL)
    {
        turnctrl.turn_start_x = players[consoleplayer].mo->x;
        turnctrl.turn_start_y = players[consoleplayer].mo->y;
        turnctrl.turn_start_z = players[consoleplayer].mo->z;
        turnctrl.turn_start_angle = players[consoleplayer].mo->angle;
    }
    // Stale-key guard: ignore held movement keys until released.
    memset(turnctrl.stale_keys, 0, sizeof(turnctrl.stale_keys));
    for (i = 0; i < sizeof(move_keys) / sizeof(move_keys[0]); i++)
        if (move_keys[i] >= 0 && move_keys[i] < 256)
            turnctrl.stale_keys[move_keys[i]] = true;
    // Clear Doom-level key states to prevent stuck movement.
    {
        extern void G_ClearMovementKeys(void);
        G_ClearMovementKeys();
    }
    // The surprise, named: whoever acquired the player gets the callout.
    if (spotter != NULL)
        M_snprintf(msg, sizeof(msg),
                   "%s spots you! COMBAT! Round 1 - your move.",
                   T_MobjShortName(spotter));
    else
        M_snprintf(msg, sizeof(msg), "COMBAT! Round 1 - your move.");
    players[consoleplayer].message = msg;
    T_DumpState("enter-combat");
}

// Test harness: the script LOAD token runs G_DoLoadGame, which goes
// through G_InitNew -> G_DoLoadLevel -> T_NewGame. That zeroes the live
// controller (clearing sync mode) and resets the script/arena one-shot
// flags, so a script that continues after LOAD would run async and then
// re-run itself forever on the next tick (also re-spawning the arena).
// Re-assert the script context here; the load itself already restored
// round/TP/cooldowns via P_UnArchiveTurn.
void T_ScriptLoadFixup(void)
{
    turnctrl.sync = true;
    t_script_done = true;
    t_arena_done = true;
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

// Ammo design: every weapon owns a magazine (see t_combat.c). The other
// Doom ammo pools stay maxed while planning so the underlying weapon
// state machine can never fail or auto-switch for lack of ammo.
// Magazines replenish via the per-tic trickle while game time runs
// (explore / progress phase), plus pickups and Mana Potions.
// Real-time mode never reaches this code.
void T_TopUpAmmo(void)
{
    player_t *p;
    int i;
    if (!T_Active())
        return;
    p = &players[consoleplayer];
    for (i = 0; i < NUMAMMO; i++)
        if (i != am_clip)
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

    // Smooth combat movement: step toward the destination stored by
    // T_ExecMove. Speed eases out as we arrive (keyboard speed when far,
    // proportional when close) for a natural friction feel. Finishes when
    // within epsilon of the target; the failsafe cap prevents a stall.
    if (turnctrl.move_tics > 0)
    {
        player_t *pl = &players[consoleplayer];
        if (pl->mo != NULL)
        {
            fixed_t dx = turnctrl.move_tx - pl->mo->x;
            fixed_t dy = turnctrl.move_ty - pl->mo->y;
            fixed_t dist = P_AproxDistance(dx, dy);

            if (dist < 4 * FRACUNIT)
            {
                // Arrived: snap to target, finish the move and end the pulse.
                P_TryMove(pl->mo, turnctrl.move_tx, turnctrl.move_ty);
                P_ZMovement(pl->mo);
                turnctrl.move_tics = 0;
                turnctrl.pulse_left = 0;
            }
            else
            {
                fixed_t speed;
                fixed_t nx, ny;
                // Ease-out: full keyboard speed (8u/tic) when far,
                // proportional slowdown within 16 units.
                if (dist > 16 * FRACUNIT)
                    speed = 8 * FRACUNIT;
                else
                    speed = dist / 2;
                if (speed < FRACUNIT)
                    speed = FRACUNIT;
                nx = pl->mo->x + FixedDiv(FixedMul(dx, speed), dist);
                ny = pl->mo->y + FixedDiv(FixedMul(dy, speed), dist);
                if (!P_TryMove(pl->mo, nx, ny))
                {
                    pl->mo->momx = nx - pl->mo->x;
                    pl->mo->momy = ny - pl->mo->y;
                    P_SlideMove(pl->mo);
                    pl->mo->momx = pl->mo->momy = 0;
                }
                P_ZMovement(pl->mo);
                turnctrl.move_tics--;
                if (turnctrl.move_tics <= 0)
                {
                    // Failsafe: stop where we are.
                    turnctrl.move_tics = 0;
                }
            }
        }
        else
        {
            turnctrl.move_tics = 0;
        }
    }

    for (i = 0; i < MAXPLAYERS; i++)
        if (playeringame[i])
        {
            // No analog input during pulses: the discrete action already
            // moved/faced the player explicitly.
            memset(&players[i].cmd, 0, sizeof(ticcmd_t));
            // Turn-mode attacks hold the fire button for the pulse.
            if (turnctrl.firing && i == consoleplayer)
                players[i].cmd.buttons |= BT_ATTACK;
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

    // Pickup by presence, mirroring P_Ticker: collect specials within
    // reach of where each living player actually is, now that this
    // tic's movement has resolved. The progress phase never runs
    // P_Ticker, so without this the sweep never fires in combat and
    // loot the player walks over (or that drops at his feet) stays on
    // the ground (Jimmy, 2026-10-22). This is position only -- input
    // stays locked during pulses, G included.
    for (i=0 ; i<MAXPLAYERS ; i++)
        if (playeringame[i] && players[i].mo && players[i].health > 0
            && players[i].playerstate == PST_LIVE)
            P_TouchNearbySpecials (players[i].mo);

    P_UpdateSpecials();
    P_RespawnSpecials();
    leveltime++;

    // (Ammo replenish is NOT here: the one cadence loop,
    // T_TickCadence, moves every magazine on every unfrozen game
    // tic from T_Ticker -- pulses included, since T_Ticker drives
    // them -- so no second regen path lives in the pulse simulator.)

    // Continuous weapon fire: while the lock is armed, the tick
    // performs the attack at the weapon's attack-speed interval --
    // the held-trigger behavior. Runs once per simulated tic.
    T_TickContinuousFire();
}

void T_BeginPulse(int tics, boolean freeze_monsters, boolean fast)
{
    // During queue execution (real-time mode), never freeze monsters:
    // the game unfreezes and enemies act while commands play out.
    if (turnctrl.executing)
        freeze_monsters = false;
    // Accumulate: a Move queued after an Attack must not cancel the
    // attack's delay. Each command's duration adds to the total.
    turnctrl.pulse_left += tics;
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

// True while a target selection is open (TARGETING/CONFIRM). The menu
// uses this to let ESC fall through to the turn responder (cancel)
// instead of opening the menu and trapping the player in targeting.
boolean T_InSelection(void)
{
    return T_Active() &&
        (turnctrl.state == TS_TARGETING || turnctrl.state == TS_CONFIRM);
}

// Round bookkeeping when a pulse finishes.
static void T_EndPulse(void)
{
    // Fast-forward death animations: monsters killed during the pulse may
    // be mid-death-animation when the pulse ends. During planning the world
    // is frozen, so they'd stick in a half-fallen pose. Advance any dying
    // monster to its final corpse state (tics == -1) now.
    {
        extern thinker_t thinkercap;
        thinker_t *th;
        for (th = thinkercap.next; th != &thinkercap; th = th->next)
        {
            if (th->function.acp1 == (actionf_p1)P_MobjThinker)
            {
                mobj_t *mo = (mobj_t *)th;
                if (mo->health <= 0 && mo->state != NULL && mo->state->tics != -1)
                {
                    int guard = 0;
                    // Advance through death states until the permanent corpse
                    // frame. Cap iterations to prevent a hang on malformed
                    // state cycles.
                    while (mo->state != NULL && mo->state->tics != -1 && guard++ < 32)
                    {
                        P_SetMobjState(mo, mo->state->nextstate);
                    }
                    mo->tics = -1;
                }
            }
        }
    }

    if (turnctrl.pulse_enemy)
    {
        // Enemy phase done: tick cooldowns (phase 5), new round.
        turnctrl.pulse_enemy = false;
        turnctrl.round++;
        T_CountRound(); // Phase 8: lifetime rounds
        turnctrl.tp = turnctrl.tp_max; // TP economy lands in phase 2
        T_KitTickCooldowns();
        // No per-round ammo grant (removed at Jimmy's order,
        // 2026-10-22): magazines replenish only via the trickle
        // while game time runs, pickups, and potions.
        turnctrl.state = TS_PLANNING;
        // Anchor the preview: turn start is the camera home for queue
        // previews, UNDO/CLEAR resets, and the END TURN teleport-back.
        if (players[consoleplayer].mo != NULL)
        {
            turnctrl.turn_start_x = players[consoleplayer].mo->x;
            turnctrl.turn_start_y = players[consoleplayer].mo->y;
            turnctrl.turn_start_z = players[consoleplayer].mo->z;
            turnctrl.turn_start_angle = players[consoleplayer].mo->angle;
        }
        // Combat exit: no visible combatants left -> back to explore.
        T_RefreshTargets();
        if (T_NumCombatants() == 0)
        {
            T_EnterExplore();
        }
        else
        {
            // A continuous-fire lock stays armed across rounds; the
            // tick resumes firing in the next progress phase.
            static char msg[96];
            if (T_LootAvailable())
                M_snprintf(msg, sizeof(msg), "Round %d - LOOT AVAILABLE! Press G.",
                           turnctrl.round);
            else
                M_snprintf(msg, sizeof(msg), "Round %d - your move.",
                           turnctrl.round);
            players[consoleplayer].message = msg;
        }
    }
    else if (turnctrl.executing)
    {
        // During queue execution: if the queue still has entries, continue
        // draining. If empty, this is the end of the progress phase —
        // back to planning (or explore if no combatants).
        if (turnctrl.queue_len > 0)
        {
            T_ExecuteNext();
        }
        else
        {
            // Continuous fire does NOT extend the progress phase
            // (Jimmy, 2026-10-22): an attack command's execution
            // lasts its attack-speed interval (the pulse T_ExecAttack
            // began for the shot) and no longer -- execution must not
            // wait for the continuous stream to stop. The lock stays
            // armed across the phase boundary and T_TickContinuousFire
            // resumes it in the next progress phase, the way the
            // weapon animation carries on after the command itself
            // has executed.
            // End of progress phase. If the player died, stop here —
            // the death flow takes over. Otherwise back to planning.
            turnctrl.executing = false;
            if (players[consoleplayer].mo == NULL
                || players[consoleplayer].health <= 0)
            {
                // Dead: clear queue and let the death flow handle it.
                turnctrl.queue_len = 0;
                turnctrl.queue_tp = 0;
                return;
            }
            T_RefreshTargets();
            if (T_NumCombatants() == 0)
            {
                T_EnterExplore();
            }
            else
            {
                turnctrl.state = TS_PLANNING;
                turnctrl.tp = turnctrl.tp_max;
                turnctrl.round++;
                if (players[consoleplayer].mo != NULL)
                {
                    turnctrl.turn_start_x = players[consoleplayer].mo->x;
                    turnctrl.turn_start_y = players[consoleplayer].mo->y;
                    turnctrl.turn_start_z = players[consoleplayer].mo->z;
                    turnctrl.turn_start_angle = players[consoleplayer].mo->angle;
                }
                // A continuous-fire lock stays armed across rounds;
                // the tick resumes firing next progress phase.
            }
        }
    }
    else
    {
        turnctrl.state = TS_PLANNING;
    }
    T_DumpState("pulse-end");
}

// VM automation driver: persistent FIFO at /tmp/doom_cmd.txt.
// Only on Muse VMs (gated by D_OnMuseVM). The FIFO is created once,
// opened O_RDWR|O_NONBLOCK (never blocks, never EOF since we hold
// both ends), and kept open for the game's lifetime. Workers write
// commands with e.g. `echo "WALK" > /tmp/doom_cmd.txt`. Each tick we
// drain whatever's available without blocking.
static int cmdfd = -2;  // -2 = uninitialized, -1 = failed/disabled

// Command queue: the reader consumes the FIFO one complete line at
// a time (see cmd_drain) into this in-memory queue, and the game
// executes exactly one queued command per tick. A new line is only
// started while the queue has a free slot (backpressure): lines not
// yet started simply stay in the pipe, unconsumed and in order.
// No command is ever dropped for lack of room.
#define CMD_QUEUE_MAX 128
#define CMD_LINE_MAX 256
static char cmd_queue[CMD_QUEUE_MAX][CMD_LINE_MAX];
static int cmd_q_head = 0;   // slot of the next command to execute
static int cmd_q_count = 0;  // commands currently queued

// WAIT command state: while >0, cmd_drain counts down one per tick
// and dispatches NOTHING (diagnostics included) — the wait is what
// makes a batched USE, WAIT, GOTO sequence cross a door: the GOTO
// must not execute until the door has had time to open, and no
// agent round-trip (screenshot, vision, decision) fits inside a
// door's open window. One WAIT is a fixed beat (no scalar input,
// per the standing rule); stack WAITs for slower doors. The total
// is capped so a pathological batch can't park the driver for
// minutes (QUIT included — it waits like everything else).
#define CMD_WAIT_TICS 70        // 2 seconds at 35 tics/s
#define CMD_WAIT_MAX  350       // 10 seconds
static int cmd_wait_tics = 0;

// Smooth-turn state: TURNL/TURNR rotate the full 45 degrees at a
// fixed rate (5 degrees per tic, 9 tics) instead of snapping
// instantly, so the rotation reads as smooth motion in screenshots
// and replay segments. Fixed per-tic rate: deterministic, never
// frame-rate dependent. While a turn runs, cmd_drain steps the
// facing once per tick and dispatches NOTHING else (same
// serialization as the WAIT gate), so a turn never interleaves
// with the next command.
#define CMD_TURN_STEP (ANG45 / 9)  // 5 degrees per tic
#define CMD_TURN_TICS 9             // 45 degrees per turn command
static int cmd_turn_tics_left = 0;  // >0 while a smooth turn runs
static int cmd_turn_dir = 0;        // +1 left, -1 right

// SURVEY: one driver command for a full 360-degree survey spin.
// Same smooth-turn machinery as TURNL/TURNR, but at double the
// step (10 degrees per tic), so 36 tics for the full circle
// (~1 second at 35 tics/s), so
// the sweep finishes while a mechanism's effect is still unfolding
// -- a slow screenshot-by-screenshot survey finishes after the
// mechanism has already restored, and its effect is invisible.
// No argument (no scalar input); always left, always 360 degrees.
// SURVEY v2 (Jimmy, 2026-10-26): the sweep dumps its own frames.
// After EACH 10-degree step the gate requests one framebuffer
// screenshot, named survey_<ts>_NN.png (NN = 01..36 in rotation
// order, one shared timestamp stem per sweep), straight to the
// doomshots directory -- no video segment involved. The worker
// reviews those 36 dumped frames, in order; motion shows as
// change between consecutive frames. It cannot miss the effect
// by sampling the wrong segment or too sparsely, because the
// capture IS the frame set. (The 10:22 farm run reviewed the
// right segments but sampled frames and missed a lift lowering
// the human saw plainly in the clip.)
// The sweep is NOT uninterruptible: the gate's "atomic" property
// means it dispatches no other FIFO commands during the sweep --
// it does NOT block a combat-mode transition (combat entry is an
// engine event, not a FIFO command). If combat starts mid-sweep
// (e.g. the USE just opened a line of sight to a monster), the
// gate aborts at once: turning stops where it is, facing does
// NOT return to start, frames dumped so far stay as a partial
// set, and the log records the interrupt + the partial count.
// SURVEY v3 (Jimmy, 2026-10-26): 36 tics at 10 degrees per tic
// (~1 second of game time at 35 tics/s). The blocking per-step
// dump stays as-is: PNG encode time is accepted overhead and
// does not count against the 1s sweep target.
#define CMD_SURVEY_TICS 36          // 360 degrees at 10 degrees per tic
#define CMD_SURVEY_STEP (ANG90 / 9)   // 10 degrees per tic; TURNL/TURNR keep CMD_TURN_STEP

// SURVEY v2 dump state: set at SURVEY dispatch, consumed by the
// smooth-turn gate, cleared at completion. cmd_survey_ts is the
// shared timestamp stem for one sweep's 36 frames.
static boolean cmd_turn_is_survey = false;
static char cmd_survey_ts[32] = "";
static int cmd_survey_frame = 0;

// Async screenshots (Jimmy, 2026-10-26): the [SURVEY] frames line
// (and the interrupt line) must fire only after the screenshot
// queue drains, so every named file is actually on disk when the
// worker reads the line. Completion/interrupt snapshots its stem +
// count here; cmd_drain prints it once V_ScreenshotQueueEmpty().
// A normal completion HOLDS dispatch until drained (the worker's
// next command waits <1s); an interrupt NEVER holds dispatch
// (combat commands must not wait on a screenshot drain).
static boolean cmd_survey_log_pending = false;
static boolean cmd_survey_log_interrupted = false;
static char cmd_survey_log_ts[32] = "";
static int cmd_survey_log_count = 0;

// Returns false when the queue is full: the caller must leave the
// line unconsumed (it stays in the FIFO) rather than drop it.
static boolean cmd_queue_push(const char *line)
{
    int tail;
    if (cmd_q_count >= CMD_QUEUE_MAX)
        return false;
    tail = (cmd_q_head + cmd_q_count) % CMD_QUEUE_MAX;
    strncpy(cmd_queue[tail], line, CMD_LINE_MAX - 1);
    cmd_queue[tail][CMD_LINE_MAX - 1] = 0;
    cmd_q_count++;
    return true;
}

// Read-only/diagnostic commands: safe to run even while the
// COLLECT autonomous walk has gameplay input blocked. They don't
// move the player or change game state, so the walk keeps its
// input block while the worker keeps its eyes.
static boolean cmd_is_diagnostic(const char *cmd)
{
    static const char *diag[] = {
        "SCREENSHOT", "REPLAY", "CAPTURE", "VERSION",
        "INVENTORY", "KEYS", "SECTORS", NULL
    };
    int i;
    for (i = 0; diag[i] != NULL; i++)
        if (!strcmp(cmd, diag[i]))
            return true;
    return false;
}

// Line assembly: bytes are fed one at a time and a line joins the
// queue only when its terminating newline arrives. Until then the
// partial bytes wait here -- never in the pipe again, and no new
// line starts before this one ends, so stream order is exact.
static char cmd_line[CMD_LINE_MAX];
static int cmd_line_len = 0;
static boolean cmd_line_overflow = false; // line longer than the buffer

// Feed one byte. Returns true when a complete line was queued.
static boolean cmd_feed_byte(char c)
{
    if (c == '\n' || c == '\r')
    {
        boolean done = false;
        if (cmd_line_len > 0 && !cmd_line_overflow)
        {
            cmd_line[cmd_line_len] = 0;
            done = cmd_queue_push(cmd_line);
            if (!done)
                printf("[CMD] queue full at line end, lost: %s\n", cmd_line);
        }
        cmd_line_len = 0;
        cmd_line_overflow = false;
        return done;
    }
    if (cmd_line_len < CMD_LINE_MAX - 1)
        cmd_line[cmd_line_len++] = c;
    else
        cmd_line_overflow = true; // discard the rest of this line
    return false;
}

static void cmd_init(void)
{
    extern boolean D_OnMuseVM(void);
    struct stat st;
    static char early[4096];
    int early_len = 0;

    if (!D_OnMuseVM())
    {
        cmdfd = -1;
        return;
    }
    // Race fix: if a worker wrote commands before we created the FIFO,
    // the path will be a regular file (not a FIFO). Preserve any pending
    // commands from it before replacing with a real FIFO.
    if (stat("/tmp/doom_cmd.txt", &st) == 0 && !S_ISFIFO(st.st_mode))
    {
        FILE *f = fopen("/tmp/doom_cmd.txt", "r");
        if (f != NULL)
        {
            size_t r = fread(early, 1, sizeof(early), f);
            fclose(f);
            if (r > 0)
            {
                printf("[CMD] recovered %d bytes from early worker write\n", (int)r);
                early_len = (int)r;
            }
        }
        unlink("/tmp/doom_cmd.txt");
    }
    if (mkfifo("/tmp/doom_cmd.txt", 0666) != 0 && errno != EEXIST)
    {
        cmdfd = -1;
        return;
    }
    // O_RDWR: open never blocks and we never see EOF (we hold a write
    // end ourselves). O_NONBLOCK: reads never stall the game tick.
    cmdfd = open("/tmp/doom_cmd.txt", O_RDWR | O_NONBLOCK);
    if (cmdfd < 0)
        cmdfd = -1;
    // Feed the recovered early write through the line assembler; a
    // trailing fragment stays in the assembler and completes when
    // its remainder arrives via the FIFO.
    if (early_len > 0 && cmdfd >= 0)
    {
        int i;
        for (i = 0; i < early_len; i++)
            cmd_feed_byte(early[i]);
    }
}

// Line protocol (no dropped commands, no reordering, no write-back):
// - Writers terminate every append with a newline, so the pipe only
//   ever holds complete lines, each delivered in a single write.
// - The reader's unit of work is the line: it reads one byte at a
//   time until the newline, and only then is the line consumed into
//   the queue. It never reads past a newline, so there is never an
//   unconsumed byte to put back -- bytes not yet read stay in the
//   pipe, in order, until their line is read.
// - Backpressure is simply not starting a new line while the queue
//   is full; the line waits in the pipe untouched.
// Because writers append whole newline-terminated lines atomically,
// a started line always has its newline already in the pipe. (If a
// writer ever broke the rule and the pipe ran dry mid-line, the
// partial bytes wait in the line assembler and the line resumes
// next tick -- still ahead of any later line, so order holds.)

// Per-tick driver step: read complete lines from the FIFO into the
// in-memory queue (see the line protocol above), then execute
// exactly ONE queued command. One per tick lets the world respond
// between commands, the way it responds between key presses.
static void cmd_drain(void (*dispatch)(const char *))
{
    if (cmdfd == -2)
        cmd_init();
    if (cmdfd < 0)
        return;

    // Ingest lines while the queue has room. A line is started only
    // when a slot is free for it (that slot is then spoken for:
    // nothing else pushes during ingest, so the newline's push
    // always succeeds). Reading stops exactly at each newline, and
    // stops entirely when the pipe runs dry (read fails) -- with a
    // line possibly mid-assembly in cmd_line, resumed next tick.
    while (cmd_q_count < CMD_QUEUE_MAX)
    {
        boolean line_done = false;
        boolean any_data = false;
        while (!line_done)
        {
            char c;
            ssize_t r = read(cmdfd, &c, 1);
            if (r <= 0)
                break; // pipe empty right now (EOF impossible: O_RDWR)
            any_data = true;
            cmd_feed_byte(c);
            if (c == '\n' || c == '\r')
                line_done = true;
        }
        if (!any_data)
            break; // nothing was available at all
        if (!line_done)
            break; // ran dry mid-line: resume it next tick
    }

    // Async screenshot drain: print a pending SURVEY log line once
    // its frames are actually on disk. Ingest above already ran,
    // so holding dispatch here loses no command -- lines keep
    // accumulating in cmd_queue until the drain finishes.
    if (cmd_survey_log_pending)
    {
        extern int V_ScreenshotQueueEmpty(void);
        if (V_ScreenshotQueueEmpty())
        {
            if (cmd_survey_log_interrupted)
            {
                if (cmd_survey_log_count > 0)
                    printf("[SURVEY] interrupted by combat: frames dumped %d (partial sweep, stem doomshots/survey_%s_NN.png, first doomshots/survey_%s_01.png last doomshots/survey_%s_%02d.png)\n",
                           cmd_survey_log_count, cmd_survey_log_ts,
                           cmd_survey_log_ts, cmd_survey_log_ts, cmd_survey_log_count);
                else
                    printf("[SURVEY] interrupted by combat: frames dumped 0 (partial sweep, stem doomshots/survey_%s_NN.png, no frames dumped)\n",
                           cmd_survey_log_ts);
            }
            else
            {
                printf("[TURN] done\n");
                printf("[SURVEY] frames: %d pattern doomshots/survey_%s_NN.png first doomshots/survey_%s_01.png last doomshots/survey_%s_%02d.png\n",
                       cmd_survey_log_count, cmd_survey_log_ts,
                       cmd_survey_log_ts, cmd_survey_log_ts, cmd_survey_log_count);
            }
            cmd_survey_log_pending = false;
        }
        else if (!cmd_survey_log_interrupted)
        {
            // Full sweep's frames still draining: hold dispatch.
            return;
        }
        // Interrupted + still draining: fall through -- combat
        // commands must never wait on a screenshot drain.
    }

    // Smooth-turn gate: while a TURNL/TURNR animation runs, step
    // the facing per tick (CMD_TURN_STEP, or CMD_SURVEY_STEP
    // for a survey) and dispatch NOTHING
    // else, so the turn completes before the next command runs.
    if (cmd_turn_tics_left > 0)
    {
        player_t *pl = &players[consoleplayer];
        if (cmd_turn_is_survey && !T_InExplore())
        {
            // Combat interrupted the sweep (Jimmy, 2026-10-26):
            // the gate's "atomic" property only blocks FIFO
            // dispatch -- it must never delay or block a combat
            // entry, and turning must not continue inside combat
            // mode as if it were still explore. Stop where we
            // are: facing does NOT return to start. Frames dumped
            // so far stay on disk as a valid partial set; the
            // log says so explicitly, so the worker never expects
            // 36 frames or treats this as a complete 360 sweep.
            // (No [TURN] done: the turn did not complete.)
            // Async: do NOT print yet -- the partial frames may
            // still be queued. Snapshot + print on drain (above);
            // dispatch is never held for an interrupt.
            cmd_survey_log_pending = true;
            cmd_survey_log_interrupted = true;
            snprintf(cmd_survey_log_ts, sizeof(cmd_survey_log_ts),
                     "%s", cmd_survey_ts);
            cmd_survey_log_count = cmd_survey_frame;
            cmd_turn_tics_left = 0;
            cmd_turn_is_survey = false;
            return;
        }
        if (pl->mo != NULL)
            pl->mo->angle += cmd_turn_dir *
                (cmd_turn_is_survey ? CMD_SURVEY_STEP : CMD_TURN_STEP);
        cmd_turn_tics_left--;
        if (cmd_turn_is_survey)
        {
            // SURVEY v2 frame dump: one screenshot per step, in
            // rotation order. The named capture runs on the NEXT
            // tick's gameaction switch, after the new view renders,
            // so frame NN shows the view after step NN.
            extern void G_ScreenShotFile(const char *filename);
            char fname[256];
            cmd_survey_frame++;
            snprintf(fname, sizeof(fname),
                     "doomshots/survey_%s_%02d.png",
                     cmd_survey_ts, cmd_survey_frame);
            G_ScreenShotFile(fname);
        }
        if (cmd_turn_tics_left == 0)
        {
            if (cmd_turn_is_survey)
            {
                // Async: the last step's screenshot was only
                // REQUESTED (queued); the [SURVEY] frames line must
                // wait until the queue drains, so snapshot + defer
                // to the drain check above. [TURN] done rides with
                // it (the turn is not "done" for the worker until
                // its frames are reviewable). Dispatch holds until
                // then (see the drain check).
                cmd_survey_log_pending = true;
                cmd_survey_log_interrupted = false;
                snprintf(cmd_survey_log_ts, sizeof(cmd_survey_log_ts),
                         "%s", cmd_survey_ts);
                cmd_survey_log_count = cmd_survey_frame;
                cmd_turn_is_survey = false;
            }
            else
                printf("[TURN] done\n");
        }
        return;
    }

    // WAIT gate: a dispatched WAIT holds every later command —
    // diagnostics too — until the beat elapses. The screenshot in
    // a USE, WAIT, SCREENSHOT sequence must show the door AFTER
    // the wait, not before it.
    if (cmd_wait_tics > 0)
    {
        cmd_wait_tics--;
        if (cmd_wait_tics == 0)
            printf("[WAIT] elapsed\n");
        return;
    }

    if (cmd_q_count > 0)
    {
        // While the COLLECT autonomous walk is running, gameplay input
        // is blocked: queued commands wait and execute once the walk
        // finishes (picked up, unreachable, timeout, or combat).
        // Diagnostic commands (SCREENSHOT/REPLAY/etc.) pass through so
        // the driver keeps its eyes during the walk.
        if (T_CollectActive() && T_InExplore()
            && !cmd_is_diagnostic(cmd_queue[cmd_q_head]))
            return;
        dispatch(cmd_queue[cmd_q_head]);
        cmd_q_head = (cmd_q_head + 1) % CMD_QUEUE_MAX;
        cmd_q_count--;
    }
}

// Dispatch a single driver command line from the FIFO.
static void cmd_dispatch(const char *cmd)
{
    // Log every command with the current mode for debugging.
    {
        const char *mode_str = "UNKNOWN";
        if (T_InExplore())
            mode_str = "EXPLORE";
        else if (turnctrl.state == TS_PLANNING)
            mode_str = "PLANNING";
        else if (turnctrl.state == TS_PULSE)
            mode_str = "PULSE";
        else if (turnctrl.state == TS_TARGETING)
            mode_str = "TARGETING";
        else if (turnctrl.state == TS_CONFIRM)
            mode_str = "CONFIRM";
        else if (turnctrl.state == TS_REACTION)
            mode_str = "REACTION";
        else if (turnctrl.state == TS_OFF)
            mode_str = "OFF";
        printf("[CMD] %s (mode=%s)\n", cmd, mode_str);
    }
    // Show the executed command as an on-screen status message
    // (Jimmy, 2026-10-24): the vanilla message pipeline -- HU_Ticker
    // picks plyr->message up next tic and shows it for HU_MSGTIMEOUT
    // (4 s), so the command is visible in screenshots and replay
    // segments, not just the log. Static buffer: the pointer must
    // stay valid until HU_Ticker consumes it. Every executed
    // command shows, diagnostics included; WAIT-held commands show
    // only when they actually execute.
    if (cmd[0] != '\0')
    {
        static char cmd_msg[96];
        M_snprintf(cmd_msg, sizeof(cmd_msg), "CMD %s", cmd);
        players[consoleplayer].message = cmd_msg;
    }
    if (!strcmp(cmd, "ATTACK"))
    {
        T_StartCtrlTargeting();
        T_ConfirmCtrlTarget();
    }
    else if (!strcmp(cmd, "UNDO"))
    {
        // Backspace parity (Jimmy, 2026-10-23): remove the last
        // queued action and refund its TP. The standing rule is
        // that the driver supports whatever the keyboard supports;
        // UNDO was keyboard-only until now.
        T_DoUndoQueue();
    }
    else if (!strcmp(cmd, "END"))
        T_DoEndTurn();
    else if (!strncmp(cmd, "WALK", 4) && (cmd[4] == '\0' || cmd[4] == ' '))
    {
        // Persistent walk: keep moving forward until enemy nearby,
        // loot nearby, combat starts, or blocked. The worker sends WALK
        // once; the game handles the rest. A manual WALK cancels GOTO.
        // (Also cancels any active walk and restarts it.)
        // WALK <angle>: turn by that relative angle first
        // (-90..+90 degrees; negative turns left, positive turns
        // right), then walk forward a small fixed distance instead
        // of until a stop condition (Jimmy, 2026-10-23).
        int deg;
        if (sscanf(cmd + 4, "%d", &deg) == 1)
            T_WalkStartStep(deg);
        else if (cmd[4] == '\0')
            T_WalkStart();
        else
            printf("[WALK] usage: WALK [angle_degrees]\n");
    }
    else if (!strcmp(cmd, "USE"))
    {
        // Use the line in front of the player (door, switch) — the
        // same P_UseLines the spacebar and the TA_USE turn action run.
        // Explore mode only: turn mode already has USE as a queued
        // action, and firing it immediately there would bypass the
        // queue. This is the farm's door opener (added 2026-10-20 at
        // Jimmy's order; the driver had no USE handler before, so
        // workers could never open doors in explore mode).
        player_t *pl = &players[consoleplayer];
        if (T_InExplore() && pl->mo != NULL && pl->health > 0)
        {
            P_UseLines(pl);
            printf("[USE] used line in front of player\n");
        }
        else
            printf("[USE] ignored (explore mode only)\n");
    }
    else if (!strcmp(cmd, "WAIT"))
    {
        // Hold all later commands for one fixed beat while the
        // world keeps ticking (Jimmy, 2026-10-22): doors open over
        // seconds and don't stay open, so crossing one is a batched
        // sequence — USE, WAIT, GOTO — with no vision round-trip in
        // between. First encounter with a door: USE, WAIT, SCREENSHOT
        // to confirm by vision that it opened. No argument: the beat
        // is fixed (no scalar input); send WAIT again to wait longer.
        cmd_wait_tics += CMD_WAIT_TICS;
        if (cmd_wait_tics > CMD_WAIT_MAX)
            cmd_wait_tics = CMD_WAIT_MAX;
        printf("[WAIT] holding %d tics\n", cmd_wait_tics);
    }
    else if (!strncmp(cmd, "GOTO", 4))
    {
        // GOTO x y: move to ABSOLUTE map coordinates (explore mode
        // only) -- Jimmy, 2026-10-22: no facing-relative form. The
        // coordinates an agent may use are the ones the game itself
        // stamps in the log (its own position trail, corpse and loot
        // positions it has seen stamped). The game pathfinds there
        // and walks the path over multiple tics. A new GOTO overrides.
        player_t *pl = &players[consoleplayer];
        if (pl->mo != NULL && T_InExplore())
        {
            int x, y;
            if (sscanf(cmd + 4, "%d %d", &x, &y) == 2)
            {
                T_GotoStart((fixed_t)x * FRACUNIT, (fixed_t)y * FRACUNIT);
            }
            else
            {
                printf("[GOTO] usage: GOTO x y (absolute map coordinates)\n");
            }
        }
        else
        {
            printf("[GOTO] blocked: explore mode only\n");
        }
    }
    else if (!strcmp(cmd, "TURNL"))
    {
        player_t *pl = &players[consoleplayer];
        if (pl->mo != NULL && T_Active())
        {
            // Start a smooth turn: the facing steps 5 degrees per
            // tick in cmd_drain until the full 45 degrees lands.
            cmd_turn_tics_left = CMD_TURN_TICS;
            cmd_turn_dir = +1;
        }
    }
    else if (!strcmp(cmd, "TURNR"))
    {
        player_t *pl = &players[consoleplayer];
        if (pl->mo != NULL && T_Active())
        {
            cmd_turn_tics_left = CMD_TURN_TICS;
            cmd_turn_dir = -1;
        }
    }
    else if (!strcmp(cmd, "SURVEY"))
    {
        // Full 360-degree survey spin (Jimmy, 2026-10-26): reuse
        // the smooth-turn gate -- 36 tics at 10 degrees per tic,
        // left -- so the sweep is atomic (no FIFO round-trips
        // between steps). Explore mode only, like USE: a survey
        // spin is an explore observation, not a combat action.
        // v2: stamp the shared timestamp stem for this sweep's
        // 36 frame dumps (see the gate above).
        player_t *pl = &players[consoleplayer];
        if (T_InExplore() && pl->mo != NULL && pl->health > 0)
        {
            time_t now = time(NULL);
            struct tm *tm = localtime(&now);
            snprintf(cmd_survey_ts, sizeof(cmd_survey_ts),
                     "%04d%02d%02d_%02d%02d%02d",
                     tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                     tm->tm_hour, tm->tm_min, tm->tm_sec);
            cmd_survey_frame = 0;
            cmd_turn_is_survey = true;
            cmd_turn_tics_left = CMD_SURVEY_TICS;
            cmd_turn_dir = +1;
        }
        else
            printf("[SURVEY] ignored (explore mode only)\n");
    }
    else if (!strcmp(cmd, "COLLECT"))
    {
        extern boolean T_DoCollect(void);
        T_DoCollect();
    }
    else if (!strcmp(cmd, "MOVE_FWD"))
    {
        // Combat movement: step forward (1 TP). Works in combat.
        extern void T_DoMove(int dir);
        T_DoMove(0);
    }
    else if (!strcmp(cmd, "MOVE_BACK"))
    {
        // Combat movement: step back (1 TP). Works in combat.
        // Kiting: back away from melee attackers.
        extern void T_DoMove(int dir);
        T_DoMove(2);
    }
    else if (!strcmp(cmd, "STRAFE_L"))
    {
        // Combat movement: strafe left (1 TP). Works in combat.
        extern void T_DoMove(int dir);
        T_DoMove(3);
    }
    else if (!strcmp(cmd, "STRAFE_R"))
    {
        // Combat movement: strafe right (1 TP). Works in combat.
        extern void T_DoMove(int dir);
        T_DoMove(1);
    }
    else if (!strcmp(cmd, "RESPAWN"))
    {
        // Manual respawn after death (requires confirmation).
        // Player stays dead at 0% until this is sent.
        extern boolean P_DoCorpseRespawn(void);
        extern void *pending_corpse;
        if (P_DoCorpseRespawn())
            printf("[FARM] RESPAWN confirmed - player reborn\n");
        else
            printf("[FARM] RESPAWN failed - no dead player found (pending_corpse=%p, playerstate=%d)\n",
                   pending_corpse, players[consoleplayer].playerstate);
    }
    else if (!strcmp(cmd, "SCREENSHOT"))
    {
        extern void G_ScreenShot(void);
        G_ScreenShot();
    }
    else if (!strcmp(cmd, "CAPTURE"))
    {
        // Dedicated capture: 15s replay video + screenshot in one command.
        // Saves the replay FIRST, before the screenshot. G_ScreenShot draws
        // a "captured" message on screen; if we screenshot first, that text
        // lands in the replay buffer and appears in the video.
        extern void G_ScreenShot(void);
        extern int D_ReplaySaveMP4(const char *filename);
        extern void D_ReplayNextFilename(char *buf, size_t bufsz);
        {
            char filename[256];
            D_ReplayNextFilename(filename, sizeof(filename));
            if (D_ReplaySaveMP4(filename) == 0)
            {
                printf("[CAPTURE] video: %s\n", filename);
            }
        }
        G_ScreenShot();
        printf("[CAPTURE] screenshot taken\n");
    }
    else if (!strcmp(cmd, "INVENTORY"))
    {
        extern void D_UIOpen(void);
        extern void D_UIClose(void);
        extern boolean D_UIIsOpen(void);
        if (D_UIIsOpen())
            D_UIClose();
        else
            D_UIOpen();
    }
    else if (!strcmp(cmd, "KEYS"))
    {
        // Log which keycards the player possesses (for door/key logic).
        player_t *pl = &players[consoleplayer];
        printf("[KEYS] red=%d blue=%d yellow=%d\n",
               pl->cards[it_redcard] || pl->cards[it_redskull],
               pl->cards[it_bluecard] || pl->cards[it_blueskull],
               pl->cards[it_yellowcard] || pl->cards[it_yellowskull]);
    }
    else if (!strcmp(cmd, "SECTORS"))
    {
        // Log sector traversal progress: current sector, visited count,
        // total. The visited set is tracked continuously by
        // T_TrackTick (the old poll-time static array only marked the
        // sector the player stood in when SECTORS was queried, so the
        // count barely moved and read as broken).
        player_t *pl = &players[consoleplayer];
        if (pl->mo != NULL && pl->mo->subsector != NULL)
        {
            int cur = pl->mo->subsector->sector - sectors;
            printf("[SECTORS] current=%d visited=%d total=%d\n",
                   cur, T_VisitedCount(), numsectors);
        }
    }
    else if (!strncmp(cmd, "EQUIP ", 6))
    {
        // EQUIP <backpack_index> <slot>: equip item from backpack to paperdoll.
        // Slot: 0=HELM,1=ARMOR,2=WEAPON,3=SHIELD,4=RING1,5=RING2,6=AMULET,7=BOOTS,8=GLOVES,9=BELT,10=FOCUS
        int bpi, slot;
        player_t *pl = &players[consoleplayer];
        if (sscanf(cmd + 6, "%d %d", &bpi, &slot) == 2
            && bpi >= 0 && bpi < 40 && slot >= 0 && slot <= 10)
        {
            int id = pl->diablo_backpack[bpi];
            if (id != -1)
            {
                extern void D_BackpackRemoveAt(struct player_s *pl, int bpi);
                extern void D_RecalcStats(struct player_s *player);
                extern boolean D_BackpackAdd(struct player_s *pl, int tier, int idx);
                int old = pl->diablo_equipped[slot];
                int new_dur = pl->diablo_backpack_dur[bpi];
                int old_dur = pl->diablo_equipped_dur[slot];
                // Simple validation: check item's slot matches (or allow swap)
                pl->diablo_equipped[slot] = id;
                pl->diablo_equipped_dur[slot] = new_dur;
                D_BackpackRemoveAt((struct player_s *)pl, bpi);
                // If there was an old item, put it back in backpack
                if (old != -1)
                {
                    // D_BackpackAdd needs tier/idx; use direct placement instead
                    int i;
                    for (i = 0; i < 40; i++)
                    {
                        if (pl->diablo_backpack[i] == -1)
                        {
                            pl->diablo_backpack[i] = old;
                            pl->diablo_backpack_dur[i] = old_dur;
                            break;
                        }
                    }
                }
                D_RecalcStats((struct player_s *)pl);
                printf("[FARM] EQUIPPED backpack[%d] -> slot[%d]\n", bpi, slot);
            }
        }
    }
    else if (!strncmp(cmd, "UNEQUIP ", 8))
    {
        // UNEQUIP <slot>: move equipped item back to backpack.
        int slot;
        player_t *pl = &players[consoleplayer];
        if (sscanf(cmd + 8, "%d", &slot) == 1 && slot >= 0 && slot <= 10)
        {
            int id = pl->diablo_equipped[slot];
            if (id != -1)
            {
                extern void D_RecalcStats(struct player_s *player);
                int i;
                for (i = 0; i < 40; i++)
                {
                    if (pl->diablo_backpack[i] == -1)
                    {
                        pl->diablo_backpack[i] = id;
                        pl->diablo_backpack_dur[i] =
                            pl->diablo_equipped_dur[slot];
                        pl->diablo_equipped[slot] = -1;
                        pl->diablo_equipped_dur[slot] = 0;
                        D_RecalcStats((struct player_s *)pl);
                        printf("[FARM] UNEQUIPPED slot[%d] -> backpack[%d]\n", slot, i);
                        break;
                    }
                }
            }
        }
    }
    else if (!strncmp(cmd, "SALVAGE ", 8))
    {
        // SALVAGE <backpack_index>: turn backpack item into essence.
        int bpi;
        player_t *pl = &players[consoleplayer];
        if (sscanf(cmd + 8, "%d", &bpi) == 1 && bpi >= 0 && bpi < 40)
        {
            int id = pl->diablo_backpack[bpi];
            if (id != -1)
            {
                extern void D_BackpackRemoveAt(struct player_s *pl, int bpi);
                int tier = ((id >> 8) & 0xff); // D_ITEMTIER macro
                int essence = 1;
                switch (tier)
                {
                  case 1: essence = 1; break;  // TIER_NORMAL
                  case 2: essence = 3; break;  // TIER_MAGIC
                  case 3: essence = 8; break;  // TIER_RARE
                  case 4: essence = 15; break; // TIER_SET
                  case 5: essence = 25; break; // TIER_UNIQUE
                }
                D_BackpackRemoveAt((struct player_s *)pl, bpi);
                pl->diablo_essence += essence;
                printf("[ESSENCE] salvaged backpack[%d] tier %d -> +%d (total %d)\n",
                       bpi, tier, essence, pl->diablo_essence);
            }
        }
    }
    else if (!strcmp(cmd, "VERSION"))
    {
        // Print the git commit hash this binary was built from.
        // Workers compare with `git rev-parse HEAD` to detect stale binaries.
        printf("[VERSION] " GIT_COMMIT_HASH "\n");
    }
    else if (!strcmp(cmd, "REPLAY"))
    {
        // Dump the last 15 seconds of gameplay to MP4 (MJPEG).
        extern int D_ReplaySaveMP4(const char *filename);
        extern int D_ReplayFrameCount(void);
        extern void D_ReplayNextFilename(char *buf, size_t bufsz);
        char filename[256];
        // Save to the current working directory (doomshots/)
        D_ReplayNextFilename(filename, sizeof(filename));
        printf("[REPLAY] capturing %d frames...\n", D_ReplayFrameCount());
        if (D_ReplaySaveMP4(filename) == 0)
            printf("[REPLAY] saved to %s\n", filename);
        else
            printf("[REPLAY] failed\n");
    }
    else if (!strncmp(cmd, "SAVE", 4)
             && (cmd[4] == '\0' || cmd[4] == ' '))
    {
        // Driver save -- these handlers did not exist before, so the
        // farm's SAVE 0 was silently ignored. Slot 0 is the farm's
        // autosave: D_AutoSaveGame writes progress_auto1.dat plus
        // doomsav0.dsg, the same save loot pickups and combat entry
        // already trigger. Slots 3-5 are plain Doom savegames, like
        // the menu's manual slots. Slots 1-2 have no writer: the
        // autosave always uses slot 0 (no rotation, by design).
        extern void D_AutoSaveGame(void);
        extern void G_DoSaveGameSlot(int slot, const char *desc);
        int n = 0;
        if (sscanf(cmd + 4, "%d", &n) != 1)
            n = 0;
        if (n == 0)
        {
            D_AutoSaveGame();
            printf("[SAVE] slot 0 saved (progress_auto1.dat + doomsav0.dsg)\n");
        }
        else if (n >= 3 && n <= 5 && gamestate == GS_LEVEL)
        {
            G_DoSaveGameSlot(n, "DRIVER");
            printf("[SAVE] Doom savegame written to slot %d\n", n);
        }
        else
        {
            printf("[SAVE] slot %d not writable (0 = autosave, 3-5 = manual)\n",
                   n);
        }
    }
    else if (!strncmp(cmd, "LOAD", 4)
             && (cmd[4] == '\0' || cmd[4] == ' '))
    {
        // Mirror of the load menu (M_LoadSelect): auto slots 0-2
        // restore Diablo progress + the Doom savegame, manual slots
        // 3-5 load the plain Doom savegame.
        extern void D_ProgressLoadAutoSlot(int auto_slot);
        extern char *P_SaveGameFile(int slot);
        extern void G_LoadGame(char *name);
        extern void D_ReplayRequestRestartOnLoad(void);
        int n = 0;
        if (sscanf(cmd + 4, "%d", &n) != 1)
            n = 0;
        if (n >= 0 && n <= 2)
        {
            printf("[LOAD] auto slot %d\n", n + 1);
            D_ReplayRequestRestartOnLoad();
            D_ProgressLoadAutoSlot(n + 1);
        }
        else if (n >= 3 && n <= 5)
        {
            printf("[LOAD] manual slot %d\n", n);
            D_ReplayRequestRestartOnLoad();
            G_LoadGame(P_SaveGameFile(n));
        }
        else
        {
            printf("[LOAD] slot %d out of range (0-5)\n", n);
        }
    }
    else if (!strcmp(cmd, "QUIT") || !strncmp(cmd, "QUIT ", 5))
    {
        // Save + clean shutdown from inside the game (Jimmy's order,
        // replacing the pkill -9 the farm used to rely on: SIGKILL
        // throws away the buffered log and can lose state, while a
        // clean exit flushes both). "QUIT" saves autosave slot 0;
        // "QUIT n" (3-5) saves manual slot n instead.
        //
        // show_endoom must be off before I_Quit: the ENDOOM exit
        // screen waits for a keypress (I_Endoom loops on TXT_GetChar),
        // which a driver-run game never gets -- I_Quit would hang in
        // its exit chain forever. A human quitting from the menu
        // still sees ENDOOM; only this driver quit skips it.
        extern void D_AutoSaveGame(void);
        extern void G_DoSaveGameSlot(int slot, const char *desc);
        extern void I_Quit(void);
        extern int show_endoom;
        int n = 0;
        if (cmd[4] == ' ' && sscanf(cmd + 5, "%d", &n) != 1)
            n = 0;
        printf("[QUIT] saving slot %d and shutting down\n", n);
        if (n >= 3 && n <= 5 && gamestate == GS_LEVEL)
            G_DoSaveGameSlot(n, "DRIVER QUIT");
        else
            D_AutoSaveGame();
        printf("[QUIT] save complete, goodbye\n");
        fflush(NULL);
        // Async screenshots: drain the PNG queue before exiting,
        // so a final screenshot (or pending survey frames) queued
        // just before QUIT still lands on disk.
        {
            extern void V_ScreenshotFlush(void);
            V_ScreenshotFlush();
        }
        show_endoom = 0;
        I_Quit();
    }
    else
    {
        // No handler matched. Say so in the log: a command that was
        // delivered but matches nothing must never vanish silently --
        // the driver deserves to learn its command went nowhere.
        printf("[CMD] unknown command (no handler): %s\n", cmd);
    }
}

void T_Ticker(void)
{
    int i, n;

    // VM automation driver: drain the FIFO first, before any early-return.
    // If T_Active() is false we still want to process VERSION/LOAD/etc.
    cmd_drain(cmd_dispatch);

    if (!T_Active())
        return;

    // Continuous visited-sector tracking (both modes: presence is
    // presence). Feeds SECTORS.
    T_TrackTick();

    // Kill banner counts down in real time, independent of turn state.
    T_TickKillBanner();

    // Floating combat text (MISS, etc.) ticks down.
    T_TickFloatText();

    // World-space damage/MISS numbers (old target-marker system) tick down.
    {
        extern int t_last_hit_tics;
        extern int t_last_miss_tics;
        if (t_last_hit_tics > 0)
            t_last_hit_tics--;
        if (t_last_miss_tics > 0)
            t_last_miss_tics--;
    }

    // Time-based cadence (charges, cooldowns, heat): independent of
    // rounds, but frozen during the planning phase, same as the ammo
    // trickle -- nothing about the player's weapons improves while
    // they plan (Jimmy, 2026-10-22).
    if (turnctrl.state != TS_PLANNING && turnctrl.state != TS_TARGETING &&
        turnctrl.state != TS_CONFIRM)
    {
        T_TickCadence();
    }

    // VM progress persistence: write the save every 120 tics.
    {
        extern void D_ProgressTick(void);
        D_ProgressTick();
    }

    // Deferred test-harness startup: arena + script on first level tick.
    // Runs in both explore and combat modes.
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

    // EXPLORE mode: free real-time movement. The normal Doom ticker
    // runs the world; P_MobjThinker freezes enemies while exploring.
    // Combat triggers when an enemy AI acquires the player as its
    // target (T_ExploreTick); the player seeing enemies only drives
    // the HUD markers.
    if (turnctrl.state == TS_EXPLORE)
    {
        T_ExploreTick();
        return;
    }

    // Belt and braces: no real-time input may leak into pulses.
    for (i = 0; i < MAXPLAYERS; i++)
        if (playeringame[i])
            memset(&players[i].cmd, 0, sizeof(ticcmd_t));

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

// ------------------------------------------------------------------
// Turn-chrome tint: the turn-based HUD draws in warm gold (STCFN red
// ramp remapped to the Doom gold ramp) to signal deviation from classic
// Doom. Unaffordable actions draw dim/desaturated gold — still the turn
// tint, never classic grey. Diablo UI (inventory, status bar) stays red
// and is untouched.
//
// The video API has no translated-patch draw, so at first HUD draw we
// clone each HU font glyph and remap its pixel runs in place.
// ------------------------------------------------------------------

static patch_t *t_font_gold[HU_FONTSIZE];
static patch_t *t_font_golddim[HU_FONTSIZE];
static patch_t *t_font_goldhi[HU_FONTSIZE];
static boolean t_fonts_ready = false;

static void T_BuildTintTables(byte *gold, byte *dim, byte *hi)
{
    int i;
    for (i = 0; i < 256; i++)
        gold[i] = dim[i] = hi[i] = (byte)i;
    // STCFN red ramp (176-191) -> Doom gold ramp (160-167).
    for (i = 0; i < 16; i++)
        gold[176 + i] = (byte)(160 + i / 2);
    // Disabled: dim/desaturated -> dark end of the gold ramp (163-167).
    for (i = 0; i < 16; i++)
        dim[176 + i] = (byte)(163 + (i * 5) / 16);
    // Highlight: brightest end of the gold ramp (160-161). Used for the
    // selected target and the END TURN exit row — still turn gold, just
    // the hottest tier, so it pops against both gold and dim gold.
    for (i = 0; i < 16; i++)
        hi[176 + i] = (byte)(160 + i / 8);
    // Pink/white highlight ramp (168-175) -> bright gold / mid gold.
    for (i = 168; i <= 175; i++)
    {
        gold[i] = (byte)(160 + (i - 168) / 4);
        dim[i] = (byte)(164 + (i - 168) / 4);
        hi[i] = (byte)160;
    }
}

// Remap every pixel run of a patch through a 256-entry table.
static void T_RemapPatch(patch_t *patch, const byte *table)
{
    int x, w = SHORT(patch->width);
    for (x = 0; x < w; x++)
    {
        byte *p = (byte *)patch + LONG(patch->columnofs[x]);
        for (;;)
        {
            int len, k;
            byte *px;
            if (p[0] == 0xFF)
                break;
            len = p[1];
            px = p + 3;
            for (k = 0; k < len; k++)
                px[k] = table[px[k]];
            p = px + len + 1;
        }
    }
}

static void T_TintFonts(void)
{
    byte gold[256], dim[256], hi[256];
    char name[16];
    int i;

    if (t_fonts_ready)
        return;
    T_BuildTintTables(gold, dim, hi);
    for (i = 0; i < HU_FONTSIZE; i++)
    {
        int lump, len;
        patch_t *src, *g, *d, *h;
        M_snprintf(name, sizeof(name), "STCFN%.3d", HU_FONTSTART + i);
        lump = W_GetNumForName(name);
        len = W_LumpLength(lump);
        src = (patch_t *)W_CacheLumpNum(lump, PU_STATIC);
        g = (patch_t *)Z_Malloc(len, PU_STATIC, NULL);
        d = (patch_t *)Z_Malloc(len, PU_STATIC, NULL);
        h = (patch_t *)Z_Malloc(len, PU_STATIC, NULL);
        memcpy(g, src, len);
        memcpy(d, src, len);
        memcpy(h, src, len);
        T_RemapPatch(g, gold);
        T_RemapPatch(d, dim);
        T_RemapPatch(h, hi);
        t_font_gold[i] = g;
        t_font_golddim[i] = d;
        t_font_goldhi[i] = h;
    }
    t_fonts_ready = true;
}

static void T_DrawTextF(int x, int y, const char *s, patch_t **font)
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
            patch_t *p = font[c - '!'];
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

static void T_DrawText(int x, int y, const char *s)
{
    T_DrawTextF(x, y, s, t_font_gold);
}

static void T_DrawTextDim(int x, int y, const char *s)
{
    T_DrawTextF(x, y, s, t_font_golddim);
}

static void T_DrawTextHi(int x, int y, const char *s)
{
    T_DrawTextF(x, y, s, t_font_goldhi);
}

static void T_DrawTextCenteredHi(int y, const char *s)
{
    T_DrawTextHi((320 - T_TextWidth(s)) / 2, y, s);
}

static void T_DrawTextCentered(int y, const char *s)
{
    T_DrawText((320 - T_TextWidth(s)) / 2, y, s);
}

// Project a target's head to 320x200 screen space. False if behind.
// Matches the engine's own R_ProjectSprite math (r_things.c): tx is
// tr_x*sin - tr_y*cos (NOT its negation — the mirror bug), and the
// vertical center is the real centery (viewheight/2 = 84 with the
// status bar), not the hardcoded 100.
static boolean T_ProjectTarget(mobj_t *mo, int *sx, int *sy)
{
    double dx, dy, dz, tz, tx;
    double va, rad;

    if (mo == NULL || sx == NULL || sy == NULL)
        return false;
    dx = (double)(mo->x - viewx) / FRACUNIT;
    dy = (double)(mo->y - viewy) / FRACUNIT;
    dz = (double)((mo->z + mo->height) - viewz) / FRACUNIT;
    va = (double)viewangle * 360.0 / 4294967296.0;
    rad = va * 3.141592653589793 / 180.0;
    tz = dx * cos(rad) + dy * sin(rad);
    tx = dx * sin(rad) - dy * cos(rad);
    if (tz < 16.0)
        return false;
    *sx = (int)(160.0 + (tx / tz) * 160.0);
    *sy = (int)((double)centery - (dz / tz) * 160.0) - 8;
    return true;
}

// Floating combat text (e.g. "MISS" above an enemy). Spawned by combat
// events, drawn at the target's projected head position, rises and fades.
#define T_MAXFLOAT 8
typedef struct {
    mobj_t *mo;          // target (may become NULL if died/removed)
    char text[16];
    int tics;            // remaining (35 = 1 sec)
} t_floattext_t;
static t_floattext_t t_floattexts[T_MAXFLOAT];
void T_TickFloatText(void);
void T_DrawFloatText(void);

void T_SpawnFloatText(mobj_t *mo, const char *text)
{
    int i, oldest = 0;
    if (mo == NULL || text == NULL)
        return;
    // Find free slot, or reuse oldest.
    for (i = 0; i < T_MAXFLOAT; i++)
    {
        if (t_floattexts[i].tics <= 0)
        {
            oldest = i;
            break;
        }
        if (t_floattexts[i].tics < t_floattexts[oldest].tics)
            oldest = i;
    }
    t_floattexts[oldest].mo = mo;
    M_snprintf(t_floattexts[oldest].text, sizeof(t_floattexts[oldest].text), "%s", text);
    t_floattexts[oldest].tics = 70; // 2 seconds (survives queue drain)
}

void T_TickFloatText(void)
{
    int i;
    for (i = 0; i < T_MAXFLOAT; i++)
    {
        if (t_floattexts[i].tics > 0)
            t_floattexts[i].tics--;
    }
}

void T_DrawFloatText(void)
{
    int i, sx, sy;
    for (i = 0; i < T_MAXFLOAT; i++)
    {
        if (t_floattexts[i].tics <= 0)
            continue;
        if (t_floattexts[i].mo == NULL)
            continue;
        // Skip if target died and was removed (health check).
        // Note: mobj pointer may dangle; we check health but the
        // pointer itself could be freed. For safety, we only draw
        // if tics are fresh (< 35, i.e. spawned this second).
        if (!T_ProjectTarget(t_floattexts[i].mo, &sx, &sy))
            continue;
        // Rise as it fades: offset up by (70 - tics) / 2 pixels.
        // tics starts at 70, ends at 0.
        sy -= (70 - t_floattexts[i].tics) / 2;
        T_DrawTextHi(sx - T_TextWidth(t_floattexts[i].text) / 2, sy,
                     t_floattexts[i].text);
    }
}

// Placeholder hit chance for the phase-3 preview (phase 4 derives the
// ------------------------------------------------------------------
// Target service (phase 3)
// ------------------------------------------------------------------

static mobj_t *t_targets[T_MAXTARGETS];
static int t_numtargets = 0;

// Test/script tooling: mobj of the current selection, or NULL.
mobj_t *T_SelectedMobj(void)
{
    if (turnctrl.selected_target < 0 ||
        turnctrl.selected_target >= t_numtargets)
        return NULL;
    return t_targets[turnctrl.selected_target];
}

void T_RefreshTargets(void)
{
    player_t *player = &players[consoleplayer];
    thinker_t *th;
    t_numtargets = 0;
    if (!T_Active() || player->mo == NULL)
        return;

    for (th = thinkercap.next; th != &thinkercap; th = th->next)
    {
        mobj_t *mo;
        if (th->function.acp1 != (actionf_p1)P_MobjThinker)
            continue;
        mo = (mobj_t *)th;
        if (mo == player->mo)
            continue;
        // Shootable, living, countable enemies only.
        if (!(mo->flags & MF_SHOOTABLE))
            continue;
        if (mo->health <= 0)
            continue;
        if (mo->player != NULL)
            continue; // never target players
        if (!P_CheckSight(player->mo, mo))
            continue;
        // Full cover blockage (Phase 6): all three traces blocked means
        // the enemy is not a legal attack target.
        if (T_CoverBlocked(player, mo) >= 3)
            continue;
        if (t_numtargets < T_MAXTARGETS)
            t_targets[t_numtargets++] = mo;
    }
}

const char *T_TargetName(mobj_t *mo)
{
    if (mo == NULL)
        return "?";
    switch (mo->type)
    {
      case MT_POSSESSED: return "ZOMBIE";
      case MT_SHOTGUY:   return "SHOTGUN";
      case MT_CHAINGUY:  return "CHAING";
      case MT_TROOP:     return "IMP";
      case MT_SERGEANT:  return "DEMON";
      case MT_SHADOWS:   return "SPECTRE";
      case MT_HEAD:      return "CACO";
      case MT_BRUISER:   return "BARON";
      case MT_KNIGHT:    return "KNIGHT";
      case MT_SKULL:     return "SOUL";
      case MT_SPIDER:    return "SPIDER";
      case MT_BABY:      return "ARACH";
      case MT_CYBORG:    return "CYBER";
      case MT_PAIN:      return "PAIN";
      default:           return "FOE";
    }
}

// Action availability for the HUD: an action is enabled when it can be
// afforded right now. ATTACK additionally needs a ready (off-cooldown)
// kit. UNDO/CLEAR need a non-empty queue. END TURN costs 0 TP, so it is
// always enabled — the player always has a legal move.
static boolean T_ActionEnabled(turnaction_t action)
{
    if (action == TA_ATTACK)
        return T_CanAfford(TA_ATTACK)
            && T_KitCanFire(players[consoleplayer].readyweapon);
    if (action == TA_UNDO)
        return turnctrl.queue_len > 0;
    return T_CanAfford(action);
}

// One action row: key, name, TP cost. Unaffordable rows draw dim gold.
// Rows scrolled off-screen (y < 8) are skipped but still advance y.
static void T_DrawActionRow(int x, int *y, const char *key, const char *name,
                            turnaction_t action, const char *coststr)
{
    char line[64];
    if (*y >= 8)
    {
        M_snprintf(line, sizeof(line), "%s %-8s %s", key, name, coststr);
        if (T_ActionEnabled(action))
            T_DrawText(x, *y, line);
        else
            T_DrawTextDim(x, *y, line);
    }
    *y += 9;
}

// Gate state for the target-preview line: " CD2", " CHG 1/2",
// " AMMO!", or "" when the kit can fire. Plasma needs no entry of
// its own: its counter is the AMMO readout, and a full inverted
// magazine reports through the AMMO! branch like any empty one.
static void T_KitGateStr(weapontype_t w, char *buf, int bufsz)
{
    const t_kitdef_t *kit = T_KitForWeapon(w);
    int cd = T_KitCooldown(w);
    buf[0] = '\0';
    if (cd > 0)
        M_snprintf(buf, bufsz, " CD%d", cd);
    else if (kit->max_charges > 0)
        M_snprintf(buf, bufsz, " CHG %d/%d", T_KitCharges(w),
                   T_KitMaxCharges(w));
    else if (!T_HasAmmoForKit(w))
        M_snprintf(buf, bufsz, " AMMO!");
}

// Planning-time action list with per-action TP costs. Disabled
// (unaffordable) actions render dim gold and their keypresses are
// refused with a "Need N TP" message — they are never selectable.
// END TURN is always the final row, separated as the exit: a dim
// divider above it and a bright-gold bracketed row so it reads as
// the way out, not just another action.
static void T_DrawActionList(int start_y)
{
    char cost[16];
    int x = 8, y = start_y;
    player_t *pl = &players[consoleplayer];

    T_DrawActionRow(x, &y, "WASD", "MOVE/STRF", TA_MOVE_N, "1TP");
    T_DrawActionRow(x, &y, "<>", "TURN", TA_TURN_L, "FREE");
    T_DrawActionRow(x, &y, "SPC", "USE", TA_USE, "FREE");
    if (!T_KitCanFire(pl->readyweapon))
    {
        // Gated: show the specific gate, not a generic "CD".
        const char *reason = T_KitDenyReason(pl->readyweapon);
        const char *shortr = "CD";
        if (reason != NULL)
        {
            if (!strcmp(reason, "OVERHEATED"))
                shortr = "HEAT";
            else if (!strcmp(reason, "NO CHARGES"))
                shortr = "CHG";
            else if (!strcmp(reason, "NO MANA"))
                shortr = "MANA";
        }
        T_DrawActionRow(x, &y, "CTRL", "TARGET", TA_ATTACK, shortr);
    }
    else
    {
        M_snprintf(cost, sizeof(cost), "%dTP", T_CostFor(TA_ATTACK));
        T_DrawActionRow(x, &y, "CTRL", "TARGET", TA_ATTACK, cost);
    }
    T_DrawActionRow(x, &y, "BKSP", "UNDO", TA_UNDO, "FREE");
    // The exit: END TURN as a simple action row, consistent with the rest.
    // (C inventory is hidden from the menu but still works during combat.)
    T_DrawActionRow(x, &y, "ENTER", "END TURN", TA_END_TURN, "0TP");
}

// Queued-action panel: FIFO order with per-action costs and the total
// reserved TP. Drawn below the action list while planning (and while
// targeting, so the queue stays visible). Hidden when empty.
static int T_DrawQueuePanelAt(int start_y)
{
    char line[64], name[48];
    int x = 8, y = start_y, i, shown;

    if (turnctrl.queue_len == 0)
        return y;
    // Show the queue during execution so commands visibly pop off as
    // they run.
    if (y >= 8)
        T_DrawText(x, y, "QUEUE");
    y += 9;
    // Show ALL queued commands (no collapsing). The unified scrolling
    // in T_DrawPlanningUI ensures the bottom-most are visible.
    for (i = 0; i < turnctrl.queue_len; i++)
    {
        t_queueentry_t *e = &turnctrl.queue[i];
        T_QueueEntryName(e, name, sizeof(name));
        M_snprintf(line, sizeof(line), "%d %s %dTP", i + 1, name, e->cost);
        if (y >= 8)
            T_DrawText(x, y, line);
        y += 9;
    }
    M_snprintf(line, sizeof(line), "%d ACTS %dTP RSVD",
               turnctrl.queue_len, turnctrl.queue_tp);
    if (y >= 8)
        T_DrawTextHi(x, y, line);
    y += 9;
    return y + 9; // gap before the menu
}

// Unified planning UI: menu and queue act as one scrolling text area.
// Only the bottom-most lines are visible; older content scrolls off
// the top as new commands are queued. No collapsing.
static void T_DrawPlanningUI(void)
{
    // Menu is ~8 rows. Queue is 1 (header) + N (items) + 1 (total) + 1 (gap).
    // Visible area: y=18 to y=160 = ~15 rows. Scroll so bottom is visible.
    int menu_rows = 8;
    int queue_rows = 0;
    int total_rows, max_rows, scroll_rows, start_y;

    if (turnctrl.queue_len > 0)
    {
        queue_rows = 1 + turnctrl.queue_len + 1 + 1;
    }

    total_rows = menu_rows + queue_rows;
    max_rows = 15; // (160 - 18) / 9
    scroll_rows = total_rows > max_rows ? total_rows - max_rows : 0;
    start_y = 18 - scroll_rows * 9;

    // Draw menu at the scrolled position (it clips when off-screen).
    T_DrawActionList(start_y);

    // Draw queue below the menu.
    T_DrawQueuePanelAt(start_y + menu_rows * 9);
}

void T_DrawHUD(void)
{
    char line[96];
    int i;
    player_t *player = &players[consoleplayer];

    if (!T_Active())
        return;
    // Hide turn UI when the player is dead.
    if (player->mo == NULL || player->mo->health <= 0)
        return;
    T_TintFonts();

    // EXPLORE mode: minimal header, no turn UI. Combat UI appears only
    // once an enemy is spotted. The controls hint shows only at the
    // start of a level and fades out after 5 seconds.
    if (turnctrl.state == TS_EXPLORE)
    {
        extern int gametic;
        extern int levelstarttic;
        int elapsed = gametic - levelstarttic;
        if (elapsed < 175) // 5 seconds at 35 tics/sec
        {
            if (elapsed < 105) // first 3 seconds: bright
                T_DrawTextCenteredHi(2, "EXPLORE - WASD MOVE - ARROWS TURN");
            else // last 2 seconds: dim (fade out)
                T_DrawTextCentered(2, "EXPLORE - WASD MOVE - ARROWS TURN");
        }
        return;
    }

    // TP readout shows reserved (queued) TP separately: "TP 4/10"
    // is spendable now, "(6 IN QUEUE)" is already committed.
    if (turnctrl.queue_len > 0)
        M_snprintf(line, sizeof(line),
                   "TURN MODE - ROUND %d - TP %d/%d (%d IN QUEUE)",
                   turnctrl.round, turnctrl.tp, turnctrl.tp_max,
                   turnctrl.queue_tp);
    else
        M_snprintf(line, sizeof(line), "TURN MODE - ROUND %d - TP %d/%d",
                   turnctrl.round, turnctrl.tp, turnctrl.tp_max);
    T_DrawTextCentered(2, line);

    if (turnctrl.state == TS_REACTION)
        T_DrawTextCentered(12, "ENEMY PHASE...");
    else if (turnctrl.state == TS_PULSE)
    {
        T_DrawTextCentered(12, "RESOLVING");
        // Keep the queue visible during the progress phase so commands
        // visibly pop off one by one as they execute.
        T_DrawQueuePanelAt(24);
    }
    else if (turnctrl.state == TS_PLANNING)
        T_DrawPlanningUI();
    else
    {
        // TARGETING / CONFIRM: the always-legal outs stay visible.
        // The action list keeps drawing (left column): it repaints the
        // border-zone pixels every frame and keeps costs/disabled states
        // visible while targeting.
        T_DrawTextCentered(12, "T END TURN  BKSP UNDO  Z CLEAR  <> TURN  ESC BACK");
        T_DrawPlanningUI();
    }

    // Target markers removed: no floating numbers on enemy heads.
    // (Targeting is via Ctrl hold/release; the target list on the
    // right side shows options during targeting.)

    // Floating combat text (MISS, damage numbers, etc.).
    T_DrawFloatText();

    // Last-hit damage number: projected above the target's head in
    // world space, using the old target-marker projection system.
    // Tracks the target as it moves.
    {
        extern int t_last_damage;
        extern mobj_t *t_last_hit_mobj;
        extern int t_last_hit_tics;
        if (t_last_hit_mobj != NULL && t_last_damage > 0 && t_last_hit_tics > 0)
        {
            int sx, sy;
            char line[16];
            if (T_ProjectTarget(t_last_hit_mobj, &sx, &sy))
            {
                M_snprintf(line, sizeof(line), "%d", t_last_damage);
                // Rise as it fades.
                sy -= (25 - t_last_hit_tics) / 2;
                T_DrawTextHi(sx - T_TextWidth(line) / 2, sy, line);
            }
        }
        // MISS text: same system.
        {
            extern mobj_t *t_last_miss_mobj;
            extern int t_last_miss_tics;
            if (t_last_miss_mobj != NULL && t_last_miss_tics > 0)
            {
                int sx, sy;
                if (T_ProjectTarget(t_last_miss_mobj, &sx, &sy))
                {
                    sy -= (25 - t_last_miss_tics) / 2;
                    T_DrawTextHi(sx - T_TextWidth("MISS") / 2, sy, "MISS");
                }
            }
        }
    }

    // Target list (right side) and preview (bottom) while targeting.
    if (turnctrl.state == TS_TARGETING || turnctrl.state == TS_CONFIRM)
    {
        int y = 44;
        T_DrawText(224, y, "TARGETS");
        y += 10;
        for (i = 0; i < t_numtargets && i < 9; i++)
        {
            mobj_t *mo = t_targets[i];
            if (mo == NULL)
                continue;
            if (i == turnctrl.selected_target)
            {
                // Selected entry: bright highlight, chevron brackets,
                // cursor-shifted left so the shape change is visible.
                M_snprintf(line, sizeof(line), ">[%d:%s %d]<",
                           i + 1, T_TargetName(mo), mo->health);
                T_DrawTextHi(216, y, line);
            }
            else
            {
                M_snprintf(line, sizeof(line), "%d:%s %d",
                           i + 1, T_TargetName(mo), mo->health);
                T_DrawTextDim(224, y, line);
            }
            y += 9;
        }
        // Preview panel for the selected target (above status bar).
        {
            mobj_t *mo = T_TargetMobj(turnctrl.selected_target);
            if (mo != NULL)
            {
                t_combatstats_t st;
                int dist, dmin, dmax;
                player_t *pl = &players[consoleplayer];
                T_DeriveStats(pl, &st);
                dist = P_AproxDistance(
                    mo->x - pl->mo->x,
                    mo->y - pl->mo->y) / FRACUNIT;
                T_DamageRange(pl, mo, &st, &dmin, &dmax);
                {
                    const t_kitdef_t *kit = T_KitForWeapon(pl->readyweapon);
                    char gate[24];
                    T_KitGateStr(pl->readyweapon, gate, sizeof(gate));
                    if (kit->splash_radius > 0)
                        M_snprintf(line, sizeof(line),
                                   "[%d]%s HP%d R%d H%d%% D%d-%d SPLASH %dTP%s",
                                   turnctrl.selected_target + 1,
                                   T_TargetName(mo), mo->health, dist,
                                   T_HitChance(pl, mo, &st),
                                   dmin, dmax,
                                   T_CostFor(TA_ATTACK),
                                   gate);
                    else
                        M_snprintf(line, sizeof(line),
                                   "[%d]%s HP%d R%d H%d%% D%d-%d %dTP%s",
                                   turnctrl.selected_target + 1,
                                   T_TargetName(mo), mo->health, dist,
                                   T_HitChance(pl, mo, &st),
                                   dmin, dmax,
                                   T_CostFor(TA_ATTACK),
                                   gate);
                }
                T_DrawTextCenteredHi(148, line);
                if (turnctrl.state == TS_CONFIRM)
                    T_DrawTextCenteredHi(158,
                        "CONFIRM: F FIRE  ESC CANCEL");
            }
        }
    }

    // Kill banner draws last: on top of everything.
    T_DrawKillBanner();
}


int T_NumTargets(void)
{
    return t_numtargets;
}

// True if this mobj is a real combatant: has a melee or missile attack
// state. Shootable props with no attack (barrels) return false — they
// stay selectable as targets but never trigger combat markers/telegraphs.
boolean T_IsCombatant(mobj_t *mo)
{
    const mobjinfo_t *mi;
    if (mo == NULL)
        return false;
    mi = &mobjinfo[mo->type];
    return mi->meleestate != S_NULL || mi->missilestate != S_NULL;
}

// Number of current targets that are real combatants (excludes barrels).
int T_NumCombatants(void)
{
    int i, n = 0;
    for (i = 0; i < t_numtargets; i++)
        if (T_IsCombatant(t_targets[i]))
            n++;
    return n;
}

mobj_t *T_TargetMobj(int idx)
{
    if (idx < 0 || idx >= t_numtargets)
        return NULL;
    return t_targets[idx];
}

void T_ValidateSelection(void)
{
    // Drop the selection if the target died, left sight, or the list
    // shrank. Called after pulses and round changes.
    if (turnctrl.selected_target < 0
        || turnctrl.selected_target >= t_numtargets)
    {
        turnctrl.selected_target = -1;
        if (turnctrl.state == TS_TARGETING || turnctrl.state == TS_CONFIRM)
            turnctrl.state = TS_PLANNING;
        return;
    }
    {
        mobj_t *mo = t_targets[turnctrl.selected_target];
        if (mo == NULL || mo->health <= 0)
        {
            turnctrl.selected_target = -1;
            if (turnctrl.state == TS_TARGETING
                || turnctrl.state == TS_CONFIRM)
                turnctrl.state = TS_PLANNING;
        }
    }
}

// ------------------------------------------------------------------
// State dump (test harness)
// ------------------------------------------------------------------

void T_DumpState(const char *why)
{
    player_t *p = &players[consoleplayer];
    static const char *names[] = {
        "OFF", "EXPLORE", "PLANNING", "TARGETING", "CONFIRM",
        "PULSE", "REACTION", "ROUND_END"
    };
    const char *st = (turnctrl.state >= 0 && turnctrl.state <= TS_ROUND_END)
                     ? names[turnctrl.state] : "?";

    printf("[TURN] %-10s round=%d tp=%d/%d qlen=%d qtp=%d exec=%d state=%s sel=%d hp=%d pos=(%d,%d,%d) floor=%d ceil=%d angle=%u\n",
           why ? why : "-",
           turnctrl.round, turnctrl.tp, turnctrl.tp_max,
           turnctrl.queue_len, turnctrl.queue_tp, turnctrl.executing ? 1 : 0,
           st,
           turnctrl.selected_target,
           p->mo ? p->health : -1,
           p->mo ? p->mo->x >> FRACBITS : 0,
           p->mo ? p->mo->y >> FRACBITS : 0,
           p->mo ? p->mo->z >> FRACBITS : 0,
           p->mo ? p->mo->floorz >> FRACBITS : 0,
           p->mo ? p->mo->ceilingz >> FRACBITS : 0,
           p->mo ? (unsigned)(p->mo->angle >> 24) : 0);
    fflush(stdout);
}

// ------------------------------------------------------------------
// Phase 8: versioned standalone profile.
// ------------------------------------------------------------------

t_profile_t t_profile;

const char *T_ProfilePath(void)
{
    static char path[256];
    // Use the savegame directory if set, else current directory.
    // For the turn-based build, this is typically the working dir.
    M_snprintf(path, sizeof(path), "turn_profile_v%d.dat", T_PROFILE_VERSION);
    return path;
}

void T_ProfileInit(void)
{
    memset(&t_profile, 0, sizeof(t_profile));
    t_profile.version = T_PROFILE_VERSION;
    M_snprintf(t_profile.name, sizeof(t_profile.name), "HERO");
    t_profile.level = 1;
}

void T_ProfileLoad(void)
{
    FILE *f;
    t_profile_t tmp;
    const char *path = T_ProfilePath();
    f = fopen(path, "rb");
    if (!f)
    {
        // No profile yet; use defaults.
        T_ProfileInit();
        return;
    }
    if (fread(&tmp, sizeof(tmp), 1, f) != 1)
    {
        fclose(f);
        T_ProfileInit();
        return;
    }
    fclose(f);
    if (tmp.version != T_PROFILE_VERSION)
    {
        // Version mismatch; start fresh (future: migrate).
        printf("[TURN] profile version %d != %d; resetting.\n",
               tmp.version, T_PROFILE_VERSION);
        T_ProfileInit();
        return;
    }
    // Ensure name is NUL-terminated.
    tmp.name[T_PROFILE_NAME_LEN - 1] = '\0';
    t_profile = tmp;
    printf("[TURN] profile loaded: %s L%d XP%d\n",
           t_profile.name, t_profile.level, t_profile.xp);
}

void T_ProfileSave(void)
{
    const char *path = T_ProfilePath();
    char tmp_path[256];
    FILE *f;
    M_snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    f = fopen(tmp_path, "wb");
    if (!f)
    {
        printf("[TURN] profile save failed: cannot write %s\n", tmp_path);
        return;
    }
    t_profile.version = T_PROFILE_VERSION;
    if (fwrite(&t_profile, sizeof(t_profile), 1, f) != 1)
    {
        fclose(f);
        printf("[TURN] profile save failed: write error\n");
        return;
    }
    fclose(f);
    // Atomic: rename temp to final.
    if (rename(tmp_path, path) != 0)
        printf("[TURN] profile save failed: rename error\n");
    else
        printf("[TURN] profile saved: %s L%d\n", t_profile.name, t_profile.level);
}

void T_GainXP(int amount)
{
    // XP/leveling scrapped: progression is purely equipment-based.
    (void)amount;
    return;
}

void T_AddStatPoint(const char *stat)
{
    // Stat points scrapped: progression is purely equipment-based.
    (void)stat;
    printf("[TURN] stat points disabled (equipment-based progression)\n");
    return;
}

void T_CountKill(mobj_t *target)
{
    t_profile.lifetime_kills++;
    // XP/leveling scrapped: progression is purely equipment-based.
    // (t_profile.xp/level/stat_points retained in struct for save compat,
    // but no longer used.)
}

// ------------------------------------------------------------------
// Kill banner: the screenshot-able kill moment. Latched on the first
// kill of an attack; the ticker counts it down in real time and the
// HUD draws it big over the game view. The death beat (see T_ExecAttack)
// freezes living monsters for ~2s so the death animation plays out
// underneath the banner.
// ------------------------------------------------------------------
static int t_killbanner_tics;
static char t_killbanner_title[32];
static char t_killbanner_sub[64];

void T_KillBanner(const char *title, const char *sub)
{
    // Always latch the newest kill: T_ResolveKill already keeps the
    // first kill of a multi-kill attack, so a fresh attack replaces a
    // stale banner instead of showing the old text.
    M_snprintf(t_killbanner_title, sizeof(t_killbanner_title), "%s", title);
    M_snprintf(t_killbanner_sub, sizeof(t_killbanner_sub), "%s", sub);
    t_killbanner_tics = 90; // ~2.5s at 35 tics/sec
}

static void T_TickKillBanner(void);

static void T_TickKillBanner(void)
{
    if (t_killbanner_tics > 0)
        t_killbanner_tics--;
}

static void T_DrawKillBanner(void)
{
    if (t_killbanner_tics <= 0)
        return;
    T_DrawTextCenteredHi(70, t_killbanner_title);
    T_DrawTextCentered(82, t_killbanner_sub);
}

void T_CountDamage(int dmg)
{
    if (dmg > 0)
        t_profile.lifetime_damage += dmg;
}

void T_CountRound(void)
{
    t_profile.lifetime_rounds++;
}

