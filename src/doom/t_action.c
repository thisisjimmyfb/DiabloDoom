//
// t_action.c — Turn-based mode: action implementations (gameplay bridge).
//
// Approved discrete commands into the existing player/world systems.
// Phase 1: MOVE (derived, collision-aware, facing-relative), USE, WAIT,
// END TURN (enemy phase). TP costs land in phase 2; attacks in phase 3.

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "t_turn.h"
#include "t_combat.h"
#include "d_diablo.h"
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
// free. Queue model: TP is RESERVED when an action is enqueued (the
// queue can never hold more than tp_max worth of actions) and refunded
// on undo, clear, or a gracefully skipped entry. An action that cannot
// be afforded is refused at enqueue with a message and changes nothing.
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
      case TA_ATTACK:
        return T_AttackCost();
      case TA_TURN_L: case TA_TURN_R:
        // View turning is view control, not a game action: free, 0 TP.
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

// Refuse an attack the kit can't afford in mana: message, no state change,
// no pulse. Mirrors T_RefuseTP — mana is validated at queue time against
// the current pool, like TP.
static void T_RefuseMana(weapontype_t w)
{
    static char msg[64];
    int need = T_KitForWeapon(w)->mana_cost;
    int have = players[consoleplayer].ammo[am_clip];
    M_snprintf(msg, sizeof(msg), "NEED %d MANA (HAVE %d).", need, have);
    players[consoleplayer].message = msg;
    printf("[TURN] refused attack: need %d mana, have %d\n", need, have);
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
// Queued actions (FIFO). Planning enqueues; END TURN drains.
//
// Every enqueue runs the same validation the immediate model ran at
// commit (TP affordability, cooldowns, target validity) and RESERVES
// TP immediately, so the queue can never hold more than tp_max worth
// of actions. Unaffordable actions stay dimmed/unqueuable.
// ------------------------------------------------------------------

// 8-wind compass name for a world-space angle (for "STEP EAST" etc.).
static const char *T_CompassName(angle_t a)
{
    static const char *names[8] =
        { "E", "NE", "N", "NW", "W", "SW", "S", "SE" };
    return names[((a + ANG45 / 2) >> 29) & 7];
}

// Human-readable queue entry name for the HUD and messages.
void T_QueueEntryName(const t_queueentry_t *e, char *buf, size_t buflen)
{
    if (e == NULL || buf == NULL || buflen == 0)
        return;
    switch (e->action)
    {
      case TA_MOVE_N: case TA_MOVE_S: case TA_MOVE_E: case TA_MOVE_W:
        M_snprintf(buf, buflen, "STEP %s", T_CompassName(e->moveangle));
        break;
      case TA_ATTACK:
        M_snprintf(buf, buflen, "ATTACK %s", e->target_name);
        break;
      case TA_USE:
        M_snprintf(buf, buflen, "USE");
        break;
      case TA_SWAP_WEAPON:
        M_snprintf(buf, buflen, "SWAP");
        break;
      default:
        M_snprintf(buf, buflen, "?");
        break;
    }
}

// Append an action to the queue, reserving its TP. Snapshots (world-
// space move angle, target actor) are taken by the caller at queue time.
static boolean T_Enqueue(turnaction_t action, int cost, angle_t moveangle,
                         mobj_t *target)
{
    t_queueentry_t *e;
    char name[48];
    static char msg[64];

    if (!T_Active())
        return false;
    if (turnctrl.queue_len >= T_QUEUE_MAX)
    {
        players[consoleplayer].message = "Queue is full.";
        printf("[TURN] enqueue refused: queue full\n");
        return false;
    }
    if (turnctrl.queue_tp + cost > turnctrl.tp_max)
    {
        // Belt and braces: T_CanAfford should have gated this already.
        T_RefuseTP(action);
        return false;
    }
    e = &turnctrl.queue[turnctrl.queue_len++];
    e->action = action;
    e->cost = cost;
    e->moveangle = moveangle;
    e->target = target;
    if (action == TA_ATTACK && target != NULL)
        M_StringCopy(e->target_name, T_TargetName(target),
                     sizeof(e->target_name));
    else
        e->target_name[0] = '\0';
    // Reserve TP immediately.
    turnctrl.tp -= cost;
    turnctrl.queue_tp += cost;
    T_QueueEntryName(e, name, sizeof(name));
    M_snprintf(msg, sizeof(msg), "Queued %s (%d TP).", name, cost);
    players[consoleplayer].message = msg;
    printf("[TURN] enqueued %s cost=%d tp_left=%d qlen=%d\n",
           name, cost, turnctrl.tp, turnctrl.queue_len);
    T_DumpState("enqueue");
    return true;
}

// ------------------------------------------------------------------
// MOVE: facing-relative discrete step. Facing never changes here —
// W/S step forward/back, A/D strafe left/right, all relative to the
// current facing. View turning is a separate free action (arrow keys).
// dir: 0=N(forward) 1=E(right) 2=S(back) 3=W(left), relative to facing.
// Doom angles increase counterclockwise (+y is north), so strafing
// right is -90 degrees and strafing left is +90.
//
// Queue model: the world-space step direction is snapshotted NOW (from
// the facing at queue time), so the queue reads concretely even if the
// view turns later. Execution keeps the existing collision-aware
// partial-progress behavior.
// ------------------------------------------------------------------
void T_DoMove(int dir)
{
    player_t *player = &players[consoleplayer];
    mobj_t *mo = player->mo;
    angle_t moveangle;
    static const angle_t diroff[4] = { 0, (angle_t)-ANG90, ANG180, ANG90 };
    static const turnaction_t kinds[4] =
        { TA_MOVE_N, TA_MOVE_E, TA_MOVE_S, TA_MOVE_W };

    if (!T_Active() || mo == NULL)
        return;
    if (!T_ActorAlive())
        return;
    if (T_InPulse() || turnctrl.executing)
        return;
    if (dir < 0 || dir > 3)
        return;
    if (!T_CanAfford(TA_MOVE_N))
    {
        T_RefuseTP(TA_MOVE_N);
        return;
    }

    // No auto-facing: steps are facing-relative and never rotate the
    // view. A/D strafe without swinging the camera; arrows turn it.
    moveangle = mo->angle + diroff[dir];
    T_Enqueue(kinds[dir], T_CostFor(TA_MOVE_N), moveangle, NULL);
}

// Executor: step along a snapshotted world-space angle. Collision-
// aware with partial progress (sub-steps avoid tunneling through thin
// lines). Returns true: a settle pulse was begun.
static boolean T_ExecMove(angle_t moveangle)
{
    player_t *player = &players[consoleplayer];
    mobj_t *mo = player->mo;
    fixed_t dx, dy;
    int k;

    if (mo == NULL)
        return false;

    dx = FixedMul(STEP_LEN, finecosine[moveangle >> ANGLETOFINESHIFT]);
    dy = FixedMul(STEP_LEN, finesine[moveangle >> ANGLETOFINESHIFT]);

    for (k = 0; k < STEP_SUB; k++)
    {
        if (!P_TryMove(mo, mo->x + dx, mo->y + dy))
            break; // blocked: stop, keep partial progress
    }
    mo->momx = mo->momy = 0;
    mo->momz = 0;

    // Settle pulse: sector effects, in-flight odds and ends. Monsters
    // stay frozen — they act on their own phase.
    T_BeginPulse(6, true, true);
    T_DumpState("exec-move");
    return true;
}

// ------------------------------------------------------------------
// TURN: free view rotation. Arrow keys rotate the player's facing by a
// fixed 45-degree increment (8 presses per full rotation — the classic
// Doom snap feel). This is view control, not a game action: 0 TP, no
// pulse, legal in PLANNING/TARGETING/CONFIRM. The target list is
// refreshed so markers track the new facing.
// dir: -1 = turn left (counterclockwise), +1 = turn right.
// ------------------------------------------------------------------
void T_DoTurn(int dir)
{
    player_t *player = &players[consoleplayer];
    mobj_t *mo = player->mo;

    if (!T_Active() || mo == NULL)
        return;
    if (!T_ActorAlive())
        return;
    if (T_InPulse())
        return;

    if (dir < 0)
        mo->angle += ANG45; // counterclockwise = left
    else
        mo->angle -= ANG45; // clockwise = right

    T_RefreshTargets();
    T_ValidateSelection();
    T_DumpState("turn");
}

// ------------------------------------------------------------------
// USE: interact with the line in front (doors, switches).
// Queue model: enqueued with the same affordability validation;
// executes from the player's position at drain time.
// ------------------------------------------------------------------
void T_DoUse(void)
{
    if (!T_Active() || players[consoleplayer].mo == NULL)
        return;
    if (!T_ActorAlive())
        return;
    if (T_InPulse() || turnctrl.executing)
        return;
    if (!T_CanAfford(TA_USE))
    {
        T_RefuseTP(TA_USE);
        return;
    }

    T_Enqueue(TA_USE, T_CostFor(TA_USE), 0, NULL);
}

// Executor: use whatever line is in front at execution time.
// Returns true: a settle pulse was begun.
static boolean T_ExecUse(void)
{
    player_t *player = &players[consoleplayer];

    if (player->mo == NULL)
        return false;
    P_UseLines(player);
    T_BeginPulse(6, true, true);
    T_DumpState("exec-use");
    return true;
}

// Executors (defined below): return true when a settle pulse was
// begun (drain pauses until T_EndPulse), false on a graceful skip
// (drain continues inline, TP refunded). T_ExecMove/T_ExecUse are
// defined above.
static boolean T_ExecAttack(mobj_t *target, int cost);
static boolean T_ExecSwap(int cost);

// ------------------------------------------------------------------
// END TURN: drain the queued actions FIFO — each entry executes through
// its normal implementation with its settle pulse — then the enemy
// phase runs as before, then round bookkeeping. An empty queue skips
// straight to the enemy phase. END TURN is always legal (0 TP) from
// every state.
// ------------------------------------------------------------------
void T_DoEndTurn(void)
{
    if (!T_Active())
        return;
    if (T_InPulse() || turnctrl.executing)
        return;
    if (!T_ActorAlive())
        return;

    // Leaving a selection state: drop the target, back to planning.
    turnctrl.selected_target = -1;
    if (turnctrl.state == TS_TARGETING || turnctrl.state == TS_CONFIRM)
        turnctrl.state = TS_PLANNING;

    if (turnctrl.queue_len == 0)
    {
        // Empty queue: just the enemy phase.
        T_BeginEnemyPhase();
    }
    else
    {
        turnctrl.executing = true;
        players[consoleplayer].message = "Executing queued actions...";
        T_ExecuteNext();
    }
    T_DumpState("end-turn");
}

// Drain one queue entry: shift it off and run its executor. Entries
// that begin a settle pulse return true and the drain pauses until
// T_EndPulse; gracefully skipped entries return false and the drain
// continues inline. When the queue is fully drained, the enemy phase
// begins. Called from T_DoEndTurn and T_EndPulse.
void T_ExecuteNext(void)
{
    while (turnctrl.queue_len > 0)
    {
        t_queueentry_t e;
        int i;
        boolean pulsed;

        e = turnctrl.queue[0];
        for (i = 1; i < turnctrl.queue_len; i++)
            turnctrl.queue[i - 1] = turnctrl.queue[i];
        turnctrl.queue_len--;
        // TP was reserved at enqueue; the reservation now funds this
        // execution. A skipped action refunds it (see executors).
        turnctrl.queue_tp -= e.cost;

        switch (e.action)
        {
          case TA_MOVE_N: case TA_MOVE_S:
          case TA_MOVE_E: case TA_MOVE_W:
            pulsed = T_ExecMove(e.moveangle);
            break;
          case TA_ATTACK:
            pulsed = T_ExecAttack(e.target, e.cost);
            break;
          case TA_USE:
            pulsed = T_ExecUse();
            break;
          case TA_SWAP_WEAPON:
            pulsed = T_ExecSwap(e.cost);
            break;
          default:
            pulsed = false;
            break;
        }
        if (pulsed)
            return; // T_EndPulse resumes the drain
    }
    // Fully drained (skips included): the enemy phase runs as usual.
    turnctrl.executing = false;
    T_BeginEnemyPhase();
}

// ------------------------------------------------------------------
// SWAP WEAPON: 2 TP. Cycle readyweapon to the next owned Doom weapon
// among the six turn-mode kits (pistol..BFG). The Diablo equipment weapon
// item is orthogonal (it grants stats); the six kits live on the Doom
// weapons. Fists are not a kit, so swap never selects them.
//
// Queue model: validated at enqueue (a candidate must exist); the swap
// itself resolves at drain time from the then-current weapon, so two
// queued swaps advance twice.
// ------------------------------------------------------------------
void T_DoSwapWeapon(void)
{
    player_t *player = &players[consoleplayer];
    int w, cand;

    if (!T_Active() || player->mo == NULL)
        return;
    if (!T_ActorAlive())
        return;
    if (T_InPulse() || turnctrl.executing)
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

    T_Enqueue(TA_SWAP_WEAPON, T_CostFor(TA_SWAP_WEAPON), 0, NULL);
}

// Executor: advance to the next owned kit weapon from the current one.
// Returns true: a frozen pulse was begun so the lower/raise animation
// completes via P_PlayerThink.
static boolean T_ExecSwap(int cost)
{
    player_t *player = &players[consoleplayer];
    int w, cand;

    if (player->mo == NULL)
        return false;

    for (w = 1; w <= (wp_bfg - wp_pistol); w++)
    {
        cand = wp_pistol + (player->readyweapon - wp_pistol + w)
                         % (wp_bfg - wp_pistol + 1);
        if (player->weaponowned[cand] && cand != player->readyweapon)
            break;
    }
    if (w > (wp_bfg - wp_pistol) || cand == player->readyweapon)
    {
        // Should not happen (validated at enqueue); skip gracefully.
        printf("[TURN] queued swap skipped: no other weapon\n");
        turnctrl.tp += cost;
        players[consoleplayer].message = "Swap skipped (+2 TP).";
        return false;
    }

    player->pendingweapon = cand;
    // Frozen pulse lets the weapon lower/raise; ammo is topped up so the
    // state machine can never reject the switch for lack of ammo.
    T_BeginPulse(30, true, true);
    printf("[TURN] swapped to weapon %d\n", cand);
    T_DumpState("exec-swap");
    return true;
}

// ------------------------------------------------------------------
// UNDO (Backspace): remove the last queued action and refund its TP.
// CLEAR (Z): drop the whole queue and refund all reserved TP.
// Both are free and legal whenever planning input is live.
// ------------------------------------------------------------------
void T_DoUndoQueue(void)
{
    t_queueentry_t *e;
    char name[48];
    static char msg[64];

    if (!T_Active())
        return;
    if (T_InPulse() || turnctrl.executing)
        return;
    if (turnctrl.queue_len == 0)
    {
        players[consoleplayer].message = "Queue is empty.";
        return;
    }
    e = &turnctrl.queue[--turnctrl.queue_len];
    turnctrl.tp += e->cost;
    turnctrl.queue_tp -= e->cost;
    T_QueueEntryName(e, name, sizeof(name));
    M_snprintf(msg, sizeof(msg), "Undid %s (+%d TP).", name, e->cost);
    players[consoleplayer].message = msg;
    printf("[TURN] undo %s refund=%d tp=%d qlen=%d\n",
           name, e->cost, turnctrl.tp, turnctrl.queue_len);
    T_DumpState("undo");
}

void T_DoClearQueue(void)
{
    int refund;
    static char msg[64];

    if (!T_Active())
        return;
    if (T_InPulse() || turnctrl.executing)
        return;
    if (turnctrl.queue_len == 0)
    {
        players[consoleplayer].message = "Queue is empty.";
        return;
    }
    refund = turnctrl.queue_tp;
    turnctrl.tp += refund;
    turnctrl.queue_tp = 0;
    turnctrl.queue_len = 0;
    M_snprintf(msg, sizeof(msg), "Queue cleared (+%d TP).", refund);
    players[consoleplayer].message = msg;
    printf("[TURN] clear queue refund=%d tp=%d\n", refund, turnctrl.tp);
    T_DumpState("clear-queue");
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
    if (T_InPulse() || turnctrl.executing)
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
        if (!T_HasManaForKit(player->readyweapon))
        {
            T_RefuseMana(player->readyweapon);
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
    if (!T_HasManaForKit(player->readyweapon))
    {
        T_RefuseMana(player->readyweapon);
        return;
    }

    // Commit: snapshot the target actor and enqueue. TP is reserved
    // now; at drain time the attack auto-faces and resolves with the
    // turn-based combat math. The selection is consumed: back to
    // planning so more actions can be queued.
    T_Enqueue(TA_ATTACK, T_CostFor(TA_ATTACK), 0, target);
    turnctrl.selected_target = -1;
    turnctrl.state = TS_PLANNING;
}

// Executor: resolve a queued attack against its snapshotted target.
// The target is validated live: an earlier queued action may have
// killed it (or the weapon may have gone on cooldown), in which case
// the attack skips gracefully with a message and a TP refund.
// Attacks resolve instantly (existing behavior — no settle pulse),
// so this returns false and the drain continues inline.
static boolean T_ExecAttack(mobj_t *target, int cost)
{
    player_t *player = &players[consoleplayer];

    if (target == NULL || target->health <= 0
        || !(target->flags & MF_SHOOTABLE))
    {
        players[consoleplayer].message = "Target down - attack skipped.";
        printf("[TURN] queued attack skipped: target invalid (+%d TP)\n",
               cost);
        turnctrl.tp += cost;
        T_DumpState("exec-attack-skip");
        return false;
    }
    if (!T_KitReady(player->readyweapon))
    {
        players[consoleplayer].message = "Weapon cooling down - skipped.";
        printf("[TURN] queued attack skipped: kit on cooldown (+%d TP)\n",
               cost);
        turnctrl.tp += cost;
        T_DumpState("exec-attack-skip");
        return false;
    }

    // Mana is spent on firing: deducted at execution, not at queue time.
    // An earlier queued action may have spent the pool since validation;
    // skip gracefully with a TP refund like the other skip paths.
    {
        const t_kitdef_t *kit = T_KitForWeapon(player->readyweapon);
        if (kit->mana_cost > 0)
        {
            if (player->ammo[am_clip] < kit->mana_cost)
            {
                players[consoleplayer].message = "Not enough mana - skipped.";
                printf("[TURN] queued attack skipped: need %d mana, have %d (+%d TP)\n",
                       kit->mana_cost, player->ammo[am_clip], cost);
                turnctrl.tp += cost;
                T_DumpState("exec-attack-skip");
                return false;
            }
            player->ammo[am_clip] -= kit->mana_cost;
        }
    }

    T_FaceTarget(target);
    T_ResolveAttack(player, target);
    // The world changed; rebuild targets for the HUD.
    T_RefreshTargets();
    T_DumpState("exec-attack");
    return false;
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
//   MOVE_N | MOVE_E | MOVE_S | MOVE_W | USE | END | DUMP | QUIT
//   SWAP | UNDO | CLEAR
//   ASSERT_TP n | ASSERT_ROUND n     (gate checks; print PASS/FAIL)
//   ASSERT_QUEUE n | ASSERT_QTP n    (queue length / reserved TP)
//   ASSERT_SEL n | ASSERT_TGT n      (selection / target count checks)
//   ASSERT_CRIT n                   (last attack crit: 1/0)
//   SAVE n | LOAD n                  (slots 0-7; synchronous)
//   TGTNAME                       (test: print selected target's name)
//   TURN_L | TURN_R                (test: arrow-key view turn, 45 deg)
//   GIVEWEAPON n                    (test: grant kit weapon n)
//   GIVEITEM t i                    (test: grant Diablo item, equip it)
//   SELECT_NEXT | SELECT_PREV | SELECT_NUM n | ATTACK | CANCEL
//
// Queue model: MOVE/USE/SWAP/ATTACK enqueue; END drains the queue FIFO
// then runs the enemy phase. Pulses run synchronously so scripts are
// fast and deterministic.
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
        else if (!strcmp(line, "END"))    T_DoEndTurn();
        else if (!strcmp(line, "SWAP"))   T_DoSwapWeapon();
        else if (!strcmp(line, "UNDO"))   T_DoUndoQueue();
        else if (!strcmp(line, "CLEAR"))   T_DoClearQueue();
        else if (sscanf(line, "ASSERT_QUEUE %d", &n) == 1)
        {
            if (turnctrl.queue_len == n)
                printf("[TURN] ASSERT_QUEUE %d: PASS\n", n);
            else
                printf("[TURN] ASSERT_QUEUE %d: FAIL (have %d)\n",
                       n, turnctrl.queue_len);
        }
        else if (sscanf(line, "ASSERT_QTP %d", &n) == 1)
        {
            if (turnctrl.queue_tp == n)
                printf("[TURN] ASSERT_QTP %d: PASS\n", n);
            else
                printf("[TURN] ASSERT_QTP %d: FAIL (have %d)\n",
                       n, turnctrl.queue_tp);
        }
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
            printf("[TURN] PROFILE: name='%s' L%d XP=%d SP=%d kills=%d dmg=%d rounds=%d\n",
                   t_profile.name, t_profile.level, t_profile.xp,
                   t_profile.stat_points, t_profile.lifetime_kills,
                   t_profile.lifetime_damage, t_profile.lifetime_rounds);
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
        else if (sscanf(line, "SPAWNPICKUP %d", &n) == 1)
        {
            // Test helper: spawn a real ammo pickup at the player's feet.
            // n: 0=clip, 1=clipbox, 2=shells, 3=cell, 4=backpack.
            mobjtype_t types[5] = { MT_CLIP, MT_MISC17, MT_MISC22, MT_MISC20,
                                     MT_MISC24 };
            mobj_t *mo = players[consoleplayer].mo;
            if (n >= 0 && n < 5 && mo)
            {
                // 32 units ahead of the player: stepping forward (MOVE_N)
                // touches it through the normal P_TryMove path.
                fixed_t nx = mo->x + FixedMul(32*FRACUNIT,
                    finecosine[mo->angle >> ANGLETOFINESHIFT]);
                fixed_t ny = mo->y + FixedMul(32*FRACUNIT,
                    finesine[mo->angle >> ANGLETOFINESHIFT]);
                P_SpawnMobj(nx, ny, ONFLOORZ, types[n]);
            }
        }
        else if (sscanf(line, "SETMANA %d", &n) == 1)
        {
            player_t *pl = &players[consoleplayer];
            pl->ammo[am_clip] = n;
            if (pl->ammo[am_clip] > pl->maxammo[am_clip])
                pl->ammo[am_clip] = pl->maxammo[am_clip];
            if (pl->ammo[am_clip] < 0)
                pl->ammo[am_clip] = 0;
            printf("[TURN] set mana %d (test)\n", pl->ammo[am_clip]);
        }
        else if (sscanf(line, "ASSERT_MAXMANA %d", &n) == 1)
        {
            int have = players[consoleplayer].maxammo[am_clip];
            printf("[TURN] ASSERT_MAXMANA %d: %s (have %d)\n",
                   n, have == n ? "PASS" : "FAIL", have);
        }
        else if (sscanf(line, "ASSERT_MANA %d", &n) == 1)
        {
            int have = players[consoleplayer].ammo[am_clip];
            if (have == n)
                printf("[TURN] ASSERT_MANA %d: PASS\n", n);
            else
                printf("[TURN] ASSERT_MANA %d: FAIL (have %d)\n", n, have);
        }
        else if (sscanf(line, "QUAFF %d", &n) == 1)
        {
            // Test: use the backpack item at index n through the real
            // consumable path (D_UseConsumable).
            D_UseBackpackItem((struct player_s *)&players[consoleplayer], n);
            printf("[TURN] quaffed backpack slot %d (test)\n", n);
        }
        else if (sscanf(line, "GIVEITEM %d %d", &n, &m) == 2)
        {
            // Test: grant a Diablo item (tier idx) and equip it via
            // the real backpack/equip path.
            player_t *pl = &players[consoleplayer];
            if (n >= TIER_NORMAL && n <= TIER_UNIQUE
                && D_ValidItem(n, m)
                && D_BackpackAdd((struct player_s *)pl, n, m))
            {
                D_EquipRecent((struct player_s *)pl);
                printf("[TURN] gave item tier=%d idx=%d (test)\n", n, m);
            }
            else
                printf("[TURN] GIVEITEM failed: tier=%d idx=%d\n", n, m);
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
        else if (!strcmp(line, "TURN_L"))   T_DoTurn(-1);
        else if (!strcmp(line, "TURN_R"))   T_DoTurn(1);
        else if (!strcmp(line, "TGTNAME"))
        {
            // Test: print the display name + HP of the current selection.
            mobj_t *tgmo = T_SelectedMobj();
            printf("[TURN] TGTNAME sel=%d name=%s hp=%d\n",
                   turnctrl.selected_target, T_TargetName(tgmo),
                   tgmo ? tgmo->health : -999);
        }
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
        else if (sscanf(line, "ASSERT_CRIT %d", &n) == 1)
        {
            // Verify the last T_ResolveAttack crit: 1 crit, 0 not.
            if (t_last_crit == n)
                printf("[TURN] ASSERT_CRIT %d: PASS (dmg=%d)\n",
                       n, t_last_damage);
            else
                printf("[TURN] ASSERT_CRIT %d: FAIL (have %d)\n",
                       n, t_last_crit);
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
