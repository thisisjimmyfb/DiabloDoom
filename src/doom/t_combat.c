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
static int t_last_kill = 0; // set when the resolved attack killed anything
static int t_shots[NUMWEAPONS]; // shots fired per weapon (Lucky rhythm)

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
    // sites (T_ResolveAttack / T_DamageRange) via T_ApplyAdPct.

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

    // Armor / MR from attributes + equipment.
    out->armor = player->diablo_stats[DSTAT_ARMOR] + str / 3 + vit / 4;
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
int T_ApplyAdPct(player_t *player, int dmg)
{
    int pct = player->diablo_stats[DSTAT_AD_PCT];
    if (pct != 0)
        dmg = dmg * (100 + pct) / 100;
    return dmg;
}

int T_ApplyApPct(player_t *player, int ap)
{
    int pct = player->diablo_stats[DSTAT_AP_PCT];
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
    if (dist > 576)
        chance -= 30;
    else if (dist > 384)
        chance -= 20;
    else if (dist > 192)
        chance -= 10;

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
    int kmin, kmax, pellets;
    (void)target;
    kit = T_KitForWeapon(player->readyweapon);
    // Mirror the T_ResolveAttack formula: AD guns stack kit base onto the
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
    // Pellets: show per-pellet range x count (incl. Splitting).
    pellets = kit->pellets;
    if (D_EquippedWeaponMech(player) & MECH_SPLITTING)
        pellets += 2;
    *minhp = kmin;
    *maxhp = kmax;
    if (pellets > 1)
    {
        *minhp = kmin * pellets;
        *maxhp = kmax * pellets;
    }
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
    { "FISTS",    1,  3,  3, 0,   0,  0, 1, false, 0,  0,  0, 0 },
    // Base kits are deliberately weak: item stats are the real damage
    // source, so hunting better gear is the progression.
    // wp_pistol: balanced AD sidearm
    { "SIDEARM",  2,  4,  4, 0,   0,  0, 1, false, 0,  0,  0, 0 },
    // wp_shotgun: close-range burst, 4 pellets
    { "SHOTGUN",  2,  3,  5, 0,   0,  0, 4, false, 0,  0,  0, 0 },
    // wp_chaingun: 3-round burst
    { "CHAINGUN", 1,  3,  5, 0,   0,  0, 3, false, 0,  0,  0, 0 },
    // wp_missile: AP rockets, enemy-targeted splash, charge-gated
    { "ROCKET",   5,  9,  6, 0, 128, 50, 1, true, 10,  0,  0, 2 },
    // wp_plasma: AP energy. Heat is inverse ammo: free to spam while
    // heat < 100, vents 40/round.
    { "PULSE",    2,  4,  4, 0,   0,  0, 1, true,  0, 25, 40, 0 },
    // wp_bfg: AP ultimate, cooldown + mana
    { "BFG",     10, 18,  8, 3, 192, 60, 1, true, 20,  0,  0, 0 },
    // wp_chainsaw: AD melee
    { "SAW",      1,  2,  3, 0,   0,  0, 1, false, 0,  0,  0, 0 },
    // wp_supershotgun: AP double-barrel, breach reload
    { "SSG",      4,  8,  6, 2,   0,  0, 8, true,  8,  0,  0, 0 },
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
static int t_heat[NUMWEAPONS];      // 0-100 (plasma): inverse ammo
static int t_charges[NUMWEAPONS];   // 0..max_charges (rocket)
static boolean t_cadence_inited;

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

int T_KitHeat(weapontype_t w)
{
    if (w < 0 || w >= NUMWEAPONS)
        return 0;
    return t_heat[w];
}

// Overheat threshold: base 100 + gear heat-capacity bonus.
int T_KitHeatMax(void)
{
    return 100 + players[consoleplayer].diablo_stats[DSTAT_HEAT_MAX];
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

// Reset all per-weapon cadence state: cooldowns and heat to zero,
// charges to full, Lucky shot counters to zero. Called on new game;
// loaded games restore their saved cadence instead (P_UnArchiveTurn).
void T_KitResetCadence(void)
{
    int w;
    for (w = 0; w < NUMWEAPONS; w++)
    {
        t_cooldowns[w] = 0;
        t_heat[w] = 0;
        t_charges[w] = t_kits[w].max_charges;
        t_shots[w] = 0;
    }
    t_cadence_inited = true;
}

// Snapshot/restore the whole per-weapon cadence state for save/load.
// Each array holds NUMWEAPONS ints: cooldowns, heat, charges, shots.
// Loads clamp garbage rather than trusting the file.
void T_KitSaveCadence(int *cool, int *heat, int *charges, int *shots)
{
    int w;
    for (w = 0; w < NUMWEAPONS; w++)
    {
        cool[w] = t_cooldowns[w];
        heat[w] = t_heat[w];
        charges[w] = t_charges[w];
        shots[w] = t_shots[w];
    }
}

void T_KitLoadCadence(const int *cool, const int *heat,
                      const int *charges, const int *shots)
{
    int w;
    for (w = 0; w < NUMWEAPONS; w++)
    {
        int maxc = t_kits[w].max_charges;
        t_cooldowns[w] = cool[w] > 0 ? cool[w] : 0;
        t_heat[w] = heat[w] < 0 ? 0 : heat[w]; // clamped at fire time
        t_charges[w] = charges[w] < 0 ? 0
                     : (charges[w] > maxc ? maxc : charges[w]);
        t_shots[w] = shots[w] > 0 ? shots[w] : 0;
    }
    t_cadence_inited = true;
}

void T_KitTickCooldowns(void)
{
    int w;
    if (!t_cadence_inited)
        T_KitResetCadence();
    for (w = 0; w < NUMWEAPONS; w++)
    {
        if (t_cooldowns[w] > 0)
            t_cooldowns[w]--;
        if (t_heat[w] > 0)
        {
            int vent = t_kits[w].heat_vent;
            // Overclocked affix: +25 dissipation while equipped.
            if (D_EquippedWeaponMech(&players[consoleplayer]) & MECH_OVERCLOCK)
                vent += 25;
            t_heat[w] -= vent;
            if (t_heat[w] < 0)
                t_heat[w] = 0;
        }
        {
            int maxc = T_KitMaxCharges((weapontype_t)w);
            if (t_charges[w] < maxc)
                t_charges[w]++;
            else if (t_charges[w] > maxc)
                t_charges[w] = maxc; // affix gun unequipped: clamp down
        }
    }
}

boolean T_KitCanFire(weapontype_t w)
{
    const t_kitdef_t *kit = T_KitForWeapon(w);
    if (T_KitCooldown(w) > 0)
        return false;
    if (kit->max_charges > 0 && T_KitCharges(w) <= 0)
        return false;
    // Heat is inverse ammo: usable while below the overheat threshold.
    if (kit->heat_per_shot > 0 && T_KitHeat(w) >= T_KitHeatMax())
        return false;
    return T_HasAmmoForKit(w);
}

const char *T_KitDenyReason(weapontype_t w)
{
    const t_kitdef_t *kit = T_KitForWeapon(w);
    if (T_KitCooldown(w) > 0)
        return "ON COOLDOWN";
    if (kit->max_charges > 0 && T_KitCharges(w) <= 0)
        return "NO CHARGES";
    if (kit->heat_per_shot > 0 && T_KitHeat(w) >= T_KitHeatMax())
        return "OVERHEATED";
    if (!T_HasAmmoForKit(w))
        return "NO AMMO";
    return NULL;
}

// Ammo affordability: AP kits spend mana_cost per attack from the
// unified ammo pool (player->ammo[am_clip]). Kits with no ammo cost are
// always affordable.
boolean T_HasAmmoForKit(weapontype_t w)
{
    const t_kitdef_t *kit = T_KitForWeapon(w);
    player_t *player = &players[consoleplayer];
    if (kit->mana_cost <= 0)
        return true;
    return player->ammo[am_clip] >= kit->mana_cost;
}

// Kill credit + kill-triggered affix mechanics for one victim.
static void T_ResolveKill(player_t *player, mobj_t *victim, int dmg,
                          const char *kitname)
{
    int mech;

    t_last_kill = 1;
    T_CountKill(victim);

    mech = D_EquippedWeaponMech(player);
    if (mech & MECH_PHOENIX)
    {
        // Kills vent 50 heat off the firing weapon.
        t_heat[player->readyweapon] -= 50;
        if (t_heat[player->readyweapon] < 0)
            t_heat[player->readyweapon] = 0;
    }
    if (mech & MECH_HUNGRY)
    {
        // Kills reduce the remaining cooldown by one round.
        if (t_cooldowns[player->readyweapon] > 0)
            t_cooldowns[player->readyweapon]--;
    }
    if (mech & MECH_BREACHER)
    {
        // Kills immediately refund the reload cooldown.
        t_cooldowns[player->readyweapon] = 0;
    }
    if (mech & MECH_REAPER)
    {
        // Kills refund 2 TP (capped at max).
        turnctrl.tp += 2;
        if (turnctrl.tp > turnctrl.tp_max)
            turnctrl.tp = turnctrl.tp_max;
    }
}

// Caldera nova: half the shot's damage to every shootable foe within
// 128 map units of the player. Kills credit normally.
static void T_CalderaNova(player_t *player, int total_dmg)
{
    thinker_t *th;
    int ndmg = total_dmg / 2;

    player->message = "CALDERA NOVA!";
    for (th = thinkercap.next; th != &thinkercap; th = th->next)
    {
        mobj_t *mo;
        int dist, dmg;
        if (th->function.acp1 != (actionf_p1)P_MobjThinker)
            continue;
        mo = (mobj_t *)th;
        if (mo == player->mo || mo->health <= 0)
            continue;
        if (!(mo->flags & MF_SHOOTABLE) || mo->player != NULL)
            continue;
        dist = P_AproxDistance(mo->x - player->mo->x,
                               mo->y - player->mo->y) / FRACUNIT;
        if (dist > 128)
            continue;
        dmg = ndmg - T_MonsterArmor(mo->type);
        if (dmg < 1)
            dmg = 1;
        P_DamageMobj(mo, player->mo, player->mo, dmg);
        if (mo->health <= 0)
            T_ResolveKill(player, mo, total_dmg, "CALDERA");
    }
}

boolean T_ResolveAttack(player_t *player, mobj_t *target)
{
    t_combatstats_t st;
    const t_kitdef_t *kit;
    int chance, roll, dmg, range, p, pellets;
    int mech;
    boolean lucky;
    int total_dmg = 0;
    int hits = 0;

    t_last_hit = 0;
    t_last_damage = 0;
    t_last_crit = 0;
    t_last_kill = 0;

    if (!player->mo || !target || target->health <= 0)
        return false;

    kit = T_KitForWeapon(player->readyweapon);
    mech = D_EquippedWeaponMech(player);
    T_DeriveStats(player, &st);

    // Cadence is spent when the weapon FIRES, not when it hits: heat
    // builds, a charge is consumed, the cooldown starts. Kill-triggered
    // affixes (Phoenix/Hungry/Breacher) run later in resolution and see
    // the spent state, so their refunds apply on top of it.
    {
        weapontype_t w = player->readyweapon;
        if (kit->heat_per_shot > 0)
        {
            int max = T_KitHeatMax();
            t_heat[w] += kit->heat_per_shot;
            if (t_heat[w] > max)
                t_heat[w] = max;
        }
        if (kit->max_charges > 0 && t_charges[w] > 0)
            t_charges[w]--;
        if (kit->cooldown > 0)
        {
            // Haste reduces cooldown rounds (min 1).
            t_combatstats_t st;
            int cd = kit->cooldown;
            T_DeriveStats(player, &st);
            cd -= cd * st.haste / 100;
            if (cd < 1)
                cd = 1;
            T_KitSetCooldown(w, cd);
        }
        t_shots[w]++; // Lucky rhythm counts shots fired, not hits
    }

    // Firing report: per-gun sound plus the muzzle flash. Flash only —
    // the weapon's atkstate sequence runs the real A_Fire* damage
    // functions, which turn mode must never trigger.
    S_StartSound(player->mo, T_KitFireSound(player->readyweapon));
    if (weaponinfo[player->readyweapon].flashstate != S_NULL)
        P_SetPsprite(player, ps_flash,
                     weaponinfo[player->readyweapon].flashstate);

    // Gun damage, approved formula. AD: kit_base + gun_dmg_range +
    // STR/2..STR, then AD%. AP: kit_base + AP/4..AP/2, then AP%.
    // (AD%/AP% used to be applied inside T_DeriveStats, before the kit
    // base was stacked on; now they apply to the whole thing.)
    if (kit->ap_weapon)
    {
        int kmin = kit->dmg_min + st.ap / 4;
        int kmax = kit->dmg_max + st.ap / 2;
        if (kmax < kmin)
            kmax = kmin;
        st.ad_min = T_ApplyApPct(player, kmin);
        st.ad_max = T_ApplyApPct(player, kmax);
    }
    else
    {
        st.ad_min = T_ApplyAdPct(player, st.ad_min + kit->dmg_min);
        st.ad_max = T_ApplyAdPct(player, st.ad_max + kit->dmg_max);
        if (st.ad_max < st.ad_min)
            st.ad_max = st.ad_min;
    }

    chance = T_HitChance(player, target, &st);

    // Pellets: each rolls hit and damage separately (shotgun/chaingun).
    // Splitting affix: +2 pellets.
    pellets = kit->pellets + ((mech & MECH_SPLITTING) ? 2 : 0);
    for (p = 0; p < pellets; p++)
    {
        roll = T_Roll100();
        if (roll > chance)
            continue; // pellet misses
        hits++;

        range = st.ad_max - st.ad_min + 1;
        dmg = st.ad_min + (range > 1 ? T_Roll(range) : 0);

        // Crit (once per attack, not per pellet). Lucky: every 3rd
        // pistol shot fired is a guaranteed crit.
        lucky = (mech & MECH_LUCKY) && player->readyweapon == wp_pistol
                && (t_shots[wp_pistol] % 3 == 0);
        if (p == 0 && (lucky || T_Roll100() <= st.crit_chance))
        {
            dmg = dmg * st.crit_mult / 100;
            t_last_crit = 1;
        }
        else if (t_last_crit)
        {
            dmg = dmg * st.crit_mult / 100;
        }

        // Target armor mitigation.
        dmg -= T_MonsterArmor(target->type);
        if (dmg < 1)
            dmg = 1;

        total_dmg += dmg;
    }

    if (hits == 0)
    {
        player->message = "MISS!";
        return false;
    }

    t_last_hit = 1;
    t_last_damage = total_dmg;
    printf("[TURN] attack resolved: hits=%d dmg=%d crit=%d ad=%d-%d\n",
           hits, total_dmg, t_last_crit, st.ad_min, st.ad_max);

    // Phase 8: lifetime counters.
    T_CountDamage(total_dmg);
    // Check if the target died (kill credit).
    // Note: P_DamageMobj is called below; we check after.
    if (t_last_crit)
    {
        player->message = "CRITICAL HIT!";
        printf("[TURN] CRITICAL HIT: %d dmg (x%d%%)\n",
               total_dmg, st.crit_mult);
    }
    else
        player->message = "HIT!";

    P_DamageMobj(target, player->mo, player->mo, total_dmg);
    // Siege: direct hits shove living foes 1 tile away from you.
    if ((mech & MECH_SIEGE) && target->health > 0)
    {
        fixed_t dx = target->x - player->mo->x;
        fixed_t dy = target->y - player->mo->y;
        int dist = P_AproxDistance(dx, dy);
        if (dist > FRACUNIT / 2)
            P_TryMove(target,
                      target->x + FixedMul(FixedDiv(dx, dist), 64 * FRACUNIT),
                      target->y + FixedMul(FixedDiv(dy, dist), 64 * FRACUNIT));
    }
    // Kill credit, banner, and kill-triggered affixes.
    if (target->health <= 0)
        T_ResolveKill(player, target, total_dmg, kit->name);

    // Splash: enemy-targeted, centered on the confirmed target.
    // Other visible enemies within radius take splash_pct% damage.
    if (kit->splash_radius > 0 && target->health > 0)
    {
        thinker_t *th;
        for (th = thinkercap.next; th != &thinkercap; th = th->next)
        {
            mobj_t *mo;
            int dist, sdmg;
            if (th->function.acp1 != (actionf_p1)P_MobjThinker)
                continue;
            mo = (mobj_t *)th;
            if (mo == target || mo == player->mo)
                continue;
            if (!(mo->flags & MF_SHOOTABLE) || mo->health <= 0)
                continue;
            if (mo->player != NULL)
                continue;
            dist = P_AproxDistance(mo->x - target->x,
                                   mo->y - target->y) / FRACUNIT;
            if (dist > kit->splash_radius)
                continue;
            // Splash needs LOS from the blast (simple: use attacker sight).
            if (!P_CheckSight(player->mo, mo))
                continue;
            sdmg = total_dmg * kit->splash_pct / 100;
            sdmg -= T_MonsterArmor(mo->type);
            if (sdmg < 1)
                sdmg = 1;
            // Event Horizon: the blast drags foes 1 tile toward its center.
            if ((mech & MECH_HORIZON) && mo->health > 0)
            {
                fixed_t dx = target->x - mo->x;
                fixed_t dy = target->y - mo->y;
                int pdist = P_AproxDistance(dx, dy);
                if (pdist > FRACUNIT / 2)
                    P_TryMove(mo,
                              mo->x + FixedMul(FixedDiv(dx, pdist),
                                               64 * FRACUNIT),
                              mo->y + FixedMul(FixedDiv(dy, pdist),
                                               64 * FRACUNIT));
            }
            P_DamageMobj(mo, player->mo, player->mo, sdmg);
            if (mo->health <= 0)
                T_ResolveKill(player, mo, total_dmg, kit->name);
        }
        player->message = "SPLASH HIT!";
    }

    // Voltaic: the explosion chains 50% damage to the nearest other foe
    // near the blast.
    if (mech & MECH_VOLTAIC)
    {
        thinker_t *th;
        mobj_t *best = NULL;
        int bestdist = kit->splash_radius > 0
                       ? kit->splash_radius * FRACUNIT : 128 * FRACUNIT;
        for (th = thinkercap.next; th != &thinkercap; th = th->next)
        {
            mobj_t *mo;
            int d;
            if (th->function.acp1 != (actionf_p1)P_MobjThinker)
                continue;
            mo = (mobj_t *)th;
            if (mo == target || mo == player->mo || mo->health <= 0)
                continue;
            if (!(mo->flags & MF_SHOOTABLE) || mo->player != NULL)
                continue;
            d = P_AproxDistance(mo->x - target->x, mo->y - target->y);
            if (d < bestdist)
            {
                bestdist = d;
                best = mo;
            }
        }
        if (best != NULL)
        {
            int cdmg = total_dmg / 2 - T_MonsterArmor(best->type);
            if (cdmg < 1)
                cdmg = 1;
            P_DamageMobj(best, player->mo, player->mo, cdmg);
            if (best->health <= 0)
                T_ResolveKill(player, best, total_dmg, kit->name);
            player->message = "VOLTAIC CHAIN!";
        }
    }

    // Caldera: overheating from this shot becomes a fire nova around
    // you and vents to zero instead of locking the gun. (Heat was
    // already added at fire time above.)
    if (kit->heat_per_shot > 0 && t_heat[player->readyweapon] >= T_KitHeatMax()
        && (mech & MECH_CALDERA))
    {
        t_heat[player->readyweapon] = 0;
        T_CalderaNova(player, total_dmg);
    }

    // of the Bull: recoil shoves you 1 tile backward (away from facing).
    if (mech & MECH_BULL)
    {
        angle_t ang = player->mo->angle;
        fixed_t nx = player->mo->x
                     - FixedMul(64 * FRACUNIT,
                                finecosine[ang >> ANGLETOFINESHIFT]);
        fixed_t ny = player->mo->y
                     - FixedMul(64 * FRACUNIT,
                                finesine[ang >> ANGLETOFINESHIFT]);
        P_TryMove(player->mo, nx, ny);
    }

    return true;
}
