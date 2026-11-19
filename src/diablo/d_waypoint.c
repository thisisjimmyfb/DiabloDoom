//
// d_waypoint.c — the WAYPOINT marker (d_waypoint.h).
//
// Part of the DiabloDoom mod (GPL-2.0-or-later, like Chocolate Doom).

#include <string.h>

#include "d_waypoint.h"
#include "d_colors.h"
#include "d_rlenv.h"
#include "doomstat.h"
#include "i_video.h"
#include "p_local.h"
#include "r_main.h"
#include "r_state.h"
#include "t_turn.h"
#include "v_video.h"

#define WP_REACH   64       // map units: closer than this, it is reached
#define WP_HEIGHT  96       // the pillar's height above the floor, map units
#define WP_WIDTH   16       // its width, map units
#define WP_MIN_W   6        // px: at least 2 px of the policy's 128-wide frame
#define WP_MAX_W   24
#define WP_DASH    4        // px: the dash length while a wall hides it
#define WP_ARROW   9        // px: the edge arrow's half height

static boolean wp_on = false;
static fixed_t wp_x, wp_y;

void D_WaypointSet(int x, int y)
{
    wp_on = true;
    wp_x = x << FRACBITS;
    wp_y = y << FRACBITS;
}

void D_WaypointClear(void)
{
    wp_on = false;
}

boolean D_WaypointGet(int *x, int *y)
{
    if (!wp_on)
        return false;
    *x = wp_x >> FRACBITS;
    *y = wp_y >> FRACBITS;
    return true;
}

void D_WaypointTick(void)
{
    mobj_t *mo = players[consoleplayer].mo;
    int dx, dy;

    if (!wp_on || mo == NULL)
        return;
    dx = (mo->x - wp_x) >> FRACBITS;
    dy = (mo->y - wp_y) >> FRACBITS;
    if (dx * dx + dy * dy > WP_REACH * WP_REACH)
        return;
    wp_on = false;
    RL_Emit("{\"event\":\"waypoint_reached\",\"x\":%d,\"y\":%d,\"gametic\":%d}",
            wp_x >> FRACBITS, wp_y >> FRACBITS, gametic);
}

// A box clipped to the 3D view.
static void WP_Box(int x, int y, int w, int h, int c)
{
    int x0 = viewwindowx, x1 = viewwindowx + scaledviewwidth;
    int y0 = viewwindowy, y1 = viewwindowy + viewheight;

    if (x < x0) { w -= x0 - x; x = x0; }
    if (y < y0) { h -= y0 - y; y = y0; }
    if (x + w > x1) w = x1 - x;
    if (y + h > y1) h = y1 - y;
    if (w > 0 && h > 0)
        V_DrawFilledBox(x, y, w, h, c);
}

// A triangle pointing left (dir -1) or right (dir 1), its tip at
// (tipx, cy).
static void WP_Triangle(int tipx, int cy, int half, int dir, int c)
{
    int r;

    for (r = -half; r <= half; r++)
    {
        int len = half - (r < 0 ? -r : r) + 1;
        WP_Box(dir > 0 ? tipx - half : tipx + half - len + 1, cy + r, len, 1, c);
    }
}

// Whether the player sees the pillar's spot (the engine's own sight
// check, from a stand-in thing standing there).
static boolean WP_InSight(mobj_t *pmo, subsector_t *ss)
{
    mobj_t t;

    memset(&t, 0, sizeof(t));
    t.x = wp_x;
    t.y = wp_y;
    t.z = ss->sector->floorheight;
    t.height = WP_HEIGHT << FRACBITS;
    t.subsector = ss;
    return P_CheckSight(pmo, &t);
}

void D_WaypointDraw(void)
{
    mobj_t *pmo = players[consoleplayer].mo;
    int core = D_Color(DC_WP_CORE), edge = D_Color(DC_WP_EDGE);
    subsector_t *ss;
    fixed_t floor;
    int sx, sbot, stop, w, y;
    double depth;

    if (!wp_on || pmo == NULL || gamestate != GS_LEVEL || automapactive)
        return;
    ss = R_PointInSubsector(wp_x, wp_y);
    floor = ss->sector->floorheight;
    if (T_ProjectPoint(wp_x, wp_y, floor, &sx, &sbot, &depth)
        && T_ProjectPoint(wp_x, wp_y, floor + (WP_HEIGHT << FRACBITS), &sx, &stop, NULL)
        && sx >= 0 && sx < SCREENWIDTH)
    {
        // The pillar: white with a black outline, solid in plain
        // sight, dashed while a wall hides it.
        w = (int) (WP_WIDTH * 160.0 / depth);
        if (w < WP_MIN_W)
            w = WP_MIN_W;
        if (w > WP_MAX_W)
            w = WP_MAX_W;
        WP_Box(sx - w / 2 - 1, stop - 1, w + 2, sbot - stop + 2, edge);
        if (WP_InSight(pmo, ss))
            WP_Box(sx - w / 2, stop, w, sbot - stop, core);
        else
            for (y = stop; y < sbot; y += 2 * WP_DASH)
                WP_Box(sx - w / 2, y, w, y + WP_DASH < sbot ? WP_DASH : sbot - y, core);
        return;
    }
    // Out of view: an arrow at the screen edge on its side.
    {
        angle_t a = R_PointToAngle2(viewx, viewy, wp_x, wp_y) - viewangle;
        int left = a < ANG180;
        int cy = viewwindowy + viewheight / 2;
        int x0 = viewwindowx, x1 = viewwindowx + scaledviewwidth - 1;

        WP_Triangle(left ? x0 : x1, cy, WP_ARROW + 2, left ? -1 : 1, edge);
        WP_Triangle(left ? x0 + 3 : x1 - 3, cy, WP_ARROW, left ? -1 : 1, core);
    }
}
