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
//	Handling interactions (i.e., collisions).
//




// Data.
#include "doomdef.h"
#include "dstrings.h"
#include "sounds.h"

#include "deh_main.h"
#include "deh_misc.h"
#include "doomstat.h"
#include "t_combat.h"

#include "m_random.h"
#include "i_system.h"
#include "d_replay.h"

// Manual respawn: corpse waiting for RESPAWN command confirmation.
mobj_t *pending_corpse = NULL;
int pending_corpse_player = -1;

// Completes the corpse-run respawn after RESPAWN command.
// Called from driver (t_turn.c). Returns true if respawned.
boolean P_DoCorpseRespawn(void)
{
    extern void G_PlayerReborn(int player);
    extern void P_SpawnPlayer(mapthing_t *mthing);
    int pnum;
    int i;
    // If no pending corpse (e.g. loaded a save with a dead player),
    // still allow respawn if the player is actually dead.
    if (pending_corpse == NULL || pending_corpse_player < 0)
    {
        for (pnum = 0; pnum < MAXPLAYERS; pnum++)
            if (playeringame[pnum] && players[pnum].playerstate == PST_DEAD)
                break;
        if (pnum >= MAXPLAYERS)
            return false;
        // Fall through with pnum set, corpse pointer stays NULL.
        // The corpse mobj (if any) remains in the world for recovery.
    }
    else
    {
        pnum = pending_corpse_player;
    }
    // G_PlayerReborn clears inventory (already saved to corpse)
    G_PlayerReborn(pnum);
    // Clear the preserved gear - it lives on the corpse now
    for (i = 0; i < 10; i++)
    {
        players[pnum].diablo_equipped[i] = -1; // D_NOITEM
        players[pnum].diablo_equipped_dur[i] = 0;
    }
    for (i = 0; i < D_BACKPACK_MAX; i++) {
        players[pnum].diablo_backpack[i] = -1;
        players[pnum].diablo_bp_gx[i] = -1;
        players[pnum].diablo_bp_gy[i] = -1;
        players[pnum].diablo_bp_page[i] = 0;
        players[pnum].diablo_backpack_dur[i] = 0;
    }
    players[pnum].diablo_bp_count = 0;
    /* Unlocked pages persist through death; only the view resets. */
    players[pnum].diablo_bp_cur = 0;
    players[pnum].diablo_essence = 0;
    players[pnum].playerstate = PST_LIVE;
    P_SpawnPlayer(&playerstarts[pnum]);
    players[pnum].message = "Retrieve your corpse!";
    pending_corpse = NULL;
    pending_corpse_player = -1;
    return true;
}

#include "am_map.h"

#include "p_local.h"
#include "p_pspr.h"
#include "t_turn.h"

// Diablo equipment backend (mod).
#include "d_diablo.h"

#include "s_sound.h"

#include "p_inter.h"


#define BONUSADD	6




// a weapon is found with two clip loads,
// a big item has five clip loads
// DiabloDoom: am_clip is the unified MANA pool (100 max; the backpack
// doubles it to 200). All ammo pickups funnel into it (see P_GiveAmmo);
// the other types keep their slots only so old code paths stay valid.
int	maxammo[NUMAMMO] = {100, 50, 300, 50};
int	clipammo[NUMAMMO] = {10, 4, 20, 1};


//
// GET STUFF
//

//
// P_GiveAmmo
// Num is the number of clip loads,
// not the individual count (0= 1/2 clip).
// Returns false if the ammo can't be picked up at all
//

boolean
P_GiveAmmo
( player_t*	player,
  ammotype_t	ammo,
  int		num )
{
    (void)player;
    (void)num;

    if (ammo == am_noammo)
	return false;

    if (ammo >= NUMAMMO)
	I_Error ("P_GiveAmmo: bad type %i", ammo);

    // DiabloDoom: ammo pickups are caches — they half-fill every
    // weapon's magazine and vent half the plasma heat. If every
    // magazine is already full the pickup stays on the ground.
    // (Per-type amounts and difficulty scaling no longer apply:
    // magazines are the ammo economy now.)
    if (!P_AmmoCacheHasRoom())
	return false;
    T_KitPickupAmmo();

    return true;
}

// Could an ammo cache do anything right now? Some magazine below
// max, or plasma heat to vent. Factored out of P_GiveAmmo so the
// targeting code (COLLECT) tests exactly what the pickup will do:
// with magazines auto-replenishing in explore, "full" is the steady
// state, and a cache that can never be consumed must not be
// targeted as if it were loot.
boolean P_AmmoCacheHasRoom(void)
{
    int w;

    for (w = 0; w < NUMWEAPONS; w++)
    {
        const t_kitdef_t *kit = T_KitForWeapon((weapontype_t)w);
        // Inverted magazines (plasma) have room when their counter
        // can vent down; normal magazines when they can fill up.
        if (kit->ammo_inverted)
        {
            if (T_KitAmmo((weapontype_t)w) > 0)
                return true;
        }
        else if (kit->ammo_max > 0
                 && T_KitAmmo((weapontype_t)w) < kit->ammo_max)
            return true;
    }
    return false;
}

// Is this special an ammo cache -- one of the sprites
// P_TouchSpecialThing feeds to P_GiveAmmo? (Diablo loot reuses some
// of these sprites; callers that must tell loot from ammo have to
// check the mobj type as well, as T_DoCollect's fallback does by
// construction -- its candidates are never loot-type mobjs.)
boolean P_IsAmmoCache(mobj_t *special)
{
    switch (special->sprite)
    {
        case SPR_CLIP:
        case SPR_AMMO:
        case SPR_ROCK:
        case SPR_BROK:
        case SPR_CELL:
        case SPR_CELP:
        case SPR_SHEL:
        case SPR_SBOX:
            return true;
        default:
            return false;
    }
}


// The rarity half of a Diablo loot roll (defined with the loot
// system below): generous odds, improved by magic find.
static int P_RollRarityTier(int magic_find);

//
// P_GiveWeapon
// The weapon name may have a MF_DROPPED flag ored in.
//
boolean
P_GiveWeapon
( player_t*	player,
  weapontype_t	weapon,
  boolean	dropped )
{
    boolean	gaveammo;
    boolean	gaveweapon;
    boolean	gaveitem = false;

    if (netgame
	&& (deathmatch!=2)
	 && !dropped )
    {
	// leave placed weapons forever on net games
	if (player->weaponowned[weapon])
	    return false;

	player->bonuscount += BONUSADD;
	player->weaponowned[weapon] = true;

	if (deathmatch)
	    P_GiveAmmo (player, weaponinfo[weapon].ammo, 5);
	else
	    P_GiveAmmo (player, weaponinfo[weapon].ammo, 2);
	// Don't auto-switch: the Diablo inventory controls the equipped weapon.
	if (player == &players[consoleplayer])
	    S_StartSound (NULL, sfx_wpnup);
	return false;
    }
	
    // Diablo: a weapon pickup grants a Diablo item of the same
    // weapon type, rolled on the standard rarity table with the
    // player's magic find -- first acquisition or duplicate alike
    // -- so the weapon exists in the system that controls
    // equipment (and duplicates are loot, not dead walk-overs:
    // with magazines auto-replenishing, the ammo half below almost
    // never fires in explore, which used to make every repeat
    // pickup a silent no-op). The tier tables have gaps (no
    // unique pistol, say), so a rarity roll that lands on a tier
    // with no weapon of this type is simply re-rolled. If the
    // item doesn't fit, the pickup is refused whole: the weapon
    // stays on the ground.
    {
        int magic_find = player->diablo_stats[DSTAT_MAGICFIND]
                       + D_LoopMagicFind();
        int id = D_NOITEM;
        int tries;

        for (tries = 0; tries < 32 && id == D_NOITEM; tries++)
            id = D_RollWeaponItem(P_RollRarityTier(magic_find), weapon);
        if (id == D_NOITEM)
            id = D_BaseItemForWeapon(weapon);

        if (id != D_NOITEM)
        {
            if (!D_BackpackAdd(player, D_ITEMTIER(id),
                               D_ITEMIDX(id)))
            {
                D_BackpackFullMsg(player);
                return false;
            }
            gaveitem = true;
            {
                const diablo_itemdef_t *gdef =
                    D_GetItemDef(D_ITEMTIER(id), D_ITEMIDX(id));
                char namebuf[96];
                D_ItemDisplayName(gdef, namebuf, sizeof(namebuf));
                printf("[LOOT] weapon pickup: granted \"%s\" (tier=%d idx=%d)\n",
                       namebuf, D_ITEMTIER(id), D_ITEMIDX(id));
            }
        }
    }

    if (weaponinfo[weapon].ammo != am_noammo)
    {
	// give one clip with a dropped weapon,
	// two clips with a found weapon
	if (dropped)
	    gaveammo = P_GiveAmmo (player, weaponinfo[weapon].ammo, 1);
	else
	    gaveammo = P_GiveAmmo (player, weaponinfo[weapon].ammo, 2);
    }
    else
	gaveammo = false;

    if (player->weaponowned[weapon])
	gaveweapon = false;
    else
    {
	gaveweapon = true;
	player->weaponowned[weapon] = true;
	// Don't auto-switch: the Diablo inventory controls the equipped
	// weapon, and the view model must reflect the inventory.
	// Explicitly clear any pending switch (vanilla may have set it).
	player->pendingweapon = wp_nochange;
	printf("[LOOT] picked up weapon=%d\n", weapon);
    }

    return (gaveweapon || gaveammo || gaveitem);
}

 

//
// P_GiveBody
// Returns false if the body isn't needed at all
//
boolean
P_GiveBody
( player_t*	player,
  int		num )
{
    if (player->health >= MAXHEALTH)
	return false;
		
    player->health += num;
    if (player->health > MAXHEALTH)
	player->health = MAXHEALTH;
    player->mo->health = player->health;
	
    return true;
}



//
// P_GiveArmor
// Returns false if the armor is worse
// than the current armor.
//
boolean
P_GiveArmor
( player_t*	player,
  int		armortype )
{
    int		hits;
	
    hits = armortype*100;
    if (player->armorpoints >= hits)
	return false;	// don't pick up
		
    player->armortype = armortype;
    player->armorpoints = hits;
	
    return true;
}



//
// P_GiveCard
//
void
P_GiveCard
( player_t*	player,
  card_t	card )
{
    if (player->cards[card])
	return;
    
    player->bonuscount = BONUSADD;
    player->cards[card] = 1;
}


//
// P_GivePower
//
boolean
P_GivePower
( player_t*	player,
  int /*powertype_t*/	power )
{
    if (power == pw_invulnerability)
    {
	player->powers[power] = INVULNTICS;
	return true;
    }
    
    if (power == pw_invisibility)
    {
	player->powers[power] = INVISTICS;
	player->mo->flags |= MF_SHADOW;
	return true;
    }
    
    if (power == pw_infrared)
    {
	player->powers[power] = INFRATICS;
	return true;
    }
    
    if (power == pw_ironfeet)
    {
	player->powers[power] = IRONTICS;
	return true;
    }
    
    if (power == pw_strength)
    {
	P_GiveBody (player, 100);
	player->powers[power] = 1;
	return true;
    }
	
    if (player->powers[power])
	return false;	// already got it
		
    player->powers[power] = 1;
    return true;
}



// --------------------------------------------------------------------
// Diablo loot system (mod).
//
// Every slain monster has a chance to drop loot.  Loot comes in five
// rarity tiers, each with its own pool of Diablo-universe item names.
// Drop odds are deliberately generous so uniques actually show up.
// --------------------------------------------------------------------

// (Diablo item names now live in d_diablo.c's data tables.)

// Spawn a loot pickup for a specific item id at (x, y). Shared by
// kill drops and player-initiated drops from the inventory.
void P_SpawnDiabloLootAt(fixed_t x, fixed_t y, int item_id)
{
    int tier = D_ITEMTIER(item_id);
    int index = D_ITEMIDX(item_id);
    mobjtype_t type = MT_LOOT_NORMAL + tier;
    mobj_t *mo;
    const diablo_itemdef_t *def;
    statenum_t st;

    mo = P_SpawnMobj(x, y, ONFLOORZ, type);
    mo->flags |= MF_DROPPED;
    mo->diablo_loot_id = item_id;

    // Show an item-appropriate sprite instead of the tier's stock sprite.
    // (Pickup still keys off the mobj type, so this is purely visual.)
    def = D_GetItemDef(tier, index);
    st = S_BON1; // fallback
    if (def)
    {
        if (def->consumable)
        {
            // Potions: healing -> stimpack, mana -> armor bonus,
            // blast -> rocket (explosive).
            if (def->usekind == USE_HEAL)
                st = S_STIM;
            else if (def->usekind == USE_MANA)
                st = S_BON2;
            else
                st = S_ROCK;
        }
        else switch (def->slot)
        {
            case ESLOT_WEAPON:
                // Gun-appropriate pickup sprite per Doom base type.
                // (Doom has no pistol pickup; the clip reads closest.)
                switch (def->doomweapon)
                {
                    case wp_pistol:   st = S_CLIP; break;
                    case wp_shotgun:  st = S_SHOT; break;
                    case wp_supershotgun:
                        // Doom 1 IWADs have no SHT2 (super shotgun pickup)
                        // sprite lumps, so S_SHOT2 would render an invalid
                        // sprite frame (and crash the renderer). Fall back
                        // to the regular shotgun sprite there.
                        // NOTE: Check SPR_SHT2 (the pickup sprite used by
                        // S_SHOT2), not SPR_SGN2 (the weapon sprite).
                        st = (sprites[SPR_SHT2].numframes > 0) ? S_SHOT2
                                                             : S_SHOT;
                        break;
                    case wp_chaingun: st = S_MGUN; break;
                    case wp_missile:  st = S_LAUN; break;
                    case wp_plasma:   st = S_PLAS; break;
                    case wp_bfg:      st = S_BFUG; break;
                    case wp_chainsaw: st = S_CSAW; break;
                    default:          st = S_SHOT; break;
                }
                break;
            case ESLOT_ARMOR:  st = S_ARM1; break; // green armor
            case ESLOT_SHIELD: st = S_ARM2; break; // blue armor
            case ESLOT_HELM:   st = S_BON2; break;
            case ESLOT_RING1:
            case ESLOT_RING2:
            case ESLOT_AMULET: st = S_BON1; break; // small shiny
            case ESLOT_BELT:   st = S_BON2; break;
            case ESLOT_BOOTS:
            case ESLOT_GLOVES: st = S_BON1; break;
            default: break;
        }
    }
    P_SetMobjState(mo, st);
    // Full-bright: loot pops in dark sectors instead of sinking
    // into the floor lighting.
    mo->frame |= FF_FULLBRIGHT;
}

// Readable monster name for loot-drop logging.
static const char *P_MonsterName(mobjtype_t type)
{
    switch (type)
    {
        case MT_POSSESSED: return "Zombieman";
        case MT_SHOTGUY:   return "ShotgunGuy";
        case MT_CHAINGUY:  return "Chaingunner";
        case MT_VILE:      return "Archvile";
        case MT_UNDEAD:    return "Revenant";
        case MT_FATSO:     return "Mancubus";
        case MT_TROOP:     return "Imp";
        case MT_SERGEANT:  return "Demon";
        case MT_SHADOWS:   return "Spectre";
        case MT_HEAD:      return "Cacodemon";
        case MT_BRUISER:   return "Baron";
        case MT_KNIGHT:    return "HellKnight";
        case MT_SKULL:     return "LostSoul";
        case MT_SPIDER:    return "Spiderdemon";
        case MT_BABY:      return "Arachnotron";
        case MT_CYBORG:    return "Cyberdemon";
        case MT_PAIN:      return "PainElemental";
        case MT_WOLFSS:    return "WolfSS";
        case MT_KEEN:      return "Keen";
        case MT_BOSSBRAIN: return "BossBrain";
        default:           return "unknown";
    }
}

// The rarity half of a Diablo loot roll: generous odds (unique
// ~6%, set ~9%, rare ~20%, magic ~40%, normal ~25%), improved by
// magic find. Shared by full item drops and weapon pickups (which
// roll rarity, then pick a weapon of the rolled tier).
static int P_RollRarityTier(int magic_find)
{
    int roll;

    roll = P_Random() - magic_find / 2;
    if (roll < 0)
        roll = 0;
    if (roll < 15)
        return TIER_UNIQUE;
    if (roll < 38)
        return TIER_SET;
    if (roll < 89)
        return TIER_RARE;
    if (roll < 191)
        return TIER_MAGIC;
    return TIER_NORMAL;
}

// Roll a random Diablo item id using the kill-drop rarity table.
// magic_find shifts the rarity roll. Returns -1 if nothing is valid.
// Shared by kill drops and map-ammo conversion (both want "a random
// item drop"; only the 70% kill gate differs).
int P_RollDiabloItemDrop(int magic_find)
{
    int tier, index;

    tier = P_RollRarityTier(magic_find);

    // Pick a specific item from the tier's table, filtered by difficulty.
    // Higher difficulties unlock stronger affix tiers.
    index = D_RollItemForDifficulty(tier);
    if (index < 0)
        return -1; // no valid items for this difficulty (shouldn't happen)

    return D_MAKEITEM(tier, index);
}

// Roll a random armor-slot Diablo item id using the kill-drop rarity
// table, for the green-armor grant (Jimmy, 2026-10-29). Armor slots
// are the durability slots (helm/armor/boots/gloves/belt, per
// D_IsDurabilityItem); shields are excluded — they carry no
// durability and produce no Defense. Difficulty-filtered like
// D_RollWeaponItem. Falls back to NORMAL tier if the rolled tier
// has no difficulty-valid armor piece. Returns -1 if nothing fits.
int P_RollArmorItemDrop(int magic_find)
{
    int tier = P_RollRarityTier(magic_find);
    int attempt, i, count, pick;
    const diablo_itemdef_t *def;

    for (attempt = 0; attempt < 2; attempt++)
    {
        int t = (attempt == 0) ? tier : TIER_NORMAL;
        count = 0;
        for (i = 0; i < D_TierCount(t); i++)
        {
            def = D_GetItemDef(t, i);
            if (def && !def->consumable && D_IsDurabilityItem(def)
                && def->diff_tier <= D_DifficultyTier())
                count++;
        }
        if (!count)
            continue;
        pick = P_Random() % count;
        for (i = 0; i < D_TierCount(t); i++)
        {
            def = D_GetItemDef(t, i);
            if (def && !def->consumable && D_IsDurabilityItem(def)
                && def->diff_tier <= D_DifficultyTier()
                && pick-- == 0)
                return D_MAKEITEM(t, i);
        }
    }
    return -1;
}

// Roll loot for a dead monster and spawn the pickup.
static void P_DropDiabloLoot(mobj_t *target, mobj_t *source)
{
    int r, item_id;
    int magic_find = 0;

    // 70% chance that a kill drops something at all.
    r = P_Random();
    if (r >= 179)
        return;

    // Magic find from the killer (if a player) shifts the rarity roll.
    // NG+ loop adds bonus magic find.
    if (source && source->player)
        magic_find = source->player->diablo_stats[DSTAT_MAGICFIND] +
                     D_LoopMagicFind();

    item_id = P_RollDiabloItemDrop(magic_find);
    if (item_id < 0)
        return;

    P_SpawnDiabloLootAt(target->x, target->y, item_id);
    {
        // Log what dropped, from whom: answers "which enemy dropped
        // which item" without digging through the save.
        const diablo_itemdef_t *def =
            D_GetItemDef(D_ITEMTIER(item_id), D_ITEMIDX(item_id));
        char namebuf[96];
        D_ItemDisplayName(def, namebuf, sizeof(namebuf));
        printf("[LOOT] dropped \"%s\" tier=%d idx=%d by %s at (%d,%d)\n",
               namebuf,
               D_ITEMTIER(item_id), D_ITEMIDX(item_id),
               P_MonsterName(target->type),
               target->x >> 16, target->y >> 16);
    }
}

// Give the player a random item of the given rarity tier (0-4).


//
// P_TouchSpecialThing
//
void
P_TouchSpecialThing
( mobj_t*	special,
  mobj_t*	toucher )
{
    player_t*	player;
    int		i;
    fixed_t	delta;
    int		sound;
		
    delta = special->z - toucher->z;

    if (delta > toucher->height
	|| delta < -8*FRACUNIT)
    {
	// out of reach
	return;
    }
    
	
    sound = sfx_itemup;	
    player = toucher->player;

    // Dead thing touching.
    // Can happen with a sliding player corpse.
    if (toucher->health <= 0)
	return;

    // Diablo corpse recovery (Jimmy, 2026-10-21): like Diablo, items
    // come off the corpse ONE BY ONE -- never a wholesale overwrite.
    // (The old code replaced the player's entire inventory with the
    // corpse snapshot, silently deleting anything looted on the
    // corpse run.) Equipped pieces re-equip into empty slots first;
    // overflow and packed items go through the normal backpack add.
    // Whatever doesn't fit STAYS on the corpse, which remains in the
    // world so the player can junk something and come back for the
    // rest. Only a fully emptied corpse is removed.
    if (special->is_player_corpse)
    {
	int i;
	int moved = 0;
	int remaining = 0;
	boolean equipped_changed = false;
	player = toucher->player;
	if (!player) return;

	// Essence is not an item: it always transfers.
	if (special->corpse_essence > 0)
	{
	    player->diablo_essence += special->corpse_essence;
	    special->corpse_essence = 0;
	    moved++;
	}

	// Equipped gear, slot by slot.
	for (i = 0; i < 10; i++)
	{
	    int id = special->corpse_equipped[i];
	    if (id == D_NOITEM)
		continue;
	    if (player->diablo_equipped[i] == D_NOITEM)
	    {
		player->diablo_equipped[i] = id;
		player->diablo_equipped_dur[i] =
		    special->corpse_equipped_dur[i];
		special->corpse_equipped[i] = D_NOITEM;
		special->corpse_equipped_dur[i] = 0;
		equipped_changed = true;
		moved++;
	    }
	    else if (D_BackpackAddDur(player, D_ITEMTIER(id),
				      D_ITEMIDX(id),
				      special->corpse_equipped_dur[i]))
	    {
		special->corpse_equipped[i] = D_NOITEM;
		special->corpse_equipped_dur[i] = 0;
		moved++;
	    }
	    else
		remaining++;
	}

	// Packed items, one by one. D_BackpackAdd re-places each on
	// the player's own grid; entries that fit are struck from the
	// corpse's list (compacted in place), the rest stay.
	{
	    int r, w = 0;
	    for (r = 0; r < special->corpse_bp_count; r++)
	    {
		int id = special->corpse_backpack[r];
		if (D_BackpackAddDur(player, D_ITEMTIER(id),
				     D_ITEMIDX(id),
				     special->corpse_backpack_dur[r]))
		    moved++;
		else
		{
		    special->corpse_backpack[w] = id;
		    special->corpse_bp_gx[w] = special->corpse_bp_gx[r];
		    special->corpse_bp_gy[w] = special->corpse_bp_gy[r];
		    special->corpse_bp_page[w] = special->corpse_bp_page[r];
		    special->corpse_backpack_dur[w] =
			special->corpse_backpack_dur[r];
		    w++;
		    remaining++;
		}
	    }
	    special->corpse_bp_count = w;
    if (special->corpse_bp_pages > player->diablo_bp_pages)
        player->diablo_bp_pages = special->corpse_bp_pages;
	}

	if (equipped_changed)
	    D_RecalcStats(player);
	if (moved > 0)
	{
	    extern void D_AutoSaveGame(void);
	    D_AutoSaveGame();
	}

	if (remaining == 0)
	{
	    player->message = "Corpse recovered!";
	    printf("[FARM] CORPSE_RECOVERED - inventory restored\n");
	    P_RemoveMobj(special);
	}
	else
	{
	    // The presence sweep re-touches every tic while the player
	    // stands on the corpse: throttle the message and the log.
	    static int corpse_msg_tic = -TICRATE;
	    if (gametic - corpse_msg_tic >= TICRATE)
	    {
		corpse_msg_tic = gametic;
		player->message = "Backpack full -- corpse keeps the rest.";
		printf("[CORPSE] partial recovery: %d taken, %d left on corpse\n",
		       moved, remaining);
	    }
	}
	return;
    }

    // Diablo loot system: loot reuses stock sprites, so identify
    // it by mobj type before the sprite-based switch below.
    if (special->type >= MT_LOOT_NORMAL && special->type <= MT_LOOT_UNIQUE)
    {
        int loot_type = special->type; // save before P_RemoveMobj
        int item_id = special->diablo_loot_id;
        int tier, idx;
        // If for some reason there's no stored item, fall back to a
        // random item of the drop's tier.
        if (item_id == D_NOITEM)
            item_id = D_MAKEITEM(loot_type - MT_LOOT_NORMAL,
                                 P_Random() % D_TierCount(loot_type - MT_LOOT_NORMAL));
        tier = D_ITEMTIER(item_id);
        idx = D_ITEMIDX(item_id);
        // Stash it in the backpack; a full backpack leaves it on the ground.
        if (D_BackpackAdd(player, tier, idx))
        {
            P_RemoveMobj(special);
            printf("[LOOT] picked up tier=%d idx=%d\n", tier, idx);
            // Auto-save after loot pickup: don't lose the new item.
            // Saves both Doom savegame (slots 7-9) and progress.dat.
            {
                extern void D_AutoSaveGame(void);
                D_AutoSaveGame();
            }
        }
        else
            D_BackpackFullMsg(player);
        player->bonuscount += BONUSADD;
        if (player == &players[consoleplayer])
            S_StartSound(NULL,
                         loot_type >= MT_LOOT_SET ? sfx_getpow
                                                : sfx_itemup);
        return;
    }

    // Identify by sprite.
    switch (special->sprite)
    {
	// armor
      case SPR_ARM1:
	if (!P_GiveArmor (player, deh_green_armor_class))
	    return;
	player->message = DEH_String(GOTARMOR);
	// Diablo durability (mod): a vanilla armor pickup restores
	// every equipped piece to full durability.
	D_RepairAllEquipped(player);
	// Green-armor grant (Jimmy, 2026-10-29): on a successful
	// green pickup, also grant one random armor-slot item straight
	// into the backpack. Vanilla armorpoints are left alone
	// (grant and refusal gate above unchanged; a future change
	// may repurpose them as the Diablo damage reduction modifier).
	// Backpack-full fallback: spawn the rolled loot at the
	// player's feet so it is not lost — collectable after
	// freeing space, under the normal loot rules.
	{
	    int magic_find = player->diablo_stats[DSTAT_MAGICFIND]
	                     + D_LoopMagicFind();
	    int item_id = P_RollArmorItemDrop(magic_find);
	    if (item_id >= 0)
	    {
		const diablo_itemdef_t *def =
		    D_GetItemDef(D_ITEMTIER(item_id), D_ITEMIDX(item_id));
		char namebuf[96];
		D_ItemDisplayName(def, namebuf, sizeof(namebuf));
		if (D_BackpackAdd((struct player_s *)player,
		                  D_ITEMTIER(item_id), D_ITEMIDX(item_id)))
		{
		    printf("[GREENARMOR] granted \"%s\" tier=%d idx=%d\n",
		           namebuf, D_ITEMTIER(item_id), D_ITEMIDX(item_id));
		}
		else
		{
		    P_SpawnDiabloLootAt(player->mo->x, player->mo->y, item_id);
		    player->message = "Backpack full - armor loot dropped at your feet.";
		    printf("[GREENARMOR] backpack full, spawned \"%s\" at player\n",
		           namebuf);
		}
	    }
	}
	break;

      case SPR_ARM2:
	if (!P_GiveArmor (player, deh_blue_armor_class))
	    return;
	player->message = DEH_String(GOTMEGA);
	D_RepairAllEquipped(player);
	break;
	
	// bonus items
      case SPR_BON1:
	player->health++;		// can go over 100%
	if (player->health > deh_max_health)
	    player->health = deh_max_health;
	player->mo->health = player->health;
	player->message = DEH_String(GOTHTHBONUS);
	break;
	
      case SPR_BON2:
	player->armorpoints++;		// can go over 100%
	if (player->armorpoints > deh_max_armor && gameversion > exe_doom_1_2)
	    player->armorpoints = deh_max_armor;
        // deh_green_armor_class only applies to the green armor shirt;
        // for the armor helmets, armortype 1 is always used.
	if (!player->armortype)
	    player->armortype = 1;
	player->message = DEH_String(GOTARMBONUS);
	break;
	
      case SPR_SOUL:
	player->health += deh_soulsphere_health;
	// Diablo vitality raises max health; soulsphere respects it.
	if (player->health > D_MaxHealth(player))
	    player->health = D_MaxHealth(player);
	player->mo->health = player->health;
	player->message = DEH_String(GOTSUPER);
	if (gameversion > exe_doom_1_2)
	    sound = sfx_getpow;
	break;
	
      case SPR_MEGA:
	if (gamemode != commercial)
	    return;
	player->health = deh_megasphere_health;
	player->mo->health = player->health;
        // We always give armor type 2 for the megasphere; dehacked only 
        // affects the MegaArmor.
	P_GiveArmor (player, 2);
	player->message = DEH_String(GOTMSPHERE);
	D_RepairAllEquipped(player);
	if (gameversion > exe_doom_1_2)
	    sound = sfx_getpow;
	break;
	
	// cards
	// leave cards for everyone
      case SPR_BKEY:
	if (!player->cards[it_bluecard])
	    player->message = DEH_String(GOTBLUECARD);
	P_GiveCard (player, it_bluecard);
	if (!netgame)
	    break;
	return;
	
      case SPR_YKEY:
	if (!player->cards[it_yellowcard])
	    player->message = DEH_String(GOTYELWCARD);
	P_GiveCard (player, it_yellowcard);
	if (!netgame)
	    break;
	return;
	
      case SPR_RKEY:
	if (!player->cards[it_redcard])
	    player->message = DEH_String(GOTREDCARD);
	P_GiveCard (player, it_redcard);
	if (!netgame)
	    break;
	return;
	
      case SPR_BSKU:
	if (!player->cards[it_blueskull])
	    player->message = DEH_String(GOTBLUESKUL);
	P_GiveCard (player, it_blueskull);
	if (!netgame)
	    break;
	return;
	
      case SPR_YSKU:
	if (!player->cards[it_yellowskull])
	    player->message = DEH_String(GOTYELWSKUL);
	P_GiveCard (player, it_yellowskull);
	if (!netgame)
	    break;
	return;
	
      case SPR_RSKU:
	if (!player->cards[it_redskull])
	    player->message = DEH_String(GOTREDSKULL);
	P_GiveCard (player, it_redskull);
	if (!netgame)
	    break;
	return;
	
	// medikits, heals
      case SPR_STIM:
	if (!P_GiveBody (player, 10))
	    return;
	player->message = DEH_String(GOTSTIM);
	break;
	
      case SPR_MEDI:
	if (!P_GiveBody (player, 25))
	    return;

	if (player->health < 25)
	    player->message = DEH_String(GOTMEDINEED);
	else
	    player->message = DEH_String(GOTMEDIKIT);
	break;

	
	// power ups
      case SPR_PINV:
	if (!P_GivePower (player, pw_invulnerability))
	    return;
	player->message = DEH_String(GOTINVUL);
	if (gameversion > exe_doom_1_2)
	    sound = sfx_getpow;
	break;
	
      case SPR_PSTR:
	if (!P_GivePower (player, pw_strength))
	    return;
	player->message = DEH_String(GOTBERSERK);
	// Don't auto-switch to fist: the Diablo inventory controls the
	// equipped weapon. Berserk just gives the strength power.
	if (gameversion > exe_doom_1_2)
	    sound = sfx_getpow;
	break;
	
      case SPR_PINS:
	if (!P_GivePower (player, pw_invisibility))
	    return;
	player->message = DEH_String(GOTINVIS);
	if (gameversion > exe_doom_1_2)
	    sound = sfx_getpow;
	break;
	
      case SPR_SUIT:
	if (!P_GivePower (player, pw_ironfeet))
	    return;
	player->message = DEH_String(GOTSUIT);
	if (gameversion > exe_doom_1_2)
	    sound = sfx_getpow;
	break;
	
      case SPR_PMAP:
	if (!P_GivePower (player, pw_allmap))
	    return;
	player->message = DEH_String(GOTMAP);
	if (gameversion > exe_doom_1_2)
	    sound = sfx_getpow;
	break;
	
      case SPR_PVIS:
	if (!P_GivePower (player, pw_infrared))
	    return;
	player->message = DEH_String(GOTVISOR);
	if (gameversion > exe_doom_1_2)
	    sound = sfx_getpow;
	break;
	
	// ammo
      case SPR_CLIP:
	if (special->flags & MF_DROPPED)
	{
	    if (!P_GiveAmmo (player,am_clip,0))
		return;
	}
	else
	{
	    if (!P_GiveAmmo (player,am_clip,1))
		return;
	}
	player->message = DEH_String(GOTCLIP);
	break;
	
      case SPR_AMMO:
	if (!P_GiveAmmo (player, am_clip,5))
	    return;
	player->message = DEH_String(GOTCLIPBOX);
	break;
	
      case SPR_ROCK:
	if (!P_GiveAmmo (player, am_misl,1))
	    return;
	player->message = DEH_String(GOTROCKET);
	break;
	
      case SPR_BROK:
	if (!P_GiveAmmo (player, am_misl,5))
	    return;
	player->message = DEH_String(GOTROCKBOX);
	break;
	
      case SPR_CELL:
	if (!P_GiveAmmo (player, am_cell,1))
	    return;
	player->message = DEH_String(GOTCELL);
	break;
	
      case SPR_CELP:
	if (!P_GiveAmmo (player, am_cell,5))
	    return;
	player->message = DEH_String(GOTCELLBOX);
	break;
	
      case SPR_SHEL:
	if (!P_GiveAmmo (player, am_shell,1))
	    return;
	player->message = DEH_String(GOTSHELLS);
	break;
	
      case SPR_SBOX:
	if (!P_GiveAmmo (player, am_shell,5))
	    return;
	player->message = DEH_String(GOTSHELLBOX);
	break;
	
      case SPR_BPAK:
	if (!player->backpack)
	{
	    for (i=0 ; i<NUMAMMO ; i++)
		player->maxammo[i] *= 2;
	    player->backpack = true;
	}
	for (i=0 ; i<NUMAMMO ; i++)
	    P_GiveAmmo (player, i, 1);
	/* Backpack pages (Jimmy, 2026-10-29): every vanilla backpack
	   pickup unlocks one more Diablo backpack page, 1 by default
	   up to 4.  Ammo doubling above stays first-pickup-only. */
	if (player->diablo_bp_pages < D_BP_MAX_PAGES)
	{
	    player->diablo_bp_pages++;
	    {
		static char buf[64];  /* message pointer must outlive this call */
		M_snprintf(buf, sizeof(buf),
			   "Backpack expanded! Page %d of %d unlocked.",
			   player->diablo_bp_pages, D_BP_MAX_PAGES);
		player->message = buf;
	    }
	}
	else
	    player->message = DEH_String(GOTBACKPACK);
	break;
	
	// weapons
      case SPR_BFUG:
	if (!P_GiveWeapon (player, wp_bfg, false) )
	    return;
	player->message = DEH_String(GOTBFG9000);
	sound = sfx_wpnup;	
	break;
	
      case SPR_MGUN:
        if (!P_GiveWeapon(player, wp_chaingun,
                          (special->flags & MF_DROPPED) != 0))
            return;
	player->message = DEH_String(GOTCHAINGUN);
	sound = sfx_wpnup;	
	break;
	
      case SPR_CSAW:
	if (!P_GiveWeapon (player, wp_chainsaw, false) )
	    return;
	player->message = DEH_String(GOTCHAINSAW);
	sound = sfx_wpnup;	
	break;
	
      case SPR_LAUN:
	if (!P_GiveWeapon (player, wp_missile, false) )
	    return;
	player->message = DEH_String(GOTLAUNCHER);
	sound = sfx_wpnup;	
	break;
	
      case SPR_PLAS:
	if (!P_GiveWeapon (player, wp_plasma, false) )
	    return;
	player->message = DEH_String(GOTPLASMA);
	sound = sfx_wpnup;	
	break;
	
      case SPR_SHOT:
        if (!P_GiveWeapon(player, wp_shotgun,
                          (special->flags & MF_DROPPED) != 0))
            return;
	player->message = DEH_String(GOTSHOTGUN);
	sound = sfx_wpnup;	
	break;
		
      case SPR_SGN2:
        if (!P_GiveWeapon(player, wp_supershotgun,
                          (special->flags & MF_DROPPED) != 0))
            return;
	player->message = DEH_String(GOTSHOTGUN2);
	sound = sfx_wpnup;	
	break;
		
      default:
	I_Error ("P_SpecialThing: Unknown gettable thing");
    }
	
    if (special->flags & MF_COUNTITEM)
	player->itemcount++;
    P_RemoveMobj (special);
    player->bonuscount += BONUSADD;
    if (player == &players[consoleplayer])
	S_StartSound (NULL, sound);
}


//
// KillMobj
//
void
P_KillMobj
( mobj_t*	source,
  mobj_t*	target )
{

    target->flags &= ~(MF_SHOOTABLE|MF_FLOAT|MF_SKULLFLY);


    if (target->type != MT_SKULL)
	target->flags &= ~MF_NOGRAVITY;

    target->flags |= MF_CORPSE|MF_DROPOFF;
    target->height >>= 2;

    if (source && source->player)
    {
	// count for intermission
	if (target->flags & MF_COUNTKILL)
	    source->player->killcount++;	

	if (target->player)
	    source->player->frags[target->player-players]++;
    }
    else if (!netgame && (target->flags & MF_COUNTKILL) )
    {
	// count all monster deaths,
	// even those caused by other monsters
	players[0].killcount++;
    }
    
    if (target->player)
    {
	// count environment kills against you
	if (!source)	
	    target->player->frags[target->player-players]++;
			
	target->flags &= ~MF_SOLID;
	// Diablo-style death: drop inventory on corpse, respawn at level start.
	// Player must run back to corpse to recover gear.
	{
	    extern void G_PlayerReborn(int player);
	    extern void P_SpawnPlayer(mapthing_t *mthing);
	    int pnum = target->player - players;
	    player_t *p = &players[pnum];
	    int i;
	    printf("[FARM] PLAYER_DIED - dropping inventory on corpse\n");
	    // Manual respawn: player stays dead at 0% until RESPAWN command.
	    // Worker takes a regular SCREENSHOT for the death moment.
	    // Store inventory on the corpse mobj
	    target->is_player_corpse = true;
	    // Make corpse touchable for inventory recovery via P_TouchSpecialThing.
	    target->flags |= MF_SPECIAL;
	    for (i = 0; i < 10; i++)
	    {
		target->corpse_equipped[i] = p->diablo_equipped[i];
		target->corpse_equipped_dur[i] = p->diablo_equipped_dur[i];
	    }
	    for (i = 0; i < D_BACKPACK_MAX; i++) {
		target->corpse_backpack[i] = p->diablo_backpack[i];
		target->corpse_bp_gx[i] = p->diablo_bp_gx[i];
		target->corpse_bp_gy[i] = p->diablo_bp_gy[i];
		target->corpse_bp_page[i] = p->diablo_bp_page[i];
		target->corpse_backpack_dur[i] = p->diablo_backpack_dur[i];
	    }
	    target->corpse_bp_count = p->diablo_bp_count;
	    target->corpse_bp_pages = p->diablo_bp_pages;
	    target->corpse_essence = p->diablo_essence;
	    // Detach corpse from player so new spawn doesn't conflict.
	    // Leave it solid=false but touchable for inventory recovery.
	    target->player = NULL;
	    // Manual respawn: player stays dead at 0% until RESPAWN command.
	    // Store corpse for later respawn (driver command will complete it).
	    {
		extern mobj_t *pending_corpse;
		extern int pending_corpse_player;
		pending_corpse = target;
		pending_corpse_player = pnum;
	    }
	    // NOTE: G_PlayerReborn / P_SpawnPlayer moved to RESPAWN command.
	    // Player remains PST_DEAD at 0% until confirmed.
	    p->playerstate = PST_DEAD;
	    // Death prompt (Jimmy, 2026-10-23): the system message
	    // says how to get back in. ST_updateLootAlert re-issues
	    // it while the player is dead (the loot prompt would
	    // otherwise stomp it -- the corpse below is loot too).
	    p->message = "Press any key to respawn";
	    // Play the death animation. The vanilla deathstate
	    // transition below never runs for players -- this branch
	    // returns first -- so without this the corpse keeps its
	    // standing play state forever instead of falling.
	    if (target->health < -target->info->spawnhealth
		&& target->info->xdeathstate)
		P_SetMobjState(target, target->info->xdeathstate);
	    else
		P_SetMobjState(target, target->info->deathstate);
	    target->tics -= P_Random()&3;
	    if (target->tics < 1)
		target->tics = 1;
	    return;
	}

	if (target->player == &players[consoleplayer]
	    && automapactive)
	{
	    // don't die in auto map,
	    // switch view prior to dying
	    AM_Stop ();
	}
	
    }

    if (target->health < -target->info->spawnhealth 
	&& target->info->xdeathstate)
    {
	P_SetMobjState (target, target->info->xdeathstate);
    }
    else
	P_SetMobjState (target, target->info->deathstate);
    target->tics -= P_Random()&3;

    if (target->tics < 1)
	target->tics = 1;
		
    //	I_StartSound (&actor->r, actor->info->deathsound);

    // In Chex Quest, monsters don't drop items.

    if (gameversion == exe_chex)
    {
        return;
    }

    // Diablo loot system: slain monsters drop loot.
    if ((target->flags & MF_COUNTKILL) && target->player == NULL)
    {
        P_DropDiabloLoot(target, source);
    }

    // Diablo (Jimmy's rule, 2026-10-21): there are NO vanilla
    // monster drops. The "Drop stuff" switch that used to live here
    // (zombiemen -> clip, shotgun guy -> shotgun, chaingun guy ->
    // chaingun) is deleted entirely: P_DropDiabloLoot above is the
    // one and only monster drop mechanic. Map-placed ammo still
    // converts into a roll at spawn (P_SpawnMapThing) -- placement
    // is not a drop.
}




//
// P_DamageMobj
// Damages both enemies and players
// "inflictor" is the thing that caused the damage
//  creature or missile, can be NULL (slime, etc)
// "source" is the thing to target after taking damage
//  creature or NULL
// Source and inflictor are the same for melee attacks.
// Source can be NULL for slime, barrel explosions
// and other environmental stuff.
//
void
P_DamageMobj
( mobj_t*	target,
  mobj_t*	inflictor,
  mobj_t*	source,
  int 		damage )
{
    unsigned	ang;
    int		saved;
    player_t*	player;
    fixed_t	thrust;
    int		temp;
	
    if ( !(target->flags & MF_SHOOTABLE) )
	return;	// shouldn't happen...
		
    if (target->health <= 0)
	return;

    if ( target->flags & MF_SKULLFLY )
    {
	target->momx = target->momy = target->momz = 0;
    }
	
    player = target->player;
    // Debug: log when the player takes damage (turn-based investigation).
    if (player != NULL && damage > 0)
    {
        const char *srcname = "unknown";
        if (source != NULL)
        {
            if (source->player != NULL)
                srcname = "player";
            else if (source->type == MT_POSSESSED)
                srcname = "Zombieman";
            else if (source->type == MT_SHOTGUY)
                srcname = "ShotgunGuy";
            else if (source->type == MT_TROOP)
                srcname = "Imp";
            else if (source->type == MT_HEAD)
                srcname = "Cacodemon";
            else if (source->type == MT_BRUISER)
                srcname = "Baron";
            else if (source->type == MT_KNIGHT)
                srcname = "Knight";
            else if (source->type == MT_SKULL)
                srcname = "LostSoul";
            else if (source->type == MT_SPIDER)
                srcname = "Spiderdemon";
            else if (source->type == MT_CYBORG)
                srcname = "Cyberdemon";
        }
        printf("[TURN] player damaged: %d from %s (hp %d -> %d)\n",
               damage, srcname, target->health, target->health - damage);
    }
    if (player && gameskill == sk_baby)
	damage >>= 1; 	// take half damage in trainer mode
    // NG+ loop: enemies deal more damage each loop.
    if (player && d_loop > 0 && source && source != target &&
        !source->player)
        damage = damage * D_LoopDmgMult() / 100;

    // Diablo equipment (mod): when a player hurts a monster, add the
    // equipped weapon's damage bonus.
    // Turn mode: the Diablo damage callback already rolled crit exactly
    // once per attack (deterministic, once-per-attack pellet semantics),
    // so the legacy D_CritRoll here must be skipped or crits double-roll.
    if (source && source->player && target->player == NULL)
    {
        // Native weapon fire override (turn-based): when the turn system
        // triggers A_Fire* for visuals, replace Doom's damage with the
        // pre-rolled Diablo value. The override is set before the fire
        // and cleared after the attack resolves.
        if (t_damage_override)
        {
            damage = t_damage_override_value;
        }
        else
        {
            damage += D_WeaponBonus(source->player);
            if (!T_Active())
                damage = D_CritRoll(source->player, damage);
        }
    }
		

    // Some close combat weapons should not
    // inflict thrust and push the victim out of reach,
    // thus kick away unless using the chainsaw.
    if (inflictor
	&& !(target->flags & MF_NOCLIP)
	&& (!source
	    || !source->player
	    || source->player->readyweapon != wp_chainsaw))
    {
	ang = R_PointToAngle2 ( inflictor->x,
				inflictor->y,
				target->x,
				target->y);
		
	thrust = damage*(FRACUNIT>>3)*100/target->info->mass;

	// make fall forwards sometimes
	if ( damage < 40
	     && damage > target->health
	     && target->z - inflictor->z > 64*FRACUNIT
	     && (P_Random ()&1) )
	{
	    ang += ANG180;
	    thrust *= 4;
	}
		
	ang >>= ANGLETOFINESHIFT;
	target->momx += FixedMul (thrust, finecosine[ang]);
	target->momy += FixedMul (thrust, finesine[ang]);
    }
    
    // player specific
    if (player)
    {
	// end of game hell hack
	if (target->subsector->sector->special == 11
	    && damage >= target->health)
	{
	    damage = target->health - 1;
	}
	

	// Below certain threshold,
	// ignore damage in GOD mode, or with INVUL power.
	if ( damage < 1000
	     && ( (player->cheats&CF_GODMODE)
		  || player->powers[pw_invulnerability] ) )
	{
	    return;
	}
	
	// Diablo equipment (mod): dexterity dodge roll.  A successful
	// dodge avoids the hit entirely.
	if (player && D_DodgeRoll(player))
	{
	    // Small feedback: the damage indicator still flashes, but
	    // no health is lost.  (No message spam; dodges are common.)
	    player->damagecount += 10;
	    if (player->damagecount > 100)
		player->damagecount = 100;
	    return;
	}

	// Diablo 2-style defense (mod): a monster's chance to hit falls
	// with the player's Defense rating; a miss avoids the hit
	// entirely (feedback like a dodge). Only monster attacks roll:
	// environment, self damage and other players always land.
	if (player && source && source != target && !source->player
	    && !D_DefenseHitRoll(player, source))
	{
	    static int def_msg_tic = -TICRATE;
	    player->damagecount += 10;
	    if (player->damagecount > 100)
		player->damagecount = 100;
	    if (gametic - def_msg_tic >= TICRATE)
	    {
		def_msg_tic = gametic;
		printf("[DEFENSE] attack missed (DEF %d, CTH %d%%)\n",
		       D_Stat(player, DSTAT_ARMOR),
		       D_DefenseChanceToHit(player, source));
	    }
	    return;
	}

	// Diablo equipment (mod): elemental resistance by damage source.
	if (player)
	    damage = D_ResistReduce(player, inflictor, damage);

	// A landed hit wears one random durability piece (mod).
	if (player && damage > 0)
	    D_WearRandomPiece(player);

	if (player->armortype)
	{
	    if (player->armortype == 1)
		saved = damage/3;
	    else
		saved = damage/2;
	    
	    if (player->armorpoints <= saved)
	    {
		// armor is used up
		saved = player->armorpoints;
		player->armortype = 0;
	    }
	    player->armorpoints -= saved;
	    damage -= saved;
	}
	player->health -= damage; 	// mirror mobj health here for Dave
	if (player->health <= 0)
	{
	    player->health = 0;
	    // Ensure the mobj also reflects death so P_KillMobj fires below.
	    // Without this, the HUD shows 0% but the player never actually dies.
	    if (target)
		target->health = 0;
	}

	
	player->attacker = source;
	player->damagecount += damage;	// add damage after armor / invuln

	if (player->damagecount > 100)
	    player->damagecount = 100;	// teleport stomp does 10k points...
	
	temp = damage < 100 ? damage : 100;

	if (player == &players[consoleplayer])
	    I_Tactile (40,10,40+temp*2);
    }
    
    // Diablo turn-mode natural simulation (mod): when the player fires in
    // turn mode, Doom's hitscan/projectile determines hit/miss. On a hit,
    // use Diablo AD/AP damage instead of vanilla damage.
    // - Projectile: inflictor->diablo_weapon >= 0 (set at fire time)
    // - Hitscan: d_diablo_hitscan_weapon >= 0 (set before P_LineAttack)
    // Spray tracers keep their precomputed vanilla damage: the Diablo
    // callback must not overwrite it with weapon damage while spraying.
    if (d_diablo_damage_cb && source && source->player && target->player == NULL
        && !P_FrustumSpraying())
    {
        int weapon = -1;
        if (inflictor && inflictor->diablo_weapon >= 0)
            weapon = inflictor->diablo_weapon;
        else if (d_diablo_hitscan_weapon >= 0)
            weapon = d_diablo_hitscan_weapon;
        if (weapon >= 0)
        {
            damage = d_diablo_damage_cb(source->player, target, weapon);
        }
    }

    // do the damage	
    target->health -= damage;	

    // Diablo equipment (mod): life steal heals the attacker for a
    // percentage of damage dealt to monsters.
    if (source && source->player && target->player == NULL)
	D_LifeSteal(source->player, damage);

    // Diablo frustum modifier (mod): on a primary hit by the player,
    // if any equipped slot carries MECH_FRUSTUM and the firing weapon
    // is not the BFG (its ball landing already sprays, gated the same
    // way), spray the cone from the player's current facing angle.
    // Weapon id: projectile inflictor first, then the turn-mode
    // hitscan global, then the ready weapon for a synchronous
    // hitscan/melee hit (inflictor is the player mobj itself).
    // P_FrustumSpraying() blocks re-entry from spray damage itself.
    if (source && source->player && target->player == NULL
        && !P_FrustumSpraying())
    {
        int frustum_weapon = -1;
        if (inflictor && inflictor->diablo_weapon >= 0)
            frustum_weapon = inflictor->diablo_weapon;
        else if (d_diablo_hitscan_weapon >= 0)
            frustum_weapon = d_diablo_hitscan_weapon;
        else if (inflictor == source)
            frustum_weapon = source->player->readyweapon;
        if (frustum_weapon >= 0 && frustum_weapon != wp_bfg
            && (D_ActiveMechs(source->player) & MECH_FRUSTUM))
            P_FrustumSpray(source, source->angle);
    }

    if (target->health <= 0)
    {
	P_KillMobj (source, target);
	return;
    }

    if ( (P_Random () < target->info->painchance)
	 && !(target->flags&MF_SKULLFLY) )
    {
	target->flags |= MF_JUSTHIT;	// fight back!
	
	P_SetMobjState (target, target->info->painstate);
    }
			
    target->reactiontime = 0;		// we're awake now...	

    if ( (!target->threshold || target->type == MT_VILE)
	 && source && (source != target || gameversion < exe_doom_1_5)
	 && source->type != MT_VILE)
    {
	// if not intent on another player,
	// chase after this one
	target->target = source;
	target->threshold = BASETHRESHOLD;
	if (target->state == &states[target->info->spawnstate]
	    && target->info->seestate != S_NULL)
	    P_SetMobjState (target, target->info->seestate);
    }
			
}


//
// P_TouchNearbySpecials
//
// Pickup by presence (Jimmy, 2026-10-21): an item is picked up when
// it lies within reach of where the player IS, tested as a circle --
// never as a side effect of testing where the player might move to.
// PIT_CheckThing used to fire P_TouchSpecialThing at the candidate
// position inside P_TryMove/P_CheckPosition: the square test reached
// ~51 units on the diagonal against 36 head-on, a move physics then
// rejected (wall clip, slide) had already collected, and pure probes
// (the COLLECT pathfinder tests hundreds of positions per walk)
// touched items cells away from the player. Called once per tic from
// P_Ticker, after all movement has resolved, this is now the only
// caller of P_TouchSpecialThing. P_TouchSpecialThing's own vertical
// gate (item z within [-8, +height] of the player's feet) still
// applies unchanged.
//
void P_TouchNearbySpecials(mobj_t *toucher)
{
    // Candidates within reach are touched in ascending cartesian
    // distance order (Jimmy, 2026-10-21): when several items are in
    // reach on the same tic -- and especially when the backpack can
    // only fit some of them -- the nearest are picked up first, not
    // whichever happens to sit earlier in the thinker list. Sorting
    // by squared distance orders identically to distance itself.
    struct { mobj_t *thing; int64_t dist_sq; } near[64];
    int n = 0, i;
    thinker_t *th;

    if (!toucher || toucher->health <= 0)
        return;

    for (th = thinkercap.next; th != &thinkercap; th = th->next)
    {
        mobj_t *thing;
        int64_t dx, dy, reach, dist_sq;

        if (th->function.acp1 != (actionf_p1)P_MobjThinker)
            continue;
        thing = (mobj_t *)th;
        if (thing == toucher || !(thing->flags & MF_SPECIAL))
            continue;

        // Circle test in fixed point: dx,dy are 16.16, so the
        // squares are 32.32 and need 64-bit intermediates.
        dx = (int64_t)thing->x - toucher->x;
        dy = (int64_t)thing->y - toucher->y;
        reach = (int64_t)toucher->radius + thing->radius;
        dist_sq = dx * dx + dy * dy;
        if (dist_sq >= reach * reach)
            continue;

        // Insertion into the sorted candidate list. Beyond capacity
        // keep the nearest 64 (in-reach counts are a handful in
        // practice); a farther candidate is simply not touched this
        // tic and is reconsidered next tic.
        if (n == 64 && dist_sq >= near[63].dist_sq)
            continue;
        i = (n < 64) ? n++ : 63;
        while (i > 0 && near[i - 1].dist_sq > dist_sq)
        {
            near[i] = near[i - 1];
            i--;
        }
        near[i].thing = thing;
        near[i].dist_sq = dist_sq;
    }

    // Removal inside P_TouchSpecialThing is deferred to
    // P_RunThinkers (sentinel), so every candidate pointer stays
    // valid for the whole pass.
    for (i = 0; i < n; i++)
        P_TouchSpecialThing(near[i].thing, toucher);
}
