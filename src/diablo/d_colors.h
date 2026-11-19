//
// d_colors.h — DiabloDoom colour scheme: every colour the Diablo HUD and
// UI draw with, named once, as RGB matched to the nearest PLAYPAL entry
// (2026-11-18; before, each file kept its own RGB table and lookup).
//
// Part of the DiabloDoom mod (GPL-2.0-or-later, like Chocolate Doom).

#ifndef __D_COLORS__
#define __D_COLORS__

typedef enum
{
    // Character screen (d_diablo_ui.c).
    DC_BG,
    DC_PANEL,
    DC_EDGE,
    DC_DIM,
    DC_WHITE,
    DC_RED,
    // Enemy health bars (t_turn.c).
    DC_HB_BG,
    DC_HB_GREEN,
    DC_HB_YELLOW,
    DC_HB_RED,
    // The WAYPOINT marker (d_waypoint.c).
    DC_WP_CORE,
    DC_WP_EDGE,
    NUM_DCOLORS
} dcolor_t;

// A named colour's palette index.
int D_Color(dcolor_t c);

// An item tier's colour (its border and name; tier is a d_diablo.h
// TIER_*), and the dark shade behind it.
int D_TierColor(int tier);
int D_TierColorDark(int tier);

// The palette entry nearest frac/256 of index a over index b.
int D_BlendColor(int a, int b, int frac);

#endif
