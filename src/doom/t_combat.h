// t_combat.h — turn-based combat stats and resolution (Phase 4).
//
// Derives AD/AP/AS/Haste/crit/Armor/MR from the four Diablo attributes
// (Strength, Dexterity, Vitality, Energy) plus equipment. Hit chance uses
// range bands and a simple wall-proximity cover check. Damage rolls use a
// dedicated fixed-seed RNG so previews and resolutions agree and tests
// are deterministic.
//
// The preview (T_HitChance, T_DamageRange) and the resolution
// (T_ResolveAttack) call the same formula functions: displayed values
// always match resolved values.

#ifndef T_COMBAT_H
#define T_COMBAT_H

#include "doomdef.h"
#include "d_player.h"
#include "p_mobj.h"

// Derived turn-combat stats for one attacker.
typedef struct
{
    int ad_min;       // attack damage range, before crit/mitigation
    int ad_max;
    int ap;           // ability power (Phase 5 abilities)
    int attack_tp;    // TP cost of ATTACK, from attack speed
    int crit_chance;  // percent
    int crit_mult;    // percent, 150 = 1.5x
    int armor;        // physical mitigation
    int mr;           // magic mitigation
    int accuracy;     // base hit percent before range/cover
    int haste;        // cooldown reduction percent (Phase 5)
} t_combatstats_t;

// Fill out from the player's attributes + equipment.
void T_DeriveStats(player_t *player, t_combatstats_t *out);

// Gear percent multipliers, applied AFTER all flat damage (kit base +
// gun damage + attributes): AD: kit_base + gun + STR/2..STR, then AD%;
// AP: kit_base + AP/4..AP/2, then AP%.
int T_ApplyAdPct(player_t *player, int dmg);
int T_ApplyApPct(player_t *player, int ap);

// Hit percent for an attack, 5..95. Same value preview shows.
int T_HitChance(player_t *player, mobj_t *target, const t_combatstats_t *st);

// Multi-point cover: number of blocked traces (0-3). 3 = full blockage.
int T_CoverBlocked(player_t *player, mobj_t *target);

// Damage range preview (before crit, before target mitigation).
void T_DamageRange(player_t *player, mobj_t *target,
                   const t_combatstats_t *st, int *minhp, int *maxhp);

// Resolve an attack against a confirmed target. Returns true on hit.
// Applies damage via P_DamageMobj and records the outcome for tests.
boolean T_ResolveAttack(player_t *player, mobj_t *target);

// Last resolution outcome (for test assertions).
extern int t_last_hit;      // 1 hit, 0 miss, -1 none yet
extern int t_last_damage;   // damage dealt (0 on miss)
extern int t_last_crit;     // 1 if the hit crit

// Fixed-seed combat RNG.
void T_SetCombatSeed(unsigned int seed);
unsigned int T_GetCombatSeed(void);

// ---------------------------------------------------------------------------
// Weapon kits (Phase 5). Six distinct kits; the resolver uses the player's
// readyweapon. Rockets/BFG are enemy-targeted: pick an enemy, splash is
// centered on them and previewed. No ground targeting, ever.

typedef struct
{
    const char *name;   // display name
    int dmg_min;        // base damage range (AD/AP scale on top)
    int dmg_max;
    int tp_cost;        // TP per attack (before AS reduction)
    int cooldown;       // rounds of cooldown after firing (0 = none)
    int splash_radius;  // splash radius in map units (0 = none)
    int splash_pct;     // splash damage as % of primary (0 = none)
    int pellets;        // separate hit rolls (shotgun/chaingun)
    boolean ap_weapon;  // true: AP scaling (ENE); false: AD (STR + gun dmg)
    int mana_cost;      // mana (ammo[am_clip]) spent per attack, 0 = none
    int heat_per_shot;  // heat gained per shot (plasma); 0 = no heat system
    int heat_vent;      // heat dissipated per round
    int max_charges;    // charge capacity (rocket); 0 = no charge system
} t_kitdef_t;

const t_kitdef_t *T_KitForWeapon(weapontype_t w);
int T_KitFireSound(weapontype_t w);     // per-gun firing report sfx
int T_KitCooldown(weapontype_t w);          // current cooldown rounds left
void T_KitSetCooldown(weapontype_t w, int rounds);
void T_KitResetCadence(void);           // new game: zero cd/heat, full
                                        // charges, zero Lucky counters
void T_KitSaveCadence(int *cool, int *heat, int *charges, int *shots);
void T_KitLoadCadence(const int *cool, const int *heat,
                      const int *charges, const int *shots);
int T_KitHeat(weapontype_t w);              // current heat 0-100
int T_KitCharges(weapontype_t w);           // current charges
int T_KitMaxCharges(weapontype_t w);        // charge capacity (+ affixes)
int T_KitShots(weapontype_t w);             // shots fired (Lucky rhythm)
void T_KitTickCooldowns(void);             // call on round start
boolean T_KitCanFire(weapontype_t w);       // all gates pass
const char *T_KitDenyReason(weapontype_t w);// "ON COOLDOWN"/"NO CHARGES"/
                                           // "OVERHEATED"/"NO MANA"/NULL
boolean T_HasManaForKit(weapontype_t w);    // consoleplayer can pay mana_cost
boolean T_LastKill(void);                   // did the last attack kill?

#endif
