// t_combat.c — turn-based combat stats and resolution (Phase 4).

#include "t_combat.h"
#include "t_turn.h"
#include "d_diablo.h"
#include "p_local.h"
#include "s_sound.h"
#include "sounds.h"
#include "m_misc.h"

int t_last_hit = -1;
int t_last_damage = 0;
int t_last_crit = 0;

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
    // Phase 7: AD_PCT multiplies attack damage (tactical ammo affix).
    {
        int adpct = player->diablo_stats[DSTAT_AD_PCT];
        if (adpct != 0)
        {
            out->ad_min = out->ad_min * (100 + adpct) / 100;
            out->ad_max = out->ad_max * (100 + adpct) / 100;
        }
    }

    // Energy feeds ability power.
    out->ap = ene * 2;
    // Phase 7: AP_PCT multiplies ability power.
    {
        int appct = player->diablo_stats[DSTAT_AP_PCT];
        if (appct != 0)
            out->ap = out->ap * (100 + appct) / 100;
    }

    // Dexterity feeds attack speed: faster attackers pay less TP.
    // 4 TP base; -1 per 20 dex above 10, floor 2.
    out->attack_tp = 4 - (dex - 10) / 20;
    if (out->attack_tp < 2)
        out->attack_tp = 2;
    if (out->attack_tp > 4)
        out->attack_tp = 4;

    // Dexterity feeds crit.
    out->crit_chance = 5 + dex / 4;
    if (out->crit_chance > 50)
        out->crit_chance = 50;
    out->crit_mult = 150 + str / 10; // stronger crits for strong heroes

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

    // Headshot modifier: -15% hit (the +2 TP is in T_CostFor).
    if (turnctrl.headshot_mod)
        chance -= 15;

    // Phase 7: Overwatch accuracy bonus from tactical affixes.
    // Applies only during overwatch reactions.
    if (T_InOverwatchReaction())
        chance += player->diablo_stats[DSTAT_OW_ACC];

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
    int str, kmin, kmax;
    (void)target;
    kit = T_KitForWeapon(player->readyweapon);
    str = 10 + player->diablo_stats[DSTAT_STR];
    kmin = kit->dmg_min + str / 2;
    kmax = kit->dmg_max + str;
    if (kit->ap_scaling)
    {
        kmin += st->ap / 4;
        kmax += st->ap / 2;
    }
    if (kmax < kmin)
        kmax = kmin;
    // Pellets: show per-pellet range x count.
    *minhp = kmin;
    *maxhp = kmax;
    if (kit->pellets > 1)
    {
        *minhp = kmin * kit->pellets;
        *maxhp = kmax * kit->pellets;
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
    { "FISTS",    1,  3,  3, 0,   0,  0, 1, false },
    // wp_pistol: balanced sidearm
    { "SIDEARM",   6, 13,  4, 0,   0,  0, 1, false },
    // wp_shotgun: close-range burst, 4 pellets
    { "SHOTGUN",  4,  8,  5, 0,   0,  0, 4, false },
    // wp_chaingun: 3-round burst
    { "CHAINGUN", 4,  7,  5, 0,   0,  0, 3, false },
    // wp_missile: rockets, enemy-targeted splash
    { "ROCKET",  15, 25,  6, 1, 128, 50, 1, false },
    // wp_plasma: AP-scaling energy
    { "PULSE",   5, 10,  4, 0,   0,  0, 1, true  },
    // wp_bfg: big enemy-targeted splash
    { "BFG",     30, 50,  8, 2, 192, 60, 1, false },
    // wp_chainsaw / wp_supershotgun (unused in turn mode)
    { "SAW",      2,  6,  3, 0,   0,  0, 1, false },
    { "SSG",     10, 20,  6, 1,   0,  0, 8, false },
};

const t_kitdef_t *T_KitForWeapon(weapontype_t w)
{
    if (w < 0 || w >= NUMWEAPONS)
        return &t_kits[wp_fist];
    return &t_kits[w];
}

// Per-weapon cooldowns, in rounds. Indexed by weapontype.
static int t_cooldowns[NUMWEAPONS];

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

void T_KitTickCooldowns(void)
{
    int w;
    for (w = 0; w < NUMWEAPONS; w++)
        if (t_cooldowns[w] > 0)
            t_cooldowns[w]--;
}

boolean T_KitReady(weapontype_t w)
{
    return T_KitCooldown(w) == 0;
}

boolean T_ResolveAttack(player_t *player, mobj_t *target)
{
    t_combatstats_t st;
    const t_kitdef_t *kit;
    int chance, roll, dmg, range, p;
    int total_dmg = 0;
    int hits = 0;

    t_last_hit = 0;
    t_last_damage = 0;
    t_last_crit = 0;

    if (!player->mo || !target || target->health <= 0)
        return false;

    kit = T_KitForWeapon(player->readyweapon);
    T_DeriveStats(player, &st);

    // Kit damage: base from kit, STR scales, AP scales plasma.
    {
        int str = 10 + player->diablo_stats[DSTAT_STR];
        int kmin = kit->dmg_min + str / 2;
        int kmax = kit->dmg_max + str;
        if (kit->ap_scaling)
        {
            kmin += st.ap / 4;
            kmax += st.ap / 2;
        }
        if (kmax < kmin)
            kmax = kmin;
        st.ad_min = kmin;
        st.ad_max = kmax;
    }

    chance = T_HitChance(player, target, &st);

    // Headshot: 2x crit effect (the -15% hit is in T_HitChance,
    // the +2 TP is in T_CostFor). Doubles the crit multiplier.
    if (turnctrl.headshot_mod)
        st.crit_mult *= 2;

    // Pellets: each rolls hit and damage separately (shotgun/chaingun).
    for (p = 0; p < kit->pellets; p++)
    {
        roll = T_Roll100();
        if (roll > chance)
            continue; // pellet misses
        hits++;

        range = st.ad_max - st.ad_min + 1;
        dmg = st.ad_min + (range > 1 ? T_Roll(range) : 0);

        // Crit (once per attack, not per pellet).
        if (p == 0 && T_Roll100() <= st.crit_chance)
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
        S_StartSound(player->mo, sfx_pistol);
        // Headshot modifier is consumed even on a miss.
        turnctrl.headshot_mod = false;
        return false;
    }

    t_last_hit = 1;
    t_last_damage = total_dmg;

    // Headshot modifier is consumed by the attack (hit or miss).
    turnctrl.headshot_mod = false;

    // Phase 8: lifetime counters.
    T_CountDamage(total_dmg);
    // Check if the target died (kill credit).
    // Note: P_DamageMobj is called below; we check after.
    if (t_last_crit)
        player->message = "CRITICAL HIT!";
    else
        player->message = "HIT!";

    S_StartSound(player->mo, sfx_pistol);
    P_DamageMobj(target, player->mo, player->mo, total_dmg);
    // Phase 8: kill credit (check after damage).
    if (target->health <= 0)
    {
        // Was this a headshot? turnctrl.headshot_mod was just cleared,
        // so we need to track it. For now, use t_last_crit as proxy?
        // Actually, headshot is a modifier, not necessarily a crit.
        // We'll pass false for headshot (refine later) and check overwatch.
        T_CountKill(target, false, T_InOverwatchReaction());
    }

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
            P_DamageMobj(mo, player->mo, player->mo, sdmg);
        }
        player->message = "SPLASH HIT!";
    }

    // Start the kit cooldown.
    if (kit->cooldown > 0)
        T_KitSetCooldown(player->readyweapon, kit->cooldown);

    return true;
}
