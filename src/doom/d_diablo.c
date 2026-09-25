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
//   armor         : damage -= damage * armor / (armor + 50), i.e. armor 50
//                   halves hits; capped at 75% reduction
//   life steal    : heal damage_dealt * lifesteal% (vs monsters), capped
//                   at max health
//   vitality      : max health = deh_max_health + 5 * vitality
//   magic find    : rarity roll shifted down by magicfind / 2 (better loot)
//   move speed    : player thrust scaled by (100 + movespeed) / 100
//   resists/dex/energy: aggregated for Phase 2 (vanilla Doom has no
//                   elemental damage types to hook cleanly).
//
// Part of the DiabloDoom mod (GPL-2.0-or-later, like Chocolate Doom).

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "d_diablo.h"
#include "d_player.h"
#include "deh_misc.h"
#include "doomdef.h"
#include "doomstat.h"
#include "m_random.h"
#include "p_local.h"

// Shorthand for the long positional initializers below.
// Fields: name, tier, slot, grid_w, grid_h, consumable, usekind, heal,
//         dmg_min, dmg_max, armor,
//         str, dex, vit, ene,
//         fres, cres, lres, pres,
//         lifesteal, magicfind, movespeed,
//         ad_pct, ap_pct, haste, crit_chance, crit_dmg
#define IT(n_, t_, s_, w_, h_, c_, u_, he_, dm_, dx_, ar_, \
           st_, de_, vi_, en_, fr_, cr_, lr_, pr_, ls_, mf_, ms_, \
           ad_, ap_, ha_, cc_, cd_) \
    { n_, t_, s_, w_, h_, c_, u_, he_, dm_, dx_, ar_, \
      st_, de_, vi_, en_, fr_, cr_, lr_, pr_, ls_, mf_, ms_, \
      ad_, ap_, ha_, cc_, cd_ }

static const diablo_itemdef_t diablo_normal[] =
{
    IT("Short Sword",       TIER_NORMAL, ESLOT_WEAPON, 1,3, 0,0,0,   2, 5, 0,
       0,0,0,0,   0,0,0,0,   0,0,0,0,0,0,5,0),
    IT("Leather Armor",     TIER_NORMAL, ESLOT_ARMOR,  2,3, 0,0,0,   0, 0, 5,
       0,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0),
    IT("Buckler",           TIER_NORMAL, ESLOT_SHIELD, 2,2, 0,0,0,   0, 0, 3,
       0,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0),
    IT("Cap",               TIER_NORMAL, ESLOT_HELM,   2,2, 0,0,0,   0, 0, 2,
       0,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0),
    IT("Sash",              TIER_NORMAL, ESLOT_BELT,   2,1, 0,0,0,   0, 0, 2,
       0,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0),
    IT("Healing Potion",    TIER_NORMAL, ESLOT_NONE,   1,1, 1,USE_HEAL,40, 0,0,0,
       0,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0),
    IT("Mana Potion",       TIER_NORMAL, ESLOT_NONE,   1,1, 1,USE_MANA,50, 0,0,0,
       0,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0),
    IT("Rancid Gas Potion", TIER_NORMAL, ESLOT_NONE,   1,1, 1,USE_BLAST,50, 0,0,0,
       0,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0),
    IT("Leather Boots",     TIER_NORMAL, ESLOT_BOOTS,  2,2, 0,0,0,   0, 0, 3,
       0,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0),
    IT("Leather Gloves",    TIER_NORMAL, ESLOT_GLOVES, 2,2, 0,0,0,   0, 0, 2,
       0,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0),
};

static const diablo_itemdef_t diablo_magic[] =
{
    IT("Cruel War Axe",       TIER_MAGIC, ESLOT_WEAPON, 2,3, 0,0,0,  6,14, 0,
       3,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,25),
    IT("King's Long Sword",   TIER_MAGIC, ESLOT_WEAPON, 1,3, 0,0,0,  5,10, 0,
       2,0,2,0,   0,0,0,0,   0,0,0,0,0,0,8,0),
    IT("Vampiric Bone Shield", TIER_MAGIC, ESLOT_SHIELD, 2,2, 0,0,0,  0, 0, 8,
       0,0,0,0,   0,0,0,0,   5,0,0,0,0,0,0,0),
    IT("Prismatic Amulet",    TIER_MAGIC, ESLOT_AMULET, 1,1, 0,0,0,  0, 0, 0,
       0,0,0,0,  10,10,10,10, 0,0,0,0,0,0,0,0),
    IT("Lizard's Ring",       TIER_MAGIC, ESLOT_RING1,  1,1, 0,0,0,  0, 0, 0,
       0,0,0,8,   0,0,0,0,   0,5,0,0,0,0,0,0),
    IT("Soldier's Chain Mail",TIER_MAGIC, ESLOT_ARMOR,  2,3, 0,0,0,  0, 0,12,
       3,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0),
    IT("Traveler's Treads", TIER_MAGIC, ESLOT_BOOTS,  2,2, 0,0,0,  0, 0, 8,
       0,2,0,0,   0,0,0,0,   0,0,10,0,0,0,0,0),
    IT("Assault Gloves",    TIER_MAGIC, ESLOT_GLOVES, 2,2, 0,0,0,  0, 0, 6,
       3,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0),
    IT("Piercing Rounds",   TIER_MAGIC, ESLOT_FOCUS,   1,1, 0,0,0,   0, 0, 0,
       0,0,0,0,   0,0,0,0,   0,0,0,  15,0,0,0,0),
    IT("Incendiary Shells", TIER_MAGIC, ESLOT_FOCUS,   1,1, 0,0,0,   0, 0, 0,
       0,0,0,0,   0,0,0,0,   0,0,0,  0,10,0,0,0),
    IT("Swift Cartridges",  TIER_MAGIC, ESLOT_FOCUS,   1,1, 0,0,0,   0, 0, 0,
       0,0,0,0,   0,0,0,0,   0,0,0,  0,0,20,0,0),
    IT("Spotter Rounds",    TIER_MAGIC, ESLOT_FOCUS,   1,1, 0,0,0,   0, 0, 0,
       0,0,0,0,   0,0,0,0,   0,0,0,  0,0,0,10,0),
};

static const diablo_itemdef_t diablo_rare[] =
{
    IT("Doombringer",       TIER_RARE, ESLOT_WEAPON, 2,3, 0,0,0,  12,24, 0,
       5,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,40),
    IT("Stormlash",         TIER_RARE, ESLOT_WEAPON, 1,3, 0,0,0,  10,20, 0,
       0,0,0,0,   0,0,15,0,  0,0,0,0,0,0,10,0),
    IT("Soulrender",        TIER_RARE, ESLOT_WEAPON, 2,3, 0,0,0,  11,22, 0,
       0,0,0,0,   0,0,0,0,   3,0,0,0,0,0,0,0),
    IT("Demonhorn Edge",    TIER_RARE, ESLOT_WEAPON, 1,3, 0,0,0,   9,18, 0,
       0,5,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0),
    IT("Nightmare Coil",    TIER_RARE, ESLOT_RING1,  1,1, 0,0,0,   0, 0, 0,
       5,0,0,0,  15,0,0,0,   0,0,0,0,0,0,5,0),
    IT("Grimward",          TIER_RARE, ESLOT_SHIELD, 2,2, 0,0,0,   0, 0,15,
       0,0,0,0,   0,0,0,15,  0,0,0,0,0,0,0,0),
    IT("Bloodletter",       TIER_RARE, ESLOT_WEAPON, 1,3, 0,0,0,  10,19, 0,
       0,0,0,0,   0,0,0,0,   4,0,0,0,0,0,12,0),
    IT("Fleshrender",       TIER_RARE, ESLOT_WEAPON, 2,3, 0,0,0,  13,23, 0,
       0,0,4,0,   0,0,0,0,   0,0,0,0,0,0,0,30),
    IT("Stormwalkers",      TIER_RARE, ESLOT_BOOTS,  2,2, 0,0,0,   0, 0,14,
       0,4,0,0,   0,0,15,0,  0,0,15,0,0,0,0,0),
    IT("Doom Grasp",        TIER_RARE, ESLOT_GLOVES, 2,2, 0,0,0,   0, 0,12,
       5,0,0,0,   0,0,0,0,   3,0,0,0,0,0,0,0),
};

static const diablo_itemdef_t diablo_set[] =
{
    IT("Tal Rasha's Horadric Crest", TIER_SET, ESLOT_HELM, 2,2, 0,0,0, 0,0,15,
       0,0,0,8,   0,0,0,0,   0,10,0,0,0,0,0,0),
    IT("Immortal King's Soul Cage", TIER_SET, ESLOT_ARMOR, 2,3, 0,0,0, 0,0,25,
       8,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0),
    IT("Trang-Oul's Guise",          TIER_SET, ESLOT_HELM, 2,2, 0,0,0, 0,0,14,
       0,0,0,0,   0,0,0,20,  0,0,0,0,0,0,0,0),
    IT("M'avina's True Sight",       TIER_SET, ESLOT_HELM, 2,2, 0,0,0, 0,0,14,
       0,8,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0),
    IT("Natalya's Shadow",           TIER_SET, ESLOT_ARMOR,2,3, 0,0,0, 0,0,22,
       0,6,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0),
    IT("Griswold's Valor",           TIER_SET, ESLOT_HELM, 2,2, 0,0,0, 0,0,16,
       6,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,0),
    IT("Berserker's Hatchet",        TIER_SET, ESLOT_WEAPON,1,3,0,0,0,15,28,0,
       6,0,0,0,   0,0,0,0,   0,0,0,0,0,0,0,35),
    IT("Sazabi's Cobalt Redeemer",   TIER_SET, ESLOT_WEAPON,1,3,0,0,0,14,26,0,
       0,0,0,0,   0,20,0,0,  0,0,0,0,0,0,0,0),
    IT("Immortal King's Pillar",    TIER_SET, ESLOT_BOOTS, 2,2,0,0,0, 0,0,18,
       5,0,0,0,   20,0,0,0,  0,0,20,0,0,0,0,0),
    IT("M'avina's Icy Clutch",      TIER_SET, ESLOT_GLOVES,2,2,0,0,0, 0,0,14,
       0,6,0,0,   0,20,0,0,  0,0,0,0,0,0,0,0),
};

static const diablo_itemdef_t diablo_unique[] =
{
    IT("Stone of Jordan",       TIER_UNIQUE, ESLOT_RING1, 1,1, 0,0,0,  0, 0, 0,
       0,0,0,15,   0,0,0,0,   5,15,0,0,0,0,0,0),
    IT("Harlequin Crest",       TIER_UNIQUE, ESLOT_HELM,  2,2, 0,0,0,  0, 0,18,
       0,0,10,0,   0,0,0,0,   0,25,0,0,0,0,0,0),
    IT("The Grandfather",       TIER_UNIQUE, ESLOT_WEAPON,2,3, 0,0,0, 25,50, 0,
       10,0,0,0,   0,0,0,0,   0,0,0,0,0,0,15,50),
    IT("Windforce",             TIER_UNIQUE, ESLOT_WEAPON,2,3, 0,0,0, 22,45, 0,
       0,10,0,0,   0,0,0,0,   0,0,0,0,0,0,20,0),
    IT("Arkaine's Valor",       TIER_UNIQUE, ESLOT_ARMOR, 2,3, 0,0,0,  0, 0,30,
       0,0,12,0,   0,0,0,0,   0,0,0,0,0,0,0,0),
    IT("Mara's Kaleidoscope",   TIER_UNIQUE, ESLOT_AMULET,1,1, 0,0,0,  0, 0, 0,
       8,0,0,0,  20,20,20,20, 0,0,0,0,0,0,0,0),
    IT("Bul-Kathos' Wedding Band",TIER_UNIQUE,ESLOT_RING1,1,1, 0,0,0,  0, 0, 0,
       0,0,8,0,    0,0,0,0,   8,0,0,0,0,0,10,25),
    IT("Titan's Revenge",       TIER_UNIQUE, ESLOT_WEAPON,1,3, 0,0,0, 20,40, 0,
       8,5,0,0,    0,0,0,0,   0,0,0,0,0,0,15,0),
    IT("Lidless Wall",          TIER_UNIQUE, ESLOT_SHIELD,2,2, 0,0,0,  0, 0,20,
       0,0,0,10,   0,0,0,0,   0,0,0,0,0,0,0,0),
    IT("Skin of the Vipermagi", TIER_UNIQUE, ESLOT_ARMOR, 2,3, 0,0,0,  0, 0,24,
       0,0,0,0,  25,0,25,0,   0,0,0,0,0,0,0,0),
    IT("Thundergod's Vigor",    TIER_UNIQUE, ESLOT_BELT,  2,1, 0,0,0,  0, 0,10,
       5,0,8,0,    0,0,25,0,  0,0,0,0,0,0,0,0),
    IT("Raven Frost",           TIER_UNIQUE, ESLOT_RING1, 1,1, 0,0,0,  0, 0, 0,
       0,8,0,0,    0,25,0,0,  0,0,0,0,0,0,0,0),
    IT("War Traveler",          TIER_UNIQUE, ESLOT_BOOTS, 2,2, 0,0,0,  0, 0,22,
       5,0,8,0,    0,0,0,0,   0,15,25,0,0,0,0,0),
    IT("Frostburn",             TIER_UNIQUE, ESLOT_GLOVES,2,2, 0,0,0,  0, 0,16,
       5,5,0,0,    0,25,0,0,  4,0,0,0,0,0,0,0),
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
// Every item has its own unique icon; rarity is shown via the UI border.
int D_GetItemIconIdx(int tier, int idx)
{
    // Icon indices from d_diablo_icons.h (52 unique, one per item).
    // Normal (10): Short Sword, Leather Armor, Buckler, Cap, Sash,
    //   Healing/Mana/Gas Potions, Leather Boots, Leather Gloves
    static const int normal_icons[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
    // Magic (8): Cruel War Axe, King's Long Sword, Vampiric Bone Shield,
    //   Prismatic Amulet, Lizard's Ring, Soldier's Chain Mail,
    //   Traveler's Treads, Assault Gloves
    static const int magic_icons[]  = { 10, 11, 12, 13, 14, 15, 16, 17 };
    // Rare (10): Doombringer, Stormlash, Soulrender, Demonhorn Edge,
    //   Nightmare Coil, Grimward, Bloodletter, Fleshrender,
    //   Stormwalkers, Doom Grasp
    static const int rare_icons[]   = { 18, 19, 20, 21, 22, 23, 24, 25, 26, 27 };
    // Set (10): Tal Rasha's Crest, IK Soul Cage, Trang-Oul's Guise,
    //   M'avina's Sight, Natalya's Shadow, Griswold's Valor,
    //   Berserker's Hatchet, Sazabi's Redeemer,
    //   IK Pillar, M'avina's Clutch
    static const int set_icons[]    = { 28, 29, 30, 31, 32, 33, 34, 35, 36, 37 };
    // Unique (14): Stone of Jordan, Harlequin Crest, The Grandfather,
    //   Windforce, Arkaine's Valor, Mara's Kaleidoscope, Bul-Kathos' Band,
    //   Titan's Revenge, Lidless Wall, Vipermagi, Thundergod's Vigor,
    //   Raven Frost, War Traveler, Frostburn
    static const int unique_icons[] = { 38, 39, 40, 41, 42, 43, 44, 45,
                                        46, 47, 48, 49, 50, 51 };

    if (!D_ValidItem(tier, idx))
        return 0;

    switch (tier)
    {
        case TIER_NORMAL: return normal_icons[idx];
        case TIER_MAGIC:  return magic_icons[idx];
        case TIER_RARE:   return rare_icons[idx];
        case TIER_SET:    return set_icons[idx];
        case TIER_UNIQUE: return unique_icons[idx];
        default: return 0;
    }
}

void D_ResetPlayer(struct player_s *pl)
{
    player_t *player = (player_t *)pl;
    int i;

    for (i = 0; i < NUM_ESLOTS; i++)
        player->diablo_equipped[i] = D_NOITEM;
    for (i = 0; i < D_BACKPACK_SIZE; i++)
    {
        player->diablo_backpack[i] = D_NOITEM;
        player->diablo_bp_gx[i] = -1;
        player->diablo_bp_gy[i] = -1;
    }
    player->diablo_bp_count = 0;
    player->diablo_recent = -1;
    for (i = 0; i < NUM_DSTATS; i++)
        player->diablo_stats[i] = 0;
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
    }
}

int D_Stat(struct player_s *pl, int stat)
{
    player_t *player = (player_t *)pl;
    if (stat < 0 || stat >= NUM_DSTATS)
        return 0;
    return player->diablo_stats[stat];
}

int D_MaxHealth(struct player_s *pl)
{
    // vitality: +5 max HP per point, on top of the dehacked max.
    return deh_max_health + D_Stat(pl, DSTAT_VIT) * 5;
}

// Phase 2 backpack grid helpers.  Each backpack entry tracks the
// top-left cell of its footprint (diablo_bp_gx/gy), or -1/-1.

// True if the footprint of backpack entry bpi fits at (gx,gy) with no
// overlap.  ignore_bpi (>=0) is skipped in the overlap test (used when
// moving an item within the grid).
static boolean D_GridCanPlaceAt(player_t *player, int bpi,
                                int gx, int gy, int ignore_bpi)
{
    const diablo_itemdef_t *def;
    int id, w, h, i, x, y, ox, oy, ow, oh, oid;
    const diablo_itemdef_t *odef;

    if (bpi < 0 || bpi >= player->diablo_bp_count)
        return false;
    id = player->diablo_backpack[bpi];
    def = D_GetItemDef(D_ITEMTIER(id), D_ITEMIDX(id));
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
        if (i == bpi || i == ignore_bpi)
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
        // AABB overlap test.
        if (gx < ox + ow && gx + w > ox && gy < oy + oh && gy + h > oy)
            return false;
    }
    return true;
}

boolean D_GridCanPlace(struct player_s *pl, int bpi, int gx, int gy)
{
    return D_GridCanPlaceAt((player_t *)pl, bpi, gx, gy, -1);
}

boolean D_GridFindSpace(struct player_s *pl, int w, int h, int *gx, int *gy)
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

void D_GridPlace(struct player_s *pl, int bpi, int gx, int gy)
{
    player_t *player = (player_t *)pl;
    if (bpi < 0 || bpi >= player->diablo_bp_count)
        return;
    player->diablo_bp_gx[bpi] = gx;
    player->diablo_bp_gy[bpi] = gy;
}

void D_GridRemove(struct player_s *pl, int bpi)
{
    player_t *player = (player_t *)pl;
    if (bpi < 0 || bpi >= D_BACKPACK_SIZE)
        return;
    player->diablo_bp_gx[bpi] = -1;
    player->diablo_bp_gy[bpi] = -1;
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
    }
    player->diablo_bp_count--;
    player->diablo_backpack[player->diablo_bp_count] = D_NOITEM;
    player->diablo_bp_gx[player->diablo_bp_count] = -1;
    player->diablo_bp_gy[player->diablo_bp_count] = -1;
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

boolean D_BackpackAdd(struct player_s *pl, int tier, int idx)
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
    if (player->diablo_bp_count >= D_BACKPACK_SIZE)
        return false;

    // Phase 2: the item needs a free footprint on the backpack grid.
    if (!D_GridFindSpace(pl, def->grid_w, def->grid_h, &gx, &gy))
        return false;

    player->diablo_backpack[player->diablo_bp_count] = D_MAKEITEM(tier, idx);
    player->diablo_bp_gx[player->diablo_bp_count] = gx;
    player->diablo_bp_gy[player->diablo_bp_count] = gy;
    player->diablo_recent = player->diablo_bp_count;
    player->diablo_bp_count++;

    D_Msg(player, tier_msgs[tier], def->name);
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
        D_Msg(player, "You quaff the %s. (+%d HP)", def->name, amount);
        break;

      case USE_MANA:
        // Mana pool: ammo[am_clip] is the unified mana pool (100 max).
        player->ammo[am_clip] += amount;
        if (player->ammo[am_clip] > player->maxammo[am_clip])
            player->ammo[am_clip] = player->maxammo[am_clip];
        D_Msg(player, "You quaff the %s. (+%d mana)",
              def->name, amount);
        break;

      default: // USE_BLAST
        // A gas bomb at your feet: hurts everything nearby, you included.
        // (Poison resist does not protect you from your own bomb.)
        P_RadiusAttack(player->mo, player->mo, amount);
        D_Msg(player, "The %s bursts into toxic gas!", def->name);
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

// Equip an item id directly into a slot.  Ring items prefer the requested
// slot, falling back to the other ring slot when taken.  Returns the
// displaced item id, or D_NOITEM when the slot was empty.  The displaced
// item is stashed in the backpack (with grid placement); returns D_NOITEM
// and does nothing when there is no room for it.
int D_EquipToSlot(struct player_s *pl, int id, int slot)
{
    player_t *player = (player_t *)pl;
    const diablo_itemdef_t *def, *odef;
    int old;

    def = D_GetItemDef(D_ITEMTIER(id), D_ITEMIDX(id));
    if (!D_SlotFits(slot, def))
        return D_NOITEM;

    // Rings: use the requested slot unless taken, then the other one.
    if (def->slot == ESLOT_RING1 && player->diablo_equipped[slot] != D_NOITEM)
        slot = (slot == ESLOT_RING1) ? ESLOT_RING2 : ESLOT_RING1;

    old = player->diablo_equipped[slot];
    if (old != D_NOITEM)
    {
        int gx, gy;
        odef = D_GetItemDef(D_ITEMTIER(old), D_ITEMIDX(old));
        if (!odef)
            return D_NOITEM;
        // Stash the displaced item; needs backpack + grid room.
        if (player->diablo_bp_count >= D_BACKPACK_SIZE
         || !D_GridFindSpace(pl, odef->grid_w, odef->grid_h, &gx, &gy))
        {
            D_BackpackFullMsg(pl);
            return D_NOITEM;
        }
        player->diablo_backpack[player->diablo_bp_count] = old;
        player->diablo_bp_gx[player->diablo_bp_count] = gx;
        player->diablo_bp_gy[player->diablo_bp_count] = gy;
        player->diablo_bp_count++;
    }

    player->diablo_equipped[slot] = id;
    D_RecalcStats(pl);
    return old;
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
         || player->diablo_bp_count >= D_BACKPACK_SIZE
         || !D_GridFindSpace(pl, odef->grid_w, odef->grid_h, &gx, &gy))
        {
            // No room to stash the swapped-out item.
            D_BackpackFullMsg(pl);
            return;
        }
    }

    D_BackpackRemove(player, bpi);
    if (old != D_NOITEM)
    {
        // D_EquipToSlot stashes via D_BackpackAdd; replicate placement.
        const diablo_itemdef_t *odef;
        int gx, gy;
        odef = D_GetItemDef(D_ITEMTIER(old), D_ITEMIDX(old));
        D_GridFindSpace(pl, odef->grid_w, odef->grid_h, &gx, &gy);
        player->diablo_backpack[player->diablo_bp_count] = old;
        player->diablo_bp_gx[player->diablo_bp_count] = gx;
        player->diablo_bp_gy[player->diablo_bp_count] = gy;
        player->diablo_bp_count++;
    }
    player->diablo_equipped[slot] = id;
    player->diablo_recent = -1;
    D_RecalcStats(pl);

    D_Msg(player, "Equipped %s.", def->name);
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
        player->diablo_bp_count++;
        moved++;
    }

    if (moved)
    {
        D_RecalcStats(pl);
        player->diablo_recent = -1;
        D_Msg(player, "Unequipped all (%d item%s).", moved,
              moved == 1 ? "" : "s");
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

int D_ArmorReduce(struct player_s *pl, int damage)
{
    int armor = D_Stat(pl, DSTAT_ARMOR);
    int reduction;

    if (armor <= 0 || damage <= 0)
        return damage;

    // Diminishing returns: armor 50 halves incoming damage.
    reduction = damage * armor / (armor + 50);
    if (reduction > damage * 3 / 4)  // cap at 75%
        reduction = damage * 3 / 4;

    return damage - reduction;
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

int D_MagicFind(struct player_s *pl)
{
    return D_Stat(pl, DSTAT_MAGICFIND);
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
