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

// Baseline movement execution delay, in tics (35/sec). Each queued
// step holds the progress phase this long; the movespeed modifier
// scales it down. Tune for game feel.
#define T_MOVE_BASE_TICS 12
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
#define STEP_LEN (24 * FRACUNIT)

// Movespeed-scaled step distance: higher movespeed = longer steps.
// Used by both T_PreviewMove and T_ExecMove so preview matches execution.
static fixed_t T_StepLen(player_t *player)
{
    extern int D_MoveSpeed(struct player_s *pl);
    int movespeed = D_MoveSpeed(player);
    if (movespeed <= 0)
        return STEP_LEN;
    return (STEP_LEN * (100 + movespeed)) / 100;
}

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
// Ranged weapons cost 0 TP -- ammo (or heat) is the limiter, so a round
// can dump the whole magazine. Melee (pistol-whip = no Diablo weapon, or
// chainsaw) costs TP.
int T_AttackCost(void)
{
    // All attacks are TP-free; the weapon's ammo system gates spam.
    return 0;
}

// Forward declaration: defined below.
static boolean T_Enqueue(turnaction_t action, int cost, angle_t moveangle,
                         mobj_t *target);

// Continuous weapons (chainsaw, chaingun, plasma) lock onto a target
// and keep firing until it dies or the player switches targets.
boolean T_IsContinuousWeapon(weapontype_t weapon)
{
    return weapon == wp_chainsaw || weapon == wp_chaingun
        || weapon == wp_plasma;
}

// Continuous-weapon target lock: auto-queue an attack if the locked
// target is still alive. Called at the start of each player turn.
// The player is free to move; the weapon keeps firing.
// Lock breaks on: target death, ammo depletion, or overheat.
void T_AutoFireLocked(void)
{
    mobj_t *lt;
    player_t *pl;
    if (!T_Active())
        return;
    if (turnctrl.locked_target == NULL)
        return;
    lt = turnctrl.locked_target;
    pl = &players[consoleplayer];
    if (lt->health <= 0 || !(lt->flags & MF_SHOOTABLE))
    {
        // Target died: clear the lock.
        turnctrl.locked_target = NULL;
        return;
    }
    if (!T_IsContinuousWeapon(pl->readyweapon))
    {
        // Switched to a non-continuous weapon: clear the lock.
        turnctrl.locked_target = NULL;
        return;
    }
    if (!T_HasAmmoForKit(pl->readyweapon))
    {
        // Out of ammo: clear the lock.
        turnctrl.locked_target = NULL;
        pl->message = "Out of ammo - lock broken.";
        return;
    }
    if (!T_KitCanFire(pl->readyweapon))
    {
        // Overheated or otherwise unable to fire: clear the lock.
        turnctrl.locked_target = NULL;
        pl->message = T_KitDenyReason(pl->readyweapon);
        return;
    }
    T_Enqueue(TA_ATTACK, 0, 0, lt);
}

int T_CostFor(turnaction_t action)
{
    switch (action)
    {
      case TA_MOVE_N: case TA_MOVE_S: case TA_MOVE_E: case TA_MOVE_W:
        return 1;
      case TA_USE:
        return 0;
      case TA_ATTACK:
        return T_AttackCost();
      case TA_TURN_L: case TA_TURN_R:
        // View turning is view control, not a game action: free, 0 TP.
        return 0;
      case TA_ABILITY: // per-kit costs land in phase 5
        return 3;
      case TA_POTION: // consumable use cost lands with the Diablo bridge
        return 2;
      case TA_SELECT_NUM:
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

// Refuse an attack the weapon's magazine can't afford: message, no state
// change, no pulse. Mirrors T_RefuseTP — ammo is validated at queue time
// against the current magazine, like TP.
static void T_RefuseMana(weapontype_t w)
{
    static char msg[64];
    int need = T_KitForWeapon(w)->ammo_cost;
    int have = T_KitAmmo(w);
    M_snprintf(msg, sizeof(msg), "NEED %d AMMO (HAVE %d).", need, have);
    players[consoleplayer].message = msg;
    printf("[TURN] refused attack: need %d ammo, have %d\n", need, have);
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
      default:
        M_snprintf(buf, buflen, "?");
        break;
    }
}

// Preview: move the player mobj along a world-space angle without any
// game logic (no pulse, no TP). Used during planning so the first-person
// camera reflects queued moves before END TURN commits them.
static void T_PreviewMove(angle_t moveangle);

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
    // Preview: reflect queued moves in the first-person camera immediately,
    // so the player sees where the queue goes before END TURN commits.
    if (action == TA_MOVE_N || action == TA_MOVE_S
        || action == TA_MOVE_E || action == TA_MOVE_W)
        T_PreviewMove(moveangle);
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
//
// Queue model: the world-space step direction is snapshotted NOW (from
// the facing at queue time), so the queue reads concretely even if the
// view turns later. Execution keeps the existing collision-aware
// partial-progress behavior.
// ------------------------------------------------------------------

// Auto-loot pathfinding: BFS over the 32-unit grid to find the nearest
// Diablo loot and queue moves to reach it. Uses P_CheckPosition for
// non-destructive walkability tests.
// ------------------------------------------------------------------

#define LOOT_GRID (32 * FRACUNIT)
#define BFS_MAX 4096  // max grid cells to explore

typedef struct {
    int gx, gy;      // grid coords
    int parent;      // index into bfs_nodes, -1 for start
    int dir_from_parent; // 0=N,1=E,2=S,3=W (direction moved to get here)
} bfs_node_t;

static bfs_node_t bfs_nodes[BFS_MAX];
static int bfs_visited[256][256]; // visited grid, offset by 128
static int bfs_q[BFS_MAX];

// Convert world coords to grid coords (centered at 0,0)
static void T_WorldToGrid(fixed_t x, fixed_t y, int *gx, int *gy)
{
    *gx = (int)(x / LOOT_GRID);
    *gy = (int)(y / LOOT_GRID);
}

// Check if a grid cell is walkable (using P_CheckPosition)
static boolean T_GridWalkable(mobj_t *mo, int gx, int gy)
{
    fixed_t x = (fixed_t)gx * LOOT_GRID + LOOT_GRID / 2;
    fixed_t y = (fixed_t)gy * LOOT_GRID + LOOT_GRID / 2;
    return P_CheckPosition(mo, x, y);
}

// Find the nearest Diablo loot mobj to the player. Returns NULL if none.
static mobj_t *T_FindNearestLoot(mobj_t *player_mo)
{
    mobj_t *mo;
    mobj_t *best = NULL;
    fixed_t best_dist = 0x7fffffff; // MAXINT
    thinker_t *th;

    for (th = thinkercap.next; th != &thinkercap; th = th->next)
    {
        if (th->function.acp1 != (actionf_p1)P_MobjThinker)
            continue;
        mo = (mobj_t *)th;
        if (mo->type < MT_LOOT_NORMAL || mo->type > MT_LOOT_UNIQUE)
            continue;
        // Skip if already picked up (removed)
        if (mo->health <= 0)
        {
            printf("[COLLECT] loot type=%d at (%d,%d) skipped: health=%d\n",
                   mo->type, mo->x >> 16, mo->y >> 16, mo->health);
            continue;
        }
        {
            fixed_t dx = mo->x - player_mo->x;
            fixed_t dy = mo->y - player_mo->y;
            // Approximate distance squared (avoid sqrt)
            fixed_t dist = (dx >> 16) * (dx >> 16) + (dy >> 16) * (dy >> 16);
            if (dist < best_dist)
            {
                best_dist = dist;
                best = mo;
            }
        }
    }
    return best;
}

// Public: collect live Diablo loot mobjs into list (up to max). Returns count.
int T_GetLootList(mobj_t **list, int max)
{
    thinker_t *th;
    mobj_t *mo;
    int count = 0;
    for (th = thinkercap.next; th != &thinkercap; th = th->next)
    {
        if (th->function.acp1 != (actionf_p1)P_MobjThinker)
            continue;
        mo = (mobj_t *)th;
        if (mo->type < MT_LOOT_NORMAL || mo->type > MT_LOOT_UNIQUE)
            continue;
        if (mo->health <= 0)
            continue;
        if (count < max)
            list[count++] = mo;
    }
    return count;
}

// Public: check if any Diablo loot exists on the map (for the HUD prompt).
boolean T_LootAvailable(void)
{
    player_t *player = &players[consoleplayer];
    thinker_t *th;
    mobj_t *mo;
    if (player->mo == NULL)
        return false;
    if (T_FindNearestLoot(player->mo) != NULL)
        return true;
    // Also show prompt for player corpses (recover via G/COLLECT or walk-over).
    for (th = thinkercap.next; th != &thinkercap; th = th->next)
    {
        if (th->function.acp1 != (actionf_p1)P_MobjThinker)
            continue;
        mo = (mobj_t *)th;
        if (mo->is_player_corpse)
            return true;
    }
    return false;
}

// BFS from player to target coords. Returns number of steps in path, or -1
// if no path. Fills path_dirs with directions (0=N,1=E,2=S,3=W).
static int T_FindPathTo(mobj_t *player_mo, fixed_t tx, fixed_t ty,
                        int *path_dirs, int max_steps)
{
    int pgx, pgy, tgx, tgy;
    int qhead = 0, qtail = 0;
    int i, found = -1;
    // Direction offsets: N=(0,1), E=(1,0), S=(0,-1), W=(-1,0) in grid coords
    // Note: Doom Y increases north
    static const int dx[4] = { 0, 1, 0, -1 };
    static const int dy[4] = { 1, 0, -1, 0 };

    T_WorldToGrid(player_mo->x, player_mo->y, &pgx, &pgy);
    T_WorldToGrid(tx, ty, &tgx, &tgy);

    // Clear visited (offset by 128 to handle negative coords)
    memset(bfs_visited, 0, sizeof(bfs_visited));

    // Start node
    bfs_nodes[0].gx = pgx;
    bfs_nodes[0].gy = pgy;
    bfs_nodes[0].parent = -1;
    bfs_nodes[0].dir_from_parent = -1;
    bfs_q[qtail++] = 0;
    bfs_visited[pgx + 128][pgy + 128] = 1;

    while (qhead < qtail && qtail < BFS_MAX)
    {
        int cur_idx = bfs_q[qhead++];
        bfs_node_t *cur = &bfs_nodes[cur_idx];

        // Check if we're adjacent to target (within 1 grid cell)
        // Pickup radius: allow stopping adjacent, not exactly on top
        if (abs(cur->gx - tgx) <= 1 && abs(cur->gy - tgy) <= 1)
        {
            found = cur_idx;
            break;
        }

        // Explore neighbors
        for (i = 0; i < 4; i++)
        {
            int ngx = cur->gx + dx[i];
            int ngy = cur->gy + dy[i];
            int vix = ngx + 128, viy = ngy + 128;

            if (vix < 0 || vix >= 256 || viy < 0 || viy >= 256)
                continue;
            if (bfs_visited[vix][viy])
                continue;
            if (!T_GridWalkable(player_mo, ngx, ngy))
                continue;

            bfs_visited[vix][viy] = 1;
            bfs_nodes[qtail].gx = ngx;
            bfs_nodes[qtail].gy = ngy;
            bfs_nodes[qtail].parent = cur_idx;
            bfs_nodes[qtail].dir_from_parent = i;
            bfs_q[qtail] = qtail;
            qtail++;
        }
    }

    if (found < 0)
        return -1;

    // Reconstruct path (reverse from found to start)
    {
        int path_len = 0;
        int cur = found;
        int rev_dirs[BFS_MAX];

        while (bfs_nodes[cur].parent >= 0 && path_len < BFS_MAX)
        {
            rev_dirs[path_len++] = bfs_nodes[cur].dir_from_parent;
            cur = bfs_nodes[cur].parent;
        }

        // Reverse into path_dirs (and cap at max_steps)
        {
            int n = path_len < max_steps ? path_len : max_steps;
            for (i = 0; i < n; i++)
                path_dirs[i] = rev_dirs[path_len - 1 - i];
            return n;
        }
    }
}

// ------------------------------------------------------------------
// GOTO driver command: pathfind to (x,y) and walk there over multiple
// tics. The worker sends "GOTO x y" (map units); the game BFS-pathfinds
// on the 32-unit grid and steps through waypoints using P_TryMove for
// collision. A new GOTO overrides; reaching the target clears the state.
// ------------------------------------------------------------------

#define GOTO_MAX_WP 64

static fixed_t goto_wp_x[GOTO_MAX_WP];
static fixed_t goto_wp_y[GOTO_MAX_WP];
static int goto_wp_count = 0;
static int goto_wp_index = 0;
static boolean goto_active = false;

void T_GotoStart(fixed_t tx, fixed_t ty)
{
    player_t *pl = &players[consoleplayer];
    int path_dirs[GOTO_MAX_WP];
    int steps, i;
    int gx, gy;

    goto_active = false;
    goto_wp_count = 0;
    goto_wp_index = 0;

    if (pl->mo == NULL)
        return;

    steps = T_FindPathTo(pl->mo, tx, ty, path_dirs, GOTO_MAX_WP);
    if (steps < 0)
    {
        printf("[GOTO] no path to (%d,%d)\n", tx >> 16, ty >> 16);
        return;
    }

    // Convert direction steps to world-coordinate waypoints.
    T_WorldToGrid(pl->mo->x, pl->mo->y, &gx, &gy);
    {
        static const int dx[4] = { 0, 1, 0, -1 };
        static const int dy[4] = { 1, 0, -1, 0 };
        for (i = 0; i < steps; i++)
        {
            gx += dx[path_dirs[i]];
            gy += dy[path_dirs[i]];
            goto_wp_x[i] = (fixed_t)gx * LOOT_GRID + LOOT_GRID / 2;
            goto_wp_y[i] = (fixed_t)gy * LOOT_GRID + LOOT_GRID / 2;
        }
    }
    goto_wp_count = steps;
    goto_wp_index = 0;
    goto_active = true;
    printf("[GOTO] path to (%d,%d): %d waypoints\n", tx >> 16, ty >> 16, steps);
}

void T_GotoCancel(void)
{
    goto_active = false;
    goto_wp_count = 0;
    goto_wp_index = 0;
}

boolean T_GotoActive(void)
{
    return goto_active;
}

// Called each tick in explore mode. Steps toward the current waypoint.
void T_GotoTick(void)
{
    player_t *pl = &players[consoleplayer];
    mobj_t *mo;
    fixed_t wx, wy, dx, dy;
    fixed_t dist_sq;
    fixed_t step = 8 * FRACUNIT; // units per tic
    fixed_t nx, ny;

    if (!goto_active)
        return;
    if (pl->mo == NULL)
    {
        T_GotoCancel();
        return;
    }
    mo = pl->mo;

    if (goto_wp_index >= goto_wp_count)
    {
        printf("[GOTO] arrived\n");
        T_GotoCancel();
        return;
    }

    wx = goto_wp_x[goto_wp_index];
    wy = goto_wp_y[goto_wp_index];
    dx = wx - mo->x;
    dy = wy - mo->y;
    dist_sq = (dx >> 16) * (dx >> 16) + (dy >> 16) * (dy >> 16);

    // Within 16 units: advance to next waypoint.
    if (dist_sq < 256)
    {
        goto_wp_index++;
        return;
    }

    // Step toward waypoint (normalized to `step` length).
    {
        fixed_t dist = P_AproxDistance(dx, dy);
        if (dist > 0)
        {
            nx = mo->x + FixedDiv(FixedMul(dx, step), dist);
            ny = mo->y + FixedDiv(FixedMul(dy, step), dist);
            // P_TryMove handles collision and sliding; also triggers
            // P_TouchSpecialThing for item pickup via the normal path.
            if (P_TryMove(mo, nx, ny))
            {
                // Face movement direction for the viewmodel/HUD.
                mo->angle = R_PointToAngle2(mo->x, mo->y, nx, ny);
            }
            else
            {
                // Blocked: abort the path rather than grinding a wall.
                printf("[GOTO] blocked at waypoint %d, aborting\n", goto_wp_index);
                T_GotoCancel();
                return;
            }
        }
    }
}

// Auto-collect: find nearest loot, path to it, queue the moves.
// The pickup happens automatically via P_TouchSpecialThing when the
// player moves onto/adjacent to the loot.
void T_DoCollect(void)
{
    player_t *player = &players[consoleplayer];
    mobj_t *mo = player->mo;
    mobj_t *loot;
    int path_dirs[64];
    int steps, i;
    // Facing-relative direction mapping: T_DoMove(dir) uses
    // dir 0=N,1=E,2=S,3=W relative to facing. Our BFS gives world-space
    // dirs. For simplicity, we convert to facing-relative.
    // Actually, T_DoMove snapshots the world-space angle from facing+dir.
    // We need to compute which dir gives us the world-space direction we want.

    if (!T_Active() || mo == NULL)
    {
        printf("[COLLECT] blocked: T_Active=%d mo=%p\n", T_Active(), (void*)mo);
        return;
    }
    if (!T_ActorAlive())
    {
        printf("[COLLECT] blocked: actor not alive\n");
        return;
    }
    if (T_InPulse() || turnctrl.executing)
    {
        printf("[COLLECT] blocked: in_pulse=%d executing=%d\n",
               T_InPulse(), turnctrl.executing);
        return;
    }
    // COLLECT only works in explore mode, not during turn-based combat.
    if (!T_InExplore())
    {
        printf("[COLLECT] blocked: not in explore mode (state=%d)\n",
               turnctrl.state);
        return;
    }

    printf("[COLLECT] attempting: player at (%d,%d)\n",
           mo->x >> 16, mo->y >> 16);
    loot = T_FindNearestLoot(mo);
    if (loot == NULL)
    {
        player->message = "NO LOOT IN SIGHT.";
        return;
    }

    // Try loot targets in order of distance; if the nearest is blocked,
    // move on to the next one.
    {
        // Collect all loot, sorted by distance (simple insertion sort,
        // max 32 items).
        mobj_t *loot_list[32];
        fixed_t loot_dist[32];
        int loot_count = 0;
        thinker_t *th;
        mobj_t *lmo;
        int li, lj;

        for (th = thinkercap.next; th != &thinkercap; th = th->next)
        {
            if (th->function.acp1 != (actionf_p1)P_MobjThinker)
                continue;
            lmo = (mobj_t *)th;
            // Include Diablo loot drops AND player corpses (both recoverable via G/COLLECT).
            if (!((lmo->type >= MT_LOOT_NORMAL && lmo->type <= MT_LOOT_UNIQUE) || lmo->is_player_corpse))
                continue;
            if (lmo->health <= 0 && !lmo->is_player_corpse)
                continue;
            if (loot_count >= 32)
                break;
            {
                fixed_t dx = lmo->x - mo->x;
                fixed_t dy = lmo->y - mo->y;
                fixed_t dist = (dx >> 16) * (dx >> 16) + (dy >> 16) * (dy >> 16);
                // Insertion sort by distance
                for (li = loot_count; li > 0 && loot_dist[li-1] > dist; li--)
                {
                    loot_list[li] = loot_list[li-1];
                    loot_dist[li] = loot_dist[li-1];
                }
                loot_list[li] = lmo;
                loot_dist[li] = dist;
                loot_count++;
            }
        }

        // Try each loot in order until we find a reachable one.
        loot = NULL;
        steps = -1;
        for (li = 0; li < loot_count; li++)
        {
            steps = T_FindPathTo(mo, loot_list[li]->x, loot_list[li]->y, path_dirs, 64);
            if (steps >= 0)
            {
                loot = loot_list[li];
                break;
            }
            printf("[COLLECT] loot %d unreachable, trying next\n", li);
        }
        if (loot == NULL)
        {
            player->message = "CAN'T REACH LOOT.";
            return;
        }
    }
    if (steps == 0)
    {
        // Already there (or adjacent) - pickup will happen via touch
        player->message = "LOOT COLLECTED.";
        return;
    }

    // Move along the path. In explore mode, move the player directly
    // (the turn queue doesn't drain outside combat). Uses P_XYMovement
    // for wall sliding and P_ZMovement for stairs, matching T_ExecMove.
    // BFS dirs are world-space: 0=N(+y),1=E(+x),2=S(-y),3=W(-x).
    {
        static const angle_t world_angles[4] = {
            ANG90,      // N = +y = 90 degrees
            0,          // E = +x = 0 degrees
            ANG270,     // S = -y = 270 degrees
            ANG180      // W = -x = 180 degrees
        };
        for (i = 0; i < steps; i++)
        {
            angle_t moveangle = world_angles[path_dirs[i]];
            fixed_t dx, dy;
            dx = FixedMul(STEP_LEN, finecosine[moveangle >> ANGLETOFINESHIFT]);
            dy = FixedMul(STEP_LEN, finesine[moveangle >> ANGLETOFINESHIFT]);
            mo->momx = dx * STEP_SUB;
            mo->momy = dy * STEP_SUB;
            P_XYMovement(mo);
            mo->momx = mo->momy = 0;
            mo->momz = 0;
            P_ZMovement(mo);
        }
        {
            fixed_t dx = mo->x - loot->x;
            fixed_t dy = mo->y - loot->y;
            fixed_t dist = P_AproxDistance(dx, dy) / FRACUNIT;
            printf("[COLLECT] moved %d steps; dist to loot now %d units (touch needs <32)\n",
                   steps, dist);
        }
    }

    {
        const diablo_itemdef_t *def;
        int item_id = loot->diablo_loot_id;
        static char msg[64];
        def = D_GetItemDef(D_ITEMTIER(item_id), D_ITEMIDX(item_id));
        if (def)
            snprintf(msg, sizeof(msg), "COLLECTING: %s", def->name);
        else
            snprintf(msg, sizeof(msg), "COLLECTING LOOT...");
        player->message = msg;
    }
}

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

// Preview: move the player mobj along a world-space angle without any
// game logic (no pulse, no TP). Used during planning so the first-person
// camera reflects queued moves before END TURN commits them.
static void T_PreviewMove(angle_t moveangle)
{
    player_t *player = &players[consoleplayer];
    mobj_t *mo = player->mo;
    fixed_t dx, dy;

    if (mo == NULL)
        return;
    {
        fixed_t step = T_StepLen(player);
        dx = FixedMul(step, finecosine[moveangle >> ANGLETOFINESHIFT]);
        dy = FixedMul(step, finesine[moveangle >> ANGLETOFINESHIFT]);
    }
    // Preview the destination T_ExecMove will animate toward: scaled STEP_LEN
    // in the move direction, with wall sliding. Matches the smooth execution.
    if (!P_TryMove(mo, mo->x + dx, mo->y + dy))
    {
        mo->momx = dx;
        mo->momy = dy;
        P_SlideMove(mo);
        mo->momx = mo->momy = 0;
    }
    mo->momz = 0;
    // Snap z to the new floor, but limited to one step (24 units). P_TryMove
    // updates floorz but not z; without a snap the preview floats over
    // declining slopes. But snapping the full drop teleports through
    // multi-step stairs and wedges the player inside stair geometry.
    // A single-step snap handles slopes and one stair; larger drops are left
    // for the execution's gravity (P_ZMovement) to settle naturally.
    if (mo->z > mo->floorz)
    {
        fixed_t drop = mo->z - mo->floorz;
        if (drop <= 24 * FRACUNIT)
            mo->z = mo->floorz;
        // else: leave z; the smooth execution will fall during its tics.
    }
    else if (mo->z < mo->floorz)
    {
        // Stepping up: snap up (P_TryMove already validated the step).
        mo->z = mo->floorz;
    }
    // Run z-physics for good measure (viewheight smoothing).
    P_ZMovement(mo);
}

// Executor: step along a snapshotted world-space angle. Collision-
// aware with partial progress (sub-steps avoid tunneling through thin
// lines). Returns true: a settle pulse was begun.
static boolean T_ExecMove(angle_t moveangle)
{
    player_t *player = &players[consoleplayer];
    mobj_t *mo = player->mo;
    fixed_t dx, dy;

    if (mo == NULL)
        return false;

    dx = FixedMul(STEP_LEN, finecosine[moveangle >> ANGLETOFINESHIFT]);
    dy = FixedMul(STEP_LEN, finesine[moveangle >> ANGLETOFINESHIFT]);

    // Smooth movement: don't teleport. Destination is STEP_LEN in the move
    // direction from the current position — the same target T_PreviewMove
    // shows during planning. T_PulseOneTic animates toward it each tick at
    // keyboard speed, easing out on arrival, and ends the pulse when within
    // epsilon. The pulse lasts until arrival (or the failsafe cap), so the
    // player loses control for the full move.
    {
        extern int D_MoveSpeed(struct player_s *pl);
        int movespeed = D_MoveSpeed(player);
        int tics = T_MOVE_BASE_TICS;
        fixed_t dx, dy;
        if (movespeed > 0)
        {
            tics = (T_MOVE_BASE_TICS * 100) / (100 + movespeed);
            if (tics < 1) tics = 1;
        }
        dx = FixedMul(T_StepLen(player), finecosine[moveangle >> ANGLETOFINESHIFT]);
        dy = FixedMul(T_StepLen(player), finesine[moveangle >> ANGLETOFINESHIFT]);
        turnctrl.move_tx = mo->x + dx;
        turnctrl.move_ty = mo->y + dy;
        turnctrl.move_tics = tics * 3; // failsafe: 3x the nominal duration
        // Note: do NOT touch mo->angle here. The move direction is already
        // baked into move_tx/move_ty; facing stays as the player set it.
        // Strafing and backing up must not rotate the view.
    }

    // Run z-physics (stairs, floor snap, smooth viewheight). P_TryMove
    // updates floorz but not z; without this, z lags behind on stairs
    // and the 24-unit step check fails.
    P_ZMovement(mo);

    // Settle pulse: lasts until the smooth move arrives (T_PulseOneTic ends
    // it early on arrival) or the failsafe cap hits. Monsters stay frozen.
    // Movement speed modifier shortens the cap.
    {
        extern int D_MoveSpeed(struct player_s *pl);
        int movespeed = D_MoveSpeed(player);
        int tics = T_MOVE_BASE_TICS;
        if (movespeed > 0)
        {
            tics = (T_MOVE_BASE_TICS * 100) / (100 + movespeed);
            if (tics < 1) tics = 1;
        }
        T_BeginPulse(tics * 3, true, false);
    }
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
    if (T_InPulse() || turnctrl.executing)
        return;

    // Queue the turn: it costs 0 TP (view control) but participates in the
    // turn order. Execution is instant (no smooth animation).
    // Preview: apply the angle immediately so planning shows the result;
    // execution resets to turn_start and re-applies in order.
    T_Enqueue(dir < 0 ? TA_TURN_L : TA_TURN_R, 0, 0, NULL);
    if (dir < 0)
        mo->angle += ANG45;
    else
        mo->angle -= ANG45;
    T_RefreshTargets();
    T_ValidateSelection();
    T_DumpState("turn-queued");
}

// Executor: instant 45-degree turn. No pulse, no animation — the view
// snaps, matching the planning preview.
static boolean T_ExecTurn(boolean left)
{
    player_t *player = &players[consoleplayer];
    mobj_t *mo = player->mo;

    if (mo == NULL)
        return false;

    if (left)
        mo->angle += ANG45; // counterclockwise = left
    else
        mo->angle -= ANG45; // clockwise = right

    T_RefreshTargets();
    T_ValidateSelection();
    T_DumpState("exec-turn");
    return false; // no pulse: instant
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
static void T_NativeFire(player_t *player);

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
        // Empty queue: run a short 0.5s settle phase to let the
        // game breathe, then back to planning. This ensures T always
        // does something visible.
        turnctrl.executing = true;
        // (Ammo trickles in gradually via T_PulseOneTic, not batch.
        // Cooldowns/charges tick every game tic via T_TickCharges,
        // not on turn end.)
        T_BeginPulse(18, false, false);
        players[consoleplayer].message = "Waiting...";
    }
    else
    {
        player_t *player = &players[consoleplayer];
        turnctrl.executing = true;
        // (Ammo trickles in gradually via T_PulseOneTic, not batch.
        // Cooldowns/charges tick every game tic via T_TickCharges,
        // not on turn end.)
        // First-person replay: teleport back to the turn start, then
        // replay the queue. Unlink/relink the mobj so the sector link
        // stays valid for P_TryMove during the replay.
        if (player->mo != NULL)
        {
            P_UnsetThingPosition(player->mo);
            player->mo->x = turnctrl.turn_start_x;
            player->mo->y = turnctrl.turn_start_y;
            player->mo->angle = turnctrl.turn_start_angle;
            P_SetThingPosition(player->mo);
            turnctrl.replay_start_x = turnctrl.turn_start_x;
            turnctrl.replay_start_y = turnctrl.turn_start_y;
            turnctrl.replay_start_angle = turnctrl.turn_start_angle;
        }
        turnctrl.replay_tic = 0;
        players[consoleplayer].message = "Replaying queued actions...";
        // Drain via the original pulse mechanism: T_ExecuteNext processes
        // one entry, settle pulses (3 tics) make each step visible.
        T_ExecuteNext();
    }
    T_DumpState("end-turn");
}

// Drain one queue entry: shift it off and run its executor. Entries
// that begin a settle pulse return true and the drain pauses until
// T_EndPulse; gracefully skipped entries return false and the drain
// continues inline. When the queue is fully drained, the enemy phase
// begins. Called from T_DoEndTurn and T_EndPulse.
// Execute a single queue entry. Returns true if it started a settle
// pulse (drain pauses until T_EndPulse), false otherwise. When the
// queue is empty, ends the replay and begins the enemy phase.
boolean T_ExecuteOne(void)
{
    t_queueentry_t e;
    int i;
    boolean pulsed;

    // Player died: stop executing, clear the queue.
    if (!T_ActorAlive())
    {
        turnctrl.queue_len = 0;
        turnctrl.queue_tp = 0;
        turnctrl.executing = false;
        return false;
    }

    if (turnctrl.queue_len == 0)
    {
        turnctrl.executing = false;
        // Queue empty: check if combat should end.
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
        }
        return false;
    }

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
      case TA_TURN_L:
        pulsed = T_ExecTurn(true);
        break;
      case TA_TURN_R:
        pulsed = T_ExecTurn(false);
        break;
      case TA_ATTACK:
        pulsed = T_ExecAttack(e.target, e.cost);
        break;
      case TA_USE:
        pulsed = T_ExecUse();
        break;
      default:
        pulsed = false;
        break;
    }
    return pulsed;
}

void T_ExecuteNext(void)
{
    while (turnctrl.queue_len > 0)
    {
        if (T_ExecuteOne())
            return; // T_EndPulse resumes the drain
    }
    // Queue drained: run a short 0.5s (18 tics) settle phase to let
    // attacks play out, then exit to planning/explore.
    T_BeginPulse(18, false, false);
    players[consoleplayer].message = "Executing...";
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
    // Preview: reverse the undone move so the camera steps back.
    if (e->action == TA_MOVE_N || e->action == TA_MOVE_S
        || e->action == TA_MOVE_E || e->action == TA_MOVE_W)
        T_PreviewMove(e->moveangle + ANG180);
    // Preview: reverse the undone turn.
    if (players[consoleplayer].mo != NULL)
    {
        if (e->action == TA_TURN_L)
            players[consoleplayer].mo->angle -= ANG45;
        else if (e->action == TA_TURN_R)
            players[consoleplayer].mo->angle += ANG45;
    }
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
    // Preview: teleport the camera back to the turn start.
    // Unlink/relink so the sector link stays valid.
    if (players[consoleplayer].mo != NULL)
    {
        mobj_t *mo = players[consoleplayer].mo;
        P_UnsetThingPosition(mo);
        mo->x = turnctrl.turn_start_x;
        mo->y = turnctrl.turn_start_y;
        mo->angle = turnctrl.turn_start_angle;
        P_SetThingPosition(mo);
    }
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
        // Enter targeting mode even with no targets — the player can
        // hold Ctrl and navigate as targets appear.
        turnctrl.selected_target = -1;
        turnctrl.state = TS_TARGETING;
        players[consoleplayer].message = "No visible targets. (arrows to scan)";
        T_DumpState("select-empty");
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

// ------------------------------------------------------------------
// Hold-Ctrl targeting: hold Ctrl to enter targeting mode, arrows
// navigate (camera snaps to each target), release Ctrl to confirm.
// ------------------------------------------------------------------
static void T_FaceTarget(mobj_t *target);

void T_StartCtrlTargeting(void)
{
    mobj_t *target;
    if (!T_Active() || players[consoleplayer].mo == NULL)
        return;
    if (!T_ActorAlive())
        return;
    // Switching targets breaks the continuous-weapon lock.
    turnctrl.locked_target = NULL;
    // Mini-combat: allow hold-Ctrl in explore mode for barrels/props.
    if (turnctrl.state == TS_EXPLORE)
    {
        // Initialize a single-turn combat. No spotter, no telegraph.
        turnctrl.state = TS_PLANNING;
        turnctrl.round = 1;
        turnctrl.tp = turnctrl.tp_max;
        turnctrl.queue_len = 0;
        turnctrl.queue_tp = 0;
        turnctrl.selected_target = -1;
        turnctrl.executing = false;
        turnctrl.mini_combat = true;
        if (players[consoleplayer].mo != NULL)
        {
            turnctrl.turn_start_x = players[consoleplayer].mo->x;
            turnctrl.turn_start_y = players[consoleplayer].mo->y;
            turnctrl.turn_start_angle = players[consoleplayer].mo->angle;
        }
        players[consoleplayer].message = "Mini-combat: targeting...";
    }
    else if (turnctrl.state != TS_PLANNING)
    {
        return;
    }
    T_RefreshTargets();
    // Enter targeting mode even with no targets — the player can hold
    // Ctrl to look for targets, arrows scan as they appear.
    turnctrl.ctrl_held = true;
    turnctrl.ctrl_targeting = true;
    T_EnterTargeting(0);
    // Snap camera to the first target (if any).
    target = T_TargetMobj(turnctrl.selected_target);
    if (target != NULL)
        T_FaceTarget(target);
    if (turnctrl.selected_target < 0)
        players[consoleplayer].message = "Targeting... (no targets, arrows to scan, release CTRL to cancel)";
    else
        players[consoleplayer].message = "Targeting... (arrows navigate, release CTRL to fire)";
}

void T_CtrlTargetNavigate(int dir)
{
    mobj_t *target;
    int n;
    if (!turnctrl.ctrl_targeting)
        return;
    T_RefreshTargets();
    n = T_NumTargets();
    if (n == 0)
        return;
    if (dir > 0)
        T_EnterTargeting((turnctrl.selected_target + 1) % n);
    else
        T_EnterTargeting((turnctrl.selected_target - 1 + n) % n);
    // Snap camera to the new target.
    target = T_TargetMobj(turnctrl.selected_target);
    if (target != NULL)
        T_FaceTarget(target);
}

void T_ConfirmCtrlTarget(void)
{
    mobj_t *target;
    player_t *player = &players[consoleplayer];
    boolean was_mini;
    turnctrl.ctrl_held = false;
    if (!turnctrl.ctrl_targeting)
        return;
    turnctrl.ctrl_targeting = false;
    was_mini = turnctrl.mini_combat;
    if (!T_Active() || player->mo == NULL)
    {
        if (was_mini) { turnctrl.mini_combat = false; turnctrl.state = TS_EXPLORE; }
        return;
    }
    if (!T_ActorAlive())
    {
        if (was_mini) { turnctrl.mini_combat = false; turnctrl.state = TS_EXPLORE; }
        return;
    }
    // Validate and queue directly, skipping the CONFIRM state.
    // The camera snap was the preview.
    T_ValidateSelection();
    if (turnctrl.selected_target < 0)
    {
        // Mini-combat with no target: allow firing in the facing direction.
        // This lets the player shoot walls (secrets) that aren't targetable
        // mobjs. The vanilla hitscan will hit whatever is in front.
        // Queues the attack but does NOT auto-end — user presses Enter.
        if (was_mini)
        {
            // Queue an untargeted attack.
            if (!T_KitCanFire(player->readyweapon))
            {
                player->message = T_KitDenyReason(player->readyweapon);
                turnctrl.mini_combat = false;
                turnctrl.state = TS_EXPLORE;
                return;
            }
            if (!T_CanAfford(TA_ATTACK))
            {
                T_RefuseTP(TA_ATTACK);
                turnctrl.mini_combat = false;
                turnctrl.state = TS_EXPLORE;
                return;
            }
            if (!T_HasAmmoForKit(player->readyweapon))
            {
                T_RefuseMana(player->readyweapon);
                turnctrl.mini_combat = false;
                turnctrl.state = TS_EXPLORE;
                return;
            }
            T_Enqueue(TA_ATTACK, T_CostFor(TA_ATTACK), 0, NULL);
            player->message = "Queued attack (no target) — press Enter to fire.";
            // Stay in planning; do NOT auto-end. User presses Enter.
            turnctrl.ctrl_targeting = false;
            turnctrl.ctrl_held = false;
            turnctrl.state = TS_PLANNING;
            return;
        }
        if (was_mini) { turnctrl.mini_combat = false; turnctrl.state = TS_EXPLORE; }
        else turnctrl.state = TS_PLANNING;
        return;
    }
    target = T_TargetMobj(turnctrl.selected_target);
    if (target == NULL)
    {
        player->message = "Target lost.";
        if (was_mini) { turnctrl.mini_combat = false; turnctrl.state = TS_EXPLORE; }
        else turnctrl.state = TS_PLANNING;
        return;
    }
    if (!T_KitCanFire(player->readyweapon))
    {
        player->message = T_KitDenyReason(player->readyweapon);
        if (was_mini) { turnctrl.mini_combat = false; turnctrl.state = TS_EXPLORE; }
        else turnctrl.state = TS_PLANNING;
        return;
    }
    if (!T_CanAfford(TA_ATTACK))
    {
        T_RefuseTP(TA_ATTACK);
        if (was_mini) { turnctrl.mini_combat = false; turnctrl.state = TS_EXPLORE; }
        else turnctrl.state = TS_PLANNING;
        return;
    }
    if (!T_HasAmmoForKit(player->readyweapon))
    {
        T_RefuseMana(player->readyweapon);
        if (was_mini) { turnctrl.mini_combat = false; turnctrl.state = TS_EXPLORE; }
        else turnctrl.state = TS_PLANNING;
        return;
    }
    // Commit: enqueue the attack.
    T_Enqueue(TA_ATTACK, T_CostFor(TA_ATTACK), 0, target);
    turnctrl.selected_target = -1;
    turnctrl.state = TS_PLANNING;
    // Mini-combat: do NOT auto-end. User presses Enter to fire.
    // (mini_combat flag stays set; cleared on turn end.)
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

// Invalidate queued attack targets pointing to a removed mobj.
// Called from P_RemoveMobj to prevent dangling-pointer SIGSEGV when
// the queued attack later executes.
void T_InvalidateTarget(mobj_t *mo)
{
    int i;
    if (mo == NULL || !T_Active())
        return;
    for (i = 0; i < turnctrl.queue_len; i++)
    {
        if (turnctrl.queue[i].target == mo)
        {
            turnctrl.queue[i].target = NULL;
            printf("[TURN] queued target invalidated (mobj removed)\n");
        }
    }
    // Clear the continuous-weapon lock if its target was removed.
    if (turnctrl.locked_target == mo)
    {
        turnctrl.locked_target = NULL;
        printf("[TURN] locked target invalidated (mobj removed)\n");
    }
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
        if (target != NULL && !T_KitCanFire(player->readyweapon))
        {
            player->message = T_KitDenyReason(player->readyweapon);
            printf("[TURN] attack refused: %s %s\n",
                   T_KitForWeapon(player->readyweapon)->name,
                   T_KitDenyReason(player->readyweapon));
            return;
        }
        if (!T_CanAfford(TA_ATTACK))
        {
            T_RefuseTP(TA_ATTACK);
            return;
        }
        if (!T_HasAmmoForKit(player->readyweapon))
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
    if (!T_KitCanFire(player->readyweapon))
    {
        player->message = T_KitDenyReason(player->readyweapon);
        printf("[TURN] attack refused: %s %s\n",
               T_KitForWeapon(player->readyweapon)->name,
               T_KitDenyReason(player->readyweapon));
        return;
    }
    if (!T_CanAfford(TA_ATTACK))
    {
        T_RefuseTP(TA_ATTACK);
        return;
    }
    if (!T_HasAmmoForKit(player->readyweapon))
    {
        T_RefuseMana(player->readyweapon);
        return;
    }

    // Commit: snapshot the target actor and enqueue. TP is reserved
    // now; at drain time the attack auto-faces and resolves with the
    // turn-based combat math. The selection is consumed: back to
    // planning so more actions can be queued.
    T_Enqueue(TA_ATTACK, T_CostFor(TA_ATTACK), 0, target);
    // Continuous weapons lock onto the target: keep firing until it
    // dies or the player switches targets. Movement does not break it.
    if (T_IsContinuousWeapon(player->readyweapon))
    {
        turnctrl.locked_target = target;
        turnctrl.lock_fire_cooldown = 0;
    }
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
    boolean untargeted = (target == NULL);

    if (!untargeted)
    {
        if (target->health <= 0 || !(target->flags & MF_SHOOTABLE))
        {
            players[consoleplayer].message = "Target down - attack skipped.";
            printf("[TURN] queued attack skipped: target invalid (+%d TP)\n",
                   cost);
            turnctrl.tp += cost;
            T_DumpState("exec-attack-skip");
            return false;
        }
    }
    // Untargeted attacks (mini-combat, no target) skip validation and fire
    // in the facing direction. Used for shootable walls/secrets.
    if (!T_KitCanFire(player->readyweapon))
    {
        players[consoleplayer].message = T_KitDenyReason(player->readyweapon);
        printf("[TURN] queued attack skipped: %s (+%d TP)\n",
               T_KitDenyReason(player->readyweapon), cost);
        turnctrl.tp += cost;
        T_DumpState("exec-attack-skip");
        return false;
    }

    // Ammo is spent on firing: deducted at execution, not at queue time.
    // An earlier queued action may have spent the magazine since
    // validation; skip gracefully with a TP refund like the other
    // skip paths.
    {
        const t_kitdef_t *kit = T_KitForWeapon(player->readyweapon);
        if (kit->ammo_cost > 0)
        {
            if (T_KitAmmo(player->readyweapon) < kit->ammo_cost)
            {
                players[consoleplayer].message = "Not enough ammo - skipped.";
                printf("[TURN] queued attack skipped: need %d ammo, have %d (+%d TP)\n",
                       kit->ammo_cost, T_KitAmmo(player->readyweapon), cost);
                turnctrl.tp += cost;
                T_DumpState("exec-attack-skip");
                return false;
            }
            T_KitSpendAmmo(player->readyweapon);
        }
    }

    // Kit setup: heat buildup, charge consumption, cooldown (with Haste
    // and flat CDR). Must happen on fire, not on hit.
    T_KitOnFire(player);

    // Face the target (instant for aiming; the smooth visual turn is
    // a follow-up). Untargeted attacks fire in the current facing.
    if (!untargeted)
        T_FaceTarget(target);

    // Natural simulation: Doom's hitscan/projectile determines hit/miss.
    // The Diablo damage callback (installed in T_KitResetCadence) computes
    // AD/AP damage on hit. No instant T_RollDiabloDamage.
    // For hitscan weapons, set the weapon (synchronous; cleared after).
    // Projectiles store it on the mobj (see P_SpawnPlayerMissile).
    {
        weapontype_t w = player->readyweapon;
        boolean is_hitscan = (w == wp_fist || w == wp_pistol || w == wp_shotgun ||
                              w == wp_chaingun || w == wp_chainsaw ||
                              w == wp_supershotgun);
        if (is_hitscan)
            d_diablo_hitscan_weapon = w;
    }
    // Reset hit tracking before native fire; the damage callback sets it.
    t_last_hit_mobj = NULL;
    t_last_damage = 0;
    t_last_crit = 0;
    T_NativeFire(player);
    d_diablo_hitscan_weapon = -1;

    // For hitscan, the callback ran synchronously; t_last_damage > 0 means hit.
    // For projectiles, the hit happens later (callback fires on impact).
    {
        const t_kitdef_t *kit = T_KitForWeapon(player->readyweapon);
        boolean is_hitscan = (player->readyweapon == wp_fist ||
                              player->readyweapon == wp_pistol ||
                              player->readyweapon == wp_shotgun ||
                              player->readyweapon == wp_chaingun ||
                              player->readyweapon == wp_chainsaw ||
                              player->readyweapon == wp_supershotgun);
        if (is_hitscan)
        {
            if (t_last_damage <= 0)
            {
                player->message = "MISS!";
                printf("[TURN] attack resolved: MISS (natural)\n");
                // World-space floating MISS (old target-marker system).
                {
                    extern int t_last_miss_tics;
                    extern mobj_t *t_last_miss_mobj;
                    // Use the attack's target (not t_last_hit_mobj, which is
                    // NULL on a clean miss).
                    if (target != NULL)
                    {
                        t_last_miss_mobj = target;
                        t_last_miss_tics = 25;
                    }
                }
            }
            else
            {
                printf("[TURN] attack resolved: hits=1 dmg=%d crit=%d (natural)\n",
                       t_last_damage, t_last_crit);
                if (t_last_crit)
                    player->message = "CRITICAL HIT!";
                else
                    player->message = "HIT!";
                // World-space floating damage number (old target-marker system).
                {
                    extern int t_last_hit_tics;
                    extern mobj_t *t_last_hit_mobj;
                    // t_last_hit_mobj is set by the damage callback.
                    if (t_last_hit_mobj != NULL)
                        t_last_hit_tics = 25;
                }
                // Untargeted attacks: use the mobj actually hit (tracked by
                // the damage callback). If it died, resolve the kill for
                // loot. Walls have no mobj, so no kill resolution.
                // Barrels (and other non-monster shootables) explode but
                // don't count as kills — skip T_ResolveKill to avoid
                // use-after-free on the removed mobj.
                if (untargeted)
                {
                    mobj_t *hit = t_last_hit_mobj;
                    if (hit && hit->type != MT_BARREL && hit->health <= 0)
                    {
                        T_ResolveKill(player, hit, t_last_damage, kit->name);
                    }
                    t_last_hit_mobj = NULL;
                }
                else if (target->type != MT_BARREL && target->health <= 0)
                {
                    T_ResolveKill(player, target, t_last_damage, kit->name);
                }
            }
        }
        else
        {
            // Projectile: in flight. Damage/kill handled on impact via callback.
            player->message = "FIRED!";
            printf("[TURN] projectile fired (natural simulation)\n");
        }
    }

    // The world changed; rebuild targets for the HUD.
    T_RefreshTargets();
    // Kill moment: a death beat freezes living monsters for ~2s (their AI
    // stays down) while the corpse's death animation plays out under the
    // kill banner — screenshot-able. The drain resumes at T_EndPulse.
    if (T_LastKill())
    {
        player->damagecount = 10; // kill flash; decays during the beat
        T_BeginPulse(70, true, false);
        T_DumpState("exec-attack-kill");
        return true;
    }
    // Attack speed delay: each command execution takes time based on the
    // weapon's attack_tics. This represents the fire animation duration.
    {
        const t_kitdef_t *kit = T_KitForWeapon(player->readyweapon);
        if (kit->attack_tics > 0)
            T_BeginPulse(kit->attack_tics, true, false);
    }
    T_DumpState("exec-attack");
    return false;
}

// Trigger the native Doom weapon fire function for visuals.
// The damage is overridden via t_damage_override (see P_DamageMobj).
// This spawns real projectiles / hitscan, plays sound and muzzle flash.
static void T_NativeFire(player_t *player)
{
    // Declarations for the native fire functions (from p_pspr.c).
    // They take (player, pspdef) but psp is unused for the core fire
    // logic; we pass NULL and handle the flash separately if needed.
    extern void A_Punch(player_t *, pspdef_t *);
    extern void A_FirePistol(player_t *, pspdef_t *);
    extern void A_FireShotgun(player_t *, pspdef_t *);
    extern void A_FireCGun(player_t *, pspdef_t *);
    extern void A_FireMissile(player_t *, pspdef_t *);
    extern void A_FirePlasma(player_t *, pspdef_t *);
    extern void A_FireBFG(player_t *, pspdef_t *);
    extern void A_Saw(player_t *, pspdef_t *);
    extern void A_FireShotgun2(player_t *, pspdef_t *);

    switch (player->readyweapon)
    {
        case wp_fist:        A_Punch(player, NULL); break;
        case wp_pistol:      A_FirePistol(player, NULL); break;
        case wp_shotgun:
            // Doom 1 WAD SSG fallback: Diablo SSG uses 20-pellet fire
            // even with shotgun sprites. A_FireShotgun2 uses the
            // readyweapon's flashstate (shotgun, safe) for the muzzle flash.
            {
                int id = player->diablo_equipped[ESLOT_WEAPON];
                boolean is_ssg = false;
                if (id != D_NOITEM)
                {
                    const diablo_itemdef_t *def =
                        D_GetItemDef(D_ITEMTIER(id), D_ITEMIDX(id));
                    if (def && def->doomweapon == wp_supershotgun)
                        is_ssg = true;
                }
                if (is_ssg)
                    A_FireShotgun2(player, NULL);
                else
                    A_FireShotgun(player, NULL);
            }
            break;
        case wp_chaingun:
            // A_FireCGun uses psp->state to select the flash frame; pass a
            // dummy pspdef pointing at S_CHAIN1 (T_NativeFire passes NULL).
            {
                pspdef_t dummy_psp;
                dummy_psp.state = &states[S_CHAIN1];
                A_FireCGun(player, &dummy_psp);
            }
            break;
        case wp_missile:     A_FireMissile(player, NULL); break;
        case wp_plasma:      A_FirePlasma(player, NULL); break;
        case wp_bfg:         A_FireBFG(player, NULL); break;
        case wp_chainsaw:    A_Saw(player, NULL); break;
        case wp_supershotgun: A_FireShotgun2(player, NULL); break;
        default:             A_FirePistol(player, NULL); break;
    }
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

// Test: remove every monster (simulates killing them all).
static void T_TestKillAll(void)
{
    thinker_t *th, *next;

    for (th = thinkercap.next; th != &thinkercap; th = next)
    {
        mobj_t *mo;
        next = th->next;
        if (th->function.acp1 != (actionf_p1)P_MobjThinker)
            continue;
        mo = (mobj_t *)th;
        if (mo->player != NULL)
            continue;
        if (!(mo->flags & MF_COUNTKILL))
            continue;
        P_RemoveMobj(mo);
    }
    printf("[TURN] test: all monsters removed\n");
}

// Test: stealth arena. Clears every monster, then spawns a single imp
// 256 units north of the player FACING AWAY (north). The player stands
// directly behind it: with the acquire==trigger rule, explore detection
// must never fire no matter how many ticks pass.
static void T_TestSneakArena(void)
{
    player_t *player = &players[consoleplayer];
    mobj_t *mo;

    if (!T_Active() || player->mo == NULL)
        return;

    T_TestKillAll();

    mo = P_SpawnMobj(player->mo->x, player->mo->y + 256 * FRACUNIT,
                     ONFLOORZ, MT_TROOP);
    if (mo != NULL)
        mo->angle = ANG90; // facing north, back to the player
    players[consoleplayer].message = "Sneak arena: 1 imp, facing away.";
    printf("[TURN] sneak arena spawned\n");
    T_DumpState("sneak-arena");
}

// ------------------------------------------------------------------
// Action replay (test harness): text scripts drive the controller with
// no mouse events. One token per line:
//
//   MOVE_N | MOVE_E | MOVE_S | MOVE_W | USE | END | DUMP | QUIT
//   UNDO | CLEAR
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
//   ARENA_SNEAK                     (test: 1 imp facing away from player)
//   KILLALL                         (test: remove every monster)
//   ACQUIRE                         (test: force a stale player acquisition)
//   FORCE_EXPLORE                   (test: leave combat, clearing targets)
//   TICK_EXPLORE n                   (test: run n explore ticks synchronously)
//   ASSERT_EXPLORE | ASSERT_COMBAT   (test: still exploring / in combat)
//
// Queue model: MOVE/USE/ATTACK enqueue; END drains the queue FIFO
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
        else if (!strcmp(line, "CADENCE"))
        {
            // Test: print per-weapon cadence state (cooldown / heat /
            // charges / shots fired).
            int w;
            static const char *wnames[] = {
                "FIST", "PISTOL", "SHOTGUN", "CHAINGUN", "ROCKET",
                "PLASMA", "BFG", "SAW", "SSG"
            };
            for (w = 0; w < NUMWEAPONS; w++)
                printf("[TURN] CADENCE %-7s cd=%d heat=%d chg=%d shots=%d\n",
                       wnames[w], T_KitCooldown(w), T_KitHeat(w),
                       T_KitCharges(w), T_KitShots(w));
        }
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
            printf("[TURN] set ammo %d (test)\n", pl->ammo[am_clip]);
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
            T_ScriptLoadFixup();
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
        else if (!strcmp(line, "ARENA_SNEAK")) T_TestSneakArena();
        else if (!strcmp(line, "KILLALL")) T_TestKillAll();
        else if (!strcmp(line, "FORCE_EXPLORE"))
        {
            // Test: leave combat directly (exercises the stand-down
            // clearing: every monster forgets the player).
            T_EnterExplore();
            printf("[TURN] FORCE_EXPLORE done\n");
        }
        else if (!strcmp(line, "ACQUIRE"))
        {
            // Test: force the first living monster to hold the player
            // as its target (a stale acquisition from an old fight).
            thinker_t *th;
            boolean done = false;
            for (th = thinkercap.next; th != &thinkercap && !done;
                 th = th->next)
            {
                mobj_t *mo;
                if (th->function.acp1 != (actionf_p1)P_MobjThinker)
                    continue;
                mo = (mobj_t *)th;
                if (mo->player != NULL || mo->health <= 0)
                    continue;
                if (!(mo->flags & MF_COUNTKILL))
                    continue;
                mo->target = players[consoleplayer].mo;
                mo->threshold = 0;
                printf("[TURN] ACQUIRE: forced acquisition\n");
                done = true;
            }
            if (!done)
                printf("[TURN] ACQUIRE: no living monster found\n");
        }
        else if (sscanf(line, "TICK_EXPLORE %d", &n) == 1)
        {
            // Test: run n explore ticks synchronously (world sim +
            // throttled detection). Stops early if combat triggers.
            int k;
            for (k = 0; k < n && T_InExplore(); k++)
                T_ExploreTick();
            printf("[TURN] TICK_EXPLORE %d: ran %d ticks, %s\n", n, k,
                   T_InExplore() ? "still exploring" : "combat triggered");
        }
        else if (!strcmp(line, "ASSERT_EXPLORE"))
        {
            if (T_InExplore())
                printf("[TURN] ASSERT_EXPLORE: PASS\n");
            else
                printf("[TURN] ASSERT_EXPLORE: FAIL (in combat)\n");
        }
        else if (!strcmp(line, "ASSERT_COMBAT"))
        {
            if (!T_InExplore())
                printf("[TURN] ASSERT_COMBAT: PASS\n");
            else
                printf("[TURN] ASSERT_COMBAT: FAIL (still exploring)\n");
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
