//
// d_diablo.c — DiabloDoom Phase 1: equipment backend.
//
// Data-driven item definitions plus the player equipment state machine:
// backpack, equip/unequip, stat aggregation, and the combat hooks that
// make the stats actually work.
//
// Stat formulas (kept deliberately simple):
//   weapon damage : +random(dmg_min..dmg_max) of equipped weapon, plus
//                   +1 per 4 strength, on every point of player damage
//   defense       : Diablo 2-style. The armor values of the durability
//                   slots (helm/armor/boots/gloves/belt) sum into a
//                   Defense rating (hundreds to thousands). A monster's
//                   chance to hit is 200 * AR / (AR + Defense), clamped
//                   to [5, 95]; AR = 60 + the attacker's spawn health.
//                   No damage reduction: a landed hit deals full damage.
//                   Pieces have durability; at 0 a piece adds no Defense
//                   until repaired (essence, or a vanilla armor pickup
//                   restores every equipped piece to full).
//   life steal    : heal damage_dealt * lifesteal% (vs monsters), capped
//                   at max health
//   vitality      : max health = deh_max_health + 5 * vitality
//   magic find    : rarity roll shifted down by magicfind / 2 (better loot)
//   move speed    : all movement displacement scales by
//                   (100 + movespeed) / 100 via D_ScaleDisplacement
//   resists/dex/energy: aggregated for Phase 2 (vanilla Doom has no
//                   elemental damage types to hook cleanly).
//
// Part of the DiabloDoom mod (GPL-2.0-or-later, like Chocolate Doom).

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "d_diablo.h"
#include "d_diablo_icons.h"
#include "d_pistol_sprite.h"
#include "d_ssg_sprite.h"
#include "d_player.h"
#include "deh_misc.h"
#include "deh_str.h"
#include "dstrings.h"
#include "doomdef.h"
#include "doomstat.h"
#include "m_random.h"
#include "p_local.h"
#include "t_combat.h"
#include "w_wad.h"

// Shorthand for the long positional initializers below.
// Fields: name, tier, slot, grid_w, grid_h, consumable, usekind, heal,
//         dmg_min, dmg_max, armor,
//         str, dex, vit, ene,
//         fres, cres, lres, pres,
//         lifesteal, magicfind, movespeed,
//         ad_pct, ap_pct, haste, crit_chance, crit_dmg
#define IT(n_, t_, s_, w_, h_, c_, u_, he_, dm_, dx_, ar_, \
           st_, de_, vi_, en_, fr_, cr_, lr_, pr_, ls_, mf_, ms_, \
           ad_, ap_, ha_, cc_, cd_, hm_, af_, pf_, cf_, dt_, dw_, me_) \
    { n_, t_, s_, w_, h_, c_, u_, he_, dm_, dx_, ar_, \
      st_, de_, vi_, en_, fr_, cr_, lr_, pr_, ls_, mf_, ms_, \
      ad_, ap_, ha_, cc_, cd_, hm_, af_, pf_, cf_, dt_, dw_, me_ }

static const diablo_itemdef_t diablo_normal[] =
{
    IT("Pistol",            TIER_NORMAL, ESLOT_WEAPON, 2,1, 0,0,0,   2, 6, 0,
       0,0,0,0,   0,0,0,0,   0,0,0,0,0,0,5,0, 0, 0, 0, 0, 0, wp_pistol, 0),
    IT("Shotgun",           TIER_NORMAL, ESLOT_WEAPON, 3,1, 0,0,0,   3, 8, 0,
       0,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0, 0, 0, 0, 0, 0, wp_shotgun, 0),

    IT("Leather Armor",     TIER_NORMAL, ESLOT_ARMOR,  2,3, 0,0,0,   0, 0, 200,
       0,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0, 0, 0, 0, 0, 0, -1, 0),
    IT("Buckler",           TIER_NORMAL, ESLOT_SHIELD, 2,2, 0,0,0,   0, 0, 3,
       0,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0, 0, 0, 0, 0, 0, -1, 0),
    IT("Cap",               TIER_NORMAL, ESLOT_HELM,   2,2, 0,0,0,   0, 0, 80,
       0,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0, 0, 0, 0, 0, 0, -1, 0),
    IT("Sash",              TIER_NORMAL, ESLOT_BELT,   2,1, 0,0,0,   0, 0, 80,
       0,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0, 0, 0, 0, 0, 0, -1, 0),
    IT("Healing Potion",    TIER_NORMAL, ESLOT_NONE,   1,1, 1,USE_HEAL,40, 0,0,0,
       0,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0, 0, 0, 0, 0, 0, -1, 0),
    IT("Mana Potion",       TIER_NORMAL, ESLOT_NONE,   1,1, 1,USE_MANA,50, 0,0,0,
       0,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0, 0, 0, 0, 0, 0, -1, 0),
    IT("Rancid Gas Potion", TIER_NORMAL, ESLOT_NONE,   1,1, 1,USE_BLAST,50, 0,0,0,
       0,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0, 0, 0, 0, 0, 0, -1, 0),
    IT("Leather Boots",     TIER_NORMAL, ESLOT_BOOTS,  2,2, 0,0,0,   0, 0, 120,
       0,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0, 0, 0, 0, 0, 0, -1, 0),
    IT("Leather Gloves",    TIER_NORMAL, ESLOT_GLOVES, 2,2, 0,0,0,   0, 0, 80,
       0,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0, 0, 0, 0, 0, 0, -1, 0),

    // Plain base items for the remaining Doom weapons (appended at
    // the end: existing indices are persisted in saves and must not
    // shift). Vanilla weapon pickups grant these (see P_GiveWeapon).
    // Damage ranges match the weapons' non-damage-affix magic bases.
    IT("Super Shotgun",     TIER_NORMAL, ESLOT_WEAPON, 3,1, 0,0,0,   9, 18, 0,
       0,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0, 0, 0, 0, 0, 0, wp_supershotgun, 0),
    IT("Chaingun",          TIER_NORMAL, ESLOT_WEAPON, 3,2, 0,0,0,   4, 10, 0,
       0,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0, 0, 0, 0, 0, 0, wp_chaingun, 0),
    IT("Rocket Launcher",   TIER_NORMAL, ESLOT_WEAPON, 3,2, 0,0,0,   8, 16, 0,
       0,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0, 0, 0, 0, 0, 0, wp_missile, 0),
    IT("Plasma Rifle",      TIER_NORMAL, ESLOT_WEAPON, 3,2, 0,0,0,   7, 15, 0,
       0,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0, 0, 0, 0, 0, 0, wp_plasma, 0),
    IT("BFG9000",           TIER_NORMAL, ESLOT_WEAPON, 3,2, 0,0,0,   12, 24, 0,
       0,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0, 0, 0, 0, 0, 0, wp_bfg, 0),
    IT("Chainsaw",          TIER_NORMAL, ESLOT_WEAPON, 3,1, 0,0,0,   5, 12, 0,
       0,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0, 0, 0, 0, 0, 0, wp_chainsaw, 0),
};

static const diablo_itemdef_t diablo_magic[] =
{
    IT("Cruel Chaingun",    TIER_MAGIC, ESLOT_WEAPON, 3,2, 0,0,0,  6,14, 0,
       3,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,25, 0, 0, 0, 0, 0, wp_chaingun, 0),
    IT("King's Shotgun",    TIER_MAGIC, ESLOT_WEAPON, 3,1, 0,0,0,  5,10, 0,
       2,0,2,0,   0,0,0,0,   0,0,0,0,0,0,8,0, 0, 0, 0, 0, 0, wp_shotgun, 0),
    IT("Vampiric Rocket Launcher", TIER_MAGIC, ESLOT_WEAPON, 3,2, 0,0,0,  8,16, 0,
       0,0,0,0,   0,0,0,0,   5,0,0,0,0,0,0,0, 0, 0, 0, 0, 0, wp_missile, 0),
    IT("Vampiric Chainsaw", TIER_MAGIC, ESLOT_WEAPON, 3,1, 0,0,0,  5,12, 0,
       0,0,0,0,   0,0,0,0,  10,0,0,0,0,0,0,0, 0, 0, 0, 0, 0, wp_chainsaw, 0),
    // Vampiric prefix on all guns: leech can spawn anywhere.
    IT("Vampiric Pistol", TIER_MAGIC, ESLOT_WEAPON, 2,1, 0,0,0,  2, 6, 0,
       0,0,0,0,   0,0,0,0,   5,0,0,0,0,0,0,0, 0, 0, 0, 0, 0, wp_pistol, 0),
    IT("Vampiric Shotgun", TIER_MAGIC, ESLOT_WEAPON, 3,1, 0,0,0,  3, 8, 0,
       0,0,0,0,   0,0,0,0,   5,0,0,0,0,0,0,0, 0, 0, 0, 0, 0, wp_shotgun, 0),
    IT("Vampiric Chaingun", TIER_MAGIC, ESLOT_WEAPON, 3,2, 0,0,0,  4,10, 0,
       0,0,0,0,   0,0,0,0,   5,0,0,0,0,0,0,0, 0, 0, 0, 0, 0, wp_chaingun, 0),
    IT("Vampiric Plasma Rifle", TIER_MAGIC, ESLOT_WEAPON, 3,2, 0,0,0,  7,15, 0,
       0,0,0,0,   0,0,0,0,   5,0,0,0,0,0,0,0, 0, 0, 0, 0, 0, wp_plasma, 0),
    IT("Vampiric Super Shotgun", TIER_MAGIC, ESLOT_WEAPON, 3,1, 0,0,0,  9,18, 0,
       0,0,0,0,   0,0,0,0,   5,0,0,0,0,0,0,0, 0, 0, 0, 0, 0, wp_supershotgun, 0),
    IT("Vampiric BFG9000", TIER_MAGIC, ESLOT_WEAPON, 3,2, 0,0,0, 12,24, 0,
       0,0,0,0,   0,0,0,0,   5,0,0,0,0,0,0,0, 0, 0, 0, 0, 0, wp_bfg, 0),
    IT("Soldier's Plasma Rifle", TIER_MAGIC, ESLOT_WEAPON, 3,2, 0,0,0,  7,15, 0,
       3,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0, 0, 0, 0, 0, 0, wp_plasma, 0),
    IT("Swift Chainsaw",    TIER_MAGIC, ESLOT_WEAPON, 3,1, 0,0,0,  5,12, 0,
       0,3,0,0,   0,0,0,0,   0,0,0,0,0,20,0,0, 0, 0, 0, 0, 0, wp_chainsaw, 0),
    IT("Heavy Super Shotgun", TIER_MAGIC, ESLOT_WEAPON, 3,1, 0,0,0,  9,18, 0,
       0,0,0,0,   0,0,0,0,   0,0,0,0,0,0,10,0, 0, 0, 0, 0, 0, wp_supershotgun, 0),


    IT("Vampiric Bone Shield", TIER_MAGIC, ESLOT_SHIELD, 2,2, 0,0,0,  0, 0, 8,
       0,0,0,0,   0,0,0,0,   5,0,0,0,0,0,0,0, 0, 0, 0, 0, 0, -1, 0),

    // Suffix-style magic weapons: AD/AP/haste/heat-capacity boosts.
    IT("Pistol of Slaying", TIER_MAGIC, ESLOT_WEAPON, 2,1, 0,0,0,  2, 6, 0,
       0,0,0,0,   0,0,0,0,   0,0,0,15,0,0,0,0, 0, 0, 0, 0, 0, wp_pistol, 0),
    IT("Shotgun of Slaying", TIER_MAGIC, ESLOT_WEAPON, 3,1, 0,0,0,  3, 8, 0,
       0,0,0,0,   0,0,0,0,   0,0,0,15,0,0,0,0, 0, 0, 0, 0, 0, wp_shotgun, 0),
    IT("Chaingun of Slaying", TIER_MAGIC, ESLOT_WEAPON, 3,2, 0,0,0,  4,10, 0,
       0,0,0,0,   0,0,0,0,   0,0,0,15,0,0,0,0, 0, 0, 0, 0, 0, wp_chaingun, 0),
    IT("Chainsaw of Slaying", TIER_MAGIC, ESLOT_WEAPON, 3,1, 0,0,0,  4,10, 0,
       0,0,0,0,   0,0,0,0,   0,0,0,15,0,0,0,0, 0, 0, 0, 0, 0, wp_chainsaw, 0),
    IT("Plasma Rifle of the Magus", TIER_MAGIC, ESLOT_WEAPON, 3,2, 0,0,0,  7,15, 0,
       0,0,0,0,   0,0,0,0,   0,0,0,0,15,0,0,0, 0, 0, 0, 0, 0, wp_plasma, 0),
    IT("Rocket Launcher of the Magus", TIER_MAGIC, ESLOT_WEAPON, 3,2, 0,0,0,  8,16, 0,
       0,0,0,0,   0,0,0,0,   0,0,0,0,15,0,0,0, 0, 0, 0, 0, 0, wp_missile, 0),
    IT("Super Shotgun of the Magus", TIER_MAGIC, ESLOT_WEAPON, 3,1, 0,0,0,  9,18, 0,
       0,0,0,0,   0,0,0,0,   0,0,0,0,15,0,0,0, 0, 0, 0, 0, 0, wp_supershotgun, 0),
    IT("BFG9000 of the Magus", TIER_MAGIC, ESLOT_WEAPON, 3,2, 0,0,0, 12,24, 0,
       0,0,0,0,   0,0,0,0,   0,0,0,0,15,0,0,0, 0, 0, 0, 0, 0, wp_bfg, 0),
    IT("Super Shotgun of Haste", TIER_MAGIC, ESLOT_WEAPON, 3,1, 0,0,0,  9,18, 0,
       0,0,0,0,   0,0,0,0,   0,0,0,0,0,20,0,0, 0, 0, 0, 0, 0, wp_supershotgun, 0),
    IT("BFG9000 of Haste", TIER_MAGIC, ESLOT_WEAPON, 3,2, 0,0,0, 12,24, 0,
       0,0,0,0,   0,0,0,0,   0,0,0,0,0,20,0,0, 0, 0, 0, 0, 0, wp_bfg, 0),
    IT("Plasma Rifle of the Furnace", TIER_MAGIC, ESLOT_WEAPON, 3,2, 0,0,0,  7,15, 0,
       0,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0, 30, 0, 0, 0, 0, wp_plasma, 0),

    // Prefix-style magic weapons: flat (constant) boosts.
    IT("Soldier's Pistol", TIER_MAGIC, ESLOT_WEAPON, 2,1, 0,0,0,  2, 6, 0,
       0,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0, 0, 4, 0, 0, 0, wp_pistol, 0),
    IT("Soldier's Shotgun", TIER_MAGIC, ESLOT_WEAPON, 3,1, 0,0,0,  3, 8, 0,
       0,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0, 0, 4, 0, 0, 0, wp_shotgun, 0),
    IT("Soldier's Chaingun", TIER_MAGIC, ESLOT_WEAPON, 3,2, 0,0,0,  4,10, 0,
       0,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0, 0, 5, 0, 0, 0, wp_chaingun, 0),
    IT("Soldier's Chainsaw", TIER_MAGIC, ESLOT_WEAPON, 3,1, 0,0,0,  4,10, 0,
       0,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0, 0, 5, 0, 0, 0, wp_chainsaw, 0),
    IT("Wizard's Plasma Rifle", TIER_MAGIC, ESLOT_WEAPON, 3,2, 0,0,0,  7,15, 0,
       0,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0, 0, 0, 8, 0, 0, wp_plasma, 0),
    IT("Wizard's Rocket Launcher", TIER_MAGIC, ESLOT_WEAPON, 3,2, 0,0,0,  8,16, 0,
       0,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0, 0, 0, 8, 0, 0, wp_missile, 0),
    IT("Wizard's Super Shotgun", TIER_MAGIC, ESLOT_WEAPON, 3,1, 0,0,0,  9,18, 0,
       0,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0, 0, 0, 8, 0, 0, wp_supershotgun, 0),
    IT("Wizard's BFG9000", TIER_MAGIC, ESLOT_WEAPON, 3,2, 0,0,0, 12,24, 0,
       0,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0, 0, 0,12, 0, 0, wp_bfg, 0),
    IT("Quick Super Shotgun", TIER_MAGIC, ESLOT_WEAPON, 3,1, 0,0,0,  9,18, 0,
       0,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0, 0, 0, 0, 1, 0, wp_supershotgun, 0),
    IT("Quick BFG9000", TIER_MAGIC, ESLOT_WEAPON, 3,2, 0,0,0, 12,24, 0,
       0,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0, 0, 0, 0, 1, 0, wp_bfg, 0),

    // Nightmare-tier affixes (diff_tier=1): stronger versions for
    // Nightmare difficulty and NG+ loop 1.
    IT("Pistol of Slaughter", TIER_MAGIC, ESLOT_WEAPON, 2,1, 0,0,0,  2, 6, 0,
       0,0,0,0,   0,0,0,0,   0,0,0,25,0,0,0,0, 0, 0, 0, 0, 1, wp_pistol, 0),
    IT("Shotgun of Slaughter", TIER_MAGIC, ESLOT_WEAPON, 3,1, 0,0,0,  3, 8, 0,
       0,0,0,0,   0,0,0,0,   0,0,0,25,0,0,0,0, 0, 0, 0, 0, 1, wp_shotgun, 0),
    IT("Plasma Rifle of the Archmagus", TIER_MAGIC, ESLOT_WEAPON, 3,2, 0,0,0,  7,15, 0,
       0,0,0,0,   0,0,0,0,   0,0,0,0,25,0,0,0, 0, 0, 0, 0, 1, wp_plasma, 0),
    IT("BFG9000 of the Archmagus", TIER_MAGIC, ESLOT_WEAPON, 3,2, 0,0,0, 12,24, 0,
       0,0,0,0,   0,0,0,0,   0,0,0,0,25,0,0,0, 0, 0, 0, 0, 1, wp_bfg, 0),
    IT("Super Shotgun of Speed", TIER_MAGIC, ESLOT_WEAPON, 3,1, 0,0,0,  9,18, 0,
       0,0,0,0,   0,0,0,0,   0,0,0,0,0,30,0,0, 0, 0, 0, 0, 1, wp_supershotgun, 0),
    IT("Veteran's Chaingun", TIER_MAGIC, ESLOT_WEAPON, 3,2, 0,0,0,  4,10, 0,
       0,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0, 0, 8, 0, 0, 1, wp_chaingun, 0),
    IT("Sorcerer's Rocket Launcher", TIER_MAGIC, ESLOT_WEAPON, 3,2, 0,0,0,  8,16, 0,
       0,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0, 0, 0,14, 0, 1, wp_missile, 0),
    IT("Bloodthirsty Chainsaw", TIER_MAGIC, ESLOT_WEAPON, 3,1, 0,0,0,  5,12, 0,
       0,0,0,0,   0,0,0,0,   8,0,0,0,0,0,0,0, 0, 0, 0, 0, 1, wp_chainsaw, 0),
    IT("Plasma Rifle of the Inferno", TIER_MAGIC, ESLOT_WEAPON, 3,2, 0,0,0,  7,15, 0,
       0,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0, 50, 0, 0, 0, 1, wp_plasma, 0),

    // Hell-tier affixes (diff_tier=2): the strongest versions, only on
    // Hell difficulty and NG+ loop 2+.
    IT("Pistol of Carnage", TIER_MAGIC, ESLOT_WEAPON, 2,1, 0,0,0,  2, 6, 0,
       0,0,0,0,   0,0,0,0,   0,0,0,40,0,0,0,0, 0, 0, 0, 0, 2, wp_pistol, 0),
    IT("BFG9000 of Carnage", TIER_MAGIC, ESLOT_WEAPON, 3,2, 0,0,0, 12,24, 0,
       0,0,0,0,   0,0,0,0,   0,0,0,40,0,0,0,0, 0, 0, 0, 0, 2, wp_bfg, 0),
    IT("Plasma Rifle of the Godslayer", TIER_MAGIC, ESLOT_WEAPON, 3,2, 0,0,0,  7,15, 0,
       0,0,0,0,   0,0,0,0,   0,0,0,0,40,0,0,0, 0, 0, 0, 0, 2, wp_plasma, 0),
    IT("BFG9000 of Lightning", TIER_MAGIC, ESLOT_WEAPON, 3,2, 0,0,0, 12,24, 0,
       0,0,0,0,   0,0,0,0,   0,0,0,0,0,45,0,0, 0, 0, 0, 0, 2, wp_bfg, 0),
    IT("Champion's Chaingun", TIER_MAGIC, ESLOT_WEAPON, 3,2, 0,0,0,  4,10, 0,
       0,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0, 0,12, 0, 0, 2, wp_chaingun, 0),
    IT("Archmage's Rocket Launcher", TIER_MAGIC, ESLOT_WEAPON, 3,2, 0,0,0,  8,16, 0,
       0,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0, 0, 0,20, 0, 2, wp_missile, 0),
    IT("Soulrender Chainsaw", TIER_MAGIC, ESLOT_WEAPON, 3,1, 0,0,0,  5,12, 0,
       0,0,0,0,   0,0,0,0,  12,0,0,0,0,0,0,0, 0, 0, 0, 0, 2, wp_chainsaw, 0),
    IT("Plasma Rifle of the Sun", TIER_MAGIC, ESLOT_WEAPON, 3,2, 0,0,0,  7,15, 0,
       0,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0, 80, 0, 0, 0, 2, wp_plasma, 0),

    IT("Prismatic Amulet",    TIER_MAGIC, ESLOT_AMULET, 1,1, 0,0,0,  0, 0, 0,
       0,0,0,0,  10,10,10,10, 0,0,0,0,0,0,0,0, 0, 0, 0, 0, 0, -1, 0),
    IT("Lizard's Ring",       TIER_MAGIC, ESLOT_RING1,  1,1, 0,0,0,  0, 0, 0,
       0,0,0,8,   0,0,0,0,   0,5,0,0,0,0,0,0, 0, 0, 0, 0, 0, -1, 0),
    IT("Soldier's Chain Mail",TIER_MAGIC, ESLOT_ARMOR,  2,3, 0,0,0,  0, 0,480,
       3,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0, 0, 0, 0, 0, 0, -1, 0),
    IT("Traveler's Treads", TIER_MAGIC, ESLOT_BOOTS,  2,2, 0,0,0,  0, 0, 320,
       0,2,0,0,   0,0,0,0,   0,0,10,0,0,0,0,0, 0, 0, 0, 0, 0, -1, 0),
    IT("Assault Gloves",    TIER_MAGIC, ESLOT_GLOVES, 2,2, 0,0,0,  0, 0, 240,
       3,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0, 0, 0, 0, 0, 0, -1, 0),

    // Combat affixes on non-weapons: rings, amulets, gloves, focus.
    IT("Ring of Slaying", TIER_MAGIC, ESLOT_RING1, 1,1, 0,0,0,  0, 0, 0,
       0,0,0,0,   0,0,0,0,   0,0,0,10,0,0,0,0, 0, 0, 0, 0, 0, -1, 0),
    IT("Ring of the Magus", TIER_MAGIC, ESLOT_RING1, 1,1, 0,0,0,  0, 0, 0,
       0,0,0,0,   0,0,0,0,   0,0,0,0,10,0,0,0, 0, 0, 0, 0, 0, -1, 0),
    IT("Ring of Haste", TIER_MAGIC, ESLOT_RING1, 1,1, 0,0,0,  0, 0, 0,
       0,0,0,0,   0,0,0,0,   0,0,0,0,0,10,0,0, 0, 0, 0, 0, 0, -1, 0),
    IT("Vampiric Ring", TIER_MAGIC, ESLOT_RING1, 1,1, 0,0,0,  0, 0, 0,
       0,0,0,0,   0,0,0,0,   5,0,0,0,0,0,0,0, 0, 0, 0, 0, 0, -1, 0),
    IT("Amulet of Slaying", TIER_MAGIC, ESLOT_AMULET, 1,1, 0,0,0,  0, 0, 0,
       0,0,0,0,   0,0,0,0,   0,0,0,10,0,0,0,0, 0, 0, 0, 0, 0, -1, 0),
    IT("Amulet of the Magus", TIER_MAGIC, ESLOT_AMULET, 1,1, 0,0,0,  0, 0, 0,
       0,0,0,0,   0,0,0,0,   0,0,0,0,10,0,0,0, 0, 0, 0, 0, 0, -1, 0),
    IT("Amulet of Haste", TIER_MAGIC, ESLOT_AMULET, 1,1, 0,0,0,  0, 0, 0,
       0,0,0,0,   0,0,0,0,   0,0,0,0,0,10,0,0, 0, 0, 0, 0, 0, -1, 0),
    IT("Gloves of Slaying", TIER_MAGIC, ESLOT_GLOVES, 2,2, 0,0,0,  0, 0, 240,
       0,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0, 0, 3, 0, 0, 0, -1, 0),
    IT("Gloves of Haste", TIER_MAGIC, ESLOT_GLOVES, 2,2, 0,0,0,  0, 0, 240,
       0,0,0,0,   0,0,0,0,   0,0,0,0,0,10,0,0, 0, 0, 0, 0, 0, -1, 0),
    IT("Cooling Rounds", TIER_MAGIC, ESLOT_FOCUS, 1,1, 0,0,0,  0, 0, 0,
       0,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0, 20, 0, 0, 0, 0, -1, 0),
    IT("Piercing Rounds",   TIER_MAGIC, ESLOT_FOCUS,   1,1, 0,0,0,   0, 0, 0,
       0,0,0,0,   0,0,0,0,   0,0,0,  15,0,0,0,0, 0, 0, 0, 0, 0, -1, 0),
    IT("Incendiary Shells", TIER_MAGIC, ESLOT_FOCUS,   1,1, 0,0,0,   0, 0, 0,
       0,0,0,0,   0,0,0,0,   0,0,0,  0,10,0,0,0, 0, 0, 0, 0, 0, -1, 0),
    IT("Swift Cartridges",  TIER_MAGIC, ESLOT_FOCUS,   1,1, 0,0,0,   0, 0, 0,
       0,0,0,0,   0,0,0,0,   0,0,0,  0,0,20,0,0, 0, 0, 0, 0, 0, -1, 0),
    IT("Spotter Rounds",    TIER_MAGIC, ESLOT_FOCUS,   1,1, 0,0,0,   0, 0, 0,
       0,0,0,0,   0,0,0,0,   0,0,0,  0,0,0,10,0, 0, 0, 0, 0, 0, -1, 0),
    // Replenish life (Jimmy, 2026-10-30): rollable replenish items,
    // appended at the end so existing indices never shift.
    { .name = "Ring of Replenishment", .tier = TIER_MAGIC,
      .slot = ESLOT_RING1, .grid_w = 1, .grid_h = 1,
      .doomweapon = -1, .replenish = 1 },
};

static const diablo_itemdef_t diablo_rare[] =
{
    IT("Doombringer BFG",       TIER_RARE, ESLOT_WEAPON, 3,2, 0,0,0,  12,24, 0,
       5,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,40, 0, 0, 0, 0, 0, wp_bfg, 0),

    IT("Stormlash Plasma Rifle",         TIER_RARE, ESLOT_WEAPON, 3,2, 0,0,0,  10,20, 0,
       0,0,0,0,   0,0,15,0,  0,0,0,0,0,0,10,0, 0, 0, 0, 0, 0, wp_plasma, 0),

    IT("Soulrender Rocket Launcher",        TIER_RARE, ESLOT_WEAPON, 3,2, 0,0,0,  11,22, 0,
       0,0,0,0,   0,0,0,0,   3,0,0,0,0,0,0,0, 0, 0, 0, 0, 0, wp_missile, 0),


    IT("Nightmare Coil",    TIER_RARE, ESLOT_RING1,  1,1, 0,0,0,   0, 0, 0,
       5,0,0,0,  15,0,0,0,   0,0,0,0,0,0,5,0, 0, 0, 0, 0, 0, -1, 0),
    IT("Grimward",          TIER_RARE, ESLOT_SHIELD, 2,2, 0,0,0,   0, 0,15,
       0,0,0,0,   0,0,0,15,  0,0,0,0,0,0,0,0, 0, 0, 0, 0, 0, -1, 0),
    IT("Bloodletter Chaingun",       TIER_RARE, ESLOT_WEAPON, 3,2, 0,0,0,  10,19, 0,
       0,0,0,0,   0,0,0,0,   4,0,0,0,0,0,12,0, 0, 0, 0, 0, 0, wp_chaingun, 0),


    IT("Stormwalkers",      TIER_RARE, ESLOT_BOOTS,  2,2, 0,0,0,   0, 0,560,
       0,4,0,0,   0,0,15,0,  0,0,15,0,0,0,0,0, 0, 0, 0, 0, 0, -1, 0),
    IT("Doom Grasp",        TIER_RARE, ESLOT_GLOVES, 2,2, 0,0,0,   0, 0,480,
       5,0,0,0,   0,0,0,0,   3,0,0,0,0,0,0,0, 0, 0, 0, 0, 0, -1, 0),

    // Mechanic-affix guns (AD/AP rework): rares grant mechanics.
    IT("Phoenix Plasma Rifle", TIER_RARE, ESLOT_WEAPON, 3,2, 0,0,0, 11,21, 0,
       0,0,0,5,   0,0,0,0,   0,0,0,0,0,0,10,0, 0, 0, 0, 0, 0, wp_plasma, MECH_PHOENIX),
    IT("Overclocked Plasma Rifle", TIER_RARE, ESLOT_WEAPON, 3,2, 0,0,0, 10,20, 0,
       0,0,0,5,   0,0,0,0,   0,0,0,0,0,0,10,0, 0, 0, 0, 0, 0, wp_plasma, MECH_OVERCLOCK),
    IT("Caldera Plasma Rifle", TIER_RARE, ESLOT_WEAPON, 3,2, 0,0,0, 12,22, 0,
       0,0,0,5,   0,0,0,0,   0,0,0,0,0,0,10,0, 0, 0, 0, 0, 0, wp_plasma, MECH_CALDERA),
    IT("Voltaic Rocket Launcher", TIER_RARE, ESLOT_WEAPON, 3,2, 0,0,0, 12,22, 0,
       3,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0, 0, 0, 0, 0, 0, wp_missile, MECH_VOLTAIC),
    IT("Bandolier Rocket Launcher", TIER_RARE, ESLOT_WEAPON, 3,2, 0,0,0, 11,21, 0,
       3,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0, 0, 0, 0, 0, 0, wp_missile, MECH_BANDOLIER),
    IT("Siege Rocket Launcher", TIER_RARE, ESLOT_WEAPON, 3,2, 0,0,0, 13,24, 0,
       4,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0, 0, 0, 0, 0, 0, wp_missile, MECH_SIEGE),
    IT("Hungry BFG9000",    TIER_RARE, ESLOT_WEAPON, 3,2, 0,0,0, 26,48, 0,
       8,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0, 0, 0, 0, 0, 0, wp_bfg, MECH_HUNGRY),
    IT("Event Horizon BFG9000", TIER_RARE, ESLOT_WEAPON, 3,2, 0,0,0, 26,48, 0,
       8,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0, 0, 0, 0, 0, 0, wp_bfg, MECH_HORIZON),
    IT("Breacher Super Shotgun", TIER_RARE, ESLOT_WEAPON, 3,1, 0,0,0, 12,22, 0,
       4,0,0,0,   0,0,0,0,   0,0,0,0,0,0,10,0, 0, 0, 0, 0, 0, wp_supershotgun, MECH_BREACHER),
    IT("Super Shotgun of the Bull", TIER_RARE, ESLOT_WEAPON, 3,1, 0,0,0, 12,22, 0,
       4,0,0,0,   0,0,0,0,   0,0,0,0,0,0,10,0, 0, 0, 0, 0, 0, wp_supershotgun, MECH_BULL),
    IT("Splitting Shotgun", TIER_RARE, ESLOT_WEAPON, 3,1, 0,0,0,  6,12, 0,
       3,0,0,0,   0,0,0,0,   0,0,0,0,0,0,10,0, 0, 0, 0, 0, 0, wp_shotgun, MECH_SPLITTING),
    IT("Reaper Chainsaw",   TIER_RARE, ESLOT_WEAPON, 3,1, 0,0,0,  8,16, 0,
       4,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0, 0, 0, 0, 0, 0, wp_chainsaw, MECH_REAPER),
    IT("Lucky Pistol",      TIER_RARE, ESLOT_WEAPON, 2,1, 0,0,0,  4, 9, 0,
       0,3,0,0,   0,0,0,0,   0,0,0,0,0,0,15,0, 0, 0, 0, 0, 0, wp_pistol, MECH_LUCKY),
    IT("Splitting Chaingun", TIER_RARE, ESLOT_WEAPON, 3,2, 0,0,0,  5,10, 0,
       2,4,0,0,   0,0,0,0,   0,0,0,0,0,0,10,0, 0, 0, 0, 0, 0, wp_chaingun, MECH_SPLITTING),
    { .name = "Amulet of Replenishment", .tier = TIER_RARE,
      .slot = ESLOT_AMULET, .grid_w = 1, .grid_h = 1,
      .doomweapon = -1, .replenish = 2 },
};

static const diablo_itemdef_t diablo_set[] =
{
    IT("Tal Rasha's Horadric Crest", TIER_SET, ESLOT_HELM, 2,2, 0,0,0, 0,0,600,
       0,0,0,8,   0,0,0,0,   0,10,0,0,0,0,0,0, 0, 0, 0, 0, 0, -1, 0),
    IT("Immortal King's Soul Cage", TIER_SET, ESLOT_ARMOR, 2,3, 0,0,0, 0,0,1000,
       8,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0, 0, 0, 0, 0, 0, -1, 0),
    IT("Trang-Oul's Guise",          TIER_SET, ESLOT_HELM, 2,2, 0,0,0, 0,0,560,
       0,0,0,0,   0,0,0,20,  0,0,0,0,0,0,0,0, 0, 0, 0, 0, 0, -1, 0),
    IT("M'avina's True Sight",       TIER_SET, ESLOT_HELM, 2,2, 0,0,0, 0,0,560,
       0,8,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0, 0, 0, 0, 0, 0, -1, 0),
    IT("Natalya's Shadow",           TIER_SET, ESLOT_ARMOR,2,3, 0,0,0, 0,0,880,
       0,6,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0, 0, 0, 0, 0, 0, -1, 0),
    IT("Griswold's Valor",           TIER_SET, ESLOT_HELM, 2,2, 0,0,0, 0,0,640,
       6,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0, 0, 0, 0, 0, 0, -1, 0),
    IT("Berserker's Boomstick",      TIER_SET, ESLOT_WEAPON,3,1,0,0,0,15,28,0,
       6,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,35, 0, 0, 0, 0, 0, wp_shotgun, 0),

    IT("Sazabi's Cobalt Repeater",   TIER_SET, ESLOT_WEAPON,3,2,0,0,0,14,26,0,
       0,0,0,0,   0,20,0,0,  0,0,0,0,0,0,0,0, 0, 0, 0, 0, 0, wp_chaingun, 0),

    IT("Immortal King's Pillar",    TIER_SET, ESLOT_BOOTS, 2,2,0,0,0, 0,0,720,
       5,0,0,0,   20,0,0,0,  0,0,20,0,0,0,0,0, 0, 0, 0, 0, 0, -1, 0),
    IT("M'avina's Icy Clutch",      TIER_SET, ESLOT_GLOVES,2,2,0,0,0, 0,0,560,
       0,6,0,0,   0,20,0,0,  0,0,0,0,0,0,0,0, 0, 0, 0, 0, 0, -1, 0),
};

static const diablo_itemdef_t diablo_unique[] =
{
    IT("Stone of Jordan",       TIER_UNIQUE, ESLOT_RING1, 1,1, 0,0,0,  0, 0, 0,
       0,0,0,15,   0,0,0,0,   5,15,0,0,0,0,0,0, 0, 0, 0, 0, 0, -1, 0),
    IT("Harlequin Crest",       TIER_UNIQUE, ESLOT_HELM,  2,2, 0,0,0,  0, 0,720,
       0,0,10,0,   0,0,0,0,   0,25,0,0,0,0,0,0, 0, 0, 0, 0, 0, -1, 0),
    IT("BFG10K",              TIER_UNIQUE, ESLOT_WEAPON,3,2, 0,0,0, 25,50, 0,
       10,0,0,0,   0,0,0,0,   0,0,0,0,0,0,15,50, 0, 0, 0, 0, 0, wp_bfg, MECH_FRUSTUM),

    IT("Unmaker",             TIER_UNIQUE, ESLOT_WEAPON,3,2, 0,0,0, 22,45, 0,
       0,10,0,0,   0,0,0,0,   0,0,0,0,0,0,20,0, 0, 0, 0, 0, 0, wp_plasma, 0),

    IT("Arkaine's Valor",       TIER_UNIQUE, ESLOT_ARMOR, 2,3, 0,0,0,  0, 0,1200,
       0,0,12,0,   0,0,0,0,   0,0,0,0,0,0,0,0, 0, 0, 0, 0, 0, -1, 0),
    IT("Mara's Kaleidoscope",   TIER_UNIQUE, ESLOT_AMULET,1,1, 0,0,0,  0, 0, 0,
       8,0,0,0,  20,20,20,20, 0,0,0,0,0,0,0,0, 0, 0, 0, 0, 0, -1, 0),
    IT("Bul-Kathos' Wedding Band",TIER_UNIQUE,ESLOT_RING1,1,1, 0,0,0,  0, 0, 0,
       0,0,8,0,    0,0,0,0,   8,0,0,0,0,0,10,25, 0, 0, 0, 0, 0, -1, 0),
    IT("Old Painless",        TIER_UNIQUE, ESLOT_WEAPON,3,2, 0,0,0, 20,40, 0,
       8,5,0,0,    0,0,0,0,   0,0,0,0,0,0,15,0, 0, 0, 0, 0, 0, wp_chaingun, 0),

    IT("Lidless Wall",          TIER_UNIQUE, ESLOT_SHIELD,2,2, 0,0,0,  0, 0,20,
       0,0,0,10,   0,0,0,0,   0,0,0,0,0,0,0,0, 0, 0, 0, 0, 0, -1, 0),
    IT("Skin of the Vipermagi", TIER_UNIQUE, ESLOT_ARMOR, 2,3, 0,0,0,  0, 0,960,
       0,0,0,0,  25,0,25,0,   0,0,0,0,0,0,0,0, 0, 0, 0, 0, 0, -1, 0),
    IT("Thundergod's Vigor",    TIER_UNIQUE, ESLOT_BELT,  2,1, 0,0,0,  0, 0,400,
       5,0,8,0,    0,0,25,0,  0,0,0,0,0,0,0,0, 0, 0, 0, 0, 0, -1, 0),
    IT("Raven Frost",           TIER_UNIQUE, ESLOT_RING1, 1,1, 0,0,0,  0, 0, 0,
       0,8,0,0,    0,25,0,0,  0,0,0,0,0,0,0,0, 0, 0, 0, 0, 0, -1, 0),
    IT("War Traveler",          TIER_UNIQUE, ESLOT_BOOTS, 2,2, 0,0,0,  0, 0,880,
       5,0,8,0,    0,0,0,0,   0,15,25,0,0,0,0,0, 0, 0, 0, 0, 0, -1, 0),
    IT("Frostburn",             TIER_UNIQUE, ESLOT_GLOVES,2,2, 0,0,0,  0, 0,640,
       5,5,0,0,    0,25,0,0,  4,0,0,0,0,0,0,0, 0, 0, 0, 0, 0, -1, 0),
};

#undef IT

static const diablo_itemdef_t *diablo_tiers[NUM_TIERS] =
{
    diablo_normal,
    diablo_magic,
    diablo_rare,
    diablo_set,
    diablo_unique
};

static const int diablo_tiercounts[NUM_TIERS] =
{
    sizeof(diablo_normal) / sizeof(diablo_normal[0]),
    sizeof(diablo_magic) / sizeof(diablo_magic[0]),
    sizeof(diablo_rare) / sizeof(diablo_rare[0]),
    sizeof(diablo_set) / sizeof(diablo_set[0]),
    sizeof(diablo_unique) / sizeof(diablo_unique[0])
};

// Message buffer for item/equipment announcements (cf. lootmsg).
static char diablo_msg[192];

static void D_Msg(player_t *player, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(diablo_msg, sizeof(diablo_msg), fmt, args);
    va_end(args);
    player->message = diablo_msg;
}

const diablo_itemdef_t *D_GetItemDef(int tier, int idx)
{
    if (!D_ValidItem(tier, idx))
        return NULL;
    return &diablo_tiers[tier][idx];
}

int D_TierCount(int tier)
{
    if (tier < 0 || tier >= NUM_TIERS)
        return 0;
    return diablo_tiercounts[tier];
}

boolean D_ValidItem(int tier, int idx)
{
    return tier >= 0 && tier < NUM_TIERS
        && idx >= 0 && idx < diablo_tiercounts[tier];
}

// Map (tier, idx) to a d_diablo_icons.h icon index.
// Guns share their base-gun icon (all variants of a base); the 4 FOCUS
// items share the ammo-box icon; everything else has a unique icon.
int D_GetItemIconIdx(int tier, int idx)
{
    const diablo_itemdef_t *def;
    // Non-gun icons by table position (guns/FOCUS return before these).
    // Normal (11): 0 Pistol, 1 Shotgun are guns.
    static const int normal_icons[] = {
        -1, -1,
        D_ICON_ARMOR_LEATHER, D_ICON_SHIELD_BUCKLER, D_ICON_HELM_CAP,
        D_ICON_BELT, D_ICON_POTION_RED, D_ICON_POTION_BLUE,
        D_ICON_POTION_GREEN, D_ICON_BOOT_LEATHER, D_ICON_GLOVE_LEATHER,
    };
    // Magic (16): 0-5 are guns, 12-15 are FOCUS.
    static const int magic_icons[]  = {
        -1, -1, -1, -1, -1, -1,
        D_ICON_M_VAMPIRIC_BONE_SHIELD, D_ICON_M_PRISMATIC_AMULET,
        D_ICON_M_LIZARDS_RING, D_ICON_M_SOLDIERS_CHAIN_MAIL,
        D_ICON_BOOT_TRAVELER, D_ICON_GLOVE_ASSAULT,
        -1, -1, -1, -1,
    };
    // Rare (22): guns at 0, 1, 2, 5, 8-21.
    static const int rare_icons[]   = {
        -1, -1, -1,
        D_ICON_R_NIGHTMARE_COIL, D_ICON_R_GRIMWARD, -1,
        D_ICON_BOOT_STORMWALKER, D_ICON_GLOVE_DOOM_GRASP,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    };
    // Set (10): 6, 7 are guns (Boomstick, Cobalt Repeater).
    static const int set_icons[]    = {
        D_ICON_S_TAL_RASHA_CREST, D_ICON_S_IK_SOUL_CAGE,
        D_ICON_S_TRANG_OUL_GUISE, D_ICON_S_MAVINA_SIGHT,
        D_ICON_S_NATALYA_SHADOW, D_ICON_S_GRISWOLD_VALOR,
        -1, -1,
        D_ICON_BOOT_IK_PILLAR, D_ICON_GLOVE_MAVINA_CLUTCH,
    };
    // Unique (14): 2, 3, 7 are guns (BFG10K, Unmaker, Old Painless).
    static const int unique_icons[] = {
        D_ICON_U_STONE_OF_JORDAN, D_ICON_U_HARLEQUIN_CREST,
        -1, -1,
        D_ICON_U_ARKAINE_VALOR, D_ICON_U_MARAS_KALEIDOSCOPE,
        D_ICON_U_BULKATHOS_BAND, -1,
        D_ICON_U_LIDLESS_WALL, D_ICON_U_VIPERMAGI,
        D_ICON_U_THUNDERGOD_VIGOR, D_ICON_U_RAVEN_FROST,
        D_ICON_BOOT_WAR_TRAVELER, D_ICON_GLOVE_FROSTBURN,
    };

    if (!D_ValidItem(tier, idx))
        return 0;

    def = D_GetItemDef(tier, idx);
    if (def->slot == ESLOT_WEAPON && def->doomweapon >= 0)
    {
        switch (def->doomweapon)
        {
            case wp_pistol:       return D_ICON_GUN_PISTOL;
            case wp_shotgun:      return D_ICON_GUN_SHOTGUN;
            case wp_chaingun:     return D_ICON_GUN_CHAINGUN;
            case wp_chainsaw:     return D_ICON_GUN_CHAINSAW;
            case wp_missile:      return D_ICON_GUN_ROCKET;
            case wp_plasma:       return D_ICON_GUN_PLASMA;
            case wp_bfg:          return D_ICON_GUN_BFG;
            case wp_supershotgun: return D_ICON_GUN_SSG;
            default: break;
        }
    }
    if (def->slot == ESLOT_FOCUS)
        return D_ICON_AMMO_BOX;

    // Bounds-checked: the arrays below must match their item tables;
    // a mismatch returns icon 0 instead of reading out of bounds.
#define ICON_AT(arr, i) \
    ((i) >= 0 && (i) < (int)(sizeof(arr) / sizeof((arr)[0])) ? (arr)[i] : 0)
    switch (tier)
    {
        case TIER_NORMAL: return ICON_AT(normal_icons, idx);
        case TIER_MAGIC:  return ICON_AT(magic_icons, idx);
        case TIER_RARE:   return ICON_AT(rare_icons, idx);
        case TIER_SET:    return ICON_AT(set_icons, idx);
        case TIER_UNIQUE: return ICON_AT(unique_icons, idx);
        default: return 0;
    }
#undef ICON_AT
}

// WAD pickup sprite (the art lying in the world) for a gun item, by
// base weapon.  The pistol and fists have no pickup sprite in any
// Doom WAD (the player starts with them), and the super shotgun's
// SGN2A0 exists only in Doom II WADs -- callers fall back to the
// built-in icon when the lump is absent.
const char *D_ItemWadSpriteName(const diablo_itemdef_t *def)
{
    if (!def || def->slot != ESLOT_WEAPON)
        return NULL;
    switch (def->doomweapon)
    {
        case wp_shotgun:      return "SHOTA0";
        case wp_supershotgun: return "SGN2A0";
        case wp_chaingun:     return "MGUNA0";
        case wp_missile:      return "LAUNA0";
        case wp_plasma:       return "PLASA0";
        case wp_bfg:          return "BFUGA0";
        case wp_chainsaw:     return "CSAWA0";
        default:              return NULL;
    }
}

// Built-in patch standing in for a WAD pickup sprite, for guns the
// IWAD cannot supply: the pistol has no pickup sprite in any WAD
// (the player starts holding it), and the super shotgun's SGN2A0
// exists only in Doom II WADs, so both use embedded patches
// (d_pistol_sprite.c, d_ssg_sprite.c) and draw through the same
// patch path as the WAD sprites, transparency included.
// NULL when the gun has none.
patch_t *D_ItemBuiltinPatch(const diablo_itemdef_t *def)
{
    if (def && def->slot == ESLOT_WEAPON && def->doomweapon == wp_pistol)
        return D_PistolSprite();
    if (def && def->slot == ESLOT_WEAPON && def->doomweapon == wp_supershotgun)
        return D_SsgSprite();
    return NULL;
}

void D_ResetPlayer(struct player_s *pl)
{
    player_t *player = (player_t *)pl;
    int i;

    for (i = 0; i < NUM_ESLOTS; i++)
    {
        player->diablo_equipped[i] = D_NOITEM;
        player->diablo_equipped_dur[i] = 0;
    }
    for (i = 0; i < D_BACKPACK_MAX; i++)
    {
        player->diablo_backpack[i] = D_NOITEM;
        player->diablo_bp_gx[i] = -1;
        player->diablo_bp_gy[i] = -1;
        player->diablo_backpack_dur[i] = 0;
        player->diablo_bp_page[i] = 0;
    }
    player->diablo_bp_count = 0;
    player->diablo_bp_pages = 1;
    player->diablo_bp_cur = 0;
    player->diablo_recent = -1;
    for (i = 0; i < NUM_DSTATS; i++)
        player->diablo_stats[i] = 0;
}

// Sync the Doom readyweapon to the equipped Diablo gun, so the
// first-person sprite and the turn-mode kit follow the gun.
// No weapon equipped -> pistol (guns only; no fist fallback).
void D_SyncDoomWeapon(struct player_s *pl)
{
    player_t *player = (player_t *)pl;
    int id, w;

    if (!player || !player->mo)
        return;

    w = wp_pistol;
    id = player->diablo_equipped[ESLOT_WEAPON];
    if (id != D_NOITEM)
    {
        const diablo_itemdef_t *def =
            D_GetItemDef(D_ITEMTIER(id), D_ITEMIDX(id));
        if (def && def->doomweapon >= 0 && def->doomweapon < NUMWEAPONS)
            w = def->doomweapon;
    }

    // Fallback: if the WAD lacks this weapon's sprites (e.g. super shotgun
    // in Doom 1 WADs), fall back to the normal shotgun entirely. The SSG
    // states (raise/lower/readystate/flash) all use SHT2 sprites; partial
    // swaps cause state-machine mismatches and R_ProjectSprite crashes.
    if (w == wp_supershotgun)
    {
        // Check if SHT2A0 exists in the WAD
        if (W_CheckNumForName("SHT2A0") < 0)
            w = wp_shotgun; // Doom 1 WAD: no SSG sprites, use shotgun
    }

    player->weaponowned[w] = true;
    if (player->readyweapon == w)
        return;
    // Defensive: validate weapon index before touching psprites.
    if (w < 0 || w >= NUMWEAPONS)
        return;
    printf("[DIABLO] equip: readyweapon %d -> %d\n", player->readyweapon, w);
    player->readyweapon = w;
    player->pendingweapon = wp_nochange;
    // Snap the first-person sprite immediately. Turn mode is frozen
    // during planning, so no lower/raise animation is needed.
    P_SetPsprite(player, ps_weapon, weaponinfo[w].readystate);
}

// Mechanic flags of the equipped gun (0 when unarmed). Kill/hit hooks
// in the combat code read this instead of caching per-item state.
int D_EquippedWeaponMech(struct player_s *pl)
{
    player_t *player = (player_t *)pl;
    int id;

    if (!player)
        return 0;
    id = player->diablo_equipped[ESLOT_WEAPON];
    if (id == D_NOITEM)
        return 0;
    {
        const diablo_itemdef_t *def =
            D_GetItemDef(D_ITEMTIER(id), D_ITEMIDX(id));
        if (def)
            return def->mech;
    }
    return 0;
}

// OR of MECH_* bits from every equipped item (not just the gun).
// Powers the inventory POWERS panel: special mechanics granted by gear,
// with plain stat boosts filtered out (those live in the STATS panel).
int D_ActiveMechs(struct player_s *pl)
{
    player_t *player = (player_t *)pl;
    int i, mechs = 0;

    if (!player)
        return 0;
    for (i = 0; i < NUM_ESLOTS; i++)
    {
        int id = player->diablo_equipped[i];
        const diablo_itemdef_t *def;
        if (id == D_NOITEM)
            continue;
        def = D_GetItemDef(D_ITEMTIER(id), D_ITEMIDX(id));
        if (def)
            mechs |= def->mech;
    }
    return mechs;
}

// One-line display text for a single MECH_* bit. NULL for unknown bits.
const char *D_MechDesc(int mech_bit)
{
    switch (mech_bit)
    {
        case MECH_PHOENIX:   return "Phoenix: kills vent 50 ammo";
        case MECH_OVERCLOCK: return "Overclock: +25 ammo vent/round";
        case MECH_CALDERA:   return "Caldera: max ammo fires nova";
        case MECH_VOLTAIC:   return "Voltaic: blasts chain 50%";
        case MECH_BANDOLIER: return "Bandolier: +1 max charge";
        case MECH_SIEGE:     return "Siege: hits knock foes back";
        case MECH_HUNGRY:    return "Hungry: kills -1 cooldown";
        case MECH_HORIZON:   return "Event Horizon: blast drags in";
        case MECH_BREACHER:  return "Breacher: kills refund reload";
        case MECH_BULL:      return "Bull: firing shoves you back";
        case MECH_SPLITTING: return "Splitting: +2 pellets";
        case MECH_REAPER:    return "Reaper: kills refund 2 TP";
        case MECH_LUCKY:     return "Lucky: every 3rd shot crits";
        case MECH_GLACIER:   return "Glacier: hits slow foes";
        case MECH_FRUSTUM:   return "Frustum: hits everything in the view cone";
        default:             return NULL;
    }
}

void D_RecalcStats(struct player_s *pl)
{
    player_t *player = (player_t *)pl;
    int s, i;
    const diablo_itemdef_t *def;

    for (i = 0; i < NUM_DSTATS; i++)
        player->diablo_stats[i] = 0;

    for (s = 0; s < NUM_ESLOTS; s++)
    {
        int id = player->diablo_equipped[s];
        if (id == D_NOITEM)
            continue;
        def = D_GetItemDef(D_ITEMTIER(id), D_ITEMIDX(id));
        if (!def || def->consumable)
            continue;
        player->diablo_stats[DSTAT_DMG_MIN] += def->dmg_min;
        player->diablo_stats[DSTAT_DMG_MAX] += def->dmg_max;
        // Defense (DSTAT_ARMOR) comes only from the durability slots
        // (helm/armor/boots/gloves/belt), and a piece worn to 0
        // contributes nothing until repaired (Jimmy, 2026-10-21).
        if (D_IsDurabilityItem(def)
         && player->diablo_equipped_dur[s] > 0)
            player->diablo_stats[DSTAT_ARMOR] += def->armor;
        player->diablo_stats[DSTAT_STR] += def->str;
        player->diablo_stats[DSTAT_DEX] += def->dex;
        player->diablo_stats[DSTAT_VIT] += def->vit;
        player->diablo_stats[DSTAT_ENE] += def->ene;
        player->diablo_stats[DSTAT_FRES] += def->fres;
        player->diablo_stats[DSTAT_CRES] += def->cres;
        player->diablo_stats[DSTAT_LRES] += def->lres;
        player->diablo_stats[DSTAT_PRES] += def->pres;
        player->diablo_stats[DSTAT_LIFESTEAL] += def->lifesteal;
        player->diablo_stats[DSTAT_MAGICFIND] += def->magicfind;
        player->diablo_stats[DSTAT_MOVESPEED] += def->movespeed;
        // Phase 7 tactical affixes.
        player->diablo_stats[DSTAT_AD_PCT] += def->ad_pct;
        player->diablo_stats[DSTAT_AP_PCT] += def->ap_pct;
        player->diablo_stats[DSTAT_HASTE] += def->haste;
        // Crit is gear-inherent (Diablo-style).
        player->diablo_stats[DSTAT_CRIT_CHANCE] += def->crit_chance;
        player->diablo_stats[DSTAT_CRIT_DMG] += def->crit_dmg;
        player->diablo_stats[DSTAT_PLASMA_CAP] += def->plasma_cap;
        player->diablo_stats[DSTAT_AD_FLAT] += def->ad_flat;
        player->diablo_stats[DSTAT_AP_FLAT] += def->ap_flat;
        player->diablo_stats[DSTAT_CD_FLAT] += def->cd_flat;
        player->diablo_stats[DSTAT_REPLENISH] += def->replenish;
    }

    D_SyncDoomWeapon(player);
}

int D_Stat(struct player_s *pl, int stat)
{
    player_t *player = (player_t *)pl;
    if (stat < 0 || stat >= NUM_DSTATS)
        return 0;
    return player->diablo_stats[stat];
}

// Equipment dump, tied to opening the inventory (Jimmy, 2026-10-31):
// D_UIOpen calls this once per successful open, so the game log
// carries the exact equipped set -- per-slot item, durability,
// affixes, and aggregated totals -- instead of the vision loop
// having to read tooltips off screenshots. Read-only: prints to
// the log only, changes no state.
void D_DumpEquippedGear(void)
{
    static const char *slotnames[NUM_ESLOTS] =
    {
        "HELM", "ARMOR", "WEAPON", "SHIELD", "RING1", "RING2",
        "AMULET", "BOOTS", "GLOVES", "BELT", "FOCUS"
    };
    static const char *tiernames[NUM_TIERS] =
    {
        "Normal", "Magic", "Rare", "Set", "Unique"
    };
    player_t *player = &players[consoleplayer];
    struct player_s *pl = (struct player_s *)player;
    int s;
    int b;

    printf("[GEAR] equipped set:\n");
    for (s = 0; s < NUM_ESLOTS; s++)
    {
        int id = player->diablo_equipped[s];
        const diablo_itemdef_t *def;
        char name[128];
        char affix[1024];
        char tmp[64];
        int dur;
        int maxdur;
        const char *tiername;

        if (id == D_NOITEM)
        {
            printf("[GEAR] %s: (empty)\n", slotnames[s]);
            continue;
        }
        def = D_GetItemDef(D_ITEMTIER(id), D_ITEMIDX(id));
        if (!def)
        {
            printf("[GEAR] %s: (bad item id %d)\n", slotnames[s], id);
            continue;
        }
        D_ItemDisplayName(def, name, sizeof(name));
        tiername = (def->tier >= 0 && def->tier < NUM_TIERS)
                       ? tiernames[def->tier] : "?";
        dur = player->diablo_equipped_dur[s];
        maxdur = D_ItemMaxDurability(def);
        if (maxdur > 0)
            printf("[GEAR] %s: %s (%s) dur %d/%d\n",
                   slotnames[s], name, tiername, dur, maxdur);
        else
            printf("[GEAR] %s: %s (%s) dur n/a\n",
                   slotnames[s], name, tiername);

        /* One affix line listing only the nonzero stats. */
        affix[0] = '\0';
        if (def->dmg_max > 0)
        {
            snprintf(tmp, sizeof(tmp), " dmg %d-%d",
                     def->dmg_min, def->dmg_max);
            strcat(affix, tmp);
        }
        if (def->armor != 0)
        {
            snprintf(tmp, sizeof(tmp), " armor %d", def->armor);
            strcat(affix, tmp);
        }
        if (D_IsDurabilityItem(def) && dur == 0)
            strcat(affix, " worn-out(armor contributes 0)");
        if (def->str != 0)
        {
            snprintf(tmp, sizeof(tmp), " STR %+d", def->str);
            strcat(affix, tmp);
        }
        if (def->dex != 0)
        {
            snprintf(tmp, sizeof(tmp), " DEX %+d", def->dex);
            strcat(affix, tmp);
        }
        if (def->vit != 0)
        {
            snprintf(tmp, sizeof(tmp), " VIT %+d", def->vit);
            strcat(affix, tmp);
        }
        if (def->ene != 0)
        {
            snprintf(tmp, sizeof(tmp), " ENE %+d", def->ene);
            strcat(affix, tmp);
        }
        if (def->fres != 0)
        {
            snprintf(tmp, sizeof(tmp), " fres %d%%", def->fres);
            strcat(affix, tmp);
        }
        if (def->cres != 0)
        {
            snprintf(tmp, sizeof(tmp), " cres %d%%", def->cres);
            strcat(affix, tmp);
        }
        if (def->lres != 0)
        {
            snprintf(tmp, sizeof(tmp), " lres %d%%", def->lres);
            strcat(affix, tmp);
        }
        if (def->pres != 0)
        {
            snprintf(tmp, sizeof(tmp), " pres %d%%", def->pres);
            strcat(affix, tmp);
        }
        if (def->lifesteal != 0)
        {
            snprintf(tmp, sizeof(tmp), " lifesteal %d%%", def->lifesteal);
            strcat(affix, tmp);
        }
        if (def->magicfind != 0)
        {
            snprintf(tmp, sizeof(tmp), " magicfind %d%%", def->magicfind);
            strcat(affix, tmp);
        }
        if (def->movespeed != 0)
        {
            snprintf(tmp, sizeof(tmp), " movespeed %d%%", def->movespeed);
            strcat(affix, tmp);
        }
        if (def->ad_pct != 0)
        {
            snprintf(tmp, sizeof(tmp), " AD %d%%", def->ad_pct);
            strcat(affix, tmp);
        }
        if (def->ap_pct != 0)
        {
            snprintf(tmp, sizeof(tmp), " AP %d%%", def->ap_pct);
            strcat(affix, tmp);
        }
        if (def->haste != 0)
        {
            snprintf(tmp, sizeof(tmp), " haste %d%%", def->haste);
            strcat(affix, tmp);
        }
        if (def->crit_chance != 0)
        {
            snprintf(tmp, sizeof(tmp), " crit %d%%", def->crit_chance);
            strcat(affix, tmp);
        }
        if (def->crit_dmg != 0)
        {
            snprintf(tmp, sizeof(tmp), " critdmg %d%%", def->crit_dmg);
            strcat(affix, tmp);
        }
        if (def->plasma_cap != 0)
        {
            snprintf(tmp, sizeof(tmp), " plasmacap %+d", def->plasma_cap);
            strcat(affix, tmp);
        }
        if (def->ad_flat != 0)
        {
            snprintf(tmp, sizeof(tmp), " ADflat %+d", def->ad_flat);
            strcat(affix, tmp);
        }
        if (def->ap_flat != 0)
        {
            snprintf(tmp, sizeof(tmp), " APflat %+d", def->ap_flat);
            strcat(affix, tmp);
        }
        if (def->cd_flat != 0)
        {
            snprintf(tmp, sizeof(tmp), " CDR %+d", def->cd_flat);
            strcat(affix, tmp);
        }
        if (def->replenish != 0)
        {
            snprintf(tmp, sizeof(tmp), " replenish %d HP/s",
                     def->replenish);
            strcat(affix, tmp);
        }
        if (affix[0] != '\0')
            printf("[GEAR] %s affixes:%s\n", slotnames[s], affix);
        for (b = 0; b < 15; b++)
        {
            const char *mdesc;
            if (!(def->mech & (1 << b)))
                continue;
            mdesc = D_MechDesc(1 << b);
            if (mdesc != NULL)
                printf("[GEAR] %s mech: %s\n", slotnames[s], mdesc);
        }
    }
    printf("[GEAR] TOTALS dmg %d-%d armor %d lifesteal %d%% "
           "replenish %d HP/s crit %d%%/%d%% AD %d%% AP %d%% haste %d%%\n",
           D_Stat(pl, DSTAT_DMG_MIN), D_Stat(pl, DSTAT_DMG_MAX),
           D_Stat(pl, DSTAT_ARMOR), D_Stat(pl, DSTAT_LIFESTEAL),
           D_Stat(pl, DSTAT_REPLENISH), D_Stat(pl, DSTAT_CRIT_CHANCE),
           D_Stat(pl, DSTAT_CRIT_DMG), D_Stat(pl, DSTAT_AD_PCT),
           D_Stat(pl, DSTAT_AP_PCT), D_Stat(pl, DSTAT_HASTE));
}

int D_MaxHealth(struct player_s *pl)
{
    // vitality: +5 max HP per point, on top of the dehacked max.
    return deh_max_health + D_Stat(pl, DSTAT_VIT) * 5;
}

// Phase 2 backpack grid helpers.  Each backpack entry tracks the
// top-left cell of its footprint (diablo_bp_gx/gy), or -1/-1.

int D_BpPages(struct player_s *pl)
{
    player_t *player = (player_t *)pl;
    if (player->diablo_bp_pages < 1)
        return 1;
    if (player->diablo_bp_pages > D_BP_MAX_PAGES)
        return D_BP_MAX_PAGES;
    return player->diablo_bp_pages;
}

int D_BpCurPage(struct player_s *pl)
{
    player_t *player = (player_t *)pl;
    int pages = D_BpPages(pl);
    if (player->diablo_bp_cur < 0)
        return 0;
    if (player->diablo_bp_cur >= pages)
        return pages - 1;
    return player->diablo_bp_cur;
}

boolean D_GridFindSpacePage(struct player_s *pl, int page,
                            int w, int h, int *gx, int *gy)
{
    player_t *player = (player_t *)pl;
    int x, y, i, ox, oy, ow, oh, oid;
    const diablo_itemdef_t *odef;
    boolean blocked;

    if (w < 1) w = 1;
    if (h < 1) h = 1;

    for (y = 0; y + h <= D_BP_GRID_H; y++)
    {
        for (x = 0; x + w <= D_BP_GRID_W; x++)
        {
            blocked = false;
            for (i = 0; i < player->diablo_bp_count; i++)
            {
                if (player->diablo_bp_page[i] != page)
                    continue;
                ox = player->diablo_bp_gx[i];
                oy = player->diablo_bp_gy[i];
                if (ox < 0 || oy < 0)
                    continue;
                oid = player->diablo_backpack[i];
                odef = D_GetItemDef(D_ITEMTIER(oid), D_ITEMIDX(oid));
                if (!odef)
                    continue;
                ow = odef->grid_w;
                oh = odef->grid_h;
                if (ow < 1) ow = 1;
                if (oh < 1) oh = 1;
                if (x < ox + ow && x + w > ox && y < oy + oh && y + h > oy)
                {
                    blocked = true;
                    break;
                }
            }
            if (!blocked)
            {
                *gx = x;
                *gy = y;
                return true;
            }
        }
    }
    return false;
}

// Current-page search, for UI placement (the UI shows one page).
boolean D_GridFindSpace(struct player_s *pl, int w, int h, int *gx, int *gy)
{
    return D_GridFindSpacePage(pl, D_BpCurPage(pl), w, h, gx, gy);
}

// Heal a loaded backpack layout: re-place, first-fit and in stored
// order, any item whose stored position no longer fits under the
// current def footprints (out of bounds, or overlapping an item
// already validated).  Footprints live in the def tables, not in
// saves, so a footprint change (the 2026-10-22 horizontal weapon
// shapes) invalidates old saves' positions; placements that are
// still valid are left exactly where the player put them.
void D_RepairBackpackLayout(struct player_s *pl)
{
    player_t *player = (player_t *)pl;
    boolean occupied[D_BP_GRID_H][D_BP_GRID_W];
    int i, x, y, cx, cy, page;

    // Repair page by page: occupancy is per page, so an item on
    // page 2 must never block (or be blocked by) a page 1 item.
    for (page = 0; page < D_BP_MAX_PAGES; page++)
    {
    memset(occupied, 0, sizeof(occupied));
    for (i = 0; i < player->diablo_bp_count; i++)
    {
        int id = player->diablo_backpack[i];
        const diablo_itemdef_t *def;
        int gx, gy, w, h;
        boolean ok;

        if (id == D_NOITEM)
            continue;
        if (player->diablo_bp_page[i] != page)
            continue;
        def = D_GetItemDef(D_ITEMTIER(id), D_ITEMIDX(id));
        if (!def)
            continue;
        w = def->grid_w;
        h = def->grid_h;
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        gx = player->diablo_bp_gx[i];
        gy = player->diablo_bp_gy[i];

        ok = gx >= 0 && gy >= 0
          && gx + w <= D_BP_GRID_W && gy + h <= D_BP_GRID_H;
        if (ok)
        {
            for (cy = gy; cy < gy + h && ok; cy++)
                for (cx = gx; cx < gx + w && ok; cx++)
                    if (occupied[cy][cx])
                        ok = false;
        }
        if (!ok)
        {
            int fx = -1, fy = -1;
            for (y = 0; y + h <= D_BP_GRID_H && fx < 0; y++)
            {
                for (x = 0; x + w <= D_BP_GRID_W && fx < 0; x++)
                {
                    boolean free = true;
                    for (cy = y; cy < y + h && free; cy++)
                        for (cx = x; cx < x + w && free; cx++)
                            if (occupied[cy][cx])
                                free = false;
                    if (free)
                    {
                        fx = x;
                        fy = y;
                    }
                }
            }
            if (fx >= 0)
            {
                printf("[DIABLO] backpack layout repair: item %d "
                       "moved (%d,%d) -> (%d,%d)\n",
                       id, gx, gy, fx, fy);
                gx = fx;
                gy = fy;
                player->diablo_bp_gx[i] = gx;
                player->diablo_bp_gy[i] = gy;
            }
            // No free spot at all: leave the stored position; its
            // clamped cells still mark occupancy below so later
            // items route around it.
        }
        for (cy = gy; cy < gy + h; cy++)
            for (cx = gx; cx < gx + w; cx++)
                if (cx >= 0 && cx < D_BP_GRID_W
                 && cy >= 0 && cy < D_BP_GRID_H)
                    occupied[cy][cx] = true;
    }
    }
}

// Remove the backpack entry at index bpi, shifting the rest down.
static void D_BackpackRemove(player_t *player, int bpi)
{
    int i;
    for (i = bpi; i + 1 < player->diablo_bp_count; i++)
    {
        player->diablo_backpack[i] = player->diablo_backpack[i + 1];
        player->diablo_bp_gx[i] = player->diablo_bp_gx[i + 1];
        player->diablo_bp_gy[i] = player->diablo_bp_gy[i + 1];
        player->diablo_backpack_dur[i] = player->diablo_backpack_dur[i + 1];
        player->diablo_bp_page[i] = player->diablo_bp_page[i + 1];
    }
    player->diablo_bp_count--;
    player->diablo_backpack[player->diablo_bp_count] = D_NOITEM;
    player->diablo_bp_gx[player->diablo_bp_count] = -1;
    player->diablo_bp_gy[player->diablo_bp_count] = -1;
    player->diablo_backpack_dur[player->diablo_bp_count] = 0;
    player->diablo_bp_page[player->diablo_bp_count] = 0;
}

// Public wrapper for the UI.
void D_BackpackRemoveAt(struct player_s *pl, int bpi)
{
    player_t *player = (player_t *)pl;
    if (bpi < 0 || bpi >= player->diablo_bp_count)
        return;
    if (player->diablo_recent == bpi)
        player->diablo_recent = -1;
    else if (player->diablo_recent > bpi)
        player->diablo_recent--;
    D_BackpackRemove(player, bpi);
}

// Roll a random weapon item of the given tier for a Doom weapon,
// honoring the difficulty gate like D_RollItemForDifficulty.
// Returns a packed item id, or D_NOITEM when the tier has no
// weapon of that type -- the tier tables have gaps (e.g. there
// is no unique pistol), and callers decide how to handle them.
int D_RollWeaponItem(int tier, int doomweapon)
{
    int i, count = 0, pick;
    const diablo_itemdef_t *def;

    if (tier < 0 || tier >= NUM_TIERS)
        return D_NOITEM;

    for (i = 0; i < D_TierCount(tier); i++)
    {
        def = D_GetItemDef(tier, i);
        if (def && def->slot == ESLOT_WEAPON
            && def->doomweapon == doomweapon
            && def->diff_tier <= D_DifficultyTier())
            count++;
    }
    if (!count)
        return D_NOITEM;

    pick = P_Random() % count;
    for (i = 0; i < D_TierCount(tier); i++)
    {
        def = D_GetItemDef(tier, i);
        if (def && def->slot == ESLOT_WEAPON
            && def->doomweapon == doomweapon
            && def->diff_tier <= D_DifficultyTier()
            && pick-- == 0)
            return D_MAKEITEM(tier, i);
    }
    return D_NOITEM;
}

// The plain normal-tier item for a Doom weapon, as a packed item
// id, or D_NOITEM when the weapon has no base item. Weapon
// pickups roll rarity first (D_RollWeaponItem); this is the
// guaranteed fallback when a roll produces nothing.
int D_BaseItemForWeapon(int doomweapon)
{
    int i, count = D_TierCount(TIER_NORMAL);
    for (i = 0; i < count; i++)
    {
        const diablo_itemdef_t *def = D_GetItemDef(TIER_NORMAL, i);
        if (def && def->slot == ESLOT_WEAPON
            && def->doomweapon == doomweapon)
            return D_MAKEITEM(TIER_NORMAL, i);
    }
    return D_NOITEM;
}

// --------------------------------------------------------------------
// WAD weapon names.
//
// A WAD renames the Doom weapons through its DEHACKED pickup
// strings (Freedoom: chaingun -> "minigun", chainsaw -> "ripsaw",
// BFG9000 -> "SKAG 1337"). A weapon item whose table name is built
// around the canonical base name inherits the WAD's name at
// display time: "Vampiric Chainsaw" shows as "Vampiric Ripsaw"
// under Freedoom. Bespoke names without the base token (Unmaker,
// BFG10K, Old Painless), weapons the WAD doesn't rename (pistol
// has no pickup string at all), and every non-weapon item keep
// their table names. Display only: item identity stays (tier, idx).
// --------------------------------------------------------------------

// Per weapon: the vanilla GOT* pickup string, the weapon's name
// inside it, the base-name token the item tables build around,
// and a shorter fallback token (or NULL).
static const struct
{
    const char *got;
    const char *token;
    const char *label;
    const char *alt;
} wad_weapon_names[NUMWEAPONS] =
{
    { NULL,         NULL,              "Fists",           NULL }, // fist
    { NULL,         NULL,              "Pistol",          NULL }, // pistol
    { GOTSHOTGUN,   "shotgun",         "Shotgun",         NULL },
    { GOTCHAINGUN,  "chaingun",        "Chaingun",        NULL },
    { GOTLAUNCHER,  "rocket launcher", "Rocket Launcher", NULL },
    { GOTPLASMA,    "plasma gun",      "Plasma Rifle",    NULL },
    { GOTBFG9000,   "BFG9000",         "BFG9000",         "BFG" },
    { GOTCHAINSAW,  "chainsaw",        "Chainsaw",        NULL },
    { GOTSHOTGUN2,  "super shotgun",   "Super Shotgun",   NULL },
};

// Display shortenings for WAD names too long to sit inside an
// item name (Jimmy's call). Matched exactly against the extracted
// name, so other WADs -- and other names from the same WAD --
// keep their full form.
static const char *D_ShortenWadName(const char *name)
{
    if (!strcmp(name, "SKAG 1337"))
        return "SKAG";
    if (!strcmp(name, "Pump-Action Shotgun"))
        return "Shotgun";
    if (!strcmp(name, "Double-Barrelled Shotgun"))
        return "Dbl Shotgun";
    if (!strcmp(name, "Polaric Energy Weapon"))
        return "Nrg Cannon";
    if (!strcmp(name, "Polaric Energy Cannon"))
        return "Nrg Cannon";
    if (!strcmp(name, "Missile Launcher"))
        return "Launcher";
    if (!strcmp(name, "Angle Grinder"))
        return "Grinder";
    return name;
}

static char wad_name_cache[NUMWEAPONS][48];
static boolean wad_name_done = false;

// The WAD's own name for a Doom weapon, title-cased, or NULL when
// the WAD keeps the vanilla name. Extracted once from the
// DEHACKED pickup string: the name follows the first " the "
// ("Got the minigun!") and runs to the sentence end.
static const char *D_WadWeaponName(int doomweapon)
{
    int w;

    if (!wad_name_done)
    {
        wad_name_done = true;
        for (w = 0; w < NUMWEAPONS; w++)
        {
            const char *vanilla = wad_weapon_names[w].got;
            const char *wad, *p, *end;
            int len, i;

            wad_name_cache[w][0] = '\0';
            if (!vanilla)
                continue;
            wad = DEH_String(vanilla);
            if (!wad || !strcmp(wad, vanilla))
                continue; // not renamed by this WAD

            p = strstr(wad, " the ");
            if (p)
                p += 5;
            else if (!strncmp(wad, "A ", 2))
                p = wad + 2;
            else
                continue;
            end = p;
            while (*end && *end != '!' && *end != '.')
                end++;
            while (end > p && end[-1] == ' ')
                end--;
            len = (int)(end - p);
            if (len < 2 || len >= (int)sizeof(wad_name_cache[w]))
                continue;
            memcpy(wad_name_cache[w], p, len);
            wad_name_cache[w][len] = '\0';
            // Title-case: uppercase each word's first letter,
            // leave the rest as written ("SKAG 1337" stays).
            for (i = 0; i < len; i++)
                if (i == 0 || wad_name_cache[w][i - 1] == ' '
                    || wad_name_cache[w][i - 1] == '-')
                    wad_name_cache[w][i] =
                        toupper((unsigned char)wad_name_cache[w][i]);
            // Then shorten the over-long ones ("SKAG 1337" -> "SKAG").
            {
                const char *short_name =
                    D_ShortenWadName(wad_name_cache[w]);
                if (short_name != wad_name_cache[w])
                    snprintf(wad_name_cache[w],
                             sizeof(wad_name_cache[w]), "%s", short_name);
            }
        }
    }

    if (doomweapon < 0 || doomweapon >= NUMWEAPONS
        || !wad_name_cache[doomweapon][0])
        return NULL;
    return wad_name_cache[doomweapon];
}

// Find token in name as a standalone word (bounded by spaces or
// the string ends), so "BFG" doesn't match inside "BFG10K".
static const char *D_FindWord(const char *name, const char *token)
{
    int tlen = (int)strlen(token);
    const char *p = name;

    while ((p = strstr(p, token)) != NULL)
    {
        if ((p == name || p[-1] == ' ')
            && (p[tlen] == '\0' || p[tlen] == ' '))
            return p;
        p++;
    }
    return NULL;
}

void D_ItemDisplayName(const diablo_itemdef_t *def, char *buf, int bufsize)
{
    const char *wad, *token, *hit;

    if (!def)
    {
        snprintf(buf, bufsize, "?");
        return;
    }
    if (def->slot != ESLOT_WEAPON
        || !(wad = D_WadWeaponName(def->doomweapon)))
    {
        snprintf(buf, bufsize, "%s", def->name);
        return;
    }

    token = wad_weapon_names[def->doomweapon].label;
    hit = D_FindWord(def->name, token);
    if (!hit && wad_weapon_names[def->doomweapon].alt)
    {
        token = wad_weapon_names[def->doomweapon].alt;
        hit = D_FindWord(def->name, token);
    }
    if (!hit)
    {
        snprintf(buf, bufsize, "%s", def->name);
        return;
    }
    snprintf(buf, bufsize, "%.*s%s%s",
             (int)(hit - def->name), def->name, wad,
             hit + strlen(token));
}

boolean D_BackpackAdd(struct player_s *pl, int tier, int idx)
{
    const diablo_itemdef_t *def = D_GetItemDef(tier, idx);
    return D_BackpackAddDur(pl, tier, idx, D_ItemMaxDurability(def));
}

boolean D_BackpackAddDur(struct player_s *pl, int tier, int idx, int dur)
{
    player_t *player = (player_t *)pl;
    const diablo_itemdef_t *def = D_GetItemDef(tier, idx);
    int gx, gy;
    static const char *tier_msgs[NUM_TIERS] =
    {
        "Picked up %s.",
        "Magic item: %s!",
        "Rare item: %s!!",
        "Set item: %s!!",
        "*** UNIQUE: %s ***"
    };

    if (!def)
        return false;
    if (player->diablo_bp_count >= D_BACKPACK_MAX)
        return false;

    // Phase 2: the item needs a free footprint on a backpack grid.
    // Try the current page first, then the other unlocked pages in
    // order, so a pickup never fails while another page has room.
    {
        int pages = D_BpPages(pl);
        int cur = D_BpCurPage(pl);
        int page, found = 0;
        /* Current page first, then the other unlocked pages. */
        if (D_GridFindSpacePage(pl, cur, def->grid_w, def->grid_h,
                                &gx, &gy))
        {
            player->diablo_bp_page[player->diablo_bp_count] = cur;
            found = 1;
        }
        for (page = 0; page < pages && !found; page++)
        {
            if (page == cur)
                continue;
            if (D_GridFindSpacePage(pl, page, def->grid_w, def->grid_h,
                                    &gx, &gy))
            {
                player->diablo_bp_page[player->diablo_bp_count] = page;
                found = 1;
            }
        }
        if (!found)
            return false;
    }

    player->diablo_backpack[player->diablo_bp_count] = D_MAKEITEM(tier, idx);
    player->diablo_bp_gx[player->diablo_bp_count] = gx;
    player->diablo_bp_gy[player->diablo_bp_count] = gy;
    player->diablo_backpack_dur[player->diablo_bp_count] = dur;
    player->diablo_recent = player->diablo_bp_count;
    player->diablo_bp_count++;

    {
        char namebuf[96];
        D_ItemDisplayName(def, namebuf, sizeof(namebuf));
        D_Msg(player, tier_msgs[tier], namebuf);
    }
    return true;
}

// Could the backpack take ANY item right now? The loot prompt keys
// on this: with a full pack the honest message is "Loot nearby, but
// backpack full", not "press G to loot" (Jimmy, 2026-10-21).
boolean D_BackpackFull(struct player_s *pl)
{
    player_t *player = (player_t *)pl;
    int gx, gy;

    if (player->diablo_bp_count >= D_BACKPACK_MAX)
        return true;
    // Nothing can be added when not even a 1x1 item finds space on
    // any unlocked page.
    {
        int pages = D_BpPages(pl);
        int page;
        for (page = 0; page < pages; page++)
            if (D_GridFindSpacePage(pl, page, 1, 1, &gx, &gy))
                return false;
    }
    return true;
}

// Throttled "Backpack full!" (pickup is re-attempted every tic while
// the player stands on the item).
void D_BackpackFullMsg(struct player_s *pl)
{
    player_t *player = (player_t *)pl;
    static int lasttic = -TICRATE * 2;

    if (gametic - lasttic < TICRATE)
        return;
    lasttic = gametic;
    D_Msg(player, "Backpack full! Press Q to unequip.");
}

static void D_UseConsumable(player_t *player, const diablo_itemdef_t *def)
{
    int max, amount;
    char namebuf[96];

    D_ItemDisplayName(def, namebuf, sizeof(namebuf));

    // Phase 4: energy boosts potion effectiveness (+2% per point).
    amount = def->heal + def->heal * D_PotionBonus((struct player_s *)player)
                          / 100;

    switch (def->usekind)
    {
      case USE_HEAL:
        max = D_MaxHealth((struct player_s *)player);
        player->health += amount;
        if (player->health > max)
            player->health = max;
        player->mo->health = player->health;
        D_Msg(player, "You quaff the %s. (+%d HP)", namebuf, amount);
        break;

      case USE_MANA:
        // Mana potion: fully reload every weapon's magazine and vent
        // all plasma heat.
        T_KitRefillAllAmmo();
        D_Msg(player, "You quaff the %s. (ammo restored)",
              namebuf);
        break;

      default: // USE_BLAST
        // A gas bomb at your feet: hurts everything nearby, you included.
        // (Poison resist does not protect you from your own bomb.)
        P_RadiusAttack(player->mo, player->mo, amount);
        D_Msg(player, "The %s bursts into toxic gas!", namebuf);
        break;
    }
}

// True if the item can be equipped in the given slot.  Rings fit either
// ring slot; consumables fit no slot.
boolean D_SlotFits(int slot, const diablo_itemdef_t *def)
{
    if (!def || def->consumable || def->slot == ESLOT_NONE)
        return false;
    if (def->slot == ESLOT_RING1)
        return slot == ESLOT_RING1 || slot == ESLOT_RING2;
    return def->slot == slot;
}

// Two-handed rule (Jimmy, 2026-10-22): a weapon is two-handed iff
// its backpack footprint takes up 2 rows; a 1-row weapon is
// one-handed and leaves the shield hand free.  The footprint comes
// from the weapon's plain base item (every variant of a base
// shares its grid size), so reshaping a footprint reclassifies
// the weapon -- with the horizontal footprints, the 3x2 guns
// (chaingun, rocket, plasma, BFG) are two-handed and the 3x1/2x1
// guns (shotgun, SSG, chainsaw, pistol) are one-handed.
boolean D_IsTwoHandedWeapon(int doomweapon)
{
    int id = D_BaseItemForWeapon(doomweapon);
    const diablo_itemdef_t *def;

    if (id == D_NOITEM)
        return false;
    def = D_GetItemDef(D_ITEMTIER(id), D_ITEMIDX(id));
    return def && def->grid_h >= 2;
}

// Use the consumable at backpack index bpi (no-op when not consumable).
void D_UseBackpackItem(struct player_s *pl, int bpi)
{
    player_t *player = (player_t *)pl;
    int id;
    const diablo_itemdef_t *def;

    if (bpi < 0 || bpi >= player->diablo_bp_count)
        return;
    id = player->diablo_backpack[bpi];
    def = D_GetItemDef(D_ITEMTIER(id), D_ITEMIDX(id));
    if (!def || !def->consumable)
        return;
    D_UseConsumable(player, def);
    D_BackpackRemove(player, bpi);
    player->diablo_recent = -1;
}

// E key (Phase 1 placeholder): equip the most recently picked-up item,
// or use it if it is a consumable.  Swaps with the equipped item when
// the slot is taken.
void D_EquipRecent(struct player_s *pl)
{
    player_t *player = (player_t *)pl;
    int bpi, id, tier, idx, slot, old;
    const diablo_itemdef_t *def;

    if (player->playerstate != PST_LIVE || player->health <= 0)
        return;

    bpi = player->diablo_recent;
    if (bpi < 0 || bpi >= player->diablo_bp_count)
        return;  // nothing new to equip (also debounces key repeat)

    id = player->diablo_backpack[bpi];
    tier = D_ITEMTIER(id);
    idx = D_ITEMIDX(id);
    def = D_GetItemDef(tier, idx);
    if (!def)
        return;

    if (def->consumable)
    {
        D_UseBackpackItem(pl, bpi);
        return;
    }

    slot = def->slot;
    if (slot == ESLOT_RING1)
    {
        // Rings fill the first free ring slot, else swap with RING1.
        if (player->diablo_equipped[ESLOT_RING1] == D_NOITEM)
            slot = ESLOT_RING1;
        else if (player->diablo_equipped[ESLOT_RING2] == D_NOITEM)
            slot = ESLOT_RING2;
        else
            slot = ESLOT_RING1;
    }

    old = player->diablo_equipped[slot];
    if (old != D_NOITEM)
    {
        const diablo_itemdef_t *odef;
        int gx, gy;
        odef = D_GetItemDef(D_ITEMTIER(old), D_ITEMIDX(old));
        if (!odef
         || player->diablo_bp_count >= D_BACKPACK_MAX
         || !D_GridFindSpace(pl, odef->grid_w, odef->grid_h, &gx, &gy))
        {
            // No room to stash the swapped-out item.
            D_BackpackFullMsg(pl);
            return;
        }
    }

    {
        int new_dur = player->diablo_backpack_dur[bpi];
        D_BackpackRemove(player, bpi);
        if (old != D_NOITEM)
        {
            // Stash the displaced item with grid placement.
            const diablo_itemdef_t *odef;
            int gx, gy;
            odef = D_GetItemDef(D_ITEMTIER(old), D_ITEMIDX(old));
            D_GridFindSpace(pl, odef->grid_w, odef->grid_h, &gx, &gy);
            player->diablo_backpack[player->diablo_bp_count] = old;
            player->diablo_bp_gx[player->diablo_bp_count] = gx;
            player->diablo_bp_gy[player->diablo_bp_count] = gy;
            player->diablo_bp_page[player->diablo_bp_count] =
                D_BpCurPage(pl);
            player->diablo_backpack_dur[player->diablo_bp_count] =
                player->diablo_equipped_dur[slot];
            player->diablo_bp_count++;
        }
        player->diablo_equipped_dur[slot] = new_dur;
    }
    player->diablo_equipped[slot] = id;
    player->diablo_recent = -1;
    D_RecalcStats(pl);

    {
        char namebuf[96];
        D_ItemDisplayName(def, namebuf, sizeof(namebuf));
        D_Msg(player, "Equipped %s.", namebuf);
    }
}

// Q key (Phase 1 placeholder): unequip everything back to the backpack.
void D_UnequipAll(struct player_s *pl)
{
    player_t *player = (player_t *)pl;
    int s, moved = 0;

    if (player->playerstate != PST_LIVE)
        return;

    for (s = 0; s < NUM_ESLOTS; s++)
    {
        int id = player->diablo_equipped[s];
        const diablo_itemdef_t *def;
        int gx, gy;
        if (id == D_NOITEM)
            continue;
        def = D_GetItemDef(D_ITEMTIER(id), D_ITEMIDX(id));
        if (!def)
            continue;
        // Find a grid spot for the item's footprint.
        if (!D_GridFindSpace(pl, def->grid_w, def->grid_h, &gx, &gy))
        {
            D_BackpackFullMsg(pl);
            break;
        }
        player->diablo_equipped[s] = D_NOITEM;
        player->diablo_backpack[player->diablo_bp_count] = id;
        player->diablo_bp_gx[player->diablo_bp_count] = gx;
        player->diablo_bp_gy[player->diablo_bp_count] = gy;
        player->diablo_bp_page[player->diablo_bp_count] =
            D_BpCurPage(pl);
        player->diablo_backpack_dur[player->diablo_bp_count] =
            player->diablo_equipped_dur[s];
        player->diablo_equipped_dur[s] = 0;
        player->diablo_bp_count++;
        moved++;
    }

    if (moved)
    {
        D_RecalcStats(pl);
        player->diablo_recent = -1;
        D_Msg(player, "Unequipped all (%d item%s).", moved,
              moved == 1 ? "" : "s");
        // Autosave: inventory changed.
        {
            extern void D_AutoSaveGame(void);
            D_AutoSaveGame();
        }
    }
}

// Combat hooks.

int D_WeaponBonus(struct player_s *pl)
{
    int lo = D_Stat(pl, DSTAT_DMG_MIN);
    int hi = D_Stat(pl, DSTAT_DMG_MAX);
    int bonus;

    if (hi <= 0)
        bonus = 0;
    else if (hi <= lo)
        bonus = lo;
    else
        bonus = lo + (P_Random() % (hi - lo + 1));

    // strength: +1 damage per 4 points, on all player attacks.
    bonus += D_Stat(pl, DSTAT_STR) / 4;

    return bonus;
}

// ------------------------------------------------------------------
// Durability + Diablo 2-style defense (Jimmy, 2026-10-21).
//
// Durability lives on the five armor-producing slots: helm, armor,
// boots, gloves, belt. Each landed monster hit wears ONE random worn
// piece by 1 point. At 0 the piece stays equipped and keeps its other
// stats, but contributes no Defense. Repair: essence in the inventory
// screen (F key), or free and full when a vanilla Doom armor pickup
// (green/blue suit, megasphere) is taken.
// ------------------------------------------------------------------

boolean D_IsDurabilityItem(const diablo_itemdef_t *def)
{
    if (!def || def->consumable)
        return false;
    return def->slot == ESLOT_HELM
        || def->slot == ESLOT_ARMOR
        || def->slot == ESLOT_BOOTS
        || def->slot == ESLOT_GLOVES
        || def->slot == ESLOT_BELT;
}

int D_ItemMaxDurability(const diablo_itemdef_t *def)
{
    if (!D_IsDurabilityItem(def))
        return 0;
    // Defense values run into the thousands after the D2 rescale;
    // 20 base + 1 per 20 Defense lands at ~30 for normal gear and
    // ~80 for the best unique chest.
    return 20 + def->armor / 20;
}

// HUD armor readout (Jimmy, 2026-10-23): the status-bar ARMOR value is
// the total remaining durability of the equipped defense pieces (the
// five durability slots), NOT vanilla armorpoints. Vanilla absorption
// still runs underneath in P_DamageMobj; this is display only.
int D_EquippedDurabilitySum(struct player_s *pl)
{
    player_t *player = (player_t *)pl;
    int sum = 0, s;

    for (s = 0; s < NUM_ESLOTS; s++)
    {
        int id = player->diablo_equipped[s];
        const diablo_itemdef_t *def;
        if (id == D_NOITEM)
            continue;
        def = D_GetItemDef(D_ITEMTIER(id), D_ITEMIDX(id));
        if (D_IsDurabilityItem(def))
            sum += player->diablo_equipped_dur[s];
    }
    return sum;
}

void D_WearRandomPiece(struct player_s *pl)
{
    player_t *player = (player_t *)pl;
    int worn[NUM_ESLOTS];
    int n = 0, s, pick;

    for (s = 0; s < NUM_ESLOTS; s++)
    {
        int id = player->diablo_equipped[s];
        const diablo_itemdef_t *def;
        if (id == D_NOITEM)
            continue;
        def = D_GetItemDef(D_ITEMTIER(id), D_ITEMIDX(id));
        if (D_IsDurabilityItem(def) && player->diablo_equipped_dur[s] > 0)
            worn[n++] = s;
    }
    if (!n)
        return;

    pick = worn[P_Random() % n];
    player->diablo_equipped_dur[pick]--;
    if (player->diablo_equipped_dur[pick] <= 0)
    {
        const diablo_itemdef_t *def = D_GetItemDef(
            D_ITEMTIER(player->diablo_equipped[pick]),
            D_ITEMIDX(player->diablo_equipped[pick]));
        char namebuf[96];
        D_ItemDisplayName(def, namebuf, sizeof(namebuf));
        D_Msg(player, "Your %s is broken! (no Defense)", namebuf);
        printf("[DURABILITY] broken: %s (slot %d)\n", namebuf, pick);
        D_RecalcStats(pl);
    }
}

void D_RepairAllEquipped(struct player_s *pl)
{
    player_t *player = (player_t *)pl;
    int s, fixed = 0;

    for (s = 0; s < NUM_ESLOTS; s++)
    {
        int id = player->diablo_equipped[s];
        const diablo_itemdef_t *def;
        int max;
        if (id == D_NOITEM)
            continue;
        def = D_GetItemDef(D_ITEMTIER(id), D_ITEMIDX(id));
        max = D_ItemMaxDurability(def);
        if (max > 0 && player->diablo_equipped_dur[s] < max)
        {
            player->diablo_equipped_dur[s] = max;
            fixed++;
        }
    }
    if (fixed)
    {
        D_Msg(player, "The armor's power restores your gear!");
        printf("[DURABILITY] vanilla armor pickup: %d piece(s) "
               "restored to full\n", fixed);
        D_RecalcStats(pl);
    }
}

int D_RepairCost(int id, int cur_dur)
{
    const diablo_itemdef_t *def;
    int max, base, missing, cost;

    if (id == D_NOITEM)
        return 0;
    def = D_GetItemDef(D_ITEMTIER(id), D_ITEMIDX(id));
    max = D_ItemMaxDurability(def);
    if (max <= 0 || cur_dur >= max)
        return 0;

    switch (D_ITEMTIER(id))
    {
      case TIER_MAGIC:  base = 4;  break;
      case TIER_RARE:   base = 8;  break;
      case TIER_SET:    base = 12; break;
      case TIER_UNIQUE: base = 20; break;
      default:          base = 2;  break;  // TIER_NORMAL
    }
    missing = max - cur_dur;
    cost = (base * missing + max - 1) / max;  // pro-rated, rounded up
    return cost < 1 ? 1 : cost;
}

// Attack rating: a flat base plus the attacker's spawn health, so
// tougher monsters stay threatening as Defense climbs into the
// thousands (a baron rates ~1060, a cyberdemon ~4060).
#define D_AR_BASE 60

int D_EnemyAttackRating(struct mobj_s *source)
{
    if (!source || !source->info)
        return D_AR_BASE;
    return D_AR_BASE + source->info->spawnhealth;
}

int D_DefenseChanceToHit(struct player_s *pl, struct mobj_s *source)
{
    int defense = D_Stat(pl, DSTAT_ARMOR);
    int ar = D_EnemyAttackRating(source);
    int cth = 200 * ar / (ar + defense);

    if (cth < 5)
        cth = 5;
    if (cth > 95)
        cth = 95;
    return cth;
}

boolean D_DefenseHitRoll(struct player_s *pl, struct mobj_s *source)
{
    return (P_Random() % 100) < D_DefenseChanceToHit(pl, source);
}

void D_LifeSteal(struct player_s *pl, int damage)
{
    player_t *player = (player_t *)pl;
    int ls = D_Stat(pl, DSTAT_LIFESTEAL);
    int heal, max;

    if (ls <= 0 || damage <= 0 || player->health <= 0)
        return;

    heal = damage * ls / 100;
    if (heal <= 0)
        return;

    max = D_MaxHealth(pl);
    player->health += heal;
    if (player->health > max)
        player->health = max;
    player->mo->health = player->health;
}

int D_MoveSpeed(struct player_s *pl)
{
    return D_Stat(pl, DSTAT_MOVESPEED);
}

// Shared movespeed factor, in percent (100 = unmodified). Every
// movement path scales through here so the modifier means the same
// thing everywhere; movespeed <= 0 is no scaling.
static int D_MovePct(struct player_s *pl)
{
    int ms = D_MoveSpeed(pl);
    return ms > 0 ? 100 + ms : 100;
}

// Scale a displacement (fixed-point units) by the movespeed modifier.
fixed_t D_ScaleDisplacement(struct player_s *pl, fixed_t base)
{
    return (base * D_MovePct(pl)) / 100;
}

// Scale a step duration (tics) by the movespeed modifier: faster
// movement = proportionally shorter steps. Never below 1 tic.
int D_ScaleMoveTics(struct player_s *pl, int base_tics)
{
    int tics = (base_tics * 100) / D_MovePct(pl);
    return tics < 1 ? 1 : tics;
}

// NG+ loop state. d_loop = 0 is the first playthrough.
int d_loop = 0;

// Enemy HP scales +50% per loop, damage +25% per loop.
int D_LoopHpMult(void)
{
    return 100 + d_loop * 50;
}

int D_LoopDmgMult(void)
{
    return 100 + d_loop * 25;
}

// Higher loops get better loot: +10% magic find per loop.
int D_LoopMagicFind(void)
{
    return d_loop * 10;
}

// Called when the final map is cleared. Increments the loop counter;
// the caller warps back to MAP01 with the player's gear intact.
void D_BeatGame(void)
{
    d_loop++;
}

// Difficulty tier for affix drops: 0=Normal, 1=Nightmare, 2=Hell.
// NG+ loops also raise the tier (loop 1 = Nightmare affixes, etc.).
int D_DifficultyTier(void)
{
    int tier = 0;
    if (gameskill == sk_hard)      // Nightmare
        tier = 1;
    else if (gameskill == sk_nightmare) // Hell
        tier = 2;
    tier += d_loop;
    if (tier > 2)
        tier = 2;
    return tier;
}

// Roll a random item from the tier's table, filtered by difficulty.
// Only items with diff_tier <= current difficulty can drop.
int D_RollItemForDifficulty(int tier)
{
    extern int P_Random(void);
    int count = 0;
    int i, pick, idx;
    const diablo_itemdef_t *table;

    if (tier < 0 || tier >= NUM_TIERS)
        return -1;

    // Get the table for this tier
    if (tier == TIER_NORMAL)
        table = diablo_normal;
    else if (tier == TIER_MAGIC)
        table = diablo_magic;
    else if (tier == TIER_RARE)
        table = diablo_rare;
    else if (tier == TIER_SET)
        table = diablo_set;
    else
        table = diablo_unique;

    // Count valid items. Ammo-restoring mana potions never roll:
    // magazines replenish on their own, so a mana drop is rerolled
    // into a random item by simply leaving it out of the pool.
    for (i = 0; i < diablo_tiercounts[tier]; i++)
        if (table[i].diff_tier <= D_DifficultyTier()
            && !(table[i].consumable && table[i].usekind == USE_MANA))
            count++;

    if (count == 0)
        return -1;

    // Pick one
    pick = P_Random() % count;
    for (i = 0, idx = 0; i < diablo_tiercounts[tier]; i++)
    {
        if (table[i].diff_tier <= D_DifficultyTier()
            && !(table[i].consumable && table[i].usekind == USE_MANA))
        {
            if (idx == pick)
                return i;
            idx++;
        }
    }
    return -1;
}

// Phase 4: dexterity -> dodge and crit; energy -> potion power;
// elemental resistances mapped from the damage source.

// Dodge chance with diminishing returns: 100 dex = 50%, capped at 40%.
int D_DodgeChance(struct player_s *pl)
{
    int dex = D_Stat(pl, DSTAT_DEX);
    int chance;

    if (dex <= 0)
        return 0;
    chance = dex * 100 / (dex + 100);
    if (chance > 40)
        chance = 40;
    return chance;
}

boolean D_DodgeRoll(struct player_s *pl)
{
    int chance = D_DodgeChance(pl);
    if (chance <= 0)
        return false;
    return (P_Random() % 100) < chance;
}

// Crit is gear-inherent (Diablo-style): weapons and items grant crit
// chance and bonus crit damage. Base crits deal double damage (x2);
// each point of crit damage adds 1% on top of that.
int D_CritChance(struct player_s *pl)
{
    int chance = D_Stat(pl, DSTAT_CRIT_CHANCE);
    if (chance > 50)
        chance = 50;
    return chance;
}

int D_CritRoll(struct player_s *pl, int damage)
{
    int chance, mult;

    if (damage <= 0)
        return damage;
    chance = D_CritChance(pl);
    if (chance <= 0)
        return damage;
    if ((P_Random() % 100) < chance)
    {
        mult = 200 + D_Stat(pl, DSTAT_CRIT_DMG);
        return damage * mult / 100;
    }
    return damage;
}

// Energy: +2% potion effectiveness per point.
int D_PotionBonus(struct player_s *pl)
{
    return D_Stat(pl, DSTAT_ENE) * 2;
}

// Map a damage inflictor to an elemental resistance stat.
// Fire: explosions and flame projectiles.  Cold: the "frost" demons'
//   projectiles (revenant, mancubus, cacodemon, baron).  Lightning:
//   plasma and BFG.  Poison: environmental/slime damage (NULL inflictor).
// Returns a DSTAT_* resistance, or -1 for physical (bullets, melee).
static int D_DamageElement(mobj_t *inflictor)
{
    mobjtype_t type;

    if (!inflictor)
        return DSTAT_PRES;  // slime, radiation, other environmental

    type = inflictor->type;
    switch (type)
    {
      case MT_BARREL:
      case MT_ROCKET:
      case MT_TROOPSHOT:   // imp fireball
      case MT_FIRE:        // archvile flames
      case MT_SPAWNFIRE:
        return DSTAT_FRES;

      case MT_HEADSHOT:    // cacodemon
      case MT_BRUISERSHOT: // baron/hellknight
      case MT_TRACER:      // revenant
      case MT_FATSHOT:     // mancubus
        return DSTAT_CRES;

      case MT_PLASMA:
      case MT_BFG:
      case MT_ARACHPLAZ:   // spiderdemon
        return DSTAT_LRES;

      default:
        return -1;  // physical: bullets, melee, crushers
    }
}

int D_ResistReduce(struct player_s *pl, struct mobj_s *inflictor, int damage)
{
    int elem, resist;

    if (damage <= 0)
        return damage;
    elem = D_DamageElement((mobj_t *)inflictor);
    if (elem < 0)
        return damage;
    resist = D_Stat(pl, elem);
    if (resist <= 0)
        return damage;
    if (resist > 75)
        resist = 75;
    return damage - damage * resist / 100;
}

