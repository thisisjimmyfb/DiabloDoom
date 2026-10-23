// t_combat.h — turn-based combat stats and resolution (Phase 4).
//
// Derives AD/AP/AS/Haste/crit/Armor/MR from the four Diablo attributes
// (Strength, Dexterity, Vitality, Energy) plus equipment. Hit chance uses
// range bands and a simple wall-proximity cover check. Damage rolls use a
// dedicated fixed-seed RNG so previews and resolutions agree and tests
// are deterministic.
//
// Attacks resolve through the live simulation: T_ExecAttack fires
// the real Doom weapon (T_NativeFire: hitscan/projectiles), and on a
// hit T_DiabloDamageCallback computes the Diablo damage. The preview
// (T_HitChance, T_DamageRange) and the callback call the same formula
// functions: displayed values always match applied values.

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

// Kill credit + kill-triggered affixes. Called after P_DamageMobj confirms a kill.
void T_ResolveKill(player_t *player, mobj_t *victim, int dmg,
                   const char *kitname);

// Time-based cadence (called every tic from T_Ticker).
void T_TickCadence(void);

// Called when a weapon fires: charges, cooldown (with Haste/CDR).
// (Plasma's per-shot resource is spent through T_KitSpendAmmo like
// every other weapon -- there is no separate heat system.)
void T_KitOnFire(player_t *player);

// Last resolution outcome (for test assertions).
extern int t_last_hit;      // 1 hit, 0 miss, -1 none yet
extern int t_last_damage;   // damage dealt (0 on miss)
extern int t_last_crit;     // 1 if the hit crit
struct mobj_s;
extern struct mobj_s *t_last_hit_mobj; // mobj hit by last Diablo damage cb

// Diablo damage override for native weapon fire (Phase 8).
// When t_damage_override is true, P_DamageMobj replaces Doom's damage
// with t_damage_override_value for player-sourced hits. Used when the
// turn system triggers native A_Fire* functions: the visuals (muzzle
// flash, projectiles) are real, but the damage uses the Diablo formula.
// Set before calling A_Fire*, cleared after the attack resolves.
extern boolean t_damage_override;
extern int t_damage_override_value;

// Fixed-seed combat RNG.
void T_SetCombatSeed(unsigned int seed);
unsigned int T_GetCombatSeed(void);

// ---------------------------------------------------------------------------
// Weapon kits (Phase 5). Six distinct kits; the resolver uses the player's
// readyweapon. Rockets/BFG are enemy-targeted: pick an enemy, splash is
// centered on them and previewed. No ground targeting, ever.

// Ammo replenish model: what drives the trickle rate.
typedef enum
{
    REPLENISH_AS,  // attack-speed driven (tp_cost * 2 tics)
    REPLENISH_CD,  // cooldown driven (effective cooldown * 12 tics)
} replenish_model_t;

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
    int ammo_cost;      // ammo moved per attack (see ammo_inverted)
                        // (0 = no ammo needed: fists)
    int ammo_max;       // magazine capacity; 0 = no magazine (see above)
    boolean ammo_inverted; // false: firing spends the counter down,
                        // replenish fills it toward ammo_max (normal).
                        // true (plasma): the counter IS heat by another
                        // name -- it starts at 0, firing adds ammo_cost,
                        // the weapon fires while the counter is below
                        // capacity, and the counter drains back toward 0
                        // at the replenish rate. No separate heat state.
    int max_charges;    // charge capacity (rocket); 0 = no charge system
    int leech;          // inherent lifesteal % (chainsaw); stacks with gear
    int knockback;      // tiles to shove target on hit (SSG); 0 = none
    int attack_tics;    // command execution delay in tics (attack speed)
    replenish_model_t replenish; // what drives ammo trickle rate
} t_kitdef_t;

const t_kitdef_t *T_KitForWeapon(weapontype_t w);
// Effective attack interval (tics) for a player's weapon: the kit's
// attack_tics scaled by the derived attack-speed stat. THE firing
// rate for both queued attacks and continuous fire.
int T_AttackInterval(player_t *player, weapontype_t w);
int T_KitFireSound(weapontype_t w);     // per-gun firing report sfx
int T_KitCooldown(weapontype_t w);          // current cooldown rounds left
void T_KitSetCooldown(weapontype_t w, int rounds);
void T_KitResetCadence(void);           // new game: zero cd, full charges
                                        // and magazines (inverted start 0)

// Diablo damage callback for turn-mode natural simulation.
// When the player fires in turn mode, Doom's hitscan/projectile system
// determines hit/miss naturally. On a hit, P_DamageMobj calls this to
// compute Diablo AD/AP damage instead of vanilla damage.
// - pl: the attacking player
// - target: the mobj being damaged
// - weapon: wp_* type that fired (for hitscan, from d_diablo_hitscan_weapon;
//   for projectiles, from inflictor->diablo_weapon)
// Returns the Diablo damage to apply.
typedef int (*diablo_damage_cb_t)(player_t *pl, mobj_t *target, int weapon);
extern diablo_damage_cb_t d_diablo_damage_cb;
// Weapon for synchronous hitscan (set before P_LineAttack, cleared after).
// Projectiles store it on the mobj instead (diablo_weapon field).
extern int d_diablo_hitscan_weapon;
// Per-weapon ammo magazines: THE resource system for every weapon.
// Every weapon with ammo_max > 0 is gated by its counter through the
// one gate (T_KitCanFire / T_HasAmmoForKit), spends through the one
// spend (T_KitSpendAmmo, called by the single attack executor), and
// replenishes through the one rate system (kit replenish model).
// Normal magazines start full and firing spends them down; plasma's
// magazine is inverted (see ammo_inverted in t_kitdef_t): the counter
// starts at 0, firing fills it, and it drains at the replenish rate --
// the same utilities, with the direction declared in the kit data.
int T_KitAmmo(weapontype_t w);          // current magazine counter
int T_KitMaxAmmo(weapontype_t w);       // magazine capacity (+ gear
                                        // capacity bonus for plasma)
void T_KitSpendAmmo(weapontype_t w);    // apply one attack's ammo move
void T_KitPickupAmmo(void);             // ammo pickup: half-fill all mags
                                        // (inverted: vent half)
void T_KitRefillAllAmmo(void);          // mana potion: full mags
                                        // (inverted: vent to 0)
                                        // charges, zero Lucky counters
// The heat array in the cadence snapshot is a save-format compat slot:
// it carries plasma's counter (an alias of its ammo) so savegames
// written before the unified-ammo change still load their plasma
// state. No separate heat state exists in the engine.
void T_KitSaveCadence(int *cool, int *heat, int *charges, int *shots,
                      int *ammo);
void T_KitLoadCadence(const int *cool, const int *heat,
                      const int *charges, const int *shots,
                      const int *ammo);
int T_KitCharges(weapontype_t w);           // current charges
int T_KitMaxCharges(weapontype_t w);        // charge capacity (+ affixes)
int T_KitShots(weapontype_t w);             // shots fired (Lucky rhythm)
void T_KitTickCooldowns(void);             // call on round start
boolean T_KitCanFire(weapontype_t w);       // all gates pass
const char *T_KitDenyReason(weapontype_t w);// "NO CHARGES"/"AMMO FULL"
                                           // (inverted at capacity)/
                                           // "NO AMMO"/NULL
boolean T_HasAmmoForKit(weapontype_t w);    // consoleplayer can pay ammo cost
boolean T_LastKill(void);                   // did the last attack kill?
void T_ClearLastKill(void);                 // reset before each shot

#endif
