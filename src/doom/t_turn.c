//
// t_turn.c — Turn-based (XCOM) mode: central turn controller.
//
// Owns the state machine, the bounded simulation pulse, round/enemy
// phases, and the test harnesses. This fork is turn-based by default:
// G_Ticker calls T_Ticker instead of P_Ticker unconditionally.

#include <string.h>
#include <stdio.h>
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
// loop and telegraph use it earlier.
boolean T_IsCombatant(mobj_t *mo);
// T_TelegraphEnemies is defined below; T_BeginEnemyPhase calls it.
void T_TelegraphEnemies(void);

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
static void T_SpotPlayer(mobj_t *mo)
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
    printf("[TURN] detect: %s acquired the player\n", T_MobjShortName(mo));

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
        // non-deaf monsters with no sight check.
        if (!(mo->flags & MF_AMBUSH)
            && mo->subsector != NULL
            && mo->subsector->sector->soundtarget == player->mo)
        {
            T_SpotPlayer(mo);
            if (spotter == NULL)
                spotter = mo;
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

        T_SpotPlayer(mo);
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
    turnctrl.rng_seed = 0xC0FFEEu;
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
    turnctrl.rng_seed = 0xC0FFEEu;
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
    // Anchor the preview at combat entry.
    if (players[consoleplayer].mo != NULL)
    {
        turnctrl.turn_start_x = players[consoleplayer].mo->x;
        turnctrl.turn_start_y = players[consoleplayer].mo->y;
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
// Magazines replenish automatically by weapon type — AD from attack
// speed, AP from cooldowns/charges (see T_KitRegenAmmo) — so guns never
// run dry in sustained use; pickups and Mana Potions top magazines up.
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

    P_UpdateSpecials();
    P_RespawnSpecials();
    leveltime++;

    // Gradual ammo trickle. The rate is tied to the weapon's cooldown or
    // attack speed (see T_KitTrickleAmmo). Runs when the game is unfrozen
    // (explore, progress phase) — not during the frozen planning phase.
    if (turnctrl.state != TS_PLANNING && turnctrl.state != TS_TARGETING &&
        turnctrl.state != TS_CONFIRM)
    {
        T_KitTrickleAmmo();
    }
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

void T_RunPulseSync(int tics, boolean freeze_monsters)
{
    boolean was_sync = turnctrl.sync;
    turnctrl.sync = true;
    T_BeginPulse(tics, freeze_monsters, true);
    turnctrl.sync = was_sync;
}

// Enemy phase removed: the game runs in real-time during execution,
// so enemies act while the player's queued commands play out.
// (Kept as a no-op for compatibility.)
void T_BeginEnemyPhase(void)
{
    // No-op: enemy phase removed per design.
}

// Round bookkeeping when a pulse finishes.
static void T_EndPulse(void)
{
    if (turnctrl.pulse_enemy)
    {
        // Enemy phase done: tick cooldowns (phase 5), new round.
        turnctrl.pulse_enemy = false;
        turnctrl.round++;
        T_CountRound(); // Phase 8: lifetime rounds
        turnctrl.tp = turnctrl.tp_max; // TP economy lands in phase 2
        T_KitTickCooldowns();
        // Ammo replenish (per-weapon magazines): AD weapons regenerate
        // attack-speed attacks' worth of ammo; AP weapons refill on
        // cooldown completion / charge regen (hooked in the tick above).
        T_KitRegenAmmo();
        turnctrl.state = TS_PLANNING;
        // Anchor the preview: turn start is the camera home for queue
        // previews, UNDO/CLEAR resets, and the END TURN teleport-back.
        if (players[consoleplayer].mo != NULL)
        {
            turnctrl.turn_start_x = players[consoleplayer].mo->x;
            turnctrl.turn_start_y = players[consoleplayer].mo->y;
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
            // Continuous-weapon target lock: auto-queue an attack if the
            // locked target is still alive. The player is free to move;
            // the weapon keeps firing.
            T_AutoFireLocked();
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
            // Continuous fire: if lock is active with a continuous weapon,
            // keep firing instead of ending the phase. Respects attack_tics.
            if (turnctrl.locked_target != NULL &&
                T_IsContinuousWeapon(players[consoleplayer].readyweapon))
            {
                if (turnctrl.lock_fire_cooldown > 0)
                {
                    turnctrl.lock_fire_cooldown--;
                    return;
                }
                T_AutoFireLocked();
                if (turnctrl.queue_len > 0)
                {
                    T_ExecuteNext();
                    {
                        const t_kitdef_t *kit = T_KitForWeapon(
                            players[consoleplayer].readyweapon);
                        int t = kit->attack_tics;
                        turnctrl.lock_fire_cooldown = t < 1 ? 1 : t;
                    }
                    return;
                }
                // Lock cleared: fall through to end phase.
            }
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
                    turnctrl.turn_start_angle = players[consoleplayer].mo->angle;
                }
                T_AutoFireLocked();
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
static char cmdbuf[4096];
static int cmdlen = 0;

static void cmd_init(void)
{
    extern boolean D_OnMuseVM(void);
    struct stat st;

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
        // Read any commands the worker wrote early into our buffer.
        FILE *f = fopen("/tmp/doom_cmd.txt", "r");
        if (f != NULL)
        {
            size_t r = fread(cmdbuf, 1, sizeof(cmdbuf) - 1, f);
            cmdbuf[r] = '\0';
            cmdlen = (int)r;
            fclose(f);
            if (cmdlen > 0)
                printf("[CMD] recovered %d bytes from early worker write\n", cmdlen);
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
}

// Drain available bytes from the FIFO, split on newlines, dispatch
// each complete command line. Partial lines stay buffered.
static void cmd_drain(void (*dispatch)(const char *))
{
    char tmp[1024];
    ssize_t r;

    if (cmdfd == -2)
        cmd_init();
    if (cmdfd < 0)
        return;

    while ((r = read(cmdfd, tmp, sizeof(tmp))) > 0)
    {
        if (cmdlen + r >= (int)sizeof(cmdbuf))
        {
            // Buffer full without a newline: drop it, resync.
            cmdlen = 0;
        }
        memcpy(cmdbuf + cmdlen, tmp, r);
        cmdlen += r;

        // Extract complete lines.
        int start = 0;
        for (int i = 0; i < cmdlen; i++)
        {
            if (cmdbuf[i] == '\n' || cmdbuf[i] == '\r')
            {
                cmdbuf[i] = 0;
                if (i > start)
                    dispatch(cmdbuf + start);
                start = i + 1;
            }
        }
        if (start > 0)
        {
            memmove(cmdbuf, cmdbuf + start, cmdlen - start);
            cmdlen -= start;
        }
    }
    // r == 0: EOF (can't happen with O_RDWR, we hold a writer).
    // r < 0: EAGAIN = no data, or a real error (leave fd open).
}

// Dispatch a single driver command line from the FIFO.
static void cmd_dispatch(const char *cmd)
{
    if (!strcmp(cmd, "ATTACK"))
    {
        T_StartCtrlTargeting();
        T_ConfirmCtrlTarget();
    }
    else if (!strcmp(cmd, "END"))
        T_DoEndTurn();
    else if (!strcmp(cmd, "WALK"))
    {
        player_t *pl = &players[consoleplayer];
        if (pl->mo != NULL && T_InExplore())
        {
            // Smooth movement like a key press: inject momentum and let the
            // normal P_XYMovement physics (friction, collision, item pickup)
            // carry the player over subsequent tics. No teleport.
            // Impulses accumulate, so batching WALKs sustains movement.
            angle_t ang = pl->mo->angle;
            pl->mo->momx += FixedMul(FRACUNIT / 2, finecosine[ang >> ANGLETOFINESHIFT]);
            pl->mo->momy += FixedMul(FRACUNIT / 2, finesine[ang >> ANGLETOFINESHIFT]);
        }
    }
    else if (!strcmp(cmd, "TURNL"))
    {
        player_t *pl = &players[consoleplayer];
        if (pl->mo != NULL && T_Active())
            pl->mo->angle += ANG45;
    }
    else if (!strcmp(cmd, "TURNR"))
    {
        player_t *pl = &players[consoleplayer];
        if (pl->mo != NULL && T_Active())
            pl->mo->angle -= ANG45;
    }
    else if (!strcmp(cmd, "COLLECT"))
    {
        extern void T_DoCollect(void);
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
                // Simple validation: check item's slot matches (or allow swap)
                pl->diablo_equipped[slot] = id;
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
                        pl->diablo_equipped[slot] = -1;
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
    else if (!strncmp(cmd, "SALVAGE_EQUIPPED ", 17))
    {
        // SALVAGE_EQUIPPED <slot>: turn equipped item into essence.
        int slot;
        player_t *pl = &players[consoleplayer];
        if (sscanf(cmd + 17, "%d", &slot) == 1 && slot >= 0 && slot <= 10)
        {
            int id = pl->diablo_equipped[slot];
            if (id != -1)
            {
                extern void D_RecalcStats(struct player_s *player);
                int tier = ((id >> 8) & 0xff); // D_ITEMTIER macro
                int essence = 1;
                switch (tier)
                {
                  case 1: essence = 1; break;
                  case 2: essence = 3; break;
                  case 3: essence = 8; break;
                  case 4: essence = 15; break;
                  case 5: essence = 25; break;
                }
                pl->diablo_equipped[slot] = -1;
                D_RecalcStats((struct player_s *)pl);
                pl->diablo_essence += essence;
                printf("[ESSENCE] salvaged slot[%d] tier %d -> +%d (total %d)\n",
                       slot, tier, essence, pl->diablo_essence);
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
}

void T_Ticker(void)
{
    int i, n;

    if (!T_Active())
        return;

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

    // Time-based cadence (charges, cooldowns, heat): independent of rounds.
    T_TickCadence();

    // VM progress persistence: write the save every 120 tics.
    {
        extern void D_ProgressTick(void);
        D_ProgressTick();
    }

    // VM automation driver: drain the FIFO (see cmd_drain above).
    cmd_drain(cmd_dispatch);

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

// Phase 6: telegraph enemies that have acquired the player but are NOT
// currently visible to them -- the true unseen threats -- before the
// enemy phase lets them act.
void T_TelegraphEnemies(void)
{
    player_t *player = &players[consoleplayer];
    thinker_t *th;

    if (!T_Active() || player->mo == NULL)
        return;

    for (th = thinkercap.next; th != &thinkercap; th = th->next)
    {
        mobj_t *mo;
        static char msg[64];

        if (th->function.acp1 != (actionf_p1)P_MobjThinker)
            continue;
        mo = (mobj_t *)th;
        if (mo == player->mo || mo->player != NULL)
            continue;
        if (!(mo->flags & MF_SHOOTABLE))
            continue;
        if (mo->health <= 0)
            continue;
        if (!T_IsCombatant(mo))
            continue; // barrels don't telegraph
        if (mo->target != player->mo)
            continue; // hasn't acquired the player
        if (P_CheckSight(player->mo, mo))
            continue; // visible: no surprise to warn about
        M_snprintf(msg, sizeof(msg), "%s is hunting you!",
                   T_MobjShortName(mo));
        players[consoleplayer].message = msg;
        printf("[TURN] telegraph: %s\n", msg);
    }
}

// real AD/AP/AS/Haste/crit model). Distance-based, deterministic.
static int T_PreviewHit(mobj_t *mo)
{
    t_combatstats_t st;
    player_t *player = &players[consoleplayer];
    if (player->mo == NULL || mo == NULL)
        return 0;
    T_DeriveStats(player, &st);
    return T_HitChance(player, mo, &st);
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

// Gate state for the target-preview line: " CD2", " HEAT 45", " CHG 1/2",
// " MANA!", or "" when the kit can fire.
static void T_KitGateStr(weapontype_t w, char *buf, int bufsz)
{
    const t_kitdef_t *kit = T_KitForWeapon(w);
    int cd = T_KitCooldown(w);
    buf[0] = '\0';
    if (cd > 0)
        M_snprintf(buf, bufsz, " CD%d", cd);
    else if (kit->heat_per_shot > 0)
        M_snprintf(buf, bufsz, " HEAT %d", T_KitHeat(w));
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

int T_XPForLevel(int level)
{
    // 100 XP per level, cumulative: L2=100, L3=300, L4=600, ...
    // Threshold to reach `level` from `level-1`.
    return (level - 1) * 100;
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

