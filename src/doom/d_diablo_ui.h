//
// d_diablo_ui.h — DiabloDoom Phase 2: character screen (backpack + paperdoll).
//
// Two-click interaction model (click to pick up, click to place):
//   C ......... open/close the character screen (single-player only)
//   ESC ....... close (stashes any held item)
//   Left click  pick up / place / equip / swap
//   Right click use held consumable, else cancel held item, else close
// Hovering an item shows a tooltip with its stats.  Rarity colors:
//   normal=white, magic=blue, rare=yellow, set=green, unique=gold.
//
// Part of the DiabloDoom mod (GPL-2.0-or-later, like Chocolate Doom).

#ifndef __D_DIABLO_UI__
#define __D_DIABLO_UI__

#include "d_event.h"
#include "doomtype.h"

// State.
boolean D_UIIsOpen(void);
void D_UIOpen(void);
void D_UIClose(void);
void D_UIResetCursor(void);  // Debug: center the UI cursor.

// Input: call from G_Responder when the UI is open.  Returns true when
// the event was consumed.
boolean D_UIResponder(event_t *ev);

// Rendering: call from D_Display after HU_Drawer when the UI is open.
void D_UIDrawer(void);

#endif
