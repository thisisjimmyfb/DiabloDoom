//
// d_diablo_ui.h — DiabloDoom Phase 2: character screen (backpack + paperdoll).
//
// Two-click interaction model (click to pick up, click to place):
//   C ......... open/close the character screen (single-player only)
//   ESC ....... close (stashes any held item)
//   Left click  pick up / place / equip / swap
//   Right click use held consumable, else cancel held item, else close
// Keyboard control (no mouse needed):
//   Arrows .... move the selection (cells in backpack, slots on paperdoll)
//   Tab ....... switch between backpack and paperdoll panes
//   Enter/Space pick up / place the selected item
//   E ......... equip held item, or equip/use the selected backpack item
//   R/Bksp .... use held consumable, else cancel held item
//   Q ......... unequip everything
// Any mouse motion hands the cursor back to the mouse.
// Hovering an item (or kb-selecting it) shows a tooltip with its stats.
// Rarity colors: normal=white, magic=blue, rare=yellow, set=green,
// unique=gold.
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

// Set by D_UIClose: the UI covered the whole screen, so D_Display must
// repaint the view border and status bar on the next frame (the 3D view
// repaints itself, but the border and status bar backgrounds do not).
extern boolean d_ui_needs_redraw;

// Input: call from G_Responder when the UI is open.  Returns true when
// the event was consumed.
boolean D_UIResponder(event_t *ev);

// Rendering: call from D_Display after HU_Drawer when the UI is open.
void D_UIDrawer(void);

#endif
