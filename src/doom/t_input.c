//
// t_input.c — Turn-based mode: input adapter.
//
// Translates raw key events into discrete turn actions. Only directions
// and (later) actor IDs ever leave this layer — never mouse position,
// analog magnitude, or crosshair state.
//
// Phase 3 keymap (character screen closed):
//   W/A/S/D or arrows .. screen-relative step (1 TP)
//   X .................. swap weapon (2 TP)
//   SPACE .............. use / interact (2 TP)
//   . or Z ............. wait (1 TP)
//   H .................. hunker (2 TP; defense bonus lands in phase 6)
//   O .................. overwatch (all remaining TP, min 3; reaction in phase 6)
//   T .................. end turn -> enemy phase
//   TAB / ] ............ next target (free)
//   [ .................. previous target (free)
//   1-9 ................ select target by number (free)
//   F or ENTER ......... attack: preview -> confirm -> fire (4 TP)
//   ESC ................ cancel targeting (free)
// C/E/Q/R keep their Diablo inventory meanings and are NOT intercepted.

#include "t_turn.h"
#include "doomstat.h"
#include "d_player.h"
#include "d_diablo_ui.h"
#include "doomkeys.h"

// Map a key code to a turn action; TA_NONE if unmapped.
static turnaction_t T_KeyAction(int key)
{
    switch (key)
    {
      case 'w': case 'W': case KEY_UPARROW:    return TA_MOVE_N;
      case 's': case 'S': case KEY_DOWNARROW:  return TA_MOVE_S;
      case 'a': case 'A': case KEY_LEFTARROW:  return TA_MOVE_W;
      case 'd': case 'D': case KEY_RIGHTARROW: return TA_MOVE_E;
      case ' ':                                return TA_USE;
      case '.': case 'z': case 'Z':             return TA_WAIT;
      case 'x': case 'X':                      return TA_SWAP_WEAPON;
      case 'h': case 'H':                      return TA_HUNKER;
      case 'o': case 'O':                      return TA_OVERWATCH;
      case 't': case 'T':                      return TA_END_TURN;
      case KEY_TAB: case ']':                   return TA_SELECT_NEXT;
      case '[':                                return TA_SELECT_PREV;
      case 'f': case 'F': case KEY_ENTER:       return TA_ATTACK;
      case KEY_ESCAPE:                         return TA_CANCEL;
      case '1': case '2': case '3':
      case '4': case '5': case '6':
      case '7': case '8': case '9':             return TA_SELECT_NUM;
      default:                                 return TA_NONE;
    }
}

static boolean T_KeyMapped(int key)
{
    return T_KeyAction(key) != TA_NONE;
}

static void T_ExecuteAction(turnaction_t action)
{
    switch (action)
    {
      case TA_MOVE_N: T_DoMove(0); break;
      case TA_MOVE_E: T_DoMove(1); break;
      case TA_MOVE_S: T_DoMove(2); break;
      case TA_MOVE_W: T_DoMove(3); break;
      case TA_USE:    T_DoUse();   break;
      case TA_WAIT:   T_DoWait();  break;
      case TA_SWAP_WEAPON: T_DoSwapWeapon(); break;
      case TA_HUNKER: T_DoHunker(); break;
      case TA_OVERWATCH: T_DoOverwatch(); break;
      case TA_END_TURN: T_DoEndTurn(); break;
      case TA_SELECT_NEXT: T_DoSelectNext(); break;
      case TA_SELECT_PREV: T_DoSelectPrev(); break;
      case TA_ATTACK: T_DoAttack(); break;
      case TA_CANCEL: T_DoCancel(); break;
      default: break;
    }
}

// Number-key selection carries its digit outside the action enum.
static void T_ExecuteNumKey(int key)
{
    turnctrl.selected_num = key - '0';
    T_DoSelectNum(turnctrl.selected_num);
}

boolean T_Responder(event_t *ev)
{
    turnaction_t action;

    if (!turnbased_mode || !T_Ready())
        return false;
    if (gamestate != GS_LEVEL)
        return false;
    // The character screen owns all input while open (it already ran
    // ahead of G_Responder in D_ProcessEvents).
    if (D_UIIsOpen())
        return false;
    // No analog input in turn mode: swallow mouse and joystick gameplay
    // events so no angle/forward motion accumulates into ticcmds.
    // (Menus and the character screen already had first crack above.)
    if (ev->type == ev_mouse || ev->type == ev_joystick)
        return true;
    if (ev->type != ev_keydown && ev->type != ev_keyup)
        return false;

    // During pulses the world is resolving: lock gameplay input.
    // (Menu keys never reach here; M_Responder runs first.)
    if (T_InPulse())
        return T_KeyMapped(ev->data1);

    if (ev->type == ev_keyup)
        return T_KeyMapped(ev->data1); // swallow key releases of our keys

    action = T_KeyAction(ev->data1);
    if (action == TA_NONE)
        return false;

    // Discrete action. TP is spent only on confirmation inside the
    // action; selection and cancellation stay free.
    if (action == TA_SELECT_NUM)
        T_ExecuteNumKey(ev->data1);
    else
        T_ExecuteAction(action);
    return true;
}
