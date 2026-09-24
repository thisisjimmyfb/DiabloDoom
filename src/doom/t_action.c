//
// t_action.c — Turn-based mode: action implementations (gameplay bridge).
//
// Approved discrete commands into the existing player/world systems.
// Phase 1: MOVE (derived, collision-aware, auto-facing), USE, WAIT,
// END TURN (enemy phase). TP costs land in phase 2; attacks in phase 3.

#include <string.h>
#include <stdio.h>

#include "t_turn.h"
#include "doomstat.h"
#include "d_player.h"
#include "g_game.h"
#include "i_system.h"
#include "p_local.h"
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

    P_UseLines(player);
    T_BeginPulse(6, true, true);
    T_DumpState("use");
}

// ------------------------------------------------------------------
// WAIT: pass a little time without acting.
// ------------------------------------------------------------------
void T_DoWait(void)
{
    if (!T_Active())
        return;

    players[consoleplayer].message = "Wait.";
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

    players[consoleplayer].message = "Enemy phase...";
    // Flag before the pulse: in sync (script) mode T_BeginPulse runs
    // T_EndPulse immediately, which needs to see the enemy phase.
    turnctrl.pulse_enemy = true;
    T_BeginPulse(70, false, false); // 2s of monster action, real-time rate
    if (!turnctrl.sync)
        turnctrl.state = TS_REACTION;
    T_DumpState("end-turn");
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
            { MT_TROOP,      384,  256 },
            { MT_TROOP,      384, -256 },
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
//
// Pulses run synchronously so scripts are fast and deterministic.
// ------------------------------------------------------------------
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
        // strip trailing newline
        line[strcspn(line, "\r\n")] = 0;

        if (!strcmp(line, "MOVE_N"))      T_DoMove(0);
        else if (!strcmp(line, "MOVE_E")) T_DoMove(1);
        else if (!strcmp(line, "MOVE_S")) T_DoMove(2);
        else if (!strcmp(line, "MOVE_W")) T_DoMove(3);
        else if (!strcmp(line, "USE"))    T_DoUse();
        else if (!strcmp(line, "WAIT"))   T_DoWait();
        else if (!strcmp(line, "END"))    T_DoEndTurn();
        else if (!strcmp(line, "DUMP"))   T_DumpState("script");
        else if (!strcmp(line, "QUIT"))   { fclose(f); turnctrl.sync = false; printf("[TURN] script done.\n"); I_Quit(); return; }
        else if (line[0] == 0 || line[0] == '#') continue;
        else printf("[TURN] unknown script token: %s\n", line);
    }

    fclose(f);
    turnctrl.sync = false;
    printf("[TURN] script done.\n");
    T_DumpState("script-done");
}
