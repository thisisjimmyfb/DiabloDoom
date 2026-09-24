//
// t_action.c — Turn-based mode: action implementations (gameplay bridge).
//
// Approved discrete commands into the existing player/world systems.
// Phase 1: MOVE (derived, collision-aware, auto-facing), USE, WAIT,
// END TURN (enemy phase). TP costs land in phase 2; attacks in phase 3.

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "t_turn.h"
#include "t_combat.h"
#include "doomstat.h"
#include "d_player.h"
#include "g_game.h"
#include "i_system.h"
#include "p_local.h"
#include "p_saveg.h"
#include "r_main.h"
#include "m_misc.h"
#include "tables.h"
#include "s_sound.h"
#include "info.h"

// Step length: 4 sub-steps of 8 units = 32 units total, collision-aware
// (sub-steps avoid tunneling through thin lines).
#define STEP_SUB 4
#define STEP_LEN (8 * FRACUNIT)

// ------------------------------------------------------------------
// Phase 2: Tempo economy.
//
// Baseline action costs (SPEC). Selection and cancellation are always
// free; TP is spent only after an action commits. An action that cannot
// be afforded is refused with a message and changes nothing.
// ------------------------------------------------------------------

// Derived attack cost (Phase 5): kit base TP, reduced by attack speed.
int T_AttackCost(void)
{
    const t_kitdef_t *kit;
    t_combatstats_t st;
    player_t *player = &players[consoleplayer];
    int cost;
    if (!T_Active() || player->mo == NULL)
        return 4;
    kit = T_KitForWeapon(player->readyweapon);
    T_DeriveStats(player, &st);
    // AS reduces the kit cost: -1 per 20 dex above 10, floor 2.
    cost = kit->tp_cost - (10 + player->diablo_stats[DSTAT_DEX] - 10) / 20;
    if (cost < 2)
        cost = 2;
    return cost;
}

int T_CostFor(turnaction_t action)
{
    switch (action)
    {
      case TA_MOVE_N: case TA_MOVE_S: case TA_MOVE_E: case TA_MOVE_W:
        return 1;
      case TA_USE:
      case TA_SWAP_WEAPON:
        return 2;
      case TA_WAIT:
        // WAIT is the always-legal pass action: 0 TP, so the player can
        // never be soft-locked with no affordable move.
        return 0;
      case TA_ATTACK:
        return T_AttackCost() + (turnctrl.headshot_mod ? 2 : 0);
      case TA_HEADSHOT: // arming the modifier is free; the +2 lands on ATTACK
        return 0;
      case TA_ABILITY: // per-kit costs land in phase 5
        return 3;
      case TA_POTION: // consumable use cost lands with the Diablo bridge
        return 2;
      case TA_SELECT_NEXT: case TA_SELECT_PREV: case TA_SELECT_NUM:
      case TA_CANCEL:
      case TA_END_TURN:
      case TA_NONE:
      default:
        return 0;
    }
}

boolean T_CanAfford(turnaction_t action)
{
    return turnctrl.tp >= T_CostFor(action);
}

void T_SpendTP(int cost)
{
    turnctrl.tp -= cost;
    if (turnctrl.tp < 0)
        turnctrl.tp = 0; // belt and braces; CanAfford gates this
}

// Refuse an unaffordable action: message, no state change, no pulse.
static void T_RefuseTP(turnaction_t action)
{
    static char msg[64];
    int need = T_CostFor(action);
    M_snprintf(msg, sizeof(msg), "Need %d TP (have %d).", need, turnctrl.tp);
    players[consoleplayer].message = msg;
    printf("[TURN] refused action %d: need %d tp, have %d\n",
           action, need, turnctrl.tp);
}

// Dead heroes take no actions. (Death/rebirth flow lands in a later phase.)
static boolean T_ActorAlive(void)
{
    player_t *player = &players[consoleplayer];
    if (player->mo == NULL || player->health <= 0)
    {
        player->message = "You are dead.";
        return false;
    }
    return true;
}

// ------------------------------------------------------------------
// MOVE: screen-relative discrete step with auto-facing.
// dir: 0=N(forward) 1=E(right) 2=S(back) 3=W(left), relative to facing.
// Doom angles increase counterclockwise (+y is north), so strafing
// right is -90 degrees and strafing left is +90.
void T_DoMove(int dir)
{
    player_t *player = &players[consoleplayer];
    mobj_t *mo = player->mo;
    angle_t moveangle;
    fixed_t dx, dy;
    int k;
    static const angle_t diroff[4] = { 0, (angle_t)-ANG90, ANG180, ANG90 };

    if (!T_Active() || mo == NULL)
        return;
    if (!T_ActorAlive())
        return;
    if (!T_CanAfford(TA_MOVE_N))
    {
        T_RefuseTP(TA_MOVE_N);
        return;
    }
    if (dir < 0 || dir > 3)
        return;

    moveangle = mo->angle + diroff[dir];

    // Auto-face the movement direction. Facing is never an action.
    mo->angle = moveangle;

    dx = FixedMul(STEP_LEN, finecosine[moveangle >> ANGLETOFINESHIFT]);
    dy = FixedMul(STEP_LEN, finesine[moveangle >> ANGLETOFINESHIFT]);

    for (k = 0; k < STEP_SUB; k++)
    {
        if (!P_TryMove(mo, mo->x + dx, mo->y + dy))
            break; // blocked: stop, keep partial progress
    }
    mo->momx = mo->momy = 0;
    mo->momz = 0;

    T_SpendTP(T_CostFor(TA_MOVE_N));

    // Settle pulse: sector effects, in-flight odds and ends. Monsters
    // stay frozen — they act on their own phase.
    T_BeginPulse(6, true, true);
    T_DumpState("move");
}

// ------------------------------------------------------------------
// USE: interact with the line in front (doors, switches).
// ------------------------------------------------------------------
void T_DoUse(void)
{
    player_t *player = &players[consoleplayer];

    if (!T_Active() || player->mo == NULL)
        return;
    if (!T_ActorAlive())
        return;
    if (!T_CanAfford(TA_USE))
    {
        T_RefuseTP(TA_USE);
        return;
    }

    P_UseLines(player);
    T_SpendTP(T_CostFor(TA_USE));
    T_BeginPulse(6, true, true);
    T_DumpState("use");
}

// ------------------------------------------------------------------
// WAIT: pass a little time without acting. Costs 0 TP: this and END
// TURN are the always-legal moves, reachable from every selection
// state, so the player can never be soft-locked at 0 TP.
// ------------------------------------------------------------------
void T_DoWait(void)
{
    if (!T_Active())
        return;
    if (!T_ActorAlive())
        return;
    if (!T_CanAfford(TA_WAIT))
    {
        T_RefuseTP(TA_WAIT);
        return;
    }

    players[consoleplayer].message = "Wait.";
    // Leaving a selection state: drop the target, back to planning.
    turnctrl.selected_target = -1;
    if (turnctrl.state == TS_TARGETING || turnctrl.state == TS_CONFIRM)
        turnctrl.state = TS_PLANNING;
    T_SpendTP(T_CostFor(TA_WAIT));
    T_BeginPulse(8, true, true);
    T_DumpState("wait");
}

// ------------------------------------------------------------------
// END TURN: enemy phase — monsters act for a bounded pulse with input
// locked — then round bookkeeping.
// ------------------------------------------------------------------
void T_DoEndTurn(void)
{
    if (!T_Active())
        return;
    if (T_InPulse())
        return;
    if (!T_ActorAlive())
        return;

    players[consoleplayer].message = "Enemy phase...";
    // Leaving a selection state: drop the target, back to planning.
    // END TURN is always legal (0 TP) from every state.
    turnctrl.selected_target = -1;
    if (turnctrl.state == TS_TARGETING || turnctrl.state == TS_CONFIRM)
        turnctrl.state = TS_PLANNING;
    // Telegraph newly alerted enemies before they get to act.
    T_TelegraphEnemies();
    // Flag before the pulse: in sync (script) mode T_BeginPulse runs
    // T_EndPulse immediately, which needs to see the enemy phase.
    turnctrl.pulse_enemy = true;
    T_BeginPulse(70, false, false); // 2s of monster action, real-time rate
    if (!turnctrl.sync)
        turnctrl.state = TS_REACTION;
    T_DumpState("end-turn");
}

// ------------------------------------------------------------------
// SWAP WEAPON: 2 TP. Cycle readyweapon to the next owned Doom weapon
// among the six turn-mode kits (pistol..BFG). The Diablo equipment weapon
// item is orthogonal (it grants stats); the six kits live on the Doom
// weapons. Fists are not a kit, so swap never selects them. Runs a short
// frozen pulse so the lower/raise animation completes via P_PlayerThink.
// ------------------------------------------------------------------
void T_DoSwapWeapon(void)
{
    player_t *player = &players[consoleplayer];
    int w, cand;

    if (!T_Active() || player->mo == NULL)
        return;
    if (!T_ActorAlive())
        return;
    if (!T_CanAfford(TA_SWAP_WEAPON))
    {
        T_RefuseTP(TA_SWAP_WEAPON);
        return;
    }

    for (w = 1; w <= (wp_bfg - wp_pistol); w++)
    {
        cand = wp_pistol + (player->readyweapon - wp_pistol + w)
                         % (wp_bfg - wp_pistol + 1);
        if (player->weaponowned[cand] && cand != player->readyweapon)
            break;
    }
    if (w > (wp_bfg - wp_pistol) || cand == player->readyweapon)
    {
        player->message = "No other weapon.";
        printf("[TURN] swap refused: no other weapon owned\n");
        return;
    }

    player->pendingweapon = cand;
    T_SpendTP(T_CostFor(TA_SWAP_WEAPON));
    // Frozen pulse lets the weapon lower/raise; ammo is topped up so the
    // state machine can never reject the switch for lack of ammo.
    T_BeginPulse(30, true, true);
    printf("[TURN] swapped to weapon %d\n", cand);
    T_DumpState("swap");
}

// HEADSHOT: free action. Arms the headshot modifier for the next ATTACK.
// The modifier adds +2 TP (in T_CostFor), -15% hit (in T_HitChance), and
// 2x crit effect (in T_ResolveAttack). It is consumed by the next attack
// (hit or miss). Arming is free and does not advance time.
void T_DoHeadshot(void)
{
    if (!T_Active())
        return;
    if (!T_ActorAlive())
        return;
    if (turnctrl.headshot_mod)
    {
        // Already armed; disarm (toggle).
        turnctrl.headshot_mod = false;
        players[consoleplayer].message = "Headshot off.";
    }
    else
    {
        turnctrl.headshot_mod = true;
        players[consoleplayer].message = "Headshot armed (+2TP -15% 2xCRIT).";
    }
    T_DumpState("headshot");
}

// ------------------------------------------------------------------
// Target selection (phase 3): free, reversible. Tab / ] cycle forward,
// [ cycles back, number keys jump directly. Selecting enters TARGETING
// with a preview; ESC cancels back to PLANNING.
// ------------------------------------------------------------------
static void T_EnterTargeting(int idx)
{
    T_RefreshTargets();
    if (T_NumTargets() == 0)
    {
        players[consoleplayer].message = "No visible targets.";
        return;
    }
    if (idx < 0)
        idx = 0;
    if (idx >= T_NumTargets())
        idx = T_NumTargets() - 1;
    turnctrl.selected_target = idx;
    turnctrl.state = TS_TARGETING;
    T_DumpState("select");
}

void T_DoSelectNext(void)
{
    int n;
    if (!T_Active() || players[consoleplayer].mo == NULL)
        return;
    if (!T_ActorAlive())
        return;
    if (T_InPulse())
        return;
    T_RefreshTargets();
    n = T_NumTargets();
    if (n == 0)
    {
        players[consoleplayer].message = "No visible targets.";
        return;
    }
    if (turnctrl.selected_target < 0)
        T_EnterTargeting(0);
    else
        T_EnterTargeting((turnctrl.selected_target + 1) % n);
}

void T_DoSelectPrev(void)
{
    int n;
    if (!T_Active() || players[consoleplayer].mo == NULL)
        return;
    if (!T_ActorAlive())
        return;
    if (T_InPulse())
        return;
    T_RefreshTargets();
    n = T_NumTargets();
    if (n == 0)
    {
        players[consoleplayer].message = "No visible targets.";
        return;
    }
    if (turnctrl.selected_target < 0)
        T_EnterTargeting(n - 1);
    else
        T_EnterTargeting((turnctrl.selected_target - 1 + n) % n);
}

void T_DoSelectNum(int num)
{
    if (!T_Active() || players[consoleplayer].mo == NULL)
        return;
    if (!T_ActorAlive())
        return;
    if (T_InPulse())
        return;
    // Numbers are 1-based for the player.
    T_EnterTargeting(num - 1);
}

void T_DoCancel(void)
{
    if (!T_Active())
        return;
    if (T_InPulse())
        return;
    if (turnctrl.state == TS_TARGETING || turnctrl.state == TS_CONFIRM)
    {
        turnctrl.selected_target = -1;
        turnctrl.state = TS_PLANNING;
        players[consoleplayer].message = "Targeting cancelled.";
        T_DumpState("cancel");
    }
}

// ------------------------------------------------------------------
// ATTACK (phase 3): preview -> confirm -> auto-face -> pulse.
// F/Enter in TARGETING enters CONFIRM; F/Enter in CONFIRM fires.
// The attack resolves against the confirmed actor ID via the real
// weapon state machine after auto-facing the target.
// ------------------------------------------------------------------
static void T_FaceTarget(mobj_t *target)
{
    player_t *player = &players[consoleplayer];
    angle_t ang;

    if (player->mo == NULL || target == NULL)
        return;
    ang = R_PointToAngle2(player->mo->x, player->mo->y,
                          target->x, target->y);
    player->mo->angle = ang;
}

void T_DoAttack(void)
{
    player_t *player = &players[consoleplayer];
    mobj_t *target;

    if (!T_Active() || player->mo == NULL)
        return;
    if (!T_ActorAlive())
        return;
    if (T_InPulse())
        return;

    if (turnctrl.state == TS_TARGETING)
    {
        // Preview -> confirm. No TP spent yet — but never enter CONFIRM
        // for an attack that cannot fire: reject here with a clear
        // message so the player is not trapped confirming the
        // unaffordable.
        T_ValidateSelection();
        if (turnctrl.selected_target < 0)
            return;
        target = T_TargetMobj(turnctrl.selected_target);
        if (target != NULL && !T_KitReady(player->readyweapon))
        {
            player->message = "Weapon cooling down.";
            printf("[TURN] attack refused: %s cooldown %d\n",
                   T_KitForWeapon(player->readyweapon)->name,
                   T_KitCooldown(player->readyweapon));
            return;
        }
        if (!T_CanAfford(TA_ATTACK))
        {
            T_RefuseTP(TA_ATTACK);
            return;
        }
        turnctrl.state = TS_CONFIRM;
        player->message = "Confirm attack: F fire, ESC cancel.";
        T_DumpState("confirm");
        return;
    }

    if (turnctrl.state != TS_CONFIRM)
        return;

    T_ValidateSelection();
    target = T_TargetMobj(turnctrl.selected_target);
    if (target == NULL)
    {
        player->message = "Target lost.";
        turnctrl.state = TS_PLANNING;
        return;
    }
    if (!T_KitReady(player->readyweapon))
    {
        player->message = "Weapon cooling down.";
        printf("[TURN] attack refused: %s cooldown %d\n",
               T_KitForWeapon(player->readyweapon)->name,
               T_KitCooldown(player->readyweapon));
        return;
    }
    if (!T_CanAfford(TA_ATTACK))
    {
        T_RefuseTP(TA_ATTACK);
        return;
    }

    // Commit: auto-face, spend TP, resolve with turn-based combat math.
    // Hit chance, damage range, and mitigation all come from the derived
    // combat stats; the fixed-seed RNG keeps previews and resolutions
    // in agreement.
    T_FaceTarget(target);
    T_SpendTP(T_CostFor(TA_ATTACK));
    T_ResolveAttack(player, target);
    // The world changed; rebuild targets and drop a dead selection.
    T_RefreshTargets();
    T_ValidateSelection();
    if (turnctrl.state == TS_CONFIRM)
        turnctrl.state = TS_PLANNING;
    T_DumpState("attack");
}

// ------------------------------------------------------------------
// Fixed-seed arena (test harness): deterministic encounter for gates.
// Spawns a fixed set of monsters at fixed offsets from the player and
// seeds the turn RNG. Same monsters, positions, RNG every run.
// ------------------------------------------------------------------
void T_SpawnArena(void)
{
    player_t *player = &players[consoleplayer];
    mobj_t *mo;
    fixed_t px, py;
    int i;

    if (!T_Active() || player->mo == NULL)
        return;

    T_SeedRNG(0xA1EA5EEDu);

    px = player->mo->x;
    py = player->mo->y;

    // 2 zombiemen + 2 imps at fixed offsets (map units).
    {
        struct { mobjtype_t type; int dx, dy; } spawns[] = {
            { MT_POSSESSED,  256,  128 },
            { MT_POSSESSED,  256, -128 },
            { MT_TROOP,      288,  128 },
            { MT_TROOP,      288, -128 },
        };
        for (i = 0; i < 4; i++)
        {
            mo = P_SpawnMobj(px + spawns[i].dx * FRACUNIT,
                             py + spawns[i].dy * FRACUNIT,
                             ONFLOORZ, spawns[i].type);
            if (mo)
            {
                // Face the player; leave AI to the enemy phase.
                mo->angle = R_PointToAngle2(mo->x, mo->y, px, py);
            }
        }
    }

    players[consoleplayer].message = "Arena: 4 hostiles. Turn mode live.";
    T_DumpState("arena");
}

// ------------------------------------------------------------------
// Action replay (test harness): text scripts drive the controller with
// no mouse events. One token per line:
//
//   MOVE_N | MOVE_E | MOVE_S | MOVE_W | USE | WAIT | END | DUMP | QUIT
//   SWAP | HEADSHOT
//   ASSERT_TP n | ASSERT_ROUND n     (gate checks; print PASS/FAIL)
//   ASSERT_SEL n | ASSERT_TGT n      (selection / target count checks)
//   SAVE n | LOAD n                  (slots 0-7; synchronous)
//   GIVEWEAPON n                    (test: grant kit weapon n)
//   SELECT_NEXT | SELECT_PREV | SELECT_NUM n | ATTACK | CANCEL
//
// Pulses run synchronously so scripts are fast and deterministic.
// ------------------------------------------------------------------
static void T_ScriptAssertTP(int want)
{
    if (turnctrl.tp == want)
        printf("[TURN] ASSERT_TP %d: PASS\n", want);
    else
        printf("[TURN] ASSERT_TP %d: FAIL (have %d)\n", want, turnctrl.tp);
}

static void T_ScriptAssertRound(int want)
{
    if (turnctrl.round == want)
        printf("[TURN] ASSERT_ROUND %d: PASS\n", want);
    else
        printf("[TURN] ASSERT_ROUND %d: FAIL (have %d)\n", want,
               turnctrl.round);
}

void T_RunScript(const char *path)
{
    FILE *f;
    char line[64];

    if (!T_Active() || path == NULL)
        return;

    f = fopen(path, "r");
    if (!f)
    {
        printf("[TURN] script not found: %s\n", path);
        return;
    }

    printf("[TURN] running script %s\n", path);
    turnctrl.sync = true;

    while (fgets(line, sizeof(line), f))
    {
        int n, m;
        unsigned int u;
        // Skip comment lines.
        if (line[0] == '#')
            continue;
        // strip trailing newline
        line[strcspn(line, "\r\n")] = 0;

        if (!strcmp(line, "MOVE_N"))      T_DoMove(0);
        else if (!strcmp(line, "MOVE_E")) T_DoMove(1);
        else if (!strcmp(line, "MOVE_S")) T_DoMove(2);
        else if (!strcmp(line, "MOVE_W")) T_DoMove(3);
        else if (!strcmp(line, "USE"))    T_DoUse();
        else if (!strcmp(line, "WAIT"))   T_DoWait();
        else if (!strcmp(line, "END"))    T_DoEndTurn();
        else if (!strcmp(line, "SWAP"))   T_DoSwapWeapon();
        else if (!strcmp(line, "HEADSHOT"))  T_DoHeadshot();
        else if (sscanf(line, "NAME %31s", t_profile.name) == 1)
        {
            printf("[TURN] name set to '%s'\n", t_profile.name);
            T_DumpState("name");
        }
        else if (!strncmp(line, "ADDSTAT ", 8))
        {
            char statname[32];
            if (sscanf(line + 8, "%31s", statname) == 1)
                T_AddStatPoint(statname);
        }
        else if (!strcmp(line, "PROFILE"))
        {
            printf("[TURN] PROFILE: name='%s' L%d XP=%d SP=%d kills=%d dmg=%d rounds=%d hs=%d\n",
                   t_profile.name, t_profile.level, t_profile.xp,
                   t_profile.stat_points, t_profile.lifetime_kills,
                   t_profile.lifetime_damage, t_profile.lifetime_rounds,
                   t_profile.lifetime_headshots);
        }
        else if (!strcmp(line, "DUMP"))   T_DumpState("script");
        else if (sscanf(line, "ASSERT_TP %d", &n) == 1) T_ScriptAssertTP(n);
        else if (sscanf(line, "ASSERT_ROUND %d", &n) == 1)
            T_ScriptAssertRound(n);
        else if (sscanf(line, "SAVE %d", &n) == 1 && n >= 0 && n < 8)
        {
            char desc[32];
            M_snprintf(desc, sizeof(desc), "tb gate r%d", turnctrl.round);
            G_DoSaveGameSlot(n, desc);
            printf("[TURN] saved slot %d\n", n);
        }
        else if (sscanf(line, "GIVEWEAPON %d", &n) == 1
                 && n >= wp_pistol && n <= wp_bfg)
        {
            players[consoleplayer].weaponowned[n] = true;
            printf("[TURN] gave weapon %d (test)\n", n);
        }
        else if (sscanf(line, "LOAD %d", &n) == 1 && n >= 0 && n < 8)
        {
            extern char savename[256];
            char *sn = P_SaveGameFile(n);
            M_StringCopy(savename, sn, sizeof(savename));
            free(sn);
            G_DoLoadGame();
            printf("[TURN] loaded slot %d\n", n);
        }
        else if (!strcmp(line, "SELECT_NEXT")) T_DoSelectNext();
        else if (!strcmp(line, "SELECT_PREV")) T_DoSelectPrev();
        else if (sscanf(line, "SELECT_NUM %d", &n) == 1) T_DoSelectNum(n);
        else if (!strcmp(line, "ATTACK"))  T_DoAttack();
        else if (!strcmp(line, "CANCEL"))  T_DoCancel();
        else if (sscanf(line, "ASSERT_SEL %d", &n) == 1)
        {
            if (turnctrl.selected_target == n - 1)
                printf("[TURN] ASSERT_SEL %d: PASS\n", n);
            else
                printf("[TURN] ASSERT_SEL %d: FAIL (have %d)\n",
                       n, turnctrl.selected_target + 1);
        }
        else if (sscanf(line, "ASSERT_TGT %d", &n) == 1)
        {
            T_RefreshTargets();
            if (T_NumTargets() == n)
                printf("[TURN] ASSERT_TGT %d: PASS\n", n);
            else
                printf("[TURN] ASSERT_TGT %d: FAIL (have %d)\n",
                       n, T_NumTargets());
        }
        else if (sscanf(line, "ASSERT_THP_LT %d %d", &n, &m) == 2)
        {
            T_RefreshTargets();
            mobj_t *mo = (n >= 1) ? T_TargetMobj(n - 1) : NULL;
            int hp = mo ? mo->health : -999;
            if (mo && hp < m)
                printf("[TURN] ASSERT_THP_LT %d %d: PASS (hp=%d)\n",
                       n, m, hp);
            else
                printf("[TURN] ASSERT_THP_LT %d %d: FAIL (hp=%d)\n",
                       n, m, hp);
        }
        else if (sscanf(line, "SETSEED %u", &u) == 1)
        {
            T_SetCombatSeed(u);
            printf("[TURN] SETSEED %u: seed=%u\n", u, T_GetCombatSeed());
        }
        else if (sscanf(line, "PREVIEW %d", &n) == 1)
        {
            // Print the previewed hit% and damage range for target n.
            // The resolver uses the same functions, so these are the
            // resolved values.
            mobj_t *mo = (n >= 1) ? T_TargetMobj(n - 1) : NULL;
            if (mo)
            {
                t_combatstats_t st;
                int dmin, dmax;
                player_t *pl = &players[consoleplayer];
                T_DeriveStats(pl, &st);
                T_DamageRange(pl, mo, &st, &dmin, &dmax);
                printf("[TURN] PREVIEW %d: HIT=%d DMG=%d-%d TP=%d\n",
                       n, T_HitChance(pl, mo, &st), dmin, dmax,
                       T_CostFor(TA_ATTACK));
            }
            else
                printf("[TURN] PREVIEW %d: FAIL (no target)\n", n);
        }
        else if (sscanf(line, "ASSERT_LASTHIT %d", &n) == 1)
        {
            // Verify the last T_ResolveAttack outcome: 1 hit, 0 miss.
            if (t_last_hit == n)
                printf("[TURN] ASSERT_LASTHIT %d: PASS (dmg=%d crit=%d)\n",
                       n, t_last_damage, t_last_crit);
            else
                printf("[TURN] ASSERT_LASTHIT %d: FAIL (have %d)\n",
                       n, t_last_hit);
        }
        else if (strncmp(line, "STATS", 5) == 0)
        {
            // Print derived combat stats for verification.
            t_combatstats_t st;
            player_t *pl = &players[consoleplayer];
            T_DeriveStats(pl, &st);
            printf("[TURN] STATS: AD=%d-%d AP=%d ATKTP=%d CRIT=%d%%x%d "
                   "ARMOR=%d MR=%d ACC=%d HASTE=%d%%\n",
                   st.ad_min, st.ad_max, st.ap, st.attack_tp,
                   st.crit_chance, st.crit_mult,
                   st.armor, st.mr, st.accuracy, st.haste);
        }
        else if (sscanf(line, "ASSERT_LASTDMG %d %d", &n, &m) == 2)
        {
            // Verify the last damage was within [n, m].
            if (t_last_damage >= n && t_last_damage <= m)
                printf("[TURN] ASSERT_LASTDMG %d %d: PASS (dmg=%d)\n",
                       n, m, t_last_damage);
            else
                printf("[TURN] ASSERT_LASTDMG %d %d: FAIL (dmg=%d)\n",
                       n, m, t_last_damage);
        }
        else if (sscanf(line, "ASSERT_THP_EQ %d %d", &n, &m) == 2)
        {
            T_RefreshTargets();
            mobj_t *mo = (n >= 1) ? T_TargetMobj(n - 1) : NULL;
            int hp = mo ? mo->health : -999;
            if (mo && hp == m)
                printf("[TURN] ASSERT_THP_EQ %d %d: PASS\n", n, m);
            else
                printf("[TURN] ASSERT_THP_EQ %d %d: FAIL (hp=%d)\n",
                       n, m, hp);
        }
        else if (!strcmp(line, "QUIT"))   { fclose(f); turnctrl.sync = false; printf("[TURN] script done.\n"); fflush(stdout); T_ProfileSave(); exit(0); }
        else if (line[0] == 0 || line[0] == '#') continue;
        else printf("[TURN] unknown script token: %s\n", line);
    }

    fclose(f);
    turnctrl.sync = false;
    printf("[TURN] script done.\n");
    T_DumpState("script-done");
}
