// d_rlenv.c - RL environment mode (-rlenv). See d_rlenv.h and
// agent/training/SPEC.md.

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
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
#include "tables.h"
#include "t_turn.h"
#include "t_combat.h"
#include "d_diablo.h"

#include "d_rlenv.h"

boolean rlenv = false;

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

    if (M_CheckParm("-rlenv") <= 0)
        return;
    rlenv = true;

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

    // Fixed tics, as fast as possible: one tic per TryRunTics call,
    // never waiting on the wall clock.
    singletics = true;
    // ENDOOM waits for a keypress nobody will send.
    show_endoom = 0;

    printf("[RL] rlenv on: cmdpipe %s, state %s\n", rl_cmdpipe,
           rl_statefd >= 0 ? "pipe" : "stdout");
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

    if (!rlenv)
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
    }
    RL_PUT("}");
#undef RL_PUT

    if (len >= (int) sizeof(buf))
        len = (int) sizeof(buf) - 1;
    buf[len] = '\0';
    RL_Emit("%s", buf);
}
