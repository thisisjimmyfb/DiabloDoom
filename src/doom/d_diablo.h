//
// d_diablo.h — DiabloDoom Phase 1: equipment backend.
//
// Item definitions, equipment slots, and the player-facing API for the
// Diablo-style equipment system.  Phase 1 is backend only: items live in
// a flat backpack list and are equipped with keys (E/Q).  The inventory
// grid UI, drag-drop, and per-item artwork arrive in Phase 2/3.
//
// Part of the DiabloDoom mod (GPL-2.0-or-later, like Chocolate Doom).

#ifndef __D_DIABLO__
#define __D_DIABLO__

#include "doomtype.h"

// Item tiers.  Order must match MT_LOOT_NORMAL..MT_LOOT_UNIQUE.
typedef enum
{
    TIER_NORMAL,
    TIER_MAGIC,
    TIER_RARE,
    TIER_SET,
    TIER_UNIQUE,
    NUM_TIERS
} diablo_tier_t;

// Equipment slots.  Rings get two slots (RING1/RING2).
typedef enum
{
    ESLOT_HELM,
    ESLOT_ARMOR,
    ESLOT_WEAPON,
    ESLOT_SHIELD,
    ESLOT_RING1,
    ESLOT_RING2,
    ESLOT_AMULET,
    ESLOT_BOOTS,
    ESLOT_GLOVES,
    ESLOT_BELT,
    NUM_ESLOTS,
    ESLOT_NONE = -1  // consumables (potions) fit no slot
} diablo_eslot_t;

// Stats aggregated from equipped items.
typedef enum
{
    DSTAT_DMG_MIN,
    DSTAT_DMG_MAX,
    DSTAT_ARMOR,
    DSTAT_STR,
    DSTAT_DEX,
    DSTAT_VIT,
    DSTAT_ENE,
    DSTAT_FRES,
    DSTAT_CRES,
    DSTAT_LRES,
    DSTAT_PRES,
    DSTAT_LIFESTEAL,  // percent of damage dealt returned as HP
    DSTAT_MAGICFIND,  // percent; improves loot rarity rolls
    DSTAT_MOVESPEED,  // percent; increases movement speed
    NUM_DSTATS
} diablo_stat_t;

// Consumable use kinds (potions).
typedef enum
{
    USE_HEAL,   // restore HP
    USE_MANA,   // restore armor (mana ward)
    USE_BLAST,  // radius damage around the player (hurts everyone)
    NUM_USETYPES
} diablo_use_t;

#define D_BACKPACK_SIZE  40
#define D_NOITEM         (-1)

// Packed item id: (tier << 8) | index.  D_NOITEM means "empty".
#define D_MAKEITEM(tier, idx)  (((tier) << 8) | (idx))
#define D_ITEMTIER(id)         (((id) >> 8) & 0xff)
#define D_ITEMIDX(id)          ((id) & 0xff)

typedef struct
{
    const char *name;
    int tier;        // diablo_tier_t
    int slot;        // diablo_eslot_t, or ESLOT_NONE for consumables
    int grid_w;      // Phase 2 inventory footprint, in cells
    int grid_h;
    int consumable;  // 1 = usable (potion), 0 = equippable
    int usekind;     // diablo_use_t (consumables only)
    int heal;        // consumable effect amount (HP / armor / damage)
    int dmg_min;     // weapon damage range
    int dmg_max;
    int armor;       // damage reduction value
    int str;         // +1 damage per 4 (all player attacks)
    int dex;         // aggregated; block/dodge hook is a Phase 2 TODO
    int vit;         // +5 max HP per point
    int ene;         // aggregated; mana pool is a Phase 2 TODO
    int fres;        // resists, percent; no elemental damage types in
    int cres;        // vanilla Doom, so these aggregate for Phase 2
    int lres;        // (mapped: barrels->fire, plasma->lightning, ...)
    int pres;
    int lifesteal;   // percent of damage dealt returned as HP
    int magicfind;   // percent; shifts loot rarity rolls in your favor
    int movespeed;   // percent movement speed bonus
} diablo_itemdef_t;

// player_t is defined in d_player.h, which includes this header;
// use the incomplete struct type in prototypes.
struct player_s;

// Item table access.
const diablo_itemdef_t *D_GetItemDef(int tier, int idx);
int D_TierCount(int tier);
boolean D_ValidItem(int tier, int idx);

// Player equipment state.
void D_ResetPlayer(struct player_s *player);
void D_RecalcStats(struct player_s *player);
int D_Stat(struct player_s *player, int stat);
int D_MaxHealth(struct player_s *player);

// Backpack / equip.  Returns false when the backpack is full.
boolean D_BackpackAdd(struct player_s *player, int tier, int idx);
void D_EquipRecent(struct player_s *player);   // E key (Phase 1)
void D_UnequipAll(struct player_s *player);    // Q key (Phase 1)
void D_BackpackFullMsg(struct player_s *player);

// Combat stat hooks.
int D_WeaponBonus(struct player_s *player);           // bonus damage
int D_ArmorReduce(struct player_s *player, int damage);
void D_LifeSteal(struct player_s *player, int damage);
int D_MoveSpeed(struct player_s *player);             // percent
int D_MagicFind(struct player_s *player);             // percent

#endif
