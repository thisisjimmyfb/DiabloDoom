// t_combat.c — turn-based combat stats and resolution (Phase 4).

#include "t_combat.h"
#include "t_turn.h"
#include "d_diablo.h"
#include "p_local.h"
#include "doomstat.h"
#include "s_sound.h"
#include "sounds.h"
#include "m_misc.h"

int t_last_hit = -1;
int t_last_damage = 0;
int t_last_crit = 0;
mobj_t *t_last_hit_mobj = NULL; // mobj hit by the last Diablo damage callback
int t_last_hit_tics = 0; // countdown for floating damage number
mobj_t *t_last_miss_mobj = NULL; // mobj missed by the last attack
int t_last_miss_tics = 0; // countdown for floating MISS text
static int t_last_kill = 0; // set when the resolved attack killed anything
static int t_shots[NUMWEAPONS]; // shots fired per weapon (Lucky rhythm)

// Diablo damage override for native weapon fire. See t_combat.h.
boolean t_damage_override = false;
int t_damage_override_value = 0;

boolean T_LastKill(void)
{
    return t_last_kill != 0;
}

// Dedicated combat RNG (LCG). Independent of the game's P_Random so
// fixed-seed tests are deterministic regardless of map RNG use.
static unsigned int t_rng = 0xC0BA7; // set via T_SetCombatSeed

void T_SetCombatSeed(unsigned int seed)
{
    t_rng = seed ? seed : 0xC0BA7;
}

unsigned int T_GetCombatSeed(void)
{
    return t_rng;
}

// Roll 0..max-1.
static int T_Roll(int max)
{
    t_rng = t_rng * 1103515245u + 12345u;
    return (int)((t_rng >> 16) % (unsigned int)max);
}

// Roll 1..100.
static int T_Roll100(void)
{
    return T_Roll(100) + 1;
}

void T_DeriveStats(player_t *player, t_combatstats_t *out)
{
    int str, dex, vit, ene;
    int dmg_min, dmg_max;

    // Base 10 in every attribute; equipment adds on top.
    str = 10 + player->diablo_stats[DSTAT_STR];
    dex = 10 + player->diablo_stats[DSTAT_DEX];
    vit = 10 + player->diablo_stats[DSTAT_VIT];
    ene = 10 + player->diablo_stats[DSTAT_ENE];

    dmg_min = player->diablo_stats[DSTAT_DMG_MIN];
    dmg_max = player->diablo_stats[DSTAT_DMG_MAX];
    if (dmg_max <= 0)
    {
        // Fists.
        dmg_min = 1;
        dmg_max = 3;
    }

    // Strength feeds weapon damage.
    out->ad_min = dmg_min + str / 2;
    out->ad_max = dmg_max + str;
    if (out->ad_max < out->ad_min)
        out->ad_max = out->ad_min;
    // NOTE: AD_PCT is NOT applied here. The approved formula applies it
    // after kit_base + gun damage + STR, so it lives at the formula
    // sites (T_DiabloDamageCallback / T_DamageRange) via T_ApplyAdPct.

    // Energy feeds ability power.
    out->ap = ene * 2;
    // NOTE: AP_PCT likewise applies after the kit base (T_ApplyApPct).

    // Dexterity feeds attack speed: faster attackers pay less TP.
    // 4 TP base; -1 per 20 dex above 10, floor 2.
    out->attack_tp = 4 - (dex - 10) / 20;
    if (out->attack_tp < 2)
        out->attack_tp = 2;
    if (out->attack_tp > 4)
        out->attack_tp = 4;

    // Crit is gear-inherent (Diablo-style): weapons and items grant
    // crit chance and bonus crit damage. No attribute derivation.
    out->crit_chance = player->diablo_stats[DSTAT_CRIT_CHANCE];
    if (out->crit_chance > 50)
        out->crit_chance = 50;
    out->crit_mult = 200 + player->diablo_stats[DSTAT_CRIT_DMG];

    // Defense is the pure equipment sum (Diablo 2-style rating;
    // attributes no longer add to it). MR from attributes + gear.
    out->armor = player->diablo_stats[DSTAT_ARMOR];
    out->mr = ene / 2 +
        (player->diablo_stats[DSTAT_FRES] + player->diablo_stats[DSTAT_CRES] +
         player->diablo_stats[DSTAT_LRES] + player->diablo_stats[DSTAT_PRES]) / 20;

    // Base accuracy from dexterity.
    out->accuracy = 80 + (dex - 10);
    if (out->accuracy > 95)
        out->accuracy = 95;
    if (out->accuracy < 50)
        out->accuracy = 50;

    // Haste trims cooldowns (consumed by Phase 5 abilities).
    // Phase 7: tactical affix HASTE adds to the ENE-derived base.
    out->haste = ene / 5 + player->diablo_stats[DSTAT_HASTE];
    if (out->haste > 50)
        out->haste = 50; // cap at 50% reduction
}

// Gear percent multipliers, applied AFTER all flat damage (kit base +
// gun damage + attributes), per the approved AD/AP formula. Diablo-style:
// +X% Attack Damage / +Y% Ability Power from items and focus buffs.
// Flat bonuses (DSTAT_AD_FLAT/DSTAT_AP_FLAT) are added before the %
// multiplier.
int T_ApplyAdPct(player_t *player, int dmg)
{
    int pct = player->diablo_stats[DSTAT_AD_PCT];
    dmg += player->diablo_stats[DSTAT_AD_FLAT];
    if (pct != 0)
        dmg = dmg * (100 + pct) / 100;
    return dmg;
}

int T_ApplyApPct(player_t *player, int ap)
{
    int pct = player->diablo_stats[DSTAT_AP_PCT];
    ap += player->diablo_stats[DSTAT_AP_FLAT];
    if (pct != 0)
        ap = ap * (100 + pct) / 100;
    return ap;
}

// Multi-point cover (Phase 6): trace from three attacker positions
// (center, left, right, offset perpendicular to the firing line) using
// the tested P_CheckSight. Returns the number of blocked traces (0-3).
// 0 = clear, 1 = -10 accuracy, 2 = -25 accuracy, 3 = full blockage
// (target is not a legal attack target).
int T_CoverBlocked(player_t *player, mobj_t *target)
{
    mobj_t *mo;
    fixed_t dx, dy, len, px, py;
    fixed_t off = 16 * FRACUNIT; // lateral offset for side traces
    int blocked = 0;
    fixed_t savex, savey;
    fixed_t positions[3][2];
    int i;

    mo = player->mo;
    if (!mo || !target)
        return 0;

    // Direction from player to target.
    dx = target->x - mo->x;
    dy = target->y - mo->y;
    len = P_AproxDistance(dx, dy);
    if (len == 0)
        return 0;

    // Perpendicular unit vector (scaled by off).
    px = -dy * off / len;
    py = dx * off / len;

    positions[0][0] = mo->x;        positions[0][1] = mo->y;
    positions[1][0] = mo->x + px;   positions[1][1] = mo->y + py;
    positions[2][0] = mo->x - px;   positions[2][1] = mo->y - py;

    savex = mo->x;
    savey = mo->y;
    for (i = 0; i < 3; i++)
    {
        mo->x = positions[i][0];
        mo->y = positions[i][1];
        if (!P_CheckSight(mo, target))
            blocked++;
    }
    mo->x = savex;
    mo->y = savey;
    return blocked;
}

int T_HitChance(player_t *player, mobj_t *target, const t_combatstats_t *st)
{
    int dist, chance;

    if (!player->mo || !target)
        return 0;

    dist = P_AproxDistance(target->x - player->mo->x,
                           target->y - player->mo->y) / FRACUNIT;

    chance = st->accuracy;

    // Range bands: close is best, long range falls off.
    // Pistols are exempt (already weak, no range penalty).
    if (player->readyweapon != wp_pistol)
    {
        if (dist > 576)
            chance -= 30;
        else if (dist > 384)
            chance -= 20;
        else if (dist > 192)
            chance -= 10;
    }

    // Cover (multi-point): 1 blocked = -10, 2 blocked = -25.
    // 3 blocked = full blockage (handled in T_RefreshTargets; target
    // is not legal, so we should never see it here).
    {
        int blocked = T_CoverBlocked(player, target);
        if (blocked == 1)
            chance -= 10;
        else if (blocked == 2)
            chance -= 25;
    }

    if (chance < 5)
        chance = 5;
    if (chance > 95)
        chance = 95;
    return chance;
}

void T_DamageRange(player_t *player, mobj_t *target,
                   const t_combatstats_t *st, int *minhp, int *maxhp)
{
    const t_kitdef_t *kit;
    int kmin, kmax;
    (void)target;
    kit = T_KitForWeapon(player->readyweapon);
    // Mirror the live damage formula: AD guns stack kit base onto the
    // derived (gun + STR) range, then AD%; AP guns scale with AP, then AP%.
    if (kit->ap_weapon)
    {
        kmin = T_ApplyApPct(player, kit->dmg_min + st->ap / 4);
        kmax = T_ApplyApPct(player, kit->dmg_max + st->ap / 2);
    }
    else
    {
        kmin = T_ApplyAdPct(player, st->ad_min + kit->dmg_min);
        kmax = T_ApplyAdPct(player, st->ad_max + kit->dmg_max);
    }
    if (kmax < kmin)
        kmax = kmin;
    // AD is damage per trigger pull (the resolver splits it across
    // pellets): show the per-pull range with no pellet multiplier.
    *minhp = kmin;
    *maxhp = kmax;
}

// Monster armor table: a little mitigation so Armor/MR aren't
// player-only concepts. Indexed by mobjtype; unknown = 0.
static int T_MonsterArmor(mobjtype_t type)
{
    switch (type)
    {
      case MT_POSSESSED:
      case MT_SHOTGUY:
      case MT_CHAINGUY:
        return 0;
      case MT_TROOP:   // imp
      case MT_SERGEANT:// demon
      case MT_SHADOWS: // spectre
        return 1;
      case MT_HEAD:    // cacodemon
      case MT_SKULL:   // lost soul
        return 2;
      case MT_BRUISER: // baron
      case MT_KNIGHT:  // hell knight
        return 4;
      case MT_SPIDER:
      case MT_CYBORG:
        return 6;
      default:
        return 0;
    }
}

// ---------------------------------------------------------------------------
// Weapon kits.

static const t_kitdef_t t_kits[NUMWEAPONS] = {
    // wp_fist (fallback; not a real kit)
    { "PISTOL",     1,   3,   3,    0,    0,   0,      1, false,    0,    0, false,   0,   0,   0,  20, REPLENISH_AS },
    // Base kits are deliberately weak: item stats are the real damage
    // source, so hunting better gear is the progression.
    // Ammo: every weapon with a magazine requires ammo to fire.
    // Replenish model is explicit per-weapon (see replenish_model_t):
    // AS = attack-speed driven, CD = cooldown driven (CDR gear speeds it up).
    // wp_pistol: balanced AD sidearm
    { "SIDEARM",    2,   4,   4,    0,    0,   0,      1, false,    1,   10, false,   0,   0,   0,  15, REPLENISH_AS },
    // wp_shotgun: close-range burst, 4 pellets
    { "SHOTGUN",    2,   3,   5,    0,    0,   0,      4, false,    2,   20, false,   0,   0,   0,  40, REPLENISH_AS },
    // wp_chaingun: 3-round burst
    { "CHAINGUN",   1,   3,   5,    0,    0,   0,      3, false,    1,   10, false,   0,   0,   0,  16, REPLENISH_AS },
    // wp_missile: AP rockets, enemy-targeted splash, charge-gated
    { "ROCKET",     5,   9,   6,    0,  128,  50,      1, true,   10,   20, false,   3,   0,   0,  80, REPLENISH_AS },
    // wp_plasma: AP energy, inverted magazine (Jimmy, 2026-10-22):
    // the ammo counter represents heat. It starts at 0, each shot
    // adds 25, the weapon fires while the counter is below the 100
    // capacity, and the counter drains at the replenish rate -- the
    // drain is the "reload" period. Same gate/spend/rate utilities
    // as every other weapon; only the direction is kit data.
    { "PULSE",      2,   4,   4,    0,    0,   0,      1, true,   25,  100,  true,   0,   0,   0,  16, REPLENISH_AS },
    // wp_bfg: AP ultimate, cooldown-driven replenish
    { "BFG",       10,  18,   8,    3,  192,  60,      1, true,   20,   20, false,   0,   0,   0,  80, REPLENISH_CD },
    // wp_chainsaw: AD melee with inherent leech (sustain for close range)
    { "SAW",        1,   2,   3,    0,    0,   0,      1, false,    1,    6, false,   0,  30,   0,  12, REPLENISH_AS },
    // wp_supershotgun: AP double-barrel, breach reload, natural knockback
    { "SSG",        4,   8,   6,    2,    0,   0,      8, true,    8,    8, false,   0,   0,   1,  50, REPLENISH_CD },
};

const t_kitdef_t *T_KitForWeapon(weapontype_t w)
{
    if (w < 0 || w >= NUMWEAPONS)
        return &t_kits[wp_fist];
    return &t_kits[w];
}

// Per-gun firing report. The chaingun uses Doom's classic plasma
// report; the chainsaw idles with the saw sound.
int T_KitFireSound(weapontype_t w)
{
    switch (w)
    {
        case wp_shotgun:      return sfx_shotgn;
        case wp_chaingun:     return sfx_plasma;
        case wp_missile:      return sfx_rlaunc;
        case wp_plasma:       return sfx_plasma;
        case wp_bfg:          return sfx_bfg;
        case wp_chainsaw:     return sfx_sawful;
        case wp_supershotgun: return sfx_dshtgn;
        case wp_pistol:
        default:              return sfx_pistol;
    }
}

// Per-weapon cadence state, in rounds/tics. Indexed by weapontype.
static int t_cooldowns[NUMWEAPONS];
static int t_charges[NUMWEAPONS];   // 0..max_charges (rocket)
static int t_ammo[NUMWEAPONS];      // THE per-weapon resource counter:
                                    // magazine rounds (plasma: inverted)
static int t_charge_tics[NUMWEAPONS]; // tics since last charge regen
static int t_cooldown_tics[NUMWEAPONS]; // tics since last cooldown tick
static int t_decay_acc[NUMWEAPONS];   // fractional drain accumulator
                                      // (inverted magazines)
static boolean t_cadence_inited;

// Diablo damage callback globals (see t_combat.h).
diablo_damage_cb_t d_diablo_damage_cb = NULL;
int d_diablo_hitscan_weapon = -1;

// Diablo damage calculation for natural simulation.
// Called by P_DamageMobj when a player-fired hitscan/projectile hits in
// turn mode. Doom's simulation determined HIT; we compute Diablo damage.
// - pl: attacking player
// - target: mobj being damaged
// - weapon: wp_* type that fired
// Returns Diablo AD/AP damage (with crit, armor mitigation).
static int T_DiabloDamageCallback(player_t *pl, mobj_t *target, int weapon)
{
    const t_kitdef_t *kit;
    t_combatstats_t st;
    int mech, dmg, range;
    boolean lucky;

    if (!pl || !target || weapon < 0 || weapon >= NUMWEAPONS)
        return 1;
    kit = T_KitForWeapon((weapontype_t)weapon);
    mech = D_EquippedWeaponMech((struct player_s *)pl);
    T_DeriveStats(pl, &st);

    // Gun damage, approved formula. AD: kit_base + gun_dmg_range +
    // STR/2..STR, then AD%. AP: kit_base + AP/4..AP/2, then AP%.
    if (kit->ap_weapon)
    {
        int kmin = kit->dmg_min + st.ap / 4;
        int kmax = kit->dmg_max + st.ap / 2;
        if (kmax < kmin)
            kmax = kmin;
        st.ad_min = T_ApplyApPct(pl, kmin);
        st.ad_max = T_ApplyApPct(pl, kmax);
    }
    else
    {
        st.ad_min = T_ApplyAdPct(pl, st.ad_min + kit->dmg_min);
        st.ad_max = T_ApplyAdPct(pl, st.ad_max + kit->dmg_max);
        if (st.ad_max < st.ad_min)
            st.ad_max = st.ad_min;
    }

    // Multi-pellet weapons (shotgun, chaingun) split AD across pellets.
    // The full AD is the gun's damage per trigger pull, not per bullet.
    if (kit->pellets > 1)
    {
        st.ad_min = (st.ad_min + kit->pellets - 1) / kit->pellets;
        st.ad_max = (st.ad_max + kit->pellets - 1) / kit->pellets;
        if (st.ad_max < st.ad_min)
            st.ad_max = st.ad_min;
    }

    // Single damage roll (no hit chance; Doom simulation determined hit).
    // For multi-pellet weapons, P_LineAttack calls this per-pellet.
    range = st.ad_max - st.ad_min + 1;
    dmg = st.ad_min + (range > 1 ? T_Roll(range) : 0);

    // Crit. Lucky: every 3rd pistol shot is a guaranteed crit.
    lucky = (mech & MECH_LUCKY) && weapon == wp_pistol
            && (t_shots[wp_pistol] % 3 == 0);
    if (lucky || T_Roll100() <= st.crit_chance)
    {
        dmg = dmg * st.crit_mult / 100;
        t_last_crit = 1;
    }

    // Target armor mitigation.
    dmg -= T_MonsterArmor(target->type);
    if (dmg < 1)
        dmg = 1;

    t_last_damage = dmg;
    t_last_hit_mobj = target;
    return dmg;
}

// Time-based cadence: 1 "round" = 10 seconds = 350 tics.
// Rocket charges: 1 per 2 seconds (70 tics).
// Cooldowns (BFG/SSG): tick down 1 round per 350 tics.
// Inverted magazines (plasma): the counter drains continuously at
// the weapon's replenish rate (T_KitReplenishTics), run in reverse.
#define ROCKET_REGEN_TICS 70
#define TICS_PER_ROUND 350

int T_KitCooldown(weapontype_t w)
{
    if (w < 0 || w >= NUMWEAPONS)
        return 0;
    return t_cooldowns[w];
}

void T_KitSetCooldown(weapontype_t w, int rounds)
{
    if (w < 0 || w >= NUMWEAPONS)
        return;
    t_cooldowns[w] = rounds;
}

// Called when a weapon fires (native fire path): consumes a charge
// and sets cooldown (with Haste and flat CDR). The ammo move itself
// (plasma's included) happens in T_KitSpendAmmo, called by the same
// single executor -- OnFire carries no resource of its own.
void T_KitOnFire(player_t *player)
{
    weapontype_t w = player->readyweapon;
    const t_kitdef_t *kit = T_KitForWeapon(w);

    if (kit->max_charges > 0 && t_charges[w] > 0)
        t_charges[w]--;
    // NOTE: Firing does NOT set a cooldown. CD is just the ammo replenish
    // rate (see T_KitTrickleAmmo). Ammo is the limiter, not a separate CD
    // resource.
}

// Magazine capacity: the kit's ammo_max plus, for plasma, the gear
// capacity bonus (the stat formerly known as heat capacity -- it is
// just plasma's max ammo now).
int T_KitMaxAmmo(weapontype_t w)
{
    const t_kitdef_t *kit = T_KitForWeapon(w);
    int max = kit->ammo_max;
    if (w == wp_plasma)
        max += players[consoleplayer].diablo_stats[DSTAT_PLASMA_CAP];
    return max;
}

int T_KitCharges(weapontype_t w)
{
    if (w < 0 || w >= NUMWEAPONS)
        return 0;
    return t_charges[w];
}

int T_KitShots(weapontype_t w)
{
    if (w < 0 || w >= NUMWEAPONS)
        return 0;
    return t_shots[w];
}

int T_KitMaxCharges(weapontype_t w)
{
    const t_kitdef_t *kit = T_KitForWeapon(w);
    int max = kit->max_charges;
    // Bandolier affix: +1 max charge while its rocket launcher is equipped.
    if (w == wp_missile &&
        (D_EquippedWeaponMech(&players[consoleplayer]) & MECH_BANDOLIER))
        max += 1;
    return max;
}

// Reset all per-weapon cadence state: cooldowns to zero, charges
// and magazines to full (inverted magazines to empty -- plasma
// starts cold at 0), Lucky shot counters to zero.
// Called on new game; loaded games restore their saved cadence
// instead (P_UnArchiveTurn).
void T_KitResetCadence(void)
{
    int w;
    for (w = 0; w < NUMWEAPONS; w++)
    {
        t_cooldowns[w] = 0;
        t_ammo[w] = t_kits[w].ammo_inverted ? 0 : t_kits[w].ammo_max;
        t_charges[w] = t_kits[w].max_charges;
        t_charge_tics[w] = 0;
        t_cooldown_tics[w] = 0;
        t_decay_acc[w] = 0;
        t_shots[w] = 0;
    }
    t_cadence_inited = true;
    // Install the Diablo damage callback for natural simulation.
    // Doom's hitscan/projectile determines hit/miss; this computes damage.
    d_diablo_damage_cb = T_DiabloDamageCallback;
}

// THE ammo replenish rate: tics per unit of magazine movement for a
// kit, from its explicit replenish model. One rate system for every
// weapon: normal magazines trickle UP at this rate during the
// progress phase (T_KitTrickleAmmo); inverted magazines drain DOWN
// at this rate on every game tic (T_TickCadence).
static int T_KitReplenishTics(const t_kitdef_t *kit, player_t *player)
{
    if (kit->replenish == REPLENISH_CD)
    {
        // Cooldown-driven: uses EFFECTIVE cooldown (after Haste %
        // and flat CDR), so CDR gear speeds up replenish.
        // BFG (cd=3) -> 36 tics/ammo base.
        int eff_cd = kit->cooldown;
        t_combatstats_t st;
        T_DeriveStats(player, &st);
        eff_cd -= eff_cd * st.haste / 100;
        eff_cd -= player->diablo_stats[DSTAT_CD_FLAT];
        if (eff_cd < 1)
            eff_cd = 1;
        return eff_cd * 12;
    }
    // REPLENISH_AS: attack speed -- tp_cost scales the interval.
    // Pistol (4) -> 8 tics, Shotgun (2) -> 4, Chaingun (1) -> 3 (min).
    {
        int interval = kit->tp_cost * 2;
        if (interval < 3)
            interval = 3;
        if (interval > 20)
            interval = 20;
        return interval;
    }
}

// Time-based cadence: called from T_Ticker on every game tic EXCEPT
// during the planning phase (TS_PLANNING/TS_TARGETING/TS_CONFIRM),
// where it is frozen -- no charge regen, no cooldown progress, and
// no inverted-magazine drain while the player plans (Jimmy,
// 2026-10-22).
// - Rocket charges: 1 per ROCKET_REGEN_TICS (2 seconds)
// - Cooldowns (BFG/SSG): 1 round per TICS_PER_ROUND (10 seconds)
// - Inverted magazines (plasma): drain at the replenish rate,
//   continuously
// All independent of round boundaries.
static void T_RefillOnCooldownReady(weapontype_t w);
void T_TickCadence(void)
{
    int w;
    for (w = 0; w < NUMWEAPONS; w++)
    {
        // Charges (rocket)
        if (t_kits[w].max_charges > 0)
        {
            int maxc = T_KitMaxCharges((weapontype_t)w);
            if (t_charges[w] < maxc)
            {
                t_charge_tics[w]++;
                if (t_charge_tics[w] >= ROCKET_REGEN_TICS)
                {
                    t_charge_tics[w] = 0;
                    t_charges[w]++;
                    if (t_kits[w].ammo_max > 0 && t_kits[w].ammo_cost > 0)
                    {
                        t_ammo[w] += t_kits[w].ammo_cost;
                        if (t_ammo[w] > t_kits[w].ammo_max)
                            t_ammo[w] = t_kits[w].ammo_max;
                    }
                }
            }
            else
            {
                t_charge_tics[w] = 0;
                if (t_charges[w] > maxc)
                    t_charges[w] = maxc;
            }
        }

        // Cooldowns (BFG/SSG)
        if (t_cooldowns[w] > 0)
        {
            t_cooldown_tics[w]++;
            if (t_cooldown_tics[w] >= TICS_PER_ROUND)
            {
                t_cooldown_tics[w] = 0;
                t_cooldowns[w]--;
                if (t_cooldowns[w] == 0)
                    T_RefillOnCooldownReady((weapontype_t)w);
            }
        }
        else
        {
            t_cooldown_tics[w] = 0;
        }

        // Inverted magazine (plasma): the counter drains toward 0
        // at the weapon's replenish rate -- the same interval normal
        // magazines trickle up at (T_KitReplenishTics), run in
        // reverse. The drain is plasma's "reload": once firing has
        // filled the counter to capacity, it must fall below
        // capacity before the weapon can fire again.
        if (t_kits[w].ammo_inverted && t_ammo[w] > 0)
        {
            int interval = T_KitReplenishTics(&t_kits[w],
                                              &players[consoleplayer]);
            int rate = TICS_PER_ROUND / interval; // units per round
            // Overclocked affix: +25 dissipation while equipped.
            if (D_EquippedWeaponMech(&players[consoleplayer]) & MECH_OVERCLOCK)
                rate += 25;
            // Accumulate fractionally to avoid integer truncation.
            t_decay_acc[w] += rate;
            if (t_decay_acc[w] >= TICS_PER_ROUND)
            {
                int dv = t_decay_acc[w] / TICS_PER_ROUND;
                t_decay_acc[w] %= TICS_PER_ROUND;
                t_ammo[w] -= dv;
                if (t_ammo[w] < 0)
                    t_ammo[w] = 0;
            }
        }
        else
        {
            t_decay_acc[w] = 0;
        }
    }
}

// Snapshot/restore the whole per-weapon cadence state for save/load.
// Each array holds NUMWEAPONS ints: cooldowns, heat, charges, shots,
// ammo. The heat slot is a save-format compat alias: since the
// unified-ammo change it carries plasma's counter (= its ammo) so
// savegames written by older builds, which stored plasma's state
// ONLY in this slot, still load it. Loads clamp garbage rather than
// trusting the file.
void T_KitSaveCadence(int *cool, int *heat, int *charges, int *shots,
                      int *ammo)
{
    int w;
    for (w = 0; w < NUMWEAPONS; w++)
    {
        cool[w] = t_cooldowns[w];
        heat[w] = t_kits[w].ammo_inverted ? t_ammo[w] : 0;
        charges[w] = t_charges[w];
        shots[w] = t_shots[w];
        ammo[w] = t_ammo[w];
    }
}

void T_KitLoadCadence(const int *cool, const int *heat,
                      const int *charges, const int *shots,
                      const int *ammo)
{
    int w;
    for (w = 0; w < NUMWEAPONS; w++)
    {
        int maxc = t_kits[w].max_charges;
        int maxa = T_KitMaxAmmo((weapontype_t)w);
        t_cooldowns[w] = cool[w] > 0 ? cool[w] : 0;
        t_charges[w] = charges[w] < 0 ? 0
                     : (charges[w] > maxc ? maxc : charges[w]);
        t_shots[w] = shots[w] > 0 ? shots[w] : 0;
        t_ammo[w] = ammo[w] < 0 ? 0 : (ammo[w] > maxa ? maxa : ammo[w]);
        if (t_kits[w].ammo_inverted)
        {
            // Plasma's counter loads from the compat heat slot (see
            // T_KitSaveCadence): saves from before the unified-ammo
            // change have it only there; new saves write both slots
            // with the same value.
            int hv = heat[w] < 0 ? 0 : heat[w];
            t_ammo[w] = hv > maxa ? maxa : hv;
        }
    }
    t_cadence_inited = true;
}

// AP magazine refill when a cooldown-gated weapon (SSG/BFG) finishes
// its cooldown. Now a no-op: the trickle (T_KitTrickleAmmo) handles
// replenishment at a cooldown-driven rate, so weapons don't instant-refill.
static void T_RefillOnCooldownReady(weapontype_t w)
{
    (void)w;
    // Intentionally empty. See T_KitTrickleAmmo for cooldown-rate replenish.
}

void T_KitTickCooldowns(void)
{
    int w;
    if (!t_cadence_inited)
        T_KitResetCadence();
    // Time-based cadence (cooldowns, charges, inverted-ammo drain)
    // is handled in T_TickCadence() every game tic. This round-based
    // hook is now a
    // no-op for cadence, kept for save/load compatibility.
    for (w = 0; w < NUMWEAPONS; w++)
    {
        // Clamp charges if over max (e.g., affix unequipped).
        {
            int maxc = T_KitMaxCharges((weapontype_t)w);
            if (t_charges[w] > maxc)
                t_charges[w] = maxc;
        }
    }
}

// THE firing gate, for every weapon: charges where the kit has
// them, and the magazine rule -- normal magazines need at least
// ammo_cost in the counter; inverted magazines (plasma) need the
// counter below capacity (Jimmy's rule, 2026-10-22: plasma fires
// while its ammo count is below max). CD does NOT block firing --
// it's just the ammo replenish rate. Ammo is the limiter.
boolean T_KitCanFire(weapontype_t w)
{
    const t_kitdef_t *kit = T_KitForWeapon(w);
    if (kit->max_charges > 0 && T_KitCharges(w) <= 0)
        return false;
    return T_HasAmmoForKit(w);
}

const char *T_KitDenyReason(weapontype_t w)
{
    const t_kitdef_t *kit = T_KitForWeapon(w);
    // CD does NOT block firing — no cooldown message needed.
    if (kit->max_charges > 0 && T_KitCharges(w) <= 0)
        return "NO CHARGES";
    if (kit->ammo_inverted && t_ammo[w] >= T_KitMaxAmmo(w))
        return "AMMO FULL";
    if (!T_HasAmmoForKit(w))
        return "NO AMMO";
    return NULL;
}

// Ammo affordability, direction-aware: the one affordability test
// every caller uses (queue validation, the attack executor, the
// continuous-fire tick). ammo_cost <= 0 means no ammo needed
// (fists). Inverted magazines are "affordable" while the counter is
// below capacity -- firing fills, never empties, an inverted
// magazine.
boolean T_HasAmmoForKit(weapontype_t w)
{
    const t_kitdef_t *kit = T_KitForWeapon(w);
    if (kit->ammo_cost <= 0)
        return true;
    if (!t_cadence_inited)
        T_KitResetCadence();
    if (kit->ammo_inverted)
        return t_ammo[w] < T_KitMaxAmmo(w);
    return t_ammo[w] >= kit->ammo_cost;
}

int T_KitAmmo(weapontype_t w)
{
    if (w < 0 || w >= NUMWEAPONS)
        return 0;
    if (!t_cadence_inited)
        T_KitResetCadence();
    return t_ammo[w];
}

// THE ammo spend, called by the single attack executor for every
// weapon: normal magazines pay ammo_cost out of the counter;
// inverted magazines (plasma) add ammo_cost to it, clamped at
// capacity. Direction is kit data, not a caller branch.
void T_KitSpendAmmo(weapontype_t w)
{
    const t_kitdef_t *kit = T_KitForWeapon(w);
    if (kit->ammo_cost <= 0)
        return;
    if (!t_cadence_inited)
        T_KitResetCadence();
    if (kit->ammo_inverted)
    {
        t_ammo[w] += kit->ammo_cost;
        if (t_ammo[w] > T_KitMaxAmmo(w))
            t_ammo[w] = T_KitMaxAmmo(w);
    }
    else
    {
        t_ammo[w] -= kit->ammo_cost;
        if (t_ammo[w] < 0)
            t_ammo[w] = 0;
    }
}

// Gradual ammo trickle: +1 to each magazine if below max. Called
// per-tic during the progress phase for visible replenishment. The
// rate is THE replenish rate (T_KitReplenishTics): cooldown-gated
// weapons trickle slower, attack-speed weapons scale with tp_cost.
// Inverted magazines never trickle up -- they drain on the cadence
// instead (T_TickCadence), at this same rate.
void T_KitTrickleAmmo(void)
{
    player_t *player = &players[consoleplayer];
    const t_kitdef_t *kit;
    static int trickle_tick[NUMWEAPONS] = {0};
    int w, interval;
    if (!t_cadence_inited)
        T_KitResetCadence();
    // Trickle all weapons, not just the ready one: if the ready weapon
    // ran dry, vanilla falls back to fist (ammo_max=0) and the real
    // weapon would never refill.
    for (w = 0; w < NUMWEAPONS; w++)
    {
        kit = &t_kits[w];
        if (kit->ammo_max <= 0 || kit->ammo_cost <= 0)
            continue;
        if (kit->ammo_inverted)
            continue;
        if (t_ammo[w] >= kit->ammo_max)
            continue;
        interval = T_KitReplenishTics(kit, player);
        if (++trickle_tick[w] < interval)
            continue;
        trickle_tick[w] = 0;
        t_ammo[w]++;
        if (t_ammo[w] > kit->ammo_max)
            t_ammo[w] = kit->ammo_max;
    }
}

// Ammo pickup: half-fill every magazine; inverted magazines
// (plasma) vent half their counter instead -- "refilling" plasma
// means cooling it.
void T_KitPickupAmmo(void)
{
    int w;
    if (!t_cadence_inited)
        T_KitResetCadence();
    for (w = 0; w < NUMWEAPONS; w++)
    {
        if (t_kits[w].ammo_inverted)
        {
            t_ammo[w] /= 2;
        }
        else if (t_kits[w].ammo_max > 0)
        {
            t_ammo[w] += t_kits[w].ammo_max / 2;
            if (t_ammo[w] > t_kits[w].ammo_max)
                t_ammo[w] = t_kits[w].ammo_max;
        }
    }
}

// Mana potion: full magazines; inverted magazines vent to 0.
void T_KitRefillAllAmmo(void)
{
    int w;
    if (!t_cadence_inited)
        T_KitResetCadence();
    for (w = 0; w < NUMWEAPONS; w++)
        t_ammo[w] = t_kits[w].ammo_inverted ? 0 : t_kits[w].ammo_max;
}

// Kill credit + kill-triggered affix mechanics for one victim.
void T_ResolveKill(player_t *player, mobj_t *victim, int dmg,
                          const char *kitname)
{
    int mech;

    t_last_kill = 1;
    T_CountKill(victim);

    mech = D_EquippedWeaponMech(player);
    if (mech & MECH_PHOENIX)
    {
        // Kills vent 50 off the firing weapon's (inverted) counter.
        if (t_kits[player->readyweapon].ammo_inverted)
        {
            t_ammo[player->readyweapon] -= 50;
            if (t_ammo[player->readyweapon] < 0)
                t_ammo[player->readyweapon] = 0;
        }
    }
    if (mech & MECH_HUNGRY)
    {
        // Kills reduce the remaining cooldown by one round.
        if (t_cooldowns[player->readyweapon] > 0)
            t_cooldowns[player->readyweapon]--;
    }
    if (mech & MECH_BREACHER)
    {
        // Kills immediately refund the reload cooldown — and reload the
        // magazine with it (AP ammo replenishes based on CD).
        t_cooldowns[player->readyweapon] = 0;
        T_RefillOnCooldownReady(player->readyweapon);
    }
    if (mech & MECH_REAPER)
    {
        // Kills refund 2 TP (capped at max).
        turnctrl.tp += 2;
        if (turnctrl.tp > turnctrl.tp_max)
            turnctrl.tp = turnctrl.tp_max;
    }
}


// Effective attack interval in tics for a player's weapon: the kit's
// base attack_tics at baseline attack speed (attack_tp 4, from
// T_DeriveStats), scaled by the DEX-driven attack-speed stat
// (attack_tp 2 = twice as fast). Min 1 tic (0 if the kit has no
// attack time). This is the ONE firing-rate system: the queued-attack
// executor pulses for this long per shot, and the continuous-fire
// tick fires at this interval.
int T_AttackInterval(player_t *player, weapontype_t w)
{
    const t_kitdef_t *kit = T_KitForWeapon(w);
    t_combatstats_t st;
    int t;
    if (kit->attack_tics <= 0)
        return 0;
    T_DeriveStats(player, &st);
    t = kit->attack_tics * st.attack_tp / 4;
    return t < 1 ? 1 : t;
}
