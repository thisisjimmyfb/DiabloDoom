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

// See-through, in DC_WP_CORE (green; 2026-11-18; solid with a black outline, and an
// arrow at the screen edge while out of view, before): a level's
// palette entry nearest wp_alpha[level]/256 of DC_WP_CORE over each entry,
// built once.
#define WP_LEVELS 3
static const int wp_alpha[WP_LEVELS] = { 90, 128, 180 };   // the middle .. the sides
static byte wp_tab[WP_LEVELS][256];
static boolean wp_tab_done = false;

static void WP_Column(int x, int top, int bot, int level)
{
    int y;

    if (!wp_tab_done)
    {
        int l, i;
        for (l = 0; l < WP_LEVELS; l++)
            for (i = 0; i < 256; i++)
                wp_tab[l][i] = (byte) D_BlendColor(D_Color(DC_WP_CORE), i, wp_alpha[l]);
        wp_tab_done = true;
    }
    if (x < viewwindowx || x >= viewwindowx + scaledviewwidth)
        return;
    if (top < viewwindowy)
        top = viewwindowy;
    if (bot > viewwindowy + viewheight)
        bot = viewwindowy + viewheight;
    for (y = top; y < bot; y++)
    {
        pixel_t *p = I_VideoBuffer + y * SCREENWIDTH + x;
        *p = wp_tab[level][*p];
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
    subsector_t *ss;
    fixed_t floor;
    int sx, sbot, stop, w, x, y;
    boolean seen;
    double depth;

    if (!wp_on || pmo == NULL || gamestate != GS_LEVEL || automapactive)
        return;
    ss = R_PointInSubsector(wp_x, wp_y);
    floor = ss->sector->floorheight;
    if (!T_ProjectPoint(wp_x, wp_y, floor, &sx, &sbot, &depth)
        || !T_ProjectPoint(wp_x, wp_y, floor + (WP_HEIGHT << FRACBITS), &sx, &stop, NULL))
        return;
    // The pillar, a see-through cylinder: more opaque at its sides, solid in
    // plain sight, dashed while a wall hides it.
    w = (int) (WP_WIDTH * 160.0 / depth);
    if (w < WP_MIN_W)
        w = WP_MIN_W;
    if (w > WP_MAX_W)
        w = WP_MAX_W;
    seen = WP_InSight(pmo, ss);
    for (x = sx - w / 2; x < sx - w / 2 + w; x++)
    {
        int off = 2 * (x - sx) + 1, level = (off < 0 ? -off : off) * WP_LEVELS / (w + 1);
        if (level >= WP_LEVELS)
            level = WP_LEVELS - 1;
        if (seen)
            WP_Column(x, stop, sbot, level);
        else
            for (y = stop; y < sbot; y += 2 * WP_DASH)
                WP_Column(x, y, y + WP_DASH < sbot ? y + WP_DASH : sbot, level);
    }
}
