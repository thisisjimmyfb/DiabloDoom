//
// t_turn.c — Turn-based (XCOM) mode: central turn controller.
//
// Owns the state machine, the bounded simulation pulse, round/enemy
// phases, and the test harnesses. This fork is turn-based by default:
// G_Ticker calls T_Ticker instead of P_Ticker unconditionally.

#include <string.h>
#include <stdio.h>

#include "hu_stuff.h"
#include "t_turn.h"
#include "t_combat.h"
#include "d_diablo.h"
#include "doomstat.h"
#include "doomdef.h"
#include "d_player.h"
#include "d_think.h"
#include "d_main.h"
#include "m_argv.h"
#include "i_swap.h"
#include "g_game.h"
#include "p_local.h"
#include "r_main.h"
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
        printf("Turn-based mode: queue actions in planning (WASD move/strafe, "
               "arrows turn view free, SPACE use, T end turn), "
               "END TURN executes the queue FIFO.\n");
        // Phase 8: load the standalone profile.
        T_ProfileLoad();
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
    // Land in a plannable state; the queue is transient planning state
    // (never saved) and starts empty.
    turnctrl.state = TS_PLANNING;
    turnctrl.pulse_left = 0;
    turnctrl.queue_len = 0;
    turnctrl.queue_tp = 0;
    turnctrl.executing = false;
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

// Mana design: am_clip is the unified mana pool, a real resource that
// pickups, Mana Potions, and kit costs move up and down. Every other ammo
// pool stays maxed while planning so the underlying weapon state machine
// can never fail or auto-switch for lack of ammo; mana itself is never
// topped up here. Real-time mode never reaches this code.
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

// Enemy phase: telegraph, then monsters act for a bounded pulse with
// input locked, then round bookkeeping (in T_EndPulse).
void T_BeginEnemyPhase(void)
{
    players[consoleplayer].message = "Enemy phase...";
    // Telegraph newly alerted enemies before they get to act.
    T_TelegraphEnemies();
    // Flag before the pulse: in sync (script) mode T_BeginPulse runs
    // T_EndPulse immediately, which needs to see the enemy phase.
    turnctrl.pulse_enemy = true;
    T_BeginPulse(ENEMY_PHASE_TICS, false, false); // 2s, real-time rate
    if (!turnctrl.sync)
        turnctrl.state = TS_REACTION;
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
        turnctrl.state = TS_PLANNING;
        {
            static char msg[64];
            M_snprintf(msg, sizeof(msg), "Round %d - your move.",
                       turnctrl.round);
            players[consoleplayer].message = msg;
        }
    }
    else if (turnctrl.executing)
    {
        // Queue drain: run the next entry; when the queue is fully
        // drained T_ExecuteNext starts the enemy phase itself.
        T_ExecuteNext();
    }
    else
    {
        turnctrl.state = TS_PLANNING;
    }
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

// Phase 6: telegraph newly alerted enemies. An enemy that can see the
// player but has not yet acquired them as a target shows a state change
// (message) before it gets to act. This gives the player fair warning
// and prevents unseen alpha strikes.
void T_TelegraphEnemies(void)
{
    player_t *player = &players[consoleplayer];
    int i;
    if (!player->mo)
        return;
    for (i = 0; i < t_numtargets; i++)
    {
        mobj_t *mo = t_targets[i];
        if (!mo || mo->health <= 0)
            continue;
        // Not yet alerted to the player: telegraph the state change.
        if (mo->target == NULL || mo->target != player->mo)
        {
            static char msg[64];
            const char *name = "Enemy";
            // Use the mobj type name if available.
            if (mo->type == MT_POSSESSED)
                name = "Zombieman";
            else if (mo->type == MT_SHOTGUY)
                name = "Shotgunner";
            else if (mo->type == MT_CHAINGUY)
                name = "Chaingunner";
            else if (mo->type == MT_TROOP)
                name = "Imp";
            else if (mo->type == MT_SERGEANT)
                name = "Demon";
            M_snprintf(msg, sizeof(msg), "%s spots you!", name);
            printf("[TURN] telegraph: %s\n", msg);
        }
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
            && T_KitReady(players[consoleplayer].readyweapon)
            && T_HasManaForKit(players[consoleplayer].readyweapon);
    if (action == TA_UNDO || action == TA_CLEAR_QUEUE)
        return turnctrl.queue_len > 0;
    return T_CanAfford(action);
}

// One action row: key, name, TP cost. Unaffordable rows draw dim gold.
static void T_DrawActionRow(int x, int *y, const char *key, const char *name,
                            turnaction_t action, const char *coststr)
{
    char line[64];
    M_snprintf(line, sizeof(line), "%s %-8s %s", key, name, coststr);
    if (T_ActionEnabled(action))
        T_DrawText(x, *y, line);
    else
        T_DrawTextDim(x, *y, line);
    *y += 9;
}

// Planning-time action list with per-action TP costs. Disabled
// (unaffordable) actions render dim gold and their keypresses are
// refused with a "Need N TP" message — they are never selectable.
// END TURN is always the final row, separated as the exit: a dim
// divider above it and a bright-gold bracketed row so it reads as
// the way out, not just another action.
static void T_DrawActionList(void)
{
    char cost[16];
    int x = 8, y = 26;
    player_t *pl = &players[consoleplayer];

    T_DrawActionRow(x, &y, "WASD", "MOVE/STRF", TA_MOVE_N, "1TP");
    T_DrawActionRow(x, &y, "<>", "TURN", TA_TURN_L, "FREE");
    T_DrawActionRow(x, &y, "SPC", "USE", TA_USE, "2TP");
    T_DrawActionRow(x, &y, "TAB", "TARGET", TA_SELECT_NEXT, "FREE");
    if (!T_KitReady(pl->readyweapon))
        T_DrawActionRow(x, &y, "F", "ATTACK", TA_ATTACK, "CD");
    else
    {
        M_snprintf(cost, sizeof(cost), "%dTP", T_CostFor(TA_ATTACK));
        T_DrawActionRow(x, &y, "F", "ATTACK", TA_ATTACK, cost);
    }
    T_DrawActionRow(x, &y, "BKSP", "UNDO", TA_UNDO, "FREE");
    T_DrawActionRow(x, &y, "Z", "CLEARQ", TA_CLEAR_QUEUE, "FREE");
    // The Diablo inventory/character screen is always available during a
    // round (free, immediate). It owns all input while open; the turn
    // controller simply waits in PLANNING.
    T_DrawActionRow(x, &y, "C", "INVEN", TA_NONE, "FREE");
    // The exit: divider, then END TURN bracketed in highlight gold.
    T_DrawTextDim(x, y, "----------------");
    y += 9;
    {
        char line[64];
        M_snprintf(line, sizeof(line), "%s %-8s %s", ">>T", "END TURN", "0TP<<");
        T_DrawTextHi(x, y, line);
        y += 9;
    }
}

// Queued-action panel: FIFO order with per-action costs and the total
// reserved TP. Drawn below the action list while planning (and while
// targeting, so the queue stays visible). Hidden when empty.
static void T_DrawQueuePanel(void)
{
    char line[64], name[48];
    int x = 8, y = 122, i, shown;

    if (turnctrl.queue_len == 0)
        return;
    // Hide the panel while the queue is executing or a pulse resolves:
    // the drain is visible in the world, not the list.
    if (turnctrl.executing || T_InPulse())
        return;
    T_DrawText(x, y, "QUEUE");
    y += 9;
    // Cap visible rows so the panel never reaches the status bar.
    shown = turnctrl.queue_len > 3 ? 3 : turnctrl.queue_len;
    for (i = 0; i < shown; i++)
    {
        t_queueentry_t *e = &turnctrl.queue[i];
        T_QueueEntryName(e, name, sizeof(name));
        M_snprintf(line, sizeof(line), "%d %s %dTP", i + 1, name, e->cost);
        T_DrawText(x, y, line);
        y += 9;
    }
    if (turnctrl.queue_len > shown)
    {
        M_snprintf(line, sizeof(line), "+%d MORE", turnctrl.queue_len - shown);
        T_DrawTextDim(x, y, line);
        y += 9;
    }
    M_snprintf(line, sizeof(line), "%d ACTS %dTP RSVD",
               turnctrl.queue_len, turnctrl.queue_tp);
    T_DrawTextHi(x, y, line);
}

void T_DrawHUD(void)
{
    char line[96];
    int i;

    if (!T_Active())
        return;
    T_TintFonts();

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
        T_DrawTextCentered(12, "RESOLVING");
    else if (turnctrl.state == TS_PLANNING)
        T_DrawActionList();
    else
    {
        // TARGETING / CONFIRM: the always-legal outs stay visible.
        // The action list keeps drawing (left column): it repaints the
        // border-zone pixels every frame and keeps costs/disabled states
        // visible while targeting.
        T_DrawTextCentered(12, "T END TURN  BKSP UNDO  Z CLEAR  <> TURN  ESC BACK");
        T_DrawActionList();
    }
    // Queued-action panel (below the action list): FIFO order,
    // per-action costs, total reserved TP.
    T_DrawQueuePanel();

    // Target markers: stable numbers projected above each visible enemy.
    // The selected target is unmistakable: bright highlight gold with
    // chevron brackets, centered over the enemy. Non-selected targets
    // are subdued dim gold numbers.
    for (i = 0; i < t_numtargets; i++)
    {
        mobj_t *mo = t_targets[i];
        int sx, sy;
        if (mo == NULL || mo->health <= 0)
            continue;
        if (!T_ProjectTarget(mo, &sx, &sy))
            continue;
        if (sx < 0 || sx > 312 || sy < 0 || sy > 192)
            continue;
        if (i == turnctrl.selected_target)
        {
            M_snprintf(line, sizeof(line), ">[%d]<", i + 1);
            T_DrawTextHi(sx - T_TextWidth(line) / 2, sy, line);
        }
        else
        {
            M_snprintf(line, sizeof(line), "%d", i + 1);
            T_DrawTextDim(sx - T_TextWidth(line) / 2, sy, line);
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
                    int cd = T_KitCooldown(pl->readyweapon);
                    if (kit->splash_radius > 0)
                        M_snprintf(line, sizeof(line),
                                   "[%d]%s HP%d R%d H%d%% D%d-%d SPLASH %dTP%s",
                                   turnctrl.selected_target + 1,
                                   T_TargetName(mo), mo->health, dist,
                                   T_HitChance(pl, mo, &st),
                                   dmin, dmax,
                                   T_CostFor(TA_ATTACK),
                                   cd > 0 ? " CD!" : "");
                    else
                        M_snprintf(line, sizeof(line),
                                   "[%d]%s HP%d R%d H%d%% D%d-%d %dTP%s",
                                   turnctrl.selected_target + 1,
                                   T_TargetName(mo), mo->health, dist,
                                   T_HitChance(pl, mo, &st),
                                   dmin, dmax,
                                   T_CostFor(TA_ATTACK),
                                   cd > 0 ? " CD!" : "");
                }
                T_DrawTextCenteredHi(148, line);
                if (turnctrl.state == TS_CONFIRM)
                    T_DrawTextCenteredHi(158,
                        "CONFIRM: F FIRE  ESC CANCEL");
            }
        }
    }
}


int T_NumTargets(void)
{
    return t_numtargets;
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
        "OFF", "PLANNING", "TARGETING", "CONFIRM",
        "PULSE", "REACTION", "ROUND_END"
    };
    const char *st = (turnctrl.state >= 0 && turnctrl.state <= TS_ROUND_END)
                     ? names[turnctrl.state] : "?";

    printf("[TURN] %-10s round=%d tp=%d/%d qlen=%d qtp=%d exec=%d state=%s sel=%d hp=%d pos=(%d,%d) angle=%u\n",
           why ? why : "-",
           turnctrl.round, turnctrl.tp, turnctrl.tp_max,
           turnctrl.queue_len, turnctrl.queue_tp, turnctrl.executing ? 1 : 0,
           st,
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
    int threshold;
    if (amount <= 0)
        return;
    t_profile.xp += amount;
    // Level up while XP meets the next threshold.
    while (1)
    {
        threshold = T_XPForLevel(t_profile.level + 1);
        if (t_profile.xp < threshold)
            break;
        t_profile.level++;
        t_profile.stat_points += 2; // 2 points per level
        printf("[TURN] LEVEL UP! Now level %d (+2 stat points)\n",
               t_profile.level);
        players[consoleplayer].message = "LEVEL UP! +2 stat points";
    }
}

void T_AddStatPoint(const char *stat)
{
    player_t *pl = &players[consoleplayer];
    if (t_profile.stat_points <= 0)
    {
        printf("[TURN] no stat points to spend\n");
        return;
    }
    if (!strcmp(stat, "str"))
        pl->diablo_stats[DSTAT_STR]++;
    else if (!strcmp(stat, "dex"))
        pl->diablo_stats[DSTAT_DEX]++;
    else if (!strcmp(stat, "vit"))
    {
        pl->diablo_stats[DSTAT_VIT]++;
        // Max HP is derived via D_MaxHealth() (+5 per VIT).
        // Heal the 5 HP gained.
        pl->health += 5;
        if (pl->mo)
            pl->mo->health = pl->health;
    }
    else if (!strcmp(stat, "ene"))
        pl->diablo_stats[DSTAT_ENE]++;
    else
    {
        printf("[TURN] unknown stat '%s' (use str/dex/vit/ene)\n", stat);
        return;
    }
    t_profile.stat_points--;
    // Re-derive? diablo_stats are the base; D_RecalcStats would wipe them.
    // For Phase 8, stat points directly increment the aggregated stats.
    // (A full implementation would store base stats separately.)
    printf("[TURN] +1 %s (%d points left)\n", stat, t_profile.stat_points);
}

void T_CountKill(mobj_t *target)
{
    int xp = 10;
    t_profile.lifetime_kills++;
    // XP by target max health (tougher = more XP).
    if (target && target->info)
        xp = 10 + target->info->spawnhealth / 10;
    T_GainXP(xp);
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

