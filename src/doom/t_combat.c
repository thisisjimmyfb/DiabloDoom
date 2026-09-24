// t_combat.c — turn-based combat stats and resolution (Phase 4).

#include "t_combat.h"
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

    // Energy feeds ability power.
    out->ap = ene * 2;

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
    out->haste = ene / 5;
    if (out->haste > 40)
        out->haste = 40;
}

// Simple cover check: is there a wall close behind the target relative
// to the attacker? Traces from the attacker toward the target and past
// it; if the trace hits a wall shortly beyond the target, the target
// is backed against cover.
static boolean T_TargetInCover(player_t *player, mobj_t *target)
{
    // Offset probes around the target: if any cardinal neighbor cell is
    // solid while the target's own cell is open, count as cover.
    // Cheap and deterministic; uses P_CheckPosition.
    fixed_t x = target->x, y = target->y;
    fixed_t r = target->radius + 8 * FRACUNIT;
    mobj_t *mo = player->mo;

    if (!mo)
        return false;

    // Only the side facing the attacker matters, but a cheap 4-way
    // probe is fine for a binary cover flag.
    if (!P_CheckPosition(mo, x + r, y))
        return true;
    if (!P_CheckPosition(mo, x - r, y))
        return true;
    if (!P_CheckPosition(mo, x, y + r))
        return true;
    if (!P_CheckPosition(mo, x, y - r))
        return true;
    return false;
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

    // Cover.
    if (T_TargetInCover(player, target))
        chance -= 15;

    if (chance < 5)
        chance = 5;
    if (chance > 95)
        chance = 95;
    return chance;
}

void T_DamageRange(player_t *player, mobj_t *target,
                   const t_combatstats_t *st, int *minhp, int *maxhp)
{
    (void)player;
    (void)target;
    *minhp = st->ad_min;
    *maxhp = st->ad_max;
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

boolean T_ResolveAttack(player_t *player, mobj_t *target)
{
    t_combatstats_t st;
    int chance, roll, dmg, range;

    t_last_hit = 0;
    t_last_damage = 0;
    t_last_crit = 0;

    if (!player->mo || !target || target->health <= 0)
        return false;

    T_DeriveStats(player, &st);

    // Hit roll.
    chance = T_HitChance(player, target, &st);
    roll = T_Roll100();
    if (roll > chance)
    {
        player->message = "MISS!";
        S_StartSound(player->mo, sfx_pistol);
        return false;
    }

    // Damage roll inside the previewed range.
    range = st.ad_max - st.ad_min + 1;
    dmg = st.ad_min + (range > 1 ? T_Roll(range) : 0);

    // Crit.
    if (T_Roll100() <= st.crit_chance)
    {
        dmg = dmg * st.crit_mult / 100;
        t_last_crit = 1;
    }

    // Target armor mitigation (physical).
    dmg -= T_MonsterArmor(target->type);
    if (dmg < 1)
        dmg = 1;

    t_last_hit = 1;
    t_last_damage = dmg;

    if (t_last_crit)
        player->message = "CRITICAL HIT!";
    else
        player->message = "HIT!";

    S_StartSound(player->mo, sfx_pistol);
    P_DamageMobj(target, player->mo, player->mo, dmg);
    return true;
}
