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
#include "m_fixed.h"

// Forward declarations to avoid circular includes.
struct player_s;
struct mobj_s;

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
// Phase 7: ESLOT_FOCUS is the single shared passive weapon-buff slot
// (renamed from ESLOT_AMMO when ammo became the mana pool). Focus items
// are non-consumable; they passively buff the equipped weapon
// (e.g. armor-piercing rounds, incendiary shells). Exactly one focus slot.
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
    ESLOT_FOCUS,
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
    // Phase 7 tactical affixes (turn-based).
    DSTAT_AD_PCT,     // percent; multiplies attack damage
    DSTAT_AP_PCT,     // percent; multiplies ability power
    DSTAT_HASTE,      // percent; reduces cooldowns (turn-based)
    // Crit is gear-inherent (Diablo-style): weapons/items grant it.
    DSTAT_CRIT_CHANCE, // percent; chance to crit
    DSTAT_CRIT_DMG,    // percent; bonus crit damage (x2 = 200 base)
    DSTAT_PLASMA_CAP,  // bonus to plasma rifle max ammo (base 125)
    DSTAT_AD_FLAT,     // flat bonus damage (added before AD%)
    DSTAT_AP_FLAT,     // flat bonus ability power (added before AP%)
    DSTAT_CD_FLAT,     // flat cooldown reduction in rounds
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

// Phase 2 backpack grid dimensions (cells).  10x4 = 40 cells.
#define D_BP_GRID_W  10
#define D_BP_GRID_H  4

// Packed item id: (tier << 8) | index.  D_NOITEM means "empty".
#define D_MAKEITEM(tier, idx)  (((tier) << 8) | (idx))
#define D_ITEMTIER(id)         (((id) >> 8) & 0xff)
#define D_ITEMIDX(id)          ((id) & 0xff)

// Mechanic affix flags (rare guns grant mechanics, not just numbers).
// Stored on diablo_itemdef_t.mech; hand-authored per item.
#define MECH_PHOENIX    (1 << 0)   // kills vent 50 ammo (plasma)
#define MECH_OVERCLOCK  (1 << 1)   // +25 ammo dissipation/round (plasma)
#define MECH_CALDERA    (1 << 2)   // max ammo -> fire nova, vent to 0 (plasma)
#define MECH_VOLTAIC    (1 << 3)   // explosions chain 50% to nearby foe (rocket)
#define MECH_BANDOLIER  (1 << 4)   // +1 max charge (rocket)
#define MECH_SIEGE      (1 << 5)   // direct hits knock foes back 1 tile (rocket)
#define MECH_HUNGRY     (1 << 6)   // kills -1 round cooldown (BFG)
#define MECH_HORIZON    (1 << 7)   // blast drags foes 1 tile inward (BFG)
#define MECH_BREACHER   (1 << 8)   // kills refund reload cooldown (SSG)
#define MECH_BULL       (1 << 9)   // firing shoves you 1 tile back (SSG)
#define MECH_SPLITTING  (1 << 10)  // +2 pellets (shotgun/chaingun)
#define MECH_REAPER     (1 << 11)  // kills refund 2 TP (any)
#define MECH_LUCKY      (1 << 12)  // every 3rd pistol shot crits (pistol)
#define MECH_GLACIER    (1 << 13)  // hits slow foes (STRETCH: enemy-phase hook)

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
    // Phase 7 tactical affixes (turn-based).
    int ad_pct;      // percent; multiplies attack damage
    int ap_pct;      // percent; multiplies ability power
    int haste;       // percent; reduces cooldowns
    // Crit is gear-inherent (Diablo-style).
    int crit_chance; // percent; chance to crit
    int crit_dmg;    // percent; bonus crit damage (base x2 = 200)
    int plasma_cap;  // bonus to plasma rifle max ammo (base 125)
    int ad_flat;     // flat bonus damage (added before AD%)
    int ap_flat;     // flat bonus ability power (added before AP%)
    int cd_flat;     // flat cooldown reduction in rounds
    int diff_tier;   // min difficulty tier to drop (0=any, 1=Nightmare+, 2=Hell+)
    int doomweapon;  // weapontype_t for ESLOT_WEAPON guns, -1 otherwise.
                     // Equipping syncs the Doom readyweapon so the
                     // first-person sprite and turn-mode kit follow the gun.
    int mech;        // MECH_* bitmask: rare affixes grant mechanics.
} diablo_itemdef_t;

// Item table access.
const diablo_itemdef_t *D_GetItemDef(int tier, int idx);
int D_TierCount(int tier);
boolean D_ValidItem(int tier, int idx);
// Display name for an item: weapon items whose table name is built
// around the canonical base name ("Vampiric Chainsaw") inherit the
// WAD's own name for that weapon ("Vampiric Ripsaw" under Freedoom).
// Everything else displays as its table name.
void D_ItemDisplayName(const diablo_itemdef_t *def, char *buf, int bufsize);
// Icon index (d_diablo_icons.h) for the given item.
int D_GetItemIconIdx(int tier, int idx);
// WAD pickup-sprite lump name for a gun item ("SHOTA0" etc.), or
// NULL when the item is not a gun or its weapon has no pickup
// sprite (pistol, fists).  Callers check the lump exists in the
// loaded WAD before using it (the SSG sprite is Doom II only).
const char *D_ItemWadSpriteName(const diablo_itemdef_t *def);

// Player equipment state.
void D_ResetPlayer(struct player_s *player);
void D_RecalcStats(struct player_s *player);
void D_SyncDoomWeapon(struct player_s *player); // gun -> readyweapon/sprite
int D_EquippedWeaponMech(struct player_s *player); // MECH_* of equipped gun
int D_ActiveMechs(struct player_s *player); // OR of MECH_* from all equipped
const char *D_MechDesc(int mech_bit); // short display text for one MECH_* bit
int D_Stat(struct player_s *player, int stat);
int D_MaxHealth(struct player_s *player);

// Backpack / equip.  Returns false when the backpack is full.
boolean D_BackpackAdd(struct player_s *player, int tier, int idx);
// Add with an explicit starting durability (corpse recovery keeps the
// dead piece's wear); D_BackpackAdd is the fresh-item wrapper (full).
boolean D_BackpackAddDur(struct player_s *player, int tier, int idx, int dur);
// True when no item of any size could be added right now.
boolean D_BackpackFull(struct player_s *player);
// Packed item id (D_MAKEITEM) of the plain normal-tier item for a
// Doom weapon (wp_*), or D_NOITEM when the weapon has no base item.
// Fallback for weapon pickups when the rarity roll comes up empty.
int D_BaseItemForWeapon(int doomweapon);
// Roll a random weapon item of one tier for a Doom weapon
// (difficulty-gated). D_NOITEM when that tier has no such weapon.
int D_RollWeaponItem(int tier, int doomweapon);
void D_EquipRecent(struct player_s *player);   // E key (Phase 1)
void D_UnequipAll(struct player_s *player);    // Q key (Phase 1)
void D_BackpackFullMsg(struct player_s *player);

// Phase 2 backpack grid.  bpi is an index into diablo_backpack[].
boolean D_GridFindSpace(struct player_s *player, int w, int h,
                        int *gx, int *gy);
// Re-place loaded backpack items whose stored position no longer
// fits the current def footprints (load-time layout repair).
void D_RepairBackpackLayout(struct player_s *player);
// Remove backpack entry bpi (also clears its grid cell).  Public for UI.
void D_BackpackRemoveAt(struct player_s *player, int bpi);
// Use the consumable at backpack index bpi.  Public for UI.
void D_UseBackpackItem(struct player_s *player, int bpi);
// True if the item def can go in the given equipment slot.
boolean D_SlotFits(int slot, const diablo_itemdef_t *def);
boolean D_IsTwoHandedWeapon(int doomweapon);

// Combat stat hooks.
int D_WeaponBonus(struct player_s *player);           // bonus damage
// Diablo 2-style defense (2026-10-21): DSTAT_ARMOR is a Defense
// rating, not a reduction. A monster's chance to hit is
// 200 * AR / (AR + Defense), clamped to [5, 95]; AR derives from the
// attacker's spawn health. A landed hit deals full damage.
int D_EnemyAttackRating(struct mobj_s *source);
int D_DefenseChanceToHit(struct player_s *player, struct mobj_s *source);
boolean D_DefenseHitRoll(struct player_s *player, struct mobj_s *source); // true = hit lands
// Durability (helm/armor/boots/gloves/belt pieces only).
boolean D_IsDurabilityItem(const diablo_itemdef_t *def);
int D_ItemMaxDurability(const diablo_itemdef_t *def); // 0 = no durability
void D_WearRandomPiece(struct player_s *player);     // one landed hit of wear
int D_EquippedDurabilitySum(struct player_s *player); // HUD armor readout: total remaining durability of equipped defense pieces
void D_RepairAllEquipped(struct player_s *player);   // vanilla armor pickup
int D_RepairCost(int id, int cur_dur);               // essence to full; 0 = n/a or full
void D_LifeSteal(struct player_s *player, int damage);
int D_MoveSpeed(struct player_s *player);             // percent
// The ONE movespeed scaling for movement: every code path that moves
// the player (keyboard thrust, turn steps, GOTO, WALK) determines its
// displacement through D_ScaleDisplacement, and step durations through
// D_ScaleMoveTics (the same factor, inverted).
fixed_t D_ScaleDisplacement(struct player_s *player, fixed_t base);
int D_ScaleMoveTics(struct player_s *player, int base_tics);

// NG+ loop: beating the game loops back to MAP01 with stronger enemies.
// d_loop = 0 is the first playthrough, 1 = NG+, 2 = NG++, etc.
extern int d_loop;
int D_LoopHpMult(void);    // enemy HP multiplier in percent (100 = normal)
int D_LoopDmgMult(void);   // enemy damage multiplier in percent
int D_LoopMagicFind(void); // bonus magic find % from loop
void D_BeatGame(void);     // called when the final map is cleared

// Affix difficulty tiers: 0=Normal, 1=Nightmare, 2=Hell.
int D_DifficultyTier(void);
int D_RollItemForDifficulty(int tier); // filtered random item by difficulty

// Phase 4: dexterity/energy/resistance hooks.
boolean D_DodgeRoll(struct player_s *player);   // true = avoided the hit
int D_DodgeChance(struct player_s *player);     // percent, for UI
int D_CritRoll(struct player_s *player, int damage);  // gear crit damage
int D_CritChance(struct player_s *player);      // percent, gear-derived, for UI
// Reduce damage by the resistance matching the inflictor's element.
// inflictor may be NULL (environmental/slime -> poison).
int D_ResistReduce(struct player_s *player, struct mobj_s *inflictor,
                   int damage);
// Energy bonus to potion effectiveness, in percent.
int D_PotionBonus(struct player_s *player);

#endif
