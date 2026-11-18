//
// d_colors.c — DiabloDoom colour scheme (d_colors.h).
//
// Part of the DiabloDoom mod (GPL-2.0-or-later, like Chocolate Doom).

#include <limits.h>

#include "d_colors.h"
#include "d_diablo.h"
#include "w_wad.h"
#include "z_zone.h"

static const byte dc_rgb[NUM_DCOLORS][3] =
{
    [DC_BG]        = { 10, 8, 14 },
    [DC_PANEL]     = { 26, 22, 30 },
    [DC_EDGE]      = { 110, 100, 110 },
    [DC_DIM]       = { 120, 120, 130 },
    [DC_WHITE]     = { 235, 235, 235 },
    [DC_RED]       = { 220, 30, 30 },
    [DC_HB_BG]     = { 32, 32, 32 },
    [DC_HB_GREEN]  = { 0, 200, 0 },
    [DC_HB_YELLOW] = { 255, 220, 0 },
    [DC_HB_RED]    = { 220, 40, 40 },
};

// Rarity colours: normal white, magic blue, rare yellow, set green,
// unique gold; and the dark shade of each.
static const byte dc_tier_rgb[NUM_TIERS][3] =
{
    [TIER_NORMAL] = { 200, 200, 200 },
    [TIER_MAGIC]  = { 90, 130, 255 },
    [TIER_RARE]   = { 255, 220, 70 },
    [TIER_SET]    = { 80, 220, 90 },
    [TIER_UNIQUE] = { 255, 170, 50 },
};

static const byte dc_tier_dark_rgb[NUM_TIERS][3] =
{
    [TIER_NORMAL] = { 40, 40, 44 },
    [TIER_MAGIC]  = { 18, 26, 60 },
    [TIER_RARE]   = { 60, 50, 14 },
    [TIER_SET]    = { 16, 52, 20 },
    [TIER_UNIQUE] = { 62, 40, 12 },
};

static int dc_index[NUM_DCOLORS];
static int dc_tier[NUM_TIERS];
static int dc_tier_dark[NUM_TIERS];
static boolean dc_done = false;

// Nearest entry of the base PLAYPAL palette (not the live one, which a
// damage or pickup flash and the gamma setting shift).
static int NearestColor(const byte *rgb)
{
    const byte *pal = W_CacheLumpName("PLAYPAL", PU_CACHE);
    int i, best = 0, bestd = INT_MAX;

    for (i = 0; i < 256; i++)
    {
        int dr = pal[3 * i] - rgb[0];
        int dg = pal[3 * i + 1] - rgb[1];
        int db = pal[3 * i + 2] - rgb[2];
        int d = dr * dr + dg * dg + db * db;

        if (d < bestd)
        {
            bestd = d;
            best = i;
        }
    }
    return best;
}

static void ResolveColors(void)
{
    int i;

    if (dc_done)
        return;
    for (i = 0; i < NUM_DCOLORS; i++)
        dc_index[i] = NearestColor(dc_rgb[i]);
    for (i = 0; i < NUM_TIERS; i++)
    {
        dc_tier[i] = NearestColor(dc_tier_rgb[i]);
        dc_tier_dark[i] = NearestColor(dc_tier_dark_rgb[i]);
    }
    dc_done = true;
}

int D_Color(dcolor_t c)
{
    ResolveColors();
    return dc_index[c];
}

int D_TierColor(int tier)
{
    ResolveColors();
    return dc_tier[tier];
}

int D_TierColorDark(int tier)
{
    ResolveColors();
    return dc_tier_dark[tier];
}

int D_BlendColor(int a, int b, int frac)
{
    const byte *pal = W_CacheLumpName("PLAYPAL", PU_CACHE);
    byte rgb[3];
    int i;

    for (i = 0; i < 3; i++)
        rgb[i] = (pal[3 * a + i] * frac + pal[3 * b + i] * (256 - frac)) >> 8;
    return NearestColor(rgb);
}
