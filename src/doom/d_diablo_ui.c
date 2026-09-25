//
// d_diablo_ui.c — DiabloDoom Phase 2: character screen.
//
// Diablo-style backpack grid + paperdoll with two-click pick-up/place,
// equipment slots, stat panel, and item tooltips.  Items are drawn with
// their Phase 3 2D icons (d_diablo_icons.c) plus a rarity-colored border.
//
// Part of the DiabloDoom mod (GPL-2.0-or-later, like Chocolate Doom).

#include <limits.h>
#include <string.h>

#include "d_diablo.h"
#include "d_diablo_icons.h"
#include "d_diablo_ui.h"
#include "d_player.h"
#include "doomdef.h"
#include "doomkeys.h"
#include "doomstat.h"
#include "p_inter.h"
#include "hu_stuff.h"
#include "i_input.h"
#include "i_swap.h"
#include "i_video.h"
#include "v_video.h"
#include "w_wad.h"
#include "z_zone.h"

// ---------------------------------------------------------------------------
// Layout (320x200).
// ---------------------------------------------------------------------------

#define UI_CELL      20                      // backpack cell size (px)
#define UI_BP_X      112                     // backpack grid origin
#define UI_BP_Y      32
#define UI_BP_W      (D_BP_GRID_W * UI_CELL) // 200
#define UI_BP_H      (D_BP_GRID_H * UI_CELL) // 80

#define UI_SLOT_W    30
#define UI_SLOT_H    24

// Equipment slot boxes: {slot, x, y}.
static const struct { int slot, x, y; } ui_slots[NUM_ESLOTS] =
{
    { ESLOT_HELM,   40,  32 },
    { ESLOT_AMULET,  6,  62 }, { ESLOT_ARMOR, 40,  62 },
    { ESLOT_WEAPON,  6,  92 }, { ESLOT_SHIELD, 40,  92 },
    { ESLOT_GLOVES,  6, 122 }, { ESLOT_BELT,  40, 122 },
    { ESLOT_BOOTS,  74, 122 },
    { ESLOT_RING1,   6, 152 }, { ESLOT_RING2, 40, 152 },
    { ESLOT_FOCUS,  74, 152 },
};

static const char *ui_slotnames[NUM_ESLOTS] =
{
    "HELM", "ARMOR", "WEAPON", "SHIELD", "RING",
    "RING", "AMULET", "BOOTS", "GLOVES", "BELT",
    "FOCUS",
};

static const char *ui_tiernames[NUM_TIERS] =
{
    "Normal", "Magic", "Rare", "Set", "Unique",
};

// ---------------------------------------------------------------------------
// State.
// ---------------------------------------------------------------------------

static boolean ui_open = false;
static boolean ui_was_paused = false;
// Set on UI close so D_Display repaints the border and status bar.
boolean d_ui_needs_redraw = false;
static int ui_cx = 160, ui_cy = 100;   // cursor
static int ui_held = D_NOITEM;         // held item id, or D_NOITEM
static int ui_held_from = -1;          // ESLOT_* when picked from paperdoll
static int ui_prevbtn = 0;
static char ui_msg[80];
static boolean ui_skip_mouse = false;  // ignore stale motion on open
// Phase 4 drag-and-drop: track press position to distinguish drag from
// click (click = pick up and hold for two-click; drag = move and release
// to place).
static boolean ui_dragging = false;
static int ui_press_x = 0, ui_press_y = 0;
static int ui_msgtic = 0;
// Keyboard cursor: selection independent of the mouse cursor, for
// mouse-free inventory management.  ui_kb_pane 0 = backpack cell
// (ui_kb_gx, ui_kb_gy), 1 = paperdoll slot (ui_kb_slot, an index into
// ui_slots).  Any real mouse motion hands control back to the mouse.
static boolean ui_kb_active = false;
static int ui_kb_pane = 0;
static int ui_kb_gx = 0, ui_kb_gy = 0;
static int ui_kb_slot = 0;

// Resolved palette indices.
static int c_bg, c_panel, c_edge, c_dim, c_white;
static int c_rarity[NUM_TIERS];
static int c_rarity_dark[NUM_TIERS];
static boolean colors_done = false;

static int FindColor(int r, int g, int b)
{
    byte *pal;
    int i, best = 0, bestd = INT_MAX;

    pal = W_CacheLumpName("PLAYPAL", PU_CACHE);
    for (i = 0; i < 256; i++)
    {
        int dr = pal[3 * i] - r;
        int dg = pal[3 * i + 1] - g;
        int db = pal[3 * i + 2] - b;
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
    if (colors_done)
        return;
    c_bg    = FindColor(10, 8, 14);
    c_panel = FindColor(26, 22, 30);
    c_edge  = FindColor(110, 100, 110);
    c_dim   = FindColor(120, 120, 130);
    c_white = FindColor(235, 235, 235);

    c_rarity[TIER_NORMAL] = FindColor(200, 200, 200);
    c_rarity[TIER_MAGIC]  = FindColor(90, 130, 255);
    c_rarity[TIER_RARE]   = FindColor(255, 220, 70);
    c_rarity[TIER_SET]    = FindColor(80, 220, 90);
    c_rarity[TIER_UNIQUE] = FindColor(255, 170, 50);

    c_rarity_dark[TIER_NORMAL] = FindColor(40, 40, 44);
    c_rarity_dark[TIER_MAGIC]  = FindColor(18, 26, 60);
    c_rarity_dark[TIER_RARE]   = FindColor(60, 50, 14);
    c_rarity_dark[TIER_SET]    = FindColor(16, 52, 20);
    c_rarity_dark[TIER_UNIQUE] = FindColor(62, 40, 12);
    colors_done = true;
}

// ---------------------------------------------------------------------------
// Text helpers (hu_font, like M_WriteText).
// ---------------------------------------------------------------------------

static int UITextWidth(const char *s)
{
    int w = 0;
    for (; *s; s++)
    {
        int c = *s;
        if (c >= 'a' && c <= 'z')
            c = c - 'a' + 'A';
        c -= HU_FONTSTART;
        if (c < 0 || c >= HU_FONTSIZE)
            w += 4;
        else
            w += SHORT(hu_font[c]->width) + 1;
    }
    return w;
}

static void UIDrawText(int x, int y, const char *s)
{
    int cx = x;
    for (; *s; s++)
    {
        int c = *s;
        int w, h;
        if (c >= 'a' && c <= 'z')
            c = c - 'a' + 'A';
        c -= HU_FONTSTART;
        if (c < 0 || c >= HU_FONTSIZE)
        {
            cx += 4;
            continue;
        }
        if (hu_font[c] == NULL)
        {
            cx += 4;
            continue;
        }
        w = SHORT(hu_font[c]->width);
        h = SHORT(hu_font[c]->height);
        // Skip glyphs that would fall outside the 320x200 canvas
        // (V_DrawPatch aborts on out-of-bounds).
        if (cx < 0 || y < 0 || cx + w > SCREENWIDTH || y + h > SCREENHEIGHT)
        {
            if (cx + w > SCREENWIDTH)
                break;
            cx += w + 1;
            continue;
        }
        V_DrawPatchDirect(cx, y, hu_font[c]);
        cx += w + 1;
    }
}

static void UIDrawTextCentered(int cx, int y, const char *s)
{
    UIDrawText(cx - UITextWidth(s) / 2, y, s);
}

static void UIMsg(const char *s)
{
    strncpy(ui_msg, s, sizeof(ui_msg) - 1);
    ui_msg[sizeof(ui_msg) - 1] = '\0';
    ui_msgtic = gametic + TICRATE * 2;
}

// ---------------------------------------------------------------------------
// Item helpers.
// ---------------------------------------------------------------------------

static const diablo_itemdef_t *UIHeldDef(void)
{
    if (ui_held == D_NOITEM)
        return NULL;
    return D_GetItemDef(D_ITEMTIER(ui_held), D_ITEMIDX(ui_held));
}

// Backpack index of the item covering grid cell (gx,gy), or -1.
static int UIBackpackAt(player_t *player, int gx, int gy)
{
    int i;
    for (i = 0; i < player->diablo_bp_count; i++)
    {
        int ox = player->diablo_bp_gx[i];
        int oy = player->diablo_bp_gy[i];
        int id = player->diablo_backpack[i];
        const diablo_itemdef_t *def;
        if (ox < 0 || oy < 0)
            continue;
        def = D_GetItemDef(D_ITEMTIER(id), D_ITEMIDX(id));
        if (!def)
            continue;
        if (gx >= ox && gx < ox + def->grid_w
         && gy >= oy && gy < oy + def->grid_h)
            return i;
    }
    return -1;
}

// Equipment slot whose box contains (x,y), or -1.
static int UISlotAt(int x, int y)
{
    int i;
    for (i = 0; i < NUM_ESLOTS; i++)
    {
        if (x >= ui_slots[i].x && x < ui_slots[i].x + UI_SLOT_W
         && y >= ui_slots[i].y && y < ui_slots[i].y + UI_SLOT_H)
            return ui_slots[i].slot;
    }
    return -1;
}

static boolean UICursorInBackpack(int *gx, int *gy)
{
    if (ui_cx < UI_BP_X || ui_cy < UI_BP_Y
     || ui_cx >= UI_BP_X + UI_BP_W || ui_cy >= UI_BP_Y + UI_BP_H)
        return false;
    *gx = (ui_cx - UI_BP_X) / UI_CELL;
    *gy = (ui_cy - UI_BP_Y) / UI_CELL;
    return true;
}

// Can item id be placed at (gx,gy)?  Overlap test against the backpack.
static boolean UICanPlaceAt(player_t *player, int id, int gx, int gy)
{
    const diablo_itemdef_t *def = D_GetItemDef(D_ITEMTIER(id), D_ITEMIDX(id));
    int w, h, i;

    if (!def)
        return false;
    w = def->grid_w;
    h = def->grid_h;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (gx < 0 || gy < 0 || gx + w > D_BP_GRID_W || gy + h > D_BP_GRID_H)
        return false;

    for (i = 0; i < player->diablo_bp_count; i++)
    {
        int ox = player->diablo_bp_gx[i];
        int oy = player->diablo_bp_gy[i];
        int oid = player->diablo_backpack[i];
        const diablo_itemdef_t *odef;
        int ow, oh;
        if (ox < 0 || oy < 0)
            continue;
        odef = D_GetItemDef(D_ITEMTIER(oid), D_ITEMIDX(oid));
        if (!odef)
            continue;
        ow = odef->grid_w;
        oh = odef->grid_h;
        if (ow < 1) ow = 1;
        if (oh < 1) oh = 1;
        if (gx < ox + ow && gx + w > ox && gy < oy + oh && gy + h > oy)
            return false;
    }
    return true;
}

// Append item id to the backpack at (gx,gy) without announcements.
static void UIAppendAt(player_t *player, int id, int gx, int gy)
{
    int bpi;
    if (player->diablo_bp_count >= D_BACKPACK_SIZE)
        return;
    bpi = player->diablo_bp_count++;
    player->diablo_backpack[bpi] = id;
    player->diablo_bp_gx[bpi] = gx;
    player->diablo_bp_gy[bpi] = gy;
}

// Stash the held item back into the backpack (first free spot).
static boolean UIStashHeld(player_t *player)
{
    const diablo_itemdef_t *def = UIHeldDef();
    int gx, gy;
    if (!def)
        return true;
    if (!D_GridFindSpace((struct player_s *)player,
                         def->grid_w, def->grid_h, &gx, &gy))
        return false;
    UIAppendAt(player, ui_held, gx, gy);
    ui_held = D_NOITEM;
    ui_held_from = -1;
    return true;
}

// ---------------------------------------------------------------------------
// Open / close.
// ---------------------------------------------------------------------------

boolean D_UIIsOpen(void)
{
    return ui_open;
}

void D_UIResetCursor(void)
{
    ui_cx = 160;
    ui_cy = 100;
}

void D_UIOpen(void)
{
    player_t *player;
    int i;

    if (ui_open)
        return;
    if (gamestate != GS_LEVEL)
        return;
    if (deathmatch || netgame)
        return;  // single-player only
    player = &players[consoleplayer];
    if (!playeringame[consoleplayer] || player->playerstate != PST_LIVE)
        return;

    ResolveColors();
    ui_open = true;
    ui_was_paused = paused;
    paused = true;
    ui_cx = 160;
    ui_cy = 100;
    ui_held = D_NOITEM;
    ui_held_from = -1;
    ui_prevbtn = 0;
    ui_dragging = false;
    ui_press_x = 160;
    ui_press_y = 100;
    ui_msg[0] = '\0';
    ui_skip_mouse = true;  // discard stale motion event from ungrab
    d_ui_accel_disabled = true;  // 1:1 cursor movement
    // Keyboard cursor: start on the first occupied backpack cell (else
    // 0,0) and remember the first occupied paperdoll slot.  The mouse
    // owns the cursor until an arrow key is pressed.
    ui_kb_active = false;
    ui_kb_pane = 0;
    ui_kb_gx = 0;
    ui_kb_gy = 0;
    for (i = 0; i < player->diablo_bp_count; i++)
    {
        if (player->diablo_bp_gx[i] >= 0 && player->diablo_bp_gy[i] >= 0)
        {
            ui_kb_gx = player->diablo_bp_gx[i];
            ui_kb_gy = player->diablo_bp_gy[i];
            break;
        }
    }
    ui_kb_slot = 0;
    for (i = 0; i < NUM_ESLOTS; i++)
    {
        if (player->diablo_equipped[ui_slots[i].slot] != D_NOITEM)
        {
            ui_kb_slot = i;
            break;
        }
    }
}

void D_UIClose(void)
{
    player_t *player = &players[consoleplayer];

    if (!ui_open)
        return;

    // Stash anything still held: prefer its origin slot, else backpack.
    if (ui_held != D_NOITEM)
    {
        if (ui_held_from >= 0
         && player->diablo_equipped[ui_held_from] == D_NOITEM)
        {
            player->diablo_equipped[ui_held_from] = ui_held;
            D_RecalcStats((struct player_s *)player);
        }
        else if (!UIStashHeld(player))
        {
            // Should not happen (we removed it from somewhere), but
            // never silently delete the player's item: scan for any
            // valid grid spot rather than overlapping at (0,0).
            const diablo_itemdef_t *def = UIHeldDef();
            int gx, gy, found = 0;
            if (def)
            {
                for (gy = 0; gy <= D_BP_GRID_H - def->grid_h && !found; gy++)
                    for (gx = 0; gx <= D_BP_GRID_W - def->grid_w && !found; gx++)
                        if (UICanPlaceAt(player, ui_held, gx, gy))
                        {
                            UIAppendAt(player, ui_held, gx, gy);
                            found = 1;
                        }
            }
            // If still no room (pathological), keep it held; the UI
            // stays open rather than deleting the item.
            if (!found)
                return;
        }
        ui_held = D_NOITEM;
        ui_held_from = -1;
    }

    ui_open = false;
    paused = ui_was_paused;
    d_ui_accel_disabled = false;
    // The 3D renderer does not repaint every pixel (view window/border),
    // so clear the UI's text artifacts; the next D_Display draws the
    // game view over this in the same frame.  The border and status bar
    // backgrounds are repainted via d_ui_needs_redraw (see D_Display).
    d_ui_needs_redraw = true;
    V_DrawFilledBox(0, 0, SCREENWIDTH, SCREENHEIGHT, 0);
}

// ---------------------------------------------------------------------------
// Click handling (two-click pick up / place).
// ---------------------------------------------------------------------------

static void UILeftClick(void)
{
    player_t *player = &players[consoleplayer];
    int slot, gx, gy;
    const diablo_itemdef_t *def;

    // Equipment slots first.
    slot = UISlotAt(ui_cx, ui_cy);
    if (slot >= 0)
    {
        if (ui_held != D_NOITEM)
        {
            int old;
            def = UIHeldDef();
            if (!D_SlotFits(slot, def))
            {
                UIMsg("That item doesn't fit there.");
                return;
            }
            // Equip; whatever was there lands on the cursor (swap).
            old = player->diablo_equipped[slot];
            player->diablo_equipped[slot] = ui_held;
            ui_held = old;
            ui_held_from = (old == D_NOITEM) ? -1 : slot;
            D_RecalcStats((struct player_s *)player);
            UIMsg(old == D_NOITEM ? "Equipped." : "Swapped.");
        }
        else
        {
            int id = player->diablo_equipped[slot];
            if (id != D_NOITEM)
            {
                ui_held = id;
                ui_held_from = slot;
                player->diablo_equipped[slot] = D_NOITEM;
                D_RecalcStats((struct player_s *)player);
            }
        }
        return;
    }

    // Backpack grid.
    if (UICursorInBackpack(&gx, &gy))
    {
        if (ui_held != D_NOITEM)
        {
            // Place the held item with its top-left at the clicked cell.
            if (UICanPlaceAt(player, ui_held, gx, gy))
            {
                UIAppendAt(player, ui_held, gx, gy);
                ui_held = D_NOITEM;
                ui_held_from = -1;
            }
            else
                UIMsg("No room there.");
        }
        else
        {
            int bpi = UIBackpackAt(player, gx, gy);
            if (bpi >= 0)
            {
                ui_held = player->diablo_backpack[bpi];
                ui_held_from = -1;
                D_BackpackRemoveAt((struct player_s *)player, bpi);
            }
        }
        return;
    }

    // Clicked empty space: nothing.
}

static void UIUseHeldConsumable(player_t *player)
{
    // D_UseBackpackItem works on backpack indices; the held item is not
    // in the backpack, so stash + use + (nothing left to clean up).
    const diablo_itemdef_t *def = UIHeldDef();
    int bpi;
    if (!def || !def->consumable)
        return;
    if (!UIStashHeld(player))
    {
        UIMsg("Backpack full!");
        return;
    }
    bpi = player->diablo_bp_count - 1;
    D_UseBackpackItem((struct player_s *)player, bpi);
}

static void UIRightClick(void)
{
    player_t *player = &players[consoleplayer];
    const diablo_itemdef_t *def = UIHeldDef();

    if (ui_held != D_NOITEM)
    {
        if (def && def->consumable)
        {
            UIUseHeldConsumable(player);
            return;
        }
        // Cancel: back to origin slot if possible, else backpack.
        if (ui_held_from >= 0
         && player->diablo_equipped[ui_held_from] == D_NOITEM)
        {
            player->diablo_equipped[ui_held_from] = ui_held;
            D_RecalcStats((struct player_s *)player);
            ui_held = D_NOITEM;
            ui_held_from = -1;
        }
        else if (UIStashHeld(player))
            UIMsg("Put back.");
        else
            UIMsg("Backpack full!");
        return;
    }

    D_UIClose();
}

// ---------------------------------------------------------------------------
// Keyboard cursor (mouse-free inventory management).
// ---------------------------------------------------------------------------

// Move the pixel cursor to the center of the kb selection so the
// existing click handlers, held-item drawing, and tooltips work
// unchanged.
static void UIKbSyncCursor(void)
{
    if (ui_kb_pane == 0)
    {
        ui_cx = UI_BP_X + ui_kb_gx * UI_CELL + UI_CELL / 2;
        ui_cy = UI_BP_Y + ui_kb_gy * UI_CELL + UI_CELL / 2;
    }
    else
    {
        ui_cx = ui_slots[ui_kb_slot].x + UI_SLOT_W / 2;
        ui_cy = ui_slots[ui_kb_slot].y + UI_SLOT_H / 2;
    }
}

static void UIKbMove(int dx, int dy)
{
    ui_kb_active = true;
    if (ui_kb_pane == 0)
    {
        ui_kb_gx += dx;
        ui_kb_gy += dy;
        if (ui_kb_gx < 0) ui_kb_gx = 0;
        if (ui_kb_gy < 0) ui_kb_gy = 0;
        if (ui_kb_gx > D_BP_GRID_W - 1) ui_kb_gx = D_BP_GRID_W - 1;
        if (ui_kb_gy > D_BP_GRID_H - 1) ui_kb_gy = D_BP_GRID_H - 1;
    }
    else
    {
        // Paperdoll: up/left = previous slot, down/right = next,
        // wrapping around.
        if (dx != 0 || dy != 0)
        {
            int d = (dx > 0 || dy > 0) ? 1 : -1;
            ui_kb_slot = (ui_kb_slot + d + NUM_ESLOTS) % NUM_ESLOTS;
        }
    }
    UIKbSyncCursor();
}

static void UIKbTogglePane(void)
{
    ui_kb_active = true;
    ui_kb_pane = 1 - ui_kb_pane;
    UIKbSyncCursor();
}

// Enter/Space: pick up / place at the kb selection.
static void UIKbClick(void)
{
    ui_kb_active = true;
    UIKbSyncCursor();
    UILeftClick();
}

// Paperdoll slot index (into ui_slots) whose box center is nearest the
// middle of the slot the item belongs to, preferring a free ring slot.
static int UIKbTargetSlot(player_t *player, const diablo_itemdef_t *def)
{
    int i, slot;
    if (!def || def->slot == ESLOT_NONE)
        return -1;
    slot = def->slot;
    if (def->slot == ESLOT_RING1)
    {
        if (player->diablo_equipped[ESLOT_RING1] == D_NOITEM)
            slot = ESLOT_RING1;
        else if (player->diablo_equipped[ESLOT_RING2] == D_NOITEM)
            slot = ESLOT_RING2;
        else
            slot = ESLOT_RING1;
    }
    for (i = 0; i < NUM_ESLOTS; i++)
        if (ui_slots[i].slot == slot)
            return i;
    return -1;
}

// E with an item held: equip it (swap-safe via UILeftClick).
static void UIKbEquipHeld(player_t *player)
{
    const diablo_itemdef_t *def = UIHeldDef();
    int si;
    if (!def)
        return;
    if (def->consumable)
    {
        UIUseHeldConsumable(player);
        return;
    }
    si = UIKbTargetSlot(player, def);
    if (si < 0)
    {
        UIMsg("Can't equip that.");
        return;
    }
    ui_cx = ui_slots[si].x + UI_SLOT_W / 2;
    ui_cy = ui_slots[si].y + UI_SLOT_H / 2;
    ui_kb_active = true;
    ui_kb_pane = 1;
    ui_kb_slot = si;
    UILeftClick();  // equip, or swap the old item onto the cursor
}

// E: equip the held item, else equip/use the kb-selected backpack item.
static void UIKbEquip(player_t *player)
{
    int bpi, si;
    int id;
    const diablo_itemdef_t *def;

    ui_kb_active = true;
    if (ui_held != D_NOITEM)
    {
        UIKbEquipHeld(player);
        return;
    }
    if (ui_kb_pane != 0)
        return;  // paperdoll selection: nothing to equip from
    bpi = UIBackpackAt(player, ui_kb_gx, ui_kb_gy);
    if (bpi < 0)
    {
        UIMsg("Nothing there.");
        return;
    }
    id = player->diablo_backpack[bpi];
    def = D_GetItemDef(D_ITEMTIER(id), D_ITEMIDX(id));
    if (!def)
        return;
    if (def->consumable)
    {
        D_UseBackpackItem((struct player_s *)player, bpi);
        UIMsg("Used.");
        return;
    }
    si = UIKbTargetSlot(player, def);
    if (si < 0)
    {
        UIMsg("Can't equip that.");
        return;
    }
    // Pick up at the selected cell, then click the slot.  The second
    // click equips (or swaps onto the cursor), so the item is never
    // lost even when the slot is taken.
    UIKbSyncCursor();
    UILeftClick();  // pick up
    if (ui_held == D_NOITEM)
        return;
    ui_cx = ui_slots[si].x + UI_SLOT_W / 2;
    ui_cy = ui_slots[si].y + UI_SLOT_H / 2;
    ui_kb_pane = 1;
    ui_kb_slot = si;
    UILeftClick();  // equip / swap
}

// X: drop the held item (or the kb-selected backpack item) on the ground
// at the player's feet as a loot pickup.
static void UIKbDrop(player_t *player)
{
    int id = D_NOITEM;
    int bpi;

    ui_kb_active = true;
    if (ui_held != D_NOITEM)
    {
        id = ui_held;
        ui_held = D_NOITEM;
        ui_held_from = -1;
    }
    else
    {
        if (ui_kb_pane != 0)
        {
            UIMsg("Select a backpack item.");
            return;
        }
        bpi = UIBackpackAt(player, ui_kb_gx, ui_kb_gy);
        if (bpi < 0)
        {
            UIMsg("Nothing there.");
            return;
        }
        id = player->diablo_backpack[bpi];
        D_BackpackRemoveAt((struct player_s *)player, bpi);
    }

    if (id != D_NOITEM && player->mo)
    {
        P_SpawnDiabloLootAt(player->mo->x, player->mo->y, id);
        UIMsg("Dropped.");
    }
}

// ---------------------------------------------------------------------------
// Input responder.
// ---------------------------------------------------------------------------

boolean D_UIResponder(event_t *ev)
{
    int buttons;

    if (!ui_open)
        return false;

    if (ev->type == ev_mouse)
    {
        // Discard the stale motion event that arrives when the mouse
        // is ungrabbed on UI open.
        if (ui_skip_mouse)
        {
            ui_skip_mouse = false;
            ui_prevbtn = ev->data1;
            return true;
        }
        // ev_mouse deltas are in window pixels; the game renders at
        // 320x200 scaled 2x to 640x400, so halve them for UI coords.
        // Note: data3 (Y) is negated by I_ReadMouse, so subtract it.
        // Real mouse motion hands the cursor back to the mouse.
        if (ev->data2 != 0 || ev->data3 != 0)
            ui_kb_active = false;
        ui_cx += ev->data2 / 2;
        ui_cy -= ev->data3 / 2;
        if (ui_cx < 0) ui_cx = 0;
        if (ui_cy < 0) ui_cy = 0;
        if (ui_cx > SCREENWIDTH - 1) ui_cx = SCREENWIDTH - 1;
        if (ui_cy > SCREENHEIGHT - 1) ui_cy = SCREENHEIGHT - 1;

        buttons = ev->data1;

        // Phase 4 drag-and-drop.  Left press picks up (or places, when
        // already holding).  Releasing after a drag places at the cursor;
        // releasing without movement keeps the item held (two-click).
        if ((buttons & 1) && !(ui_prevbtn & 1))
        {
            // Left button pressed.
            ui_press_x = ui_cx;
            ui_press_y = ui_cy;
            if (ui_held == D_NOITEM)
            {
                UILeftClick();  // pick up if over an item
                ui_dragging = (ui_held != D_NOITEM);
            }
            else
            {
                UILeftClick();  // place (two-click)
                ui_dragging = false;
            }
        }
        if (!(buttons & 1) && (ui_prevbtn & 1))
        {
            // Left button released.
            if (ui_dragging && ui_held != D_NOITEM)
            {
                int dx = ui_cx - ui_press_x;
                int dy = ui_cy - ui_press_y;
                // Drag threshold: 5px.  Beyond that, drop at the cursor.
                if (dx * dx + dy * dy > 25)
                    UILeftClick();  // place (drag-and-drop)
                // Else: keep holding for two-click placement.
            }
            ui_dragging = false;
        }
        if ((buttons & 2) && !(ui_prevbtn & 2))
            UIRightClick();
        ui_prevbtn = buttons;
        return true;
    }

    if (ev->type == ev_keydown)
    {
        player_t *player = &players[consoleplayer];
        int k = ev->data1;

        if (k == KEY_ESCAPE || k == 'c' || k == 'C')
            D_UIClose();
        else if (k == KEY_TAB)
            UIKbTogglePane();
        else if (k == KEY_UPARROW)
            UIKbMove(0, -1);
        else if (k == KEY_DOWNARROW)
            UIKbMove(0, 1);
        else if (k == KEY_LEFTARROW)
            UIKbMove(-1, 0);
        else if (k == KEY_RIGHTARROW)
            UIKbMove(1, 0);
        else if (k == KEY_ENTER || k == ' ')
            UIKbClick();
        else if (k == 'e' || k == 'E')
            UIKbEquip(player);
        else if (k == 'r' || k == 'R' || k == KEY_BACKSPACE)
            UIRightClick();  // use held consumable, else cancel hold
        else if (k == 'q' || k == 'Q')
        {
            D_UnequipAll((struct player_s *)player);
            UIMsg("Unequipped.");
        }
        else if (k == 'x' || k == 'X')
            UIKbDrop(player);
        // Eat all other keys too: no game input while the screen is up.
        return true;
    }

    if (ev->type == ev_keyup)
        return true;

    return false;
}

// ---------------------------------------------------------------------------
// Rendering.
// ---------------------------------------------------------------------------

// Draw an item icon: rarity-colored box with the item's initial.
// (x,y) is the top-left, in pixels.
static void UIDrawItemIcon(int x, int y, int w, int h, int id)
{
    const diablo_itemdef_t *def;
    const unsigned char *icon;
    int icon_idx;
    int sx, sy, dx, dy;
    pixel_t *dest;

    def = D_GetItemDef(D_ITEMTIER(id), D_ITEMIDX(id));
    if (!def)
        return;

    // Draw the item's icon, aspect-ratio preserved (letterboxed).
    // The source icon is square; fit it into the largest centered
    // square that fits in w x h so tall/wide items don't stretch.
    icon_idx = D_GetItemIconIdx(D_ITEMTIER(id), D_ITEMIDX(id));
    if (icon_idx >= 0 && icon_idx < d_num_item_icons
        && d_item_icons[icon_idx] != NULL)
    {
        int box, ox, oy;  // centered square side, top-left offset
        icon = d_item_icons[icon_idx];
        box = (w < h) ? w : h;
        ox = (w - box) / 2;
        oy = (h - box) / 2;
        for (dy = 0; dy < box; ++dy)
        {
            // Clamp to screen to avoid writing out of bounds.
            if (y + oy + dy < 0 || y + oy + dy >= SCREENHEIGHT)
                continue;
            sy = dy * d_icon_size / box;
            dest = I_VideoBuffer + SCREENWIDTH * (y + oy + dy) + (x + ox);
            for (dx = 0; dx < box; ++dx)
            {
                if (x + ox + dx < 0 || x + ox + dx >= SCREENWIDTH)
                {
                    dest++;
                    continue;
                }
                sx = dx * d_icon_size / box;
                *dest++ = (pixel_t)icon[sy * d_icon_size + sx];
            }
        }
    }
    else
    {
        // Fallback: dark rarity background if no icon.
        V_DrawFilledBox(x, y, w, h, c_rarity_dark[def->tier]);
    }

    // Rarity border (kept so rarity stays visible).
    V_DrawHorizLine(x, y, w, c_rarity[def->tier]);
    V_DrawHorizLine(x, y + h - 1, w, c_rarity[def->tier]);
    V_DrawVertLine(x, y, h, c_rarity[def->tier]);
    V_DrawVertLine(x + w - 1, y, h, c_rarity[def->tier]);
}

// Draw one equipment slot box (with item icon when filled, name when empty).
static void UIDrawSlot(player_t *player, int slot, int x, int y)
{
    int id = player->diablo_equipped[slot];

    V_DrawFilledBox(x, y, UI_SLOT_W, UI_SLOT_H, c_panel);
    V_DrawHorizLine(x, y, UI_SLOT_W, c_edge);
    V_DrawHorizLine(x, y + UI_SLOT_H - 1, UI_SLOT_W, c_edge);
    V_DrawVertLine(x, y, UI_SLOT_H, c_edge);
    V_DrawVertLine(x + UI_SLOT_W - 1, y, UI_SLOT_H, c_edge);

    if (id != D_NOITEM)
        UIDrawItemIcon(x + 2, y + 2, UI_SLOT_W - 4, UI_SLOT_H - 4, id);
    else
        UIDrawTextCentered(x + UI_SLOT_W / 2, y + UI_SLOT_H / 2 - 4,
                           ui_slotnames[slot]);
}

// Build tooltip lines for an item id.  Returns line count.
static int UITooltipLines(int id, char lines[12][48])
{
    const diablo_itemdef_t *def;
    int n = 0;

    def = D_GetItemDef(D_ITEMTIER(id), D_ITEMIDX(id));
    if (!def)
        return 0;

    snprintf(lines[n++], 48, "%s", def->name);
    snprintf(lines[n++], 48, "%s", ui_tiernames[def->tier]);

    if (def->consumable)
    {
        if (def->usekind == USE_HEAL)
            snprintf(lines[n++], 48, "Restores %d HP", def->heal);
        else if (def->usekind == USE_MANA)
            snprintf(lines[n++], 48, "Mana ward: +%d armor", def->heal);
        else
            snprintf(lines[n++], 48, "Throw: %d gas damage", def->heal);
        snprintf(lines[n++], 48, "Right-click to use");
        return n;
    }

    if (def->dmg_min > 0 || def->dmg_max > 0)
        snprintf(lines[n++], 48, "Damage %d-%d", def->dmg_min, def->dmg_max);
    if (def->armor > 0)
        snprintf(lines[n++], 48, "Armor %d", def->armor);
    if (def->str) snprintf(lines[n++], 48, "+%d Strength", def->str);
    if (def->dex) snprintf(lines[n++], 48, "+%d Dexterity", def->dex);
    if (def->vit) snprintf(lines[n++], 48, "+%d Vitality", def->vit);
    if (def->ene) snprintf(lines[n++], 48, "+%d Energy", def->ene);
    if (def->fres || def->cres || def->lres || def->pres)
        snprintf(lines[n++], 48, "Res F%d C%d L%d P%d",
                 def->fres, def->cres, def->lres, def->pres);
    if (def->lifesteal)
        snprintf(lines[n++], 48, "Life steal %d%%", def->lifesteal);
    if (def->magicfind)
        snprintf(lines[n++], 48, "Magic find %d%%", def->magicfind);
    if (def->movespeed)
        snprintf(lines[n++], 48, "Speed +%d%%", def->movespeed);
    // Phase 7 tactical affixes (turn-based).
    if (def->ad_pct)
        snprintf(lines[n++], 48, "Attack dmg +%d%%", def->ad_pct);
    if (def->ap_pct)
        snprintf(lines[n++], 48, "Ability power +%d%%", def->ap_pct);
    if (def->haste)
        snprintf(lines[n++], 48, "Haste %d%% (cooldowns)", def->haste);
    // Crit is gear-inherent (Diablo-style).
    if (def->crit_chance)
        snprintf(lines[n++], 48, "+%d%% Crit Chance", def->crit_chance);
    if (def->crit_dmg)
        snprintf(lines[n++], 48, "+%d%% Crit Damage", def->crit_dmg);
    return n;
}

// Item id under the cursor (held, hovered slot, or hovered backpack cell).
static int UIHoverItem(player_t *player)
{
    int slot, gx, gy, bpi;

    if (ui_held != D_NOITEM)
        return D_NOITEM;  // held item drawn at cursor; tooltip skipped

    slot = UISlotAt(ui_cx, ui_cy);
    if (slot >= 0)
        return player->diablo_equipped[slot];

    if (UICursorInBackpack(&gx, &gy))
    {
        bpi = UIBackpackAt(player, gx, gy);
        if (bpi >= 0)
            return player->diablo_backpack[bpi];
    }
    return D_NOITEM;
}

static void UIDrawTooltip(int id)
{
    char lines[12][48];
    int n, i, w = 0, h, x, y;
    const diablo_itemdef_t *def;

    n = UITooltipLines(id, lines);
    if (n <= 0)
        return;
    def = D_GetItemDef(D_ITEMTIER(id), D_ITEMIDX(id));

    for (i = 0; i < n; i++)
    {
        int lw = UITextWidth(lines[i]);
        if (lw > w)
            w = lw;
    }
    w += 16;
    h = n * 10 + 8;

    // Place right of the cursor; flip left when near the right edge.
    x = ui_cx + 12;
    if (x + w > SCREENWIDTH)
        x = ui_cx - 12 - w;
    y = ui_cy - 8;
    if (y + h > SCREENHEIGHT)
        y = SCREENHEIGHT - h;
    if (y < 0)
        y = 0;

    V_DrawFilledBox(x, y, w, h, c_bg);
    V_DrawHorizLine(x, y, w, c_edge);
    V_DrawHorizLine(x, y + h - 1, w, c_edge);
    V_DrawVertLine(x, y, h, c_edge);
    V_DrawVertLine(x + w - 1, y, h, c_edge);

    // Title bar in the rarity color.
    V_DrawFilledBox(x + 1, y + 1, w - 2, 12, c_rarity[def->tier]);
    UIDrawText(x + 5, y + 3, lines[0]);
    for (i = 1; i < n; i++)
        UIDrawText(x + 5, y + 4 + i * 10, lines[i]);
}

static void UIDrawStats(player_t *player, int y); // fwd
static int UIDrawPowers(player_t *player); // fwd, returns y below section

static void UIDrawStats(player_t *player, int y)
{
    char buf[64];
    int x = UI_BP_X;

    UIDrawText(x, y, "STATS");
    y += 10;
    snprintf(buf, sizeof(buf), "DMG %d-%d   ARM %d",
             D_Stat((struct player_s *)player, DSTAT_DMG_MIN),
             D_Stat((struct player_s *)player, DSTAT_DMG_MAX),
             D_Stat((struct player_s *)player, DSTAT_ARMOR));
    UIDrawText(x, y, buf);
    y += 8;
    snprintf(buf, sizeof(buf), "STR %d DEX %d VIT %d ENE %d POT +%d%%",
             D_Stat((struct player_s *)player, DSTAT_STR),
             D_Stat((struct player_s *)player, DSTAT_DEX),
             D_Stat((struct player_s *)player, DSTAT_VIT),
             D_Stat((struct player_s *)player, DSTAT_ENE),
             D_PotionBonus((struct player_s *)player));
    UIDrawText(x, y, buf);
    y += 8;
    snprintf(buf, sizeof(buf), "DODGE %d%% CRIT %d%% LEECH %d%% FIND %d%%",
             D_DodgeChance((struct player_s *)player),
             D_CritChance((struct player_s *)player),
             D_Stat((struct player_s *)player, DSTAT_LIFESTEAL),
             D_Stat((struct player_s *)player, DSTAT_MAGICFIND));
    UIDrawText(x, y, buf);
    y += 8;
    snprintf(buf, sizeof(buf), "RES F%d C%d L%d P%d SPD +%d%% HP %d/%d",
             D_Stat((struct player_s *)player, DSTAT_FRES),
             D_Stat((struct player_s *)player, DSTAT_CRES),
             D_Stat((struct player_s *)player, DSTAT_LRES),
             D_Stat((struct player_s *)player, DSTAT_PRES),
             D_Stat((struct player_s *)player, DSTAT_MOVESPEED),
             player->health,
             D_MaxHealth((struct player_s *)player));
    UIDrawText(x, y, buf);
}

// Special mechanics granted by equipped gear (MECH_*), below the
// backpack. Plain stat boosts are filtered out -- those live in STATS.
// Returns the y coordinate below the section.
static int UIDrawPowers(player_t *player)
{
    int mechs = D_ActiveMechs((struct player_s *)player);
    int x = UI_BP_X, y = 115;
    int i, shown = 0, total = 0;
    char buf[48];

    UIDrawText(x, y, "POWERS");
    y += 10;

    // Count first, so we can show "+N more" when it overflows.
    for (i = 0; i < 14; i++)
        if (mechs & (1 << i))
            total++;

    for (i = 0; i < 14 && shown < 3; i++)
    {
        if (mechs & (1 << i))
        {
            const char *desc = D_MechDesc(1 << i);
            if (desc)
            {
                UIDrawText(x, y, desc);
                y += 8;
                shown++;
            }
        }
    }
    if (total > shown)
    {
        snprintf(buf, sizeof(buf), "+%d more", total - shown);
        UIDrawText(x, y, buf);
        y += 8;
    }
    if (total == 0)
    {
        UIDrawText(x, y, "(none)");
        y += 8;
    }
    return y;
}

void D_UIDrawer(void)
{
    player_t *player = &players[consoleplayer];
    int i, gx, gy, hover;

    if (!ui_open)
        return;
    ResolveColors();

    // Background.
    V_DrawFilledBox(0, 0, SCREENWIDTH, SCREENHEIGHT, c_bg);

    // Title bar.
    UIDrawText(10, 6, "CHARACTER");
    UIDrawText(SCREENWIDTH - 10 - UITextWidth("C/ESC: CLOSE"), 6,
               "C/ESC: CLOSE");
    V_DrawHorizLine(0, 20, SCREENWIDTH, c_edge);

    // Paperdoll.
    UIDrawText(10, 26, "EQUIPMENT");
    for (i = 0; i < NUM_ESLOTS; i++)
        UIDrawSlot(player, ui_slots[i].slot, ui_slots[i].x, ui_slots[i].y);

    // Backpack.
    UIDrawText(UI_BP_X, 26, "BACKPACK");
    V_DrawFilledBox(UI_BP_X - 2, UI_BP_Y - 2, UI_BP_W + 4, UI_BP_H + 4,
                    c_panel);
    for (gy = 0; gy <= D_BP_GRID_H; gy++)
        V_DrawHorizLine(UI_BP_X, UI_BP_Y + gy * UI_CELL, UI_BP_W, c_edge);
    for (gx = 0; gx <= D_BP_GRID_W; gx++)
        V_DrawVertLine(UI_BP_X + gx * UI_CELL, UI_BP_Y, UI_BP_H, c_edge);
    for (i = 0; i < player->diablo_bp_count; i++)
    {
        int id = player->diablo_backpack[i];
        const diablo_itemdef_t *def;
        int ox = player->diablo_bp_gx[i];
        int oy = player->diablo_bp_gy[i];
        if (ox < 0 || oy < 0)
            continue;
        def = D_GetItemDef(D_ITEMTIER(id), D_ITEMIDX(id));
        if (!def)
            continue;
        UIDrawItemIcon(UI_BP_X + ox * UI_CELL + 1,
                       UI_BP_Y + oy * UI_CELL + 1,
                       def->grid_w * UI_CELL - 2,
                       def->grid_h * UI_CELL - 2,
                       id);
    }

    // Powers (special mechanics) and stats under the backpack.
    {
        int py = UIDrawPowers(player);
        UIDrawStats(player, py + 4);
    }

    // Keyboard cursor highlight (mouse mode keeps the crosshair).
    if (ui_kb_active)
    {
        int hx, hy, hw, hh;
        if (ui_kb_pane == 0)
        {
            hx = UI_BP_X + ui_kb_gx * UI_CELL;
            hy = UI_BP_Y + ui_kb_gy * UI_CELL;
            hw = UI_CELL;
            hh = UI_CELL;
        }
        else
        {
            hx = ui_slots[ui_kb_slot].x;
            hy = ui_slots[ui_kb_slot].y;
            hw = UI_SLOT_W;
            hh = UI_SLOT_H;
        }
        V_DrawHorizLine(hx - 1, hy - 1, hw + 2, c_white);
        V_DrawHorizLine(hx - 1, hy + hh, hw + 2, c_white);
        V_DrawVertLine(hx - 1, hy - 1, hh + 2, c_white);
        V_DrawVertLine(hx + hw, hy - 1, hh + 2, c_white);
    }

    // Hint lines.
    UIDrawText(10, 180, "ARROWS:MOVE TAB:PANE ENTER:PICK/PLACE E:EQUIP");
    UIDrawText(10, 190, "R:CANCEL Q:UNEQUIP ALL X:DROP  MOUSE:DRAG/CLICK");

    // Transient message.
    if (ui_msg[0] && gametic < ui_msgtic)
        UIDrawTextCentered(SCREENWIDTH / 2, 168, ui_msg);

    // Held item follows the cursor.
    if (ui_held != D_NOITEM)
    {
        const diablo_itemdef_t *def = UIHeldDef();
        if (def)
            UIDrawItemIcon(ui_cx - 8, ui_cy - 8,
                           def->grid_w * UI_CELL,
                           def->grid_h * UI_CELL,
                           ui_held);
    }

    // Tooltip for the hovered item (mouse cursor or kb selection;
    // the kb cursor syncs ui_cx/ui_cy, so this works for both).
    hover = UIHoverItem(player);
    if (hover != D_NOITEM)
        UIDrawTooltip(hover);

    // Cursor crosshair (mouse mode only; kb mode has the highlight box).
    if (!ui_kb_active)
    {
        V_DrawHorizLine(ui_cx - 4, ui_cy, 9, c_white);
        V_DrawVertLine(ui_cx, ui_cy - 4, 9, c_white);
    }
}
