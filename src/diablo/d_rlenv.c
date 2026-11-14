// d_rlenv.c - RL environment mode (-rlenv). See d_rlenv.h and
// agent/training/SPEC.md.

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "doomtype.h"
#include "doomstat.h"
#include "d_player.h"
#include "i_video.h"
#include "m_argv.h"
#include "d_loop.h"
#include "p_local.h"
#include "r_state.h"
#include "r_main.h"
#include "m_misc.h"
#include "tables.h"
#include "t_turn.h"
#include "t_combat.h"
#include "d_diablo.h"

#include "d_rlenv.h"

boolean rlenv = false;
boolean rldrive = false;

static const char *rl_cmdpipe = "/tmp/doom_cmd.txt";
static int rl_statefd = -1;     // -1 = stdout with "[RL] " prefix

// Frame segment layout (little-endian, all uint32 in the header):
//   0  magic 'DDFB' (0x42464444)
//   4  version (1)
//   8  width (320)      12 height (200)
//  16  seq: odd while a frame is being written, even when complete;
//      bumped by 2 per published frame
//  20  gametic of the frame
//  24..63 reserved
//  64  pixels, width*height palette indices, row-major
//  64+w*h  palette, 256 RGB triples (768 bytes), as displayed
#define RL_SHM_HDR   64
#define RL_SHM_PIX   (SCREENWIDTH * SCREENHEIGHT)
#define RL_SHM_SIZE  (RL_SHM_HDR + RL_SHM_PIX + 768)
static byte *rl_shm = NULL;
static uint32_t rl_frame_seq = 0;

void RL_Init(void)
{
    extern int show_endoom;
    int p;

    if (M_CheckParm("-rlenv") > 0)
        rlenv = true;
    // -rlenv implies -rldrive: control and output are common to both.
    if (rlenv || M_CheckParm("-rldrive") > 0)
        rldrive = true;
    if (!rldrive)
        return;

    p = M_CheckParmWithArgs("-cmdpipe", 1);
    if (p > 0)
        rl_cmdpipe = myargv[p + 1];

    p = M_CheckParmWithArgs("-statepipe", 1);
    if (p > 0)
    {
        const char *path = myargv[p + 1];
        struct stat st;
        // A dead reader must not kill the game with SIGPIPE: writes
        // then fail with EPIPE and RL_Emit stops writing.
        signal(SIGPIPE, SIG_IGN);
        if (stat(path, &st) == 0 && S_ISFIFO(st.st_mode))
        {
            // Blocking open: waits until the env opens the read end.
            printf("[RL] waiting for reader on state pipe %s\n", path);
            fflush(stdout);
            rl_statefd = open(path, O_WRONLY);
        }
        else
        {
            rl_statefd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        }
        if (rl_statefd < 0)
            printf("[RL] cannot open state pipe %s: %s (using stdout)\n",
                   path, strerror(errno));
    }

    p = M_CheckParmWithArgs("-shm", 1);
    if (p > 0)
    {
        const char *name = myargv[p + 1];
        int fd = shm_open(name, O_CREAT | O_RDWR, 0600);
        if (fd < 0 || ftruncate(fd, RL_SHM_SIZE) != 0)
        {
            printf("[RL] cannot create shm %s: %s\n", name, strerror(errno));
        }
        else
        {
            void *m = mmap(NULL, RL_SHM_SIZE, PROT_READ | PROT_WRITE,
                           MAP_SHARED, fd, 0);
            if (m == MAP_FAILED)
                printf("[RL] cannot map shm %s: %s\n", name, strerror(errno));
            else
            {
                uint32_t *h = (uint32_t *) m;
                rl_shm = (byte *) m;
                memset(rl_shm, 0, RL_SHM_HDR);
                h[0] = 0x42464444;      // 'DDFB'
                h[1] = 1;
                h[2] = SCREENWIDTH;
                h[3] = SCREENHEIGHT;
            }
        }
        if (fd >= 0)
            close(fd);
    }

    if (rlenv)
    {
        // Fixed tics, as fast as possible: one tic per TryRunTics call,
        // never waiting on the wall clock.
        singletics = true;
        // ENDOOM waits for a keypress nobody will send.
        show_endoom = 0;
        // -nodraw (vanilla reads it only under -timedemo): no D_Display
        // and no I_FinishUpdate at all. Game logic is unaffected, but the
        // published frame and any replay capture are never drawn.
        if (M_ParmExists("-nodraw"))
        {
            nodrawers = true;
            printf("[RL] -nodraw: rendering off (frames and capture are not drawn)\n");
        }
    }

    printf("[RL] %s on: cmdpipe %s, state %s\n", rlenv ? "rlenv" : "rldrive",
           rl_cmdpipe, rl_statefd >= 0 ? "pipe" : "stdout");
}

const char *RL_CmdPipePath(void)
{
    return rl_cmdpipe;
}

void RL_Emit(const char *fmt, ...)
{
    char buf[4096];
    va_list args;
    int n;

    if (!rldrive)
        return;

    va_start(args, fmt);
    n = vsnprintf(buf, sizeof(buf) - 1, fmt, args);
    va_end(args);
    if (n < 0)
        return;
    if (n > (int)sizeof(buf) - 2)
        n = (int)sizeof(buf) - 2;
    buf[n++] = '\n';
    buf[n] = '\0';

    if (rl_statefd < 0)
    {
        printf("[RL] %s", buf);
        fflush(stdout);
        return;
    }
    {
        // One write per line: lines under PIPE_BUF (4096) are atomic.
        const char *s = buf;
        while (n > 0)
        {
            ssize_t w = write(rl_statefd, s, n);
            if (w < 0)
            {
                if (errno == EINTR)
                    continue;
                printf("[RL] state pipe write failed: %s (using stdout)\n",
                       strerror(errno));
                close(rl_statefd);
                rl_statefd = -1;
                return;
            }
            s += w;
            n -= (int)w;
        }
    }
}

void RL_PublishFrame(void)
{
    volatile uint32_t *h;

    if (rl_shm == NULL || I_VideoBuffer == NULL)
        return;
    h = (volatile uint32_t *) rl_shm;
    h[4] = ++rl_frame_seq;              // odd: writing
    __sync_synchronize();
    memcpy(rl_shm + RL_SHM_HDR, I_VideoBuffer, RL_SHM_PIX);
    I_GetPaletteRGB(rl_shm + RL_SHM_HDR + RL_SHM_PIX);
    h[5] = (uint32_t) gametic;
    __sync_synchronize();
    h[4] = ++rl_frame_seq;              // even: complete
}

static const char *RL_ModeName(void)
{
    static const char *names[] = {
        "OFF", "EXPLORE", "PLANNING", "TARGETING", "CONFIRM",
        "PULSE", "REACTION", "ROUND_END"
    };
    switch (gamestate)
    {
      case GS_LEVEL:
        break;
      case GS_INTERMISSION:
        return "INTERMISSION";
      case GS_FINALE:
        return "FINALE";
      default:
        return "DEMOSCREEN";
    }
    if (turnctrl.state >= 0 && turnctrl.state <= TS_ROUND_END)
        return names[turnctrl.state];
    return "?";
}

// Angle in whole degrees, 0..359 (Doom convention: counterclockwise
// from east).
static int RL_AngleDeg(angle_t a)
{
    return (int) (((uint64_t) a * 360) >> 32);
}

static int rl_bpfull_tic = -1000000;   // gametic of the last failed pickup on a full backpack

void RL_NoteBackpackFull(void)
{
    rl_bpfull_tic = gametic;
}

void RL_EmitState(const char *why, int tics)
{
    char buf[3600];
    int len = 0;
    player_t *p = &players[consoleplayer];
    mobj_t *mo = p->mo;
    boolean level = (gamestate == GS_LEVEL && mo != NULL);
    int i, n;
    boolean first = true;

#define RL_PUT(...) \
    do { \
        if (len < (int) sizeof(buf)) \
            len += snprintf(buf + len, sizeof(buf) - len, __VA_ARGS__); \
    } while (0)

    RL_PUT("{\"event\":\"%s\",\"gametic\":%d,\"tics\":%d,\"mode\":\"%s\","
           "\"episode\":%d,\"map\":%d,\"frame\":%u",
           why, gametic, tics, RL_ModeName(), gameepisode, gamemap,
           (unsigned) rl_frame_seq);
    if (level)
    {
        weapontype_t w = p->readyweapon;
        RL_PUT(",\"hp\":%d,\"dead\":%s,\"armor\":%d,"
               "\"x\":%d,\"y\":%d,\"z\":%d,\"angle\":%d,"
               "\"tp\":%d,\"tp_max\":%d,\"qlen\":%d,"
               "\"weapon\":%d,\"ammo\":%d,\"ammo_max\":%d,"
               "\"kills\":%d,\"sectors\":%d",
               p->health,
               (p->playerstate == PST_DEAD || p->health <= 0) ? "true" : "false",
               D_EquippedDurabilitySum(p),
               mo->x >> FRACBITS, mo->y >> FRACBITS, mo->z >> FRACBITS,
               RL_AngleDeg(mo->angle),
               turnctrl.tp, turnctrl.tp_max, turnctrl.queue_len,
               (int) w, T_KitAmmo(w), T_KitMaxAmmo(w),
               p->killcount, T_VisitedCount());

        // Up to 9 targets from the turn controller's list (what the
        // player can see), in list order.
        RL_PUT(",\"targets\":[");
        n = T_NumTargets();
        if (n > 9)
            n = 9;
        for (i = 0; i < n; i++)
        {
            mobj_t *t = T_TargetMobj(i);
            int dist, rel;
            if (t == NULL)
                continue;
            dist = P_AproxDistance(t->x - mo->x, t->y - mo->y) >> FRACBITS;
            rel = RL_AngleDeg(R_PointToAngle2(mo->x, mo->y, t->x, t->y)
                              - mo->angle);
            if (rel > 180)
                rel -= 360;
            RL_PUT("%s{\"type\":%d,\"hp\":%d,\"dist\":%d,\"rel\":%d}",
                   first ? "" : ",", (int) t->type, t->health, dist, rel);
            first = false;
        }
        RL_PUT("]");

        // CONSULT triggers (SPEC.md §5.3). monstervis: a living monster
        // on screen (line of sight, within the 90-degree view) while exploring -- enemies are frozen in
        // explore and an acquisition starts combat at once, so it has
        // not acquired the player (barrels are targets but never count).
        // backpack_full: a pickup failed on a full backpack within the
        // last 4 s (the message's lifetime) and it is still full.
        {
            boolean vis = false;
            if (T_InExplore())
            {
                n = T_NumTargets();
                for (i = 0; i < n && !vis; i++)
                {
                    mobj_t *t = T_TargetMobj(i);
                    int rel;
                    if (t == NULL || !(t->flags & MF_COUNTKILL) || t->health <= 0)
                        continue;
                    // On screen: within the 90-degree view (the target
                    // list is line of sight in every direction).
                    rel = RL_AngleDeg(R_PointToAngle2(mo->x, mo->y, t->x, t->y)
                                      - mo->angle);
                    vis = rel <= 45 || rel >= 315;
                }
            }
            RL_PUT(",\"monstervis\":%s,\"backpack_full\":%s,\"bp_count\":%d",
                   vis ? "true" : "false",
                   (gametic - rl_bpfull_tic < 4 * TICRATE && D_BackpackFull(p))
                       ? "true" : "false",
                   p->diablo_bp_count);
        }
    }
    RL_PUT("}");
#undef RL_PUT

    if (len >= (int) sizeof(buf))
        len = (int) sizeof(buf) - 1;
    buf[len] = '\0';
    RL_Emit("%s", buf);
}

// ------------------------------------------------------------------
// Mechanism use reporting (see d_rlenv.h). RL_UseBegin snapshots which
// sectors already have an active special; RL_UseEnd reports the ones
// that gained one during the use, plus the switch lines flipped.
// ------------------------------------------------------------------
#define RL_USE_MAX_LINES 16

static byte *rl_use_busy = NULL;   // per sector: specialdata set at RL_UseBegin
static int rl_use_busy_cap = 0;
static boolean rl_using = false;
static int rl_use_lines[RL_USE_MAX_LINES];
static int rl_use_nlines = 0;

void RL_UseBegin(void)
{
    int i;

    if (!rldrive || numsectors <= 0)
        return;
    if (rl_use_busy_cap < numsectors)
    {
        free(rl_use_busy);
        rl_use_busy = malloc(numsectors);
        rl_use_busy_cap = rl_use_busy ? numsectors : 0;
        if (rl_use_busy == NULL)
            return;
    }
    for (i = 0; i < numsectors; i++)
        rl_use_busy[i] = sectors[i].specialdata != NULL;
    rl_use_nlines = 0;
    rl_using = true;
}

void RL_NoteSwitch(int line)
{
    if (rl_using && rl_use_nlines < RL_USE_MAX_LINES)
        rl_use_lines[rl_use_nlines++] = line;
}

void RL_UseEnd(void)
{
    char buf[2048];
    int len, i, nsec = 0;

    if (!rl_using)
        return;
    rl_using = false;
    len = snprintf(buf, sizeof(buf), "{\"event\":\"use\",\"sectors\":[");
    for (i = 0; i < numsectors; i++)
    {
        if (rl_use_busy[i] || sectors[i].specialdata == NULL)
            continue;
        if (len < (int) sizeof(buf) - 64)
            len += snprintf(buf + len, sizeof(buf) - len, "%s%d", nsec ? "," : "", i);
        nsec++;
    }
    if (nsec == 0 && rl_use_nlines == 0)
        return;                     // the use did nothing
    len += snprintf(buf + len, sizeof(buf) - len, "],\"lines\":[");
    for (i = 0; i < rl_use_nlines; i++)
        len += snprintf(buf + len, sizeof(buf) - len, "%s%d", i ? "," : "", rl_use_lines[i]);
    snprintf(buf + len, sizeof(buf) - len, "],\"gametic\":%d}", gametic);
    RL_Emit("%s", buf);
}


// ---------------------------------------------------------------------
// CONSULT practice monsters and the CONSULT command (see d_rlenv.h).
// ---------------------------------------------------------------------

#define RL_SPAWN_MAX     4
#define RL_SPAWN_FORGET  (5 * TICRATE)  // out of sight this long: removed
#define RL_SPAWN_TRIES   16             // directions tried in view, then all around
#define RL_SPAWN_NEAR    128            // spawn distance range, map units
#define RL_SPAWN_FAR     640

static int rl_spawn_period = -1;        // -rlspawn; -1 unparsed, 0 off
static uint32_t rl_spawn_rng = 1;
static mobj_t *rl_spawned[RL_SPAWN_MAX];
static int rl_spawned_unseen[RL_SPAWN_MAX];

static uint32_t RL_SpawnRand(void)
{
    rl_spawn_rng = rl_spawn_rng * 1664525u + 1013904223u;
    return rl_spawn_rng >> 8;
}

void RL_SpawnSeed(int n)
{
    rl_spawn_rng = 0x9e3779b9u ^ (uint32_t) n;
    RL_SpawnRand();
}

void RL_LevelSetup(void)
{
    memset(rl_spawned, 0, sizeof(rl_spawned));
}

// A tracked pointer is used only after finding it in the thinker list
// (a removed mobj's memory may already be gone or reused).
static boolean RL_IsLive(mobj_t *mo)
{
    thinker_t *th;
    for (th = thinkercap.next; th != &thinkercap; th = th->next)
        if (th == &mo->thinker)
            return th->function.acp1 == (actionf_p1) P_MobjThinker;
    return false;
}

static int RL_SpawnedIndex(mobj_t *mo)
{
    int i;
    for (i = 0; i < RL_SPAWN_MAX; i++)
        if (rl_spawned[i] == mo)
            return i;
    return -1;
}

static boolean RL_TrySpawn(mobj_t *pmo)
{
    static const mobjtype_t types[] = { MT_POSSESSED, MT_SHOTGUY, MT_TROOP, MT_SERGEANT };
    int slot, attempt;

    for (slot = 0; slot < RL_SPAWN_MAX && rl_spawned[slot] != NULL; slot++)
        ;
    if (slot == RL_SPAWN_MAX)
        return false;
    // RL_SPAWN_TRIES directions within 30 degrees of the view, then as
    // many in any direction: a player that WALKed until blocked often
    // faces a wall, and a monster in sight beside or behind it comes on
    // screen when it turns. A point that is blocked or out of sight is
    // walked back toward the player, down to RL_SPAWN_NEAR units, before
    // trying the next direction.
    for (attempt = 0; attempt < 2 * RL_SPAWN_TRIES; attempt++)
    {
        int dist = RL_SPAWN_NEAR + (int) (RL_SpawnRand() % (RL_SPAWN_FAR - RL_SPAWN_NEAR + 1));
        int deg = attempt < RL_SPAWN_TRIES ? (int) (RL_SpawnRand() % 61) - 30
                                           : (int) (RL_SpawnRand() % 360) - 180;
        angle_t an = pmo->angle + (angle_t) ((int64_t) deg * (ANG90 / 90));
        mobjtype_t type = types[RL_SpawnRand() % (sizeof(types) / sizeof(types[0]))];

        for (; dist >= RL_SPAWN_NEAR; dist = dist * 3 / 4)
        {
            fixed_t x = pmo->x + dist * finecosine[an >> ANGLETOFINESHIFT];
            fixed_t y = pmo->y + dist * finesine[an >> ANGLETOFINESHIFT];
            mobj_t *mo = P_SpawnMobj(x, y, ONFLOORZ, type);

            if (!P_CheckPosition(mo, x, y) || !P_CheckSight(pmo, mo)
                || mo->subsector->sector->ceilingheight - mo->floorz < mo->height)
            {
                P_RemoveMobj(mo);
                continue;
            }
            mo->angle = R_PointToAngle2(pmo->x, pmo->y, x, y);  // back to the player
            mo->flags |= MF_AMBUSH;                              // deaf
            rl_spawned[slot] = mo;
            rl_spawned_unseen[slot] = 0;
            printf("[RL] spawned type %d at (%d,%d), %d units, %d deg\n",
                   (int) type, x >> FRACBITS, y >> FRACBITS, dist, deg);
            T_RefreshTargets();
            return true;
        }
    }
    return false;
}

void RL_SpawnTick(void)
{
    player_t *pl = &players[consoleplayer];
    int i, live = 0;

    if (rl_spawn_period < 0)
    {
        int p = M_CheckParmWithArgs("-rlspawn", 1);
        rl_spawn_period = (rlenv && p > 0) ? atoi(myargv[p + 1]) : 0;
        if (rl_spawn_period < 0)
            rl_spawn_period = 0;
    }
    if (rl_spawn_period == 0 || gamestate != GS_LEVEL || pl->mo == NULL
        || pl->health <= 0 || !T_InExplore())
        return;

    // Forget dead or vanished ones; remove those long out of sight.
    for (i = 0; i < RL_SPAWN_MAX; i++)
    {
        mobj_t *mo = rl_spawned[i];
        if (mo == NULL)
            continue;
        if (!RL_IsLive(mo) || mo->health <= 0)
        {
            rl_spawned[i] = NULL;
            continue;
        }
        if (P_CheckSight(pl->mo, mo))
            rl_spawned_unseen[i] = 0;
        else if (++rl_spawned_unseen[i] >= RL_SPAWN_FORGET)
        {
            P_RemoveMobj(mo);
            rl_spawned[i] = NULL;
            continue;
        }
        live++;
    }
    if (live == 0 && RL_SpawnRand() % (uint32_t) rl_spawn_period == 0)
        RL_TrySpawn(pl->mo);
}

void RL_Consult(void)
{
    player_t *pl = &players[consoleplayer];
    thinker_t *th, *next;
    boolean combat;
    int removed = 0, killed = 0;

    if (pl->mo == NULL || pl->health <= 0)
        return;
    combat = !T_InExplore();
    for (th = thinkercap.next; th != &thinkercap; th = next)
    {
        mobj_t *mo;
        int si;
        next = th->next;
        if (th->function.acp1 != (actionf_p1) P_MobjThinker)
            continue;
        mo = (mobj_t *) th;
        if (!(mo->flags & MF_COUNTKILL) || mo->health <= 0 || mo->player != NULL)
            continue;
        if (!P_CheckSight(pl->mo, mo) && !(combat && mo->target == pl->mo))
            continue;
        si = RL_SpawnedIndex(mo);
        if (si >= 0 && !combat)
        {
            // Explore: nothing else holds it (the target list is
            // refreshed below, health bars validate against thinkers).
            rl_spawned[si] = NULL;
            P_RemoveMobj(mo);
            removed++;
        }
        else
        {
            // Combat may hold it (spotter, selection): kill it, the
            // corpse stays a valid mobj. No source: the player-damage
            // path would swap in a pending turn-attack damage override,
            // and a sourceless kill still counts (vanilla's
            // single-player branch).
            if (si >= 0)
                rl_spawned[si] = NULL;
            P_DamageMobj(mo, NULL, NULL, mo->health + 1000);
            killed++;
        }
    }
    printf("[RL] CONSULT: %d spawned removed, %d killed\n", removed, killed);
    T_RefreshTargets();
}

