//
// Copyright(C) 1993-1996 Id Software, Inc.
// Copyright(C) 2005-2014 Simon Howard
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//	Weapon sprite animation, weapon objects.
//	Action functions for weapons.
//


#include "doomdef.h"
#include "d_event.h"

#include "deh_misc.h"

#include "m_random.h"
#include "p_local.h"
#include "s_sound.h"
#include "w_wad.h"

// State.
#include "doomstat.h"

// Data.
#include "sounds.h"

#include "p_pspr.h"
#include "d_diablo.h"
#include "t_turn.h"
#include "t_combat.h"

#define LOWERSPEED		FRACUNIT*6
#define RAISESPEED		FRACUNIT*6

#define WEAPONBOTTOM	128*FRACUNIT
#define WEAPONTOP		32*FRACUNIT

// Diablo turn-combat chain-fire suppression. P_FireDiabloWeapon fires
// the shot synchronously and then sets the weapon psprite to the
// attack state so the animation (including the SSG reload frames)
// plays; without this flag the state machine would walk into the
// fire frame a few tics later and fire a second time (the c30dbad7
// double-fire). The A_Fire* functions return early while it is set.
// Cleared when the weapon returns to ready (A_WeaponReady), at the
// start of every P_FireWeapon / P_FireDiabloWeapon call (so a stale
// set from a save/load can never suppress a fresh shot), and at the
// start of P_FireDiabloWeapon before the direct fire. (2026-11-06)
static boolean t_diablo_chain_fire_suppressed = false;

// Diablo attack/reload chain split (2026-11-07, weapon by weapon).
// The attack state chain is split at the fire state (the state whose
// action fires the shot): attack frames (atkstate through the fire
// state, inclusive) scale with attack speed, reload frames (after
// the fire state, up to the readystate) scale with the cooldown.
// The split is per weapon -- nine cases, one pivot each -- because
// every chain has a different shape (the chaingun and chainsaw fire
// on two states; the plasma fires on its first state; the rocket
// and chaingun have no real reload segment, just a 0-tic ReFire).
// The scale is stored as two fractions (num/den) applied to each
// frame's vanilla tics, so the relative timing within a segment is
// preserved. Attack scale is the derived attack-speed factor
// (attack_tp / 4: 1.0 at Dex 10, 0.5 at high Dex). Reload scale is
// the effective cooldown over the vanilla reload-segment total, so
// the reload segment lasts exactly the cooldown in tics; on a
// normal (non-emptying) shot the reload segment plays at vanilla
// speed (1:1), which makes a real reload visibly slower/longer
// than a normal shot's follow-through. The phase flag flips when
// the chain advances past the fire state. Cleared when the weapon
// returns to ready (A_WeaponReady) and at the start of
// P_FireDiabloWeapon.
static boolean t_diablo_reload_active = false;
static statenum_t t_diablo_chain_fire_state = S_NULL; // pivot, this chain
static boolean t_diablo_chain_in_reload = false; // past the fire state
static int t_diablo_atk_num = 1;   // attack-speed scale numerator
static int t_diablo_atk_den = 1;   // attack-speed scale denominator
static int t_diablo_rld_num = 1;   // reload scale numerator
static int t_diablo_rld_den = 1;   // reload scale denominator



//
// P_SetPsprite
//
void
P_SetPsprite
( player_t*	player,
  int		position,
  statenum_t	stnum ) 
{
    pspdef_t*	psp;
    state_t*	state;
	
    psp = &player->psprites[position];
	
    do
    {
	if (!stnum)
	{
	    // object removed itself
	    psp->state = NULL;
	    break;	
	}
	
	state = &states[stnum];
	psp->state = state;
	psp->tics = state->tics;	// could be 0

	if (state->misc1)
	{
	    // coordinate set
	    psp->sx = state->misc1 << FRACBITS;
	    psp->sy = state->misc2 << FRACBITS;
	}
	
	// Call action routine.
	// Modified handling.
	if (state->action.acp2)
	{
	    state->action.acp2(player, psp);
	    if (!psp->state)
		break;
	}
	
	stnum = psp->state->nextstate;
	
    } while (!psp->tics);
    // an initial state of 0 could cycle through
}



//
// P_CalcSwing
//	
fixed_t		swingx;
fixed_t		swingy;

void P_CalcSwing (player_t*	player)
{
    fixed_t	swing;
    int		angle;
	
    // OPTIMIZE: tablify this.
    // A LUT would allow for different modes,
    //  and add flexibility.

    swing = player->bob;

    angle = (FINEANGLES/70*leveltime)&FINEMASK;
    swingx = FixedMul ( swing, finesine[angle]);

    angle = (FINEANGLES/70*leveltime+FINEANGLES/2)&FINEMASK;
    swingy = -FixedMul ( swingx, finesine[angle]);
}



//
// P_BringUpWeapon
// Starts bringing the pending weapon up
// from the bottom of the screen.
// Uses player
//
void P_BringUpWeapon (player_t* player)
{
    statenum_t	newstate;
	
    if (player->pendingweapon == wp_nochange)
	player->pendingweapon = player->readyweapon;
		
    if (player->pendingweapon == wp_chainsaw)
	S_StartSound (player->mo, sfx_sawup);
		
    newstate = weaponinfo[player->pendingweapon].upstate;

    player->pendingweapon = wp_nochange;
    player->psprites[ps_weapon].sy = WEAPONBOTTOM;

    P_SetPsprite (player, ps_weapon, newstate);
}

//
// P_CheckAmmo
// Returns true if there is enough ammo to shoot.
// If not, selects the next weapon to use.
//
boolean P_CheckAmmo (player_t* player)
{
    ammotype_t		ammo;
    int			count;

    ammo = weaponinfo[player->readyweapon].ammo;

    // Minimal amount for one shot varies.
    if (player->readyweapon == wp_bfg)
	count = deh_bfg_cells_per_shot;
    else if (player->readyweapon == wp_supershotgun)
	count = 2;	// Double barrel.
    else
	count = 1;	// Regular.

    // Some do not need ammunition anyway.
    // Return if current ammunition sufficient.
    if (ammo == am_noammo || player->ammo[ammo] >= count)
	return true;
		
    // Out of ammo: DO NOT auto-switch weapons. The Diablo inventory
    // system controls the equipped weapon; vanilla auto-switch would
    // bypass the Diablo equip path and change the viewmodel silently.
    // Just return false (don't fire); the Diablo ammo system (T_KitCanFire)
    // enforces the real ammo limit and shows NO AMMO in the HUD.
    return false;
}


//
// P_FireWeapon.
//
void P_FireWeapon (player_t* player)
{
    // Diablo mode is always active in this fork (single-player, in a
    // level; T_Active is false only in netgame, which the fork does
    // not support, or outside a level, where the player cannot fire).
    // Route all fire through P_FireDiabloWeapon so explore and turn
    // combat share one fire path, including the reload flag and chain
    // scaling -- the magazine refills when the reload animation
    // completes in both modes. (2026-11-07)
    if (T_Active())
    {
        P_FireDiabloWeapon(player);
        return;
    }

    // Vanilla fallback (unreachable in gameplay).
    if (!P_CheckAmmo (player))
        return;

    P_SetMobjState (player->mo, S_PLAY_ATK1);
    P_SetPsprite (player, ps_weapon,
                  weaponinfo[player->readyweapon].atkstate);
    P_NoiseAlert (player->mo, player->mo);
}



// The fire state (pivot) of a weapon's attack chain, weapon by
// weapon (2026-11-07). The attack segment runs from atkstate
// through this state, inclusive; the reload segment is everything
// after it up to the readystate. Weapons with two firing states
// (chaingun, chainsaw) pivot on the LAST one, so both firing
// states count as attack. The plasma fires on its first state,
// so its attack segment is a single state and its long hold is
// the whole reload segment.
static statenum_t T_DiabloFireState(weapontype_t w)
{
    switch (w)
    {
        case wp_fist:         return S_PUNCH2;   // A_Punch
        case wp_pistol:       return S_PISTOL2;  // A_FirePistol
        case wp_shotgun:      return S_SGUN2;    // A_FireShotgun
        case wp_chaingun:     return S_CHAIN2;   // last A_FireCGun
        case wp_missile:      return S_MISSILE2; // A_FireMissile
        case wp_plasma:       return S_PLASMA1;  // A_FirePlasma
        case wp_bfg:          return S_BFG3;     // A_FireBFG
        case wp_chainsaw:     return S_SAW2;     // last A_Saw
        case wp_supershotgun: return S_DSGUN2;   // A_FireShotgun2
        default:              return weaponinfo[w].atkstate;
    }
}

// Total vanilla tics of a weapon's reload segment: from the state
// after the fire state up to the readystate (exclusive of the
// readystate itself). Used as the denominator for the reload scale,
// so the reload segment lasts exactly the cooldown in tics.
// Bounded so a malformed chain cannot loop forever. (2026-11-07)
static int T_DiabloReloadChainTics(weapontype_t w, statenum_t fire)
{
    statenum_t st = states[fire].nextstate;
    statenum_t ready = weaponinfo[w].readystate;
    int total = 0;
    int guard = 0;
    while (st != ready && st != S_NULL && guard < 32)
    {
        int t = states[st].tics;
        if (t < 1)
            t = 1;
        total += t;
        st = states[st].nextstate;
        guard++;
    }
    if (total < 1)
        total = 1;
    return total;
}



// P_FireDiabloWeapon.
//
// The fork's single fire path: explore and turn combat both fire
// through here (P_FireWeapon delegates to it in Diablo mode, and the
// turn executor calls it directly). Gates and spends the magazine,
// then fires the shot synchronously by calling the A_Fire*
// functions directly, so the turn executor's hit resolution
// (t_last_damage, T_ResolveKill, kill beat) still reads the result
// right after the call. A deferred shot (vanilla P_FireWeapon) would
// break that resolution; the old T_NativeFire fired synchronously but
// never set the psprite, so no weapon showed its attack animation in
// turn mode. The chain would re-fire a few tics later (the c30dbad7
// double-fire), so the direct fire sets t_diablo_chain_fire_suppressed
// and the A_Fire* functions skip while it is set; the suppression
// clears when the weapon returns to ready (A_WeaponReady). Returns
// true if the shot fired, false if the weapon could not fire (gate).
// (2026-11-06; single fire path 2026-11-07)
//
boolean
P_FireDiabloWeapon
( player_t* player )
{
    const t_kitdef_t *kit;

    // Clear any stale suppression from an interrupted chain, so the
    // direct fire below is never suppressed. Also clear any stale
    // reload scale, so a previous weapon's cooldown cannot stretch
    // this shot's animation.
    t_diablo_chain_fire_suppressed = false;
    t_diablo_reload_active = false;
    t_diablo_chain_in_reload = false;
    t_diablo_chain_fire_state = S_NULL;

    // Gate + spend, exactly as P_FireWeapon's Diablo branch. (The turn
    // executor still validates first for its point-refund skip; this
    // is the second line of defense.)
    kit = T_KitForWeapon(player->readyweapon);
    if (kit->ammo_cost > 0 && !T_KitCanFire(player->readyweapon))
    {
        const char *reason = T_KitDenyReason(player->readyweapon);
        if (reason != NULL)
            player->message = reason;
        return false;
    }
    if (kit->ammo_cost > 0)
    {
        T_KitSpendAmmo(player->readyweapon);
        T_KitOnFire(player);
    }

    // Fire synchronously (the old T_NativeFire logic, folded in): the
    // damage callback runs now, so the executor reads t_last_damage
    // right after this returns. The A_Fire* functions take (player,
    // pspdef) but psp is unused for the core fire logic; pass NULL
    // except for the chaingun, whose A_FireCGun uses psp->state to
    // select the flash frame (dummy pspdef pointing at S_CHAIN1).
    switch (player->readyweapon)
    {
        case wp_fist:        A_Punch(player, NULL); break;
        case wp_pistol:      A_FirePistol(player, NULL); break;
        case wp_shotgun:
            // Doom 1 WAD SSG fallback: Diablo SSG uses 20-pellet fire
            // even with shotgun sprites. A_FireShotgun2 uses the
            // readyweapon's flashstate (shotgun, safe) for the muzzle flash.
            {
                int id = player->diablo_equipped[ESLOT_WEAPON];
                boolean is_ssg = false;
                if (id != D_NOITEM)
                {
                    const diablo_itemdef_t *def =
                        D_GetItemDef(D_ITEMTIER(id), D_ITEMIDX(id));
                    if (def && def->doomweapon == wp_supershotgun)
                        is_ssg = true;
                }
                if (is_ssg)
                    A_FireShotgun2(player, NULL);
                else
                    A_FireShotgun(player, NULL);
            }
            break;
        case wp_chaingun:
            {
                pspdef_t dummy_psp;
                dummy_psp.state = &states[S_CHAIN1];
                A_FireCGun(player, &dummy_psp);
            }
            break;
        case wp_missile:     A_FireMissile(player, NULL); break;
        case wp_plasma:      A_FirePlasma(player, NULL); break;
        case wp_bfg:         A_FireBFG(player, NULL); break;
        case wp_chainsaw:    A_Saw(player, NULL); break;
        case wp_supershotgun: A_FireShotgun2(player, NULL); break;
        default:             A_FirePistol(player, NULL); break;
    }

    // Play the attack state chain on EVERY shot, not just on reload
    // (vanilla P_FireWeapon behavior, now the single fire path).
    // Attack/reload split (2026-11-07, weapon by weapon): the chain
    // is split at the fire state. Attack frames (atkstate through
    // the fire state) scale with attack speed (attack_tp / 4, from
    // T_DeriveStats: 1.0 at Dex 10, 0.5 at high Dex). Reload frames
    // (after the fire state) scale with the cooldown -- on a reload
    // shot the reload segment lasts exactly the cooldown in tics;
    // on a normal shot it plays at vanilla speed, which makes a
    // real reload visibly slower/longer than a normal shot's
    // follow-through. The reload flag is set only when the magazine
    // is empty, so the refill triggers on chain completion; on a
    // normal shot the chain just plays and the weapon returns to
    // ready. A cooldown of 0 (fists) means no reload to time, but
    // the attack chain still plays. Do not early-return without
    // the animation.
    // Reload sequence note: vanilla has a reload sequence only for
    // the shotgun (pump, S_SGUN3-8) and SSG (breach, S_DSGUN3-9).
    // The other guns have no reload sequence; their reload is the
    // recovery frames (the unfireable state) stretched to fill the
    // cooldown -- a compromise, not a fake reload.
    {
        boolean reloading = (kit->ammo_cost > 0
                             && !T_HasAmmoForKit(player->readyweapon));
        int cd = T_KitReloadTics(player->readyweapon);
        statenum_t fire = T_DiabloFireState(player->readyweapon);
        int vanilla_rld =
            T_DiabloReloadChainTics(player->readyweapon, fire);
        t_combatstats_t st;

        t_diablo_chain_fire_state = fire;
        t_diablo_chain_in_reload = false;

        // Attack scale: derived attack speed (attack_tp / 4).
        T_DeriveStats(player, &st);
        t_diablo_atk_num = st.attack_tp;
        t_diablo_atk_den = 4;

        if (reloading)
        {
            // Reload: the reload segment lasts the cooldown.
            // Refill on completion (T_KitRefillAmmo is a no-op
            // for the inverted plasma magazine; its heat drains
            // on its own).
            t_diablo_rld_num = cd;
            t_diablo_rld_den = vanilla_rld;
            t_diablo_reload_active = true;
        }
        else
        {
            // Normal shot: the reload segment plays at vanilla
            // speed. No refill.
            t_diablo_rld_num = 1;
            t_diablo_rld_den = 1;
            t_diablo_reload_active = false;
        }
    }

    // Suppress the chain re-fire, then play the animation, mirroring
    // P_FireWeapon. The psprite is set to the attack state, so the
    // weapon animation — including the SSG reload frames — plays, but
    // the fire frame's A_Fire* call returns early on the flag. The
    // first attack frame's tics are scaled by attack speed;
    // P_MovePsprites scales the rest, switching to the reload
    // factor once the chain advances past the fire state.
    // Chainsaw (2026-11-07): when attacking, play S_SAW2 (the
    // cutting frame); when reloading, go back to S_SAW1 (the
    // atkstate), so the reload plays the chain from the start.
    t_diablo_chain_fire_suppressed = true;
    P_SetMobjState (player->mo, S_PLAY_ATK1);
    {
        statenum_t atkstate =
            weaponinfo[player->readyweapon].atkstate;
        if (player->readyweapon == wp_chainsaw
            && !t_diablo_reload_active)
            atkstate = S_SAW2;
        P_SetPsprite (player, ps_weapon, atkstate);
    }
    {
        // The first frame is always in the attack segment
        // (atkstate is at or before the fire state), so it
        // scales with attack speed.
        pspdef_t *psp = &player->psprites[ps_weapon];
        int scaled = psp->tics * t_diablo_atk_num
                     / t_diablo_atk_den;
        if (scaled < 1)
            scaled = 1;
        psp->tics = scaled;
    }
    P_NoiseAlert (player->mo, player->mo);
    return true;
}



//
// P_DropWeapon
// Player died, so put the weapon away.
//
void P_DropWeapon (player_t* player)
{
    P_SetPsprite (player,
		  ps_weapon,
		  weaponinfo[player->readyweapon].downstate);
}



//
// A_WeaponReady
// The player can fire the weapon
// or change to another weapon at this time.
// Follows after getting weapon up,
// or after previous attack/fire sequence.
//
void
A_WeaponReady
( player_t*	player,
  pspdef_t*	psp )
{	
    statenum_t	newstate;
    int		angle;

    // Diablo turn combat: the attack chain is over; chain fire is no
    // longer suppressed, and any cooldown-tied reload scale ends.
    // (2026-11-06) A finished reload refills the magazine to full
    // at once -- ammo is infinite (2026-11-07).
    t_diablo_chain_fire_suppressed = false;
    if (t_diablo_reload_active)
    {
        T_KitRefillAmmo(player->readyweapon);
        t_diablo_reload_active = false;
    }
    
    // get out of attack state
    if (player->mo->state == &states[S_PLAY_ATK1]
	|| player->mo->state == &states[S_PLAY_ATK2] )
    {
	P_SetMobjState (player->mo, S_PLAY);
    }
    
    if (player->readyweapon == wp_chainsaw
	&& psp->state == &states[S_SAW])
    {
	S_StartSound (player->mo, sfx_sawidl);
    }
    
    // check for change
    //  if player is dead, put the weapon away
    if (player->pendingweapon != wp_nochange || !player->health)
    {
	// change weapon
	//  (pending weapon should allready be validated)
	newstate = weaponinfo[player->readyweapon].downstate;
	P_SetPsprite (player, ps_weapon, newstate);
	return;	
    }
    
    // check for fire
    //  the missile launcher and bfg do not auto fire
    if (player->cmd.buttons & BT_ATTACK)
    {
	if ( !player->attackdown
	     || (player->readyweapon != wp_missile
		 && player->readyweapon != wp_bfg) )
	{
	    player->attackdown = true;
	    P_FireWeapon (player);		
	    return;
	}
    }
    else
	player->attackdown = false;
    
    // bob the weapon based on movement speed
    angle = (128*leveltime)&FINEMASK;
    psp->sx = FRACUNIT + FixedMul (player->bob, finecosine[angle]);
    angle &= FINEANGLES/2-1;
    psp->sy = WEAPONTOP + FixedMul (player->bob, finesine[angle]);
}



//
// A_ReFire
// The player can re-fire the weapon
// without lowering it entirely.
//
void A_ReFire
( player_t*	player,
  pspdef_t*	psp )
{
    
    // check for fire
    //  (if a weaponchange is pending, let it go through instead)
    if ( (player->cmd.buttons & BT_ATTACK) 
	 && player->pendingweapon == wp_nochange
	 && player->health)
    {
	player->refire++;
	P_FireWeapon (player);
    }
    else
    {
	player->refire = 0;
	P_CheckAmmo (player);
    }
}


void
A_CheckReload
( player_t*	player,
  pspdef_t*	psp )
{
    P_CheckAmmo (player);
#if 0
    if (player->ammo[am_shell]<2)
	P_SetPsprite (player, ps_weapon, S_DSNR1);
#endif
}



//
// A_Lower
// Lowers current weapon,
//  and changes weapon at bottom.
//
void
A_Lower
( player_t*	player,
  pspdef_t*	psp )
{	
    psp->sy += LOWERSPEED;

    // Is already down.
    if (psp->sy < WEAPONBOTTOM )
	return;

    // Player is dead.
    if (player->playerstate == PST_DEAD)
    {
	psp->sy = WEAPONBOTTOM;

	// don't bring weapon back up
	return;		
    }
    
    // The old weapon has been lowered off the screen,
    // so change the weapon and start raising it
    if (!player->health)
    {
	// Player is dead, so keep the weapon off screen.
	P_SetPsprite (player,  ps_weapon, S_NULL);
	return;	
    }
	
    player->readyweapon = player->pendingweapon; 

    P_BringUpWeapon (player);
}


//
// A_Raise
//
void
A_Raise
( player_t*	player,
  pspdef_t*	psp )
{
    statenum_t	newstate;
	
    psp->sy -= RAISESPEED;

    if (psp->sy > WEAPONTOP )
	return;
    
    psp->sy = WEAPONTOP;
    
    // The weapon has been raised all the way,
    //  so change to the ready state.
    newstate = weaponinfo[player->readyweapon].readystate;

    P_SetPsprite (player, ps_weapon, newstate);
}



//
// A_GunFlash
//
void
A_GunFlash
( player_t*	player,
  pspdef_t*	psp ) 
{
    P_SetMobjState (player->mo, S_PLAY_ATK2);
    P_SetPsprite (player,ps_flash,weaponinfo[player->readyweapon].flashstate);
}



//
// WEAPON ATTACKS
//


//
// A_Punch
//
void
A_Punch
( player_t*	player,
  pspdef_t*	psp ) 
{
    angle_t	angle;
    int		damage;
    int		slope;

    // Diablo turn combat: the shot was already fired synchronously by
    // P_FireDiabloWeapon; the state chain reaching this frame must not
    // fire again. (2026-11-06)
    if (t_diablo_chain_fire_suppressed)
	return;
	
    damage = (P_Random ()%10+1)<<1;

    if (player->powers[pw_strength])	
	damage *= 10;

    angle = player->mo->angle;
    angle += P_SubRandom() << 18;
    slope = P_AimLineAttack (player->mo, angle, MELEERANGE);
    P_LineAttack (player->mo, angle, MELEERANGE, slope, damage);

    // turn to face target
    if (linetarget)
    {
	S_StartSound (player->mo, sfx_punch);
	player->mo->angle = R_PointToAngle2 (player->mo->x,
					     player->mo->y,
					     linetarget->x,
					     linetarget->y);
    }
}


//
// A_Saw
//
void
A_Saw
( player_t*	player,
  pspdef_t*	psp ) 
{
    angle_t	angle;
    int		damage;
    int		slope;

    // Diablo turn combat: the shot was already fired synchronously by
    // P_FireDiabloWeapon; the state chain reaching this frame must not
    // fire again. (2026-11-06)
    if (t_diablo_chain_fire_suppressed)
	return;

    damage = 2*(P_Random ()%10+1);
    angle = player->mo->angle;
    angle += P_SubRandom() << 18;
    
    // use meleerange + 1 se the puff doesn't skip the flash
    slope = P_AimLineAttack (player->mo, angle, MELEERANGE+1);
    P_LineAttack (player->mo, angle, MELEERANGE+1, slope, damage);

    if (!linetarget)
    {
	S_StartSound (player->mo, sfx_sawful);
	return;
    }
    S_StartSound (player->mo, sfx_sawhit);
	
    // turn to face target
    angle = R_PointToAngle2 (player->mo->x, player->mo->y,
			     linetarget->x, linetarget->y);
    if (angle - player->mo->angle > ANG180)
    {
	if ((signed int) (angle - player->mo->angle) < -ANG90/20)
	    player->mo->angle = angle + ANG90/21;
	else
	    player->mo->angle -= ANG90/20;
    }
    else
    {
	if (angle - player->mo->angle > ANG90/20)
	    player->mo->angle = angle - ANG90/21;
	else
	    player->mo->angle += ANG90/20;
    }
    player->mo->flags |= MF_JUSTATTACKED;
}

// Doom does not check the bounds of the ammo array.  As a result,
// it is possible to use an ammo type > 4 that overflows into the
// maxammo array and affects that instead.  Through dehacked, for
// example, it is possible to make a weapon that decreases the max
// number of ammo for another weapon.  Emulate this.

static void DecreaseAmmo(player_t *player, int ammonum, int amount)
{
    if (ammonum < NUMAMMO)
    {
        player->ammo[ammonum] -= amount;
    }
    else
    {
        player->maxammo[ammonum - NUMAMMO] -= amount;
    }
}


//
// A_FireMissile
//
void
A_FireMissile
( player_t*	player,
  pspdef_t*	psp ) 
{
    // Diablo turn combat: the shot was already fired synchronously by
    // P_FireDiabloWeapon; the state chain reaching this frame must not
    // fire again. (2026-11-06)
    if (t_diablo_chain_fire_suppressed)
	return;

    DecreaseAmmo(player, weaponinfo[player->readyweapon].ammo, 1);
    P_SpawnPlayerMissile (player->mo, MT_ROCKET);
}


//
// A_FireBFG
//
void
A_FireBFG
( player_t*	player,
  pspdef_t*	psp ) 
{
    // Diablo turn combat: the shot was already fired synchronously by
    // P_FireDiabloWeapon; the state chain reaching this frame must not
    // fire again. (2026-11-06)
    if (t_diablo_chain_fire_suppressed)
	return;

    DecreaseAmmo(player, weaponinfo[player->readyweapon].ammo, 
                 deh_bfg_cells_per_shot);
    P_SpawnPlayerMissile (player->mo, MT_BFG);
}



//
// A_FirePlasma
//
void
A_FirePlasma
( player_t*	player,
  pspdef_t*	psp ) 
{
    // Diablo turn combat: the shot was already fired synchronously by
    // P_FireDiabloWeapon; the state chain reaching this frame must not
    // fire again. (2026-11-06)
    if (t_diablo_chain_fire_suppressed)
	return;

    DecreaseAmmo(player, weaponinfo[player->readyweapon].ammo, 1);

    P_SetPsprite (player,
		  ps_flash,
		  weaponinfo[player->readyweapon].flashstate+(P_Random ()&1) );

    P_SpawnPlayerMissile (player->mo, MT_PLASMA);
}



//
// P_BulletSlope
// Sets a slope so a near miss is at aproximately
// the height of the intended target
//
fixed_t		bulletslope;


void P_BulletSlope (mobj_t*	mo)
{
    angle_t	an;
    
    // see which target is to be aimed at
    an = mo->angle;
    bulletslope = P_AimLineAttack (mo, an, 16*64*FRACUNIT);

    if (!linetarget)
    {
	an += 1<<26;
	bulletslope = P_AimLineAttack (mo, an, 16*64*FRACUNIT);
	if (!linetarget)
	{
	    an -= 2<<26;
	    bulletslope = P_AimLineAttack (mo, an, 16*64*FRACUNIT);
	}
    }
}


//
// P_GunShot
//
void
P_GunShot
( mobj_t*	mo,
  boolean	accurate,
  boolean	cone )
{
    angle_t	angle;
    int		damage;
    fixed_t	slope;
    int		spread_pct = 100;	// percent of full spread

    // Dexterity tightens the spread (2026-11-07, option B):
    // factor = 100 / (100 + 2*dex), so the spread is quartered
    // (the sniper point) at dex 150 -- only a dedicated max-dex
    // build gets there. Effective dex = 10 + gear Dex, the same
    // dex Block/Defense/accuracy use. Only the player's shots
    // scale; monsters have no Dex. Applies to the shotgun cone
    // (all 7 pellets), the chaingun cone (refire shots), and the
    // pistol's refire spread; the pistol's first shot is accurate
    // (no spread) and is untouched, and the SSG's own inline
    // spread is untouched.
    if (mo->player != NULL)
    {
	int dex = 10 + mo->player->diablo_stats[DSTAT_DEX];
	spread_pct = 10000 / (100 + 2 * dex);
    }

    damage = 5*(P_Random ()%3+1);
    angle = mo->angle;
    slope = bulletslope;

    if (!accurate)
    {
	int r = P_SubRandom();
	angle += (r * spread_pct / 100) << 18;
	if (cone)
	{
	    r = P_SubRandom();
	    slope += (r * spread_pct / 100) << 5;
	}
    }

    P_LineAttack (mo, angle, MISSILERANGE, slope, damage);
}


//
// A_FirePistol
//
void
A_FirePistol
( player_t*	player,
  pspdef_t*	psp ) 
{
    // Diablo turn combat: the shot was already fired synchronously by
    // P_FireDiabloWeapon; the state chain reaching this frame must not
    // fire again. (2026-11-06)
    if (t_diablo_chain_fire_suppressed)
	return;

    S_StartSound (player->mo, sfx_pistol);

    P_SetMobjState (player->mo, S_PLAY_ATK2);
    DecreaseAmmo(player, weaponinfo[player->readyweapon].ammo, 1);

    P_SetPsprite (player,
		  ps_flash,
		  weaponinfo[player->readyweapon].flashstate);

    P_BulletSlope (player->mo);
    P_GunShot (player->mo, !player->refire, false);
}


//
// A_FireShotgun
//
void
A_FireShotgun
( player_t*	player,
  pspdef_t*	psp ) 
{
    int		i;
	
    // Diablo turn combat: the shot was already fired synchronously by
    // P_FireDiabloWeapon; the state chain reaching this frame must not
    // fire again. (2026-11-06)
    if (t_diablo_chain_fire_suppressed)
	return;

    S_StartSound (player->mo, sfx_shotgn);
    P_SetMobjState (player->mo, S_PLAY_ATK2);

    DecreaseAmmo(player, weaponinfo[player->readyweapon].ammo, 1);

    P_SetPsprite (player,
		  ps_flash,
		  weaponinfo[player->readyweapon].flashstate);

    P_BulletSlope (player->mo);
	
    for (i=0 ; i<7 ; i++)
	P_GunShot (player->mo, false, true);
}



//
// A_FireShotgun2
//
void
A_FireShotgun2
( player_t*	player,
  pspdef_t*	psp ) 
{
    int		i;
    angle_t	angle;
    int		damage;

    // Diablo turn combat: the shot was already fired synchronously by
    // P_FireDiabloWeapon; the state chain reaching this frame must not
    // fire again. (2026-11-06)
    if (t_diablo_chain_fire_suppressed)
	return;
		
	
    // Fallback: if the WAD lacks the SSG sound (Doom 1), use shotgun sound.
    if (W_CheckNumForName("DSDBOPN") < 0 && W_CheckNumForName("DSDSHTGN") < 0)
        S_StartSound (player->mo, sfx_shotgn);
    else
        S_StartSound (player->mo, sfx_dshtgn);
    P_SetMobjState (player->mo, S_PLAY_ATK2);

    DecreaseAmmo(player, weaponinfo[player->readyweapon].ammo, 2);

    P_SetPsprite (player,
		  ps_flash,
		  weaponinfo[player->readyweapon].flashstate);

    P_BulletSlope (player->mo);
	
    for (i=0 ; i<20 ; i++)
    {
	damage = 5*(P_Random ()%3+1);
	angle = player->mo->angle;
	angle += P_SubRandom() << ANGLETOFINESHIFT;
	P_LineAttack (player->mo,
		      angle,
		      MISSILERANGE,
		      bulletslope + (P_SubRandom() << 5), damage);
    }
}


//
// A_FireCGun
//
void
A_FireCGun
( player_t*	player,
  pspdef_t*	psp ) 
{
    // Diablo turn combat: the shot was already fired synchronously by
    // P_FireDiabloWeapon; the state chain reaching this frame must not
    // fire again. (2026-11-06)
    if (t_diablo_chain_fire_suppressed)
	return;

    S_StartSound (player->mo, sfx_pistol);

    if (!player->ammo[weaponinfo[player->readyweapon].ammo]
        && !T_Active())
	return;
		
    P_SetMobjState (player->mo, S_PLAY_ATK2);
    DecreaseAmmo(player, weaponinfo[player->readyweapon].ammo, 1);

    P_SetPsprite (player,
		  ps_flash,
		  weaponinfo[player->readyweapon].flashstate
		  + psp->state
		  - &states[S_CHAIN1] );

    P_BulletSlope (player->mo);
	
    P_GunShot (player->mo, !player->refire, true);
}



//
// ?
//
void A_Light0 (player_t *player, pspdef_t *psp)
{
    player->extralight = 0;
}

void A_Light1 (player_t *player, pspdef_t *psp)
{
    player->extralight = 1;
}

void A_Light2 (player_t *player, pspdef_t *psp)
{
    player->extralight = 2;
}


//
// P_FrustumSpray
// The BFG cone effect, separated from the BFG: fire 40 tracers over
// a 90-degree cone around the originator's facing angle, damaging
// every monster in view. Damage is the vanilla spray damage
// (15 rolls of 1-8 per tracer), not Diablo AD/AP: spray tracers
// carry no weapon id, so the Diablo damage callback does not apply
// to them. Guarded against re-entry: spray damage itself must not
// trigger another spray (see the P_DamageMobj frustum hook).
//
static boolean frustum_spraying = false;

void P_FrustumSpray (mobj_t *originator, angle_t angle)
{
    int			i;
    int			j;
    int			damage;
    angle_t		an;

    if (!originator || frustum_spraying)
        return;
    frustum_spraying = true;

    // offset angles from its attack angle
    for (i=0 ; i<40 ; i++)
    {
	an = angle - ANG90/2 + ANG90/40*i;

	P_AimLineAttack (originator, an, 16*64*FRACUNIT);

	if (!linetarget)
	    continue;

	P_SpawnMobj (linetarget->x,
		     linetarget->y,
		     linetarget->z + (linetarget->height>>2),
		     MT_EXTRABFG);

	damage = 0;
	for (j=0;j<15;j++)
	    damage += (P_Random()&7) + 1;

	P_DamageMobj (linetarget, originator, originator, damage);
    }

    frustum_spraying = false;
}

boolean P_FrustumSpraying(void)
{
    return frustum_spraying;
}

//
// A_BFGSpray
// Spawn a BFG explosion on every monster in view -- but only when
// the firing player has the frustum modifier on ANY equipped slot.
// Without it, the BFG ball still deals its direct hit; the cone
// effect is the modifier's, not the weapon's.
//
void A_BFGSpray (mobj_t* mo)
{
    player_t *player;

    if (!mo || !mo->target)
        return;
    player = mo->target->player;
    if (!player)
        return;
    if (!(D_ActiveMechs(player) & MECH_BFSTORM))
        return;
    P_FrustumSpray(mo->target, mo->angle);
}


//
// A_BFGsound
//
void
A_BFGsound
( player_t*	player,
  pspdef_t*	psp )
{
    S_StartSound (player->mo, sfx_bfg);
}



//
// P_SetupPsprites
// Called at start of level for each player.
//
void P_SetupPsprites (player_t* player) 
{
    int	i;
	
    // remove all psprites
    for (i=0 ; i<NUMPSPRITES ; i++)
	player->psprites[i].state = NULL;
		
    // spawn the gun
    player->pendingweapon = player->readyweapon;
    P_BringUpWeapon (player);
}




//
// P_MovePsprites
// Called every tic by player thinking routine.
//
void P_MovePsprites (player_t* player) 
{
    int		i;
    pspdef_t*	psp;
	
    psp = &player->psprites[0];
    for (i=0 ; i<NUMPSPRITES ; i++, psp++)
    {
	// a null state means not active
	if (psp->state)
	{
	    // drop tic count and possibly change state

	    // a -1 tic count never changes
	    if (psp->tics != -1)	
	    {
		psp->tics--;
		if (!psp->tics)
		{
		    // Diablo attack/reload split: note whether the
		    // state being left is the fire state, so the
		    // new state can be classified. The phase flips
		    // to reload once the chain advances past it.
		    boolean leaving_fire =
			(i == ps_weapon
			 && t_diablo_chain_fire_suppressed
			 && psp->state ==
			    &states[t_diablo_chain_fire_state]);
		    P_SetPsprite (player, i, psp->state->nextstate);
		    if (leaving_fire)
			t_diablo_chain_in_reload = true;
		    // Diablo reload: when the chain reaches the ready
		    // state the reload is done (A_WeaponReady also
		    // clears the flag; this is the safety net) -- and
		    // ammo is infinite, so the finished reload
		    // refills the magazine to full at once
		    // (2026-11-07). Otherwise, scale the new frame:
		    // attack frames by attack speed, reload frames
		    // by the cooldown, for the whole chain (not just
		    // on a reload shot).
		    if (i == ps_weapon && t_diablo_chain_fire_suppressed)
		    {
			if (psp->state ==
			    &states[weaponinfo[player->readyweapon]
				    .readystate])
			{
			    if (t_diablo_reload_active)
			    {
				T_KitRefillAmmo(player->readyweapon);
				t_diablo_reload_active = false;
			    }
			}
			else
			{
			    int scaled;
			    if (t_diablo_chain_in_reload)
				scaled = psp->tics * t_diablo_rld_num
					 / t_diablo_rld_den;
			    else
				scaled = psp->tics * t_diablo_atk_num
					 / t_diablo_atk_den;
			    if (scaled < 1)
				scaled = 1;
			    psp->tics = scaled;
			}
		    }
		}
	    }				
	}
    }
    
    player->psprites[ps_flash].sx = player->psprites[ps_weapon].sx;
    player->psprites[ps_flash].sy = player->psprites[ps_weapon].sy;
}


