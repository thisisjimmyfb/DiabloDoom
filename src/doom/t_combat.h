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
    int dmg_min;        // base damage range (STR/AP scale on top)
    int dmg_max;
    int tp_cost;        // TP per attack (before AS reduction)
    int cooldown;       // rounds of cooldown after firing (0 = none)
    int splash_radius;  // splash radius in map units (0 = none)
    int splash_pct;     // splash damage as % of primary (0 = none)
    int pellets;        // separate hit rolls (shotgun/chaingun)
    boolean ap_scaling; // plasma: damage scales with AP
    int mana_cost;      // mana (ammo[am_clip]) spent per attack, 0 = none
} t_kitdef_t;

const t_kitdef_t *T_KitForWeapon(weapontype_t w);
int T_KitCooldown(weapontype_t w);          // current cooldown rounds left
void T_KitSetCooldown(weapontype_t w, int rounds);
void T_KitTickCooldowns(void);             // call on round start
boolean T_KitReady(weapontype_t w);         // cooldown == 0
boolean T_HasManaForKit(weapontype_t w);    // consoleplayer can pay mana_cost

#endif
