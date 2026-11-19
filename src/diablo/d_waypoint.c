//
// d_waypoint.c — the WAYPOINT marker (d_waypoint.h).
//
// Part of the DiabloDoom mod (GPL-2.0-or-later, like Chocolate Doom).


#include "d_waypoint.h"
#include "d_rlenv.h"
#include "doomstat.h"
#include "i_video.h"
#include "p_local.h"
#include "r_main.h"
#include "r_state.h"
#include "t_turn.h"
#include "v_video.h"
#include "deh_str.h"
#include "i_swap.h"
#include "v_patch.h"
#include "w_wad.h"
#include "z_zone.h"

#define WP_REACH   64       // map units: closer than this, it is reached
#define WP_MIN_W   6        // px: at least 2 px of the policy's 128-wide frame

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

// Drawn as the menu skull (2026-11-18: M_SKULL1 / M_SKULL2, each IWAD's
// own, blinking as in the menu, 8 tics a frame), at WP_SCALE map units a
// pixel, standing on the floor and over everything, so it shows through
// walls. Before: the item respawn fog (IFOG); a pillar textured with
// the status bar, see-through and dashed behind walls at first; flat
// green, white with a black outline and an edge arrow before that.
#define WP_SCALE   2
#define WP_FRAME   8        // tics per frame, as M_Ticker
#define WP_MAX_H   100      // px
static const char *const wp_frames[2] = { "M_SKULL1", "M_SKULL2" };

// Green (2026-11-18; the art's own colours before): each colour
// mapped to the entry of the palette's green ramp (112-127, the green
// marine's) nearest its brightness.
static byte wp_trans[256];
static boolean wp_trans_ok = false;

static void WP_BuildTrans(void)
{
    byte *pal = W_CacheLumpName(DEH_String("PLAYPAL"), PU_STATIC);
    int i, g;

    for (i = 0; i < 256; i++)
    {
        int lum = pal[i * 3] * 3 + pal[i * 3 + 1] * 6 + pal[i * 3 + 2], best = 112, bd = -1;
        for (g = 112; g < 128; g++)
        {
            int d = pal[g * 3] * 3 + pal[g * 3 + 1] * 6 + pal[g * 3 + 2] - lum;
            if (d < 0)
                d = -d;
            if (bd < 0 || d < bd)
            {
                bd = d;
                best = g;
            }
        }
        wp_trans[i] = (byte) best;
    }
    W_ReleaseLumpName(DEH_String("PLAYPAL"));
    wp_trans_ok = true;
}

static patch_t *WP_Patch(void)
{
    int lump = W_CheckNumForName(DEH_String(wp_frames[(gametic / WP_FRAME) & 1]));

    return lump >= 0 ? W_CacheLumpNum(lump, PU_CACHE) : NULL;
}

void D_WaypointDraw(void)
{
    mobj_t *pmo = players[consoleplayer].mo;
    subsector_t *ss;
    patch_t *pat;
    fixed_t floor;
    int sx, sbot, stop, pw, ph, w, h, x0, x;
    double depth;

    if (!wp_on || pmo == NULL || gamestate != GS_LEVEL || automapactive)
        return;
    if (!wp_trans_ok)
        WP_BuildTrans();
    pat = WP_Patch();
    if (pat == NULL)
        return;
    pw = SHORT(pat->width);
    ph = SHORT(pat->height);
    ss = R_PointInSubsector(wp_x, wp_y);
    floor = ss->sector->floorheight;
    if (!T_ProjectPoint(wp_x, wp_y, floor, &sx, &sbot, &depth)
        || !T_ProjectPoint(wp_x, wp_y, floor + ((ph * WP_SCALE) << FRACBITS), &sx, &stop, NULL))
        return;
    h = sbot - stop;
    if (h > WP_MAX_H)
        h = WP_MAX_H;
    w = h * pw / ph;
    if (w < WP_MIN_W)          // far away: kept readable, the aspect kept
    {
        w = WP_MIN_W;
        h = w * ph / pw;
    }
    stop = sbot - h;
    x0 = sx - w / 2;
    for (x = x0; x < x0 + w; x++)
    {
        column_t *col;
        if (x < viewwindowx || x >= viewwindowx + scaledviewwidth)
            continue;
        col = (column_t *) ((byte *) pat + LONG(pat->columnofs[(x - x0) * pw / w]));
        while (col->topdelta != 0xff)
        {
            byte *src = (byte *) col + 3;
            int y = stop + col->topdelta * h / ph, end = stop + (col->topdelta + col->length) * h / ph;
            for (; y < end; y++)
                if (y >= viewwindowy && y < viewwindowy + viewheight)
                    I_VideoBuffer[y * SCREENWIDTH + x] = wp_trans[src[(y - stop) * ph / h - col->topdelta]];
            col = (column_t *) ((byte *) col + col->length + 4);
        }
    }
}
