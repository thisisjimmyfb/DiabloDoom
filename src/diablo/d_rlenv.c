// d_rlenv.c - RL environment mode (-rlenv). See d_rlenv.h and
// agent/training/SPEC.md.

#include <errno.h>
#include <math.h>
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
#include "p_inter.h"
#include "i_timer.h"

#include "d_rlenv.h"
#include "d_waypoint.h"

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

static int rl_fast_speed = -1;          // -rlfast (map units per tic); -1 unparsed, 0 off
static int rl_fast_tics;                // tics at or over it since the last decision
static mobj_t *rl_move_mo;              // the player mobj at the last explore tic
static fixed_t rl_move_x, rl_move_y;
static int rl_move_tic = -1;

static int rl_bpfull_tic = -1000000;   // gametic of the last failed pickup on a full backpack

void RL_NoteBackpackFull(void)
{
    rl_bpfull_tic = gametic;
}

// The point of the segment v1-v2 closest to (px,py), in map units.
static void RL_SegClosest(double px, double py, const line_t *ld, double *cx, double *cy)
{
    double ax = ld->v1->x / (double) FRACUNIT, ay = ld->v1->y / (double) FRACUNIT;
    double bx = ld->v2->x / (double) FRACUNIT, by = ld->v2->y / (double) FRACUNIT;
    double dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy, t = 0;
    if (len2 > 0)
    {
        t = ((px - ax) * dx + (py - ay) * dy) / len2;
        t = t < 0 ? 0 : t > 1 ? 1 : t;
    }
    *cx = ax + t * dx;
    *cy = ay + t * dy;
}

// Distance from (px,py) to the segment v1-v2, in map units.
static double RL_SegDist(double px, double py, const line_t *ld)
{
    double cx, cy;
    RL_SegClosest(px, py, ld, &cx, &cy);
    return sqrt((cx - px) * (cx - px) + (cy - py) * (cy - py));
}

static boolean RL_IsExitLine(const line_t *ld)
{
    // S1 / W1 exit and secret exit (vanilla specials).
    return ld->special == 11 || ld->special == 52
        || ld->special == 51 || ld->special == 124;
}

// A frontier portal: a two-sided line from a visited sector into an
// unvisited one that the player could pass -- a step up of at most 24
// units and room to stand (56) -- or that has a special of its own (a
// Door and lift sectors (2026-11-17): a Doom door is a small sector whose
// ceiling moves, a lift one whose floor moves. Found from the line
// specials that move them: a manual door's line moves its back sector,
// a remote door or lift trigger moves the sectors with its tag. Built
// once per level, on first use.
#define RL_DOOR 1
#define RL_LIFT 2
static byte *rl_passage;
static sector_t *rl_passage_for;
static int rl_passage_n;
static boolean rl_passage_dirty = true;

// 1 manual door (moves the line's back sector), 2 remote door, 3 lift
// (both move the tagged sectors), 0 neither.
static int RL_PassageSpecial(int special)
{
    switch (special)
    {
      case 1: case 26: case 27: case 28: case 31: case 32: case 33: case 34:
      case 117: case 118:
        return 1;
      case 2: case 3: case 4: case 16: case 29: case 42: case 46: case 50:
      case 61: case 63: case 75: case 76: case 86: case 90: case 99: case 103:
      case 105: case 106: case 107: case 108: case 109: case 110: case 111:
      case 112: case 113: case 114: case 115: case 116:
      case 133: case 134: case 135: case 136: case 137:
        return 2;
      case 10: case 21: case 62: case 88: case 120: case 121: case 122: case 123:
        return 3;
      default:
        return 0;
    }
}

// RL_DOOR, RL_LIFT or 0 for a sector.
static int RL_Passage(const sector_t *sec)
{
    int i, t;
    if (rl_passage_dirty || rl_passage_for != sectors || rl_passage_n != numsectors)
    {
        free(rl_passage);
        rl_passage = calloc(numsectors > 0 ? numsectors : 1, 1);
        rl_passage_for = sectors;
        rl_passage_n = numsectors;
        rl_passage_dirty = false;
        for (i = 0; i < numlines; i++)
        {
            line_t *li = &lines[i];
            int k = RL_PassageSpecial(li->special);
            if (k == 1 && li->backsector != NULL)
                rl_passage[li->backsector - sectors] = RL_DOOR;
            else if (k > 1 && li->tag != 0)     // tag 0 would match every untagged sector
                for (t = -1; (t = P_FindSectorFromLineTag(li, t)) >= 0;)
                    if (rl_passage[t] != RL_DOOR)
                        rl_passage[t] = k == 2 ? RL_DOOR : RL_LIFT;
        }
    }
    return sec != NULL && rl_passage != NULL ? rl_passage[sec - sectors] : 0;
}

// Whether the player holds the key a locked door special needs (true for
// any other special).
static boolean RL_HasKeyFor(int special)
{
    player_t *p = &players[consoleplayer];
    switch (special)
    {
      case 26: case 32: case 99: case 133:
        return p->cards[it_bluecard] || p->cards[it_blueskull];
      case 27: case 34: case 136: case 137:
        return p->cards[it_yellowcard] || p->cards[it_yellowskull];
      case 28: case 33: case 134: case 135:
        return p->cards[it_redcard] || p->cards[it_redskull];
      default:
        return true;
    }
}

// A portal through a door or onto a lift: either side is one.
static boolean RL_PassagePortal(const line_t *ld)
{
    return RL_Passage(ld->frontsector) != 0 || RL_Passage(ld->backsector) != 0;
}

// door or lift opened from it). Blocking lines never count.
static boolean RL_IsFrontier(const line_t *ld)
{
    sector_t *a = ld->frontsector, *b = ld->backsector;
    boolean va, vb;
    fixed_t top, bottom;
    if (b == NULL || a == NULL || (ld->flags & ML_BLOCKING))
        return false;
    va = T_SectorVisited(a - sectors);
    vb = T_SectorVisited(b - sectors);
    if (va == vb)
        return false;
    if (!va)
    {
        sector_t *t = a; a = b; b = t;          // a visited, b not
    }
    if (ld->special != 0)
        return true;
    top = a->ceilingheight < b->ceilingheight ? a->ceilingheight : b->ceilingheight;
    bottom = a->floorheight > b->floorheight ? a->floorheight : b->floorheight;
    return b->floorheight - a->floorheight <= 24 * FRACUNIT
        && top - bottom >= 56 * FRACUNIT;
}

// A portal of sector s (2026-11-16): a two-sided line between s and
// another sector, visited or not, that the player could pass from s --
// the same test as a frontier: a step up of at most 24 units and room to
// stand (56), or a special of its own (a door or lift opened from it).
// Blocking lines never count.
static boolean RL_IsPortalOf(const line_t *ld, const sector_t *s)
{
    const sector_t *b;
    fixed_t top, bottom;
    if (ld->frontsector == NULL || ld->backsector == NULL || (ld->flags & ML_BLOCKING))
        return false;
    if (ld->frontsector == s)
        b = ld->backsector;
    else if (ld->backsector == s)
        b = ld->frontsector;
    else
        return false;
    if (b == s)
        return false;
    if (ld->special != 0)
        return true;
    top = s->ceilingheight < b->ceilingheight ? s->ceilingheight : b->ceilingheight;
    bottom = s->floorheight > b->floorheight ? s->floorheight : b->floorheight;
    return b->floorheight - s->floorheight <= 24 * FRACUNIT
        && top - bottom >= 56 * FRACUNIT;
}

// A line the player could USE: one with a special (a locked door only
// with its key, 2026-11-17), a switch already pressed (a one-shot switch
// loses its special but keeps a switch texture), or (2026-11-17) the
// face of a door sector (a remote door: USE doesn't open it, but it is
// no random press).
extern int switchlist[];
extern int numswitches;
static boolean RL_UsableLine(line_t *li)
{
    side_t *sd = &sides[li->sidenum[0]];
    int i;
    if (li->special != 0)
        return RL_HasKeyFor(li->special);
    if (li->frontsector != NULL && li->backsector != NULL
        && (RL_Passage(li->frontsector) == RL_DOOR || RL_Passage(li->backsector) == RL_DOOR))
        return true;
    for (i = 0; i < numswitches * 2; i++)
        if (switchlist[i] == sd->toptexture || switchlist[i] == sd->midtexture
            || switchlist[i] == sd->bottomtexture)
            return true;
    return false;
}

// wall_dist (2026-11-16): the distance along the facing to the first line
// the player can't pass -- one-sided, blocking, or an opening too low or
// a step too high -- within RL_WALL_RANGE; -1 when none. The env's
// wall_look penalty (staring at a wall up close). Reward shaping only.
#define RL_WALL_RANGE 128
static mobj_t *rl_wall_mo;
static int rl_wall_dist;
static boolean rl_wall_special;    // the line the trace stopped at is usable
static int rl_wall_line;           // the line the trace stopped at, -1 none

static boolean RL_WallTraverse(intercept_t *in)
{
    line_t *li;
    if (!in->isaline)
        return true;
    li = in->d.line;
    if (li->backsector != NULL && !(li->flags & ML_BLOCKING))
    {
        P_LineOpening(li);
        if (openrange >= 56 * FRACUNIT && openbottom - rl_wall_mo->z <= 24 * FRACUNIT)
            return true;                    // passable: look on through it
    }
    rl_wall_dist = FixedMul(in->frac, RL_WALL_RANGE * FRACUNIT) >> FRACBITS;
    rl_wall_line = (int) (li - lines);
    rl_wall_special = RL_UsableLine(li);
    return false;
}

static int RL_WallDist(mobj_t *mo)
{
    int a = mo->angle >> ANGLETOFINESHIFT;
    rl_wall_mo = mo;
    rl_wall_dist = -1;
    rl_wall_line = -1;
    rl_wall_special = false;
    P_PathTraverse(mo->x, mo->y,
                   mo->x + RL_WALL_RANGE * finecosine[a], mo->y + RL_WALL_RANGE * finesine[a],
                   PT_ADDLINES, RL_WallTraverse);
    return rl_wall_dist;
}

// wall_use: a usable wall in reach -- a line with a special that the
// player can't walk through by the wall trace's own rule (a switch, a
// closed or still half-open door, a pressed one-shot switch (2026-11-17);
// walk-over triggers are passable and
// don't count) within USE range (64) of the player and within 45 degrees
// of the facing. The env never charges wall_look then, so lining up on a
// switch or waiting at a door as it opens costs nothing.
static boolean RL_WallUse(mobj_t *mo)
{
    double px = mo->x / (double) FRACUNIT, py = mo->y / (double) FRACUNIT;
    int i;
    for (i = 0; i < numlines; i++)
    {
        line_t *li = &lines[i];
        double cx, cy, rel;
        if (!RL_UsableLine(li))
            continue;
        if (li->backsector != NULL && !(li->flags & ML_BLOCKING))
        {
            P_LineOpening(li);
            if (openrange >= 56 * FRACUNIT && openbottom - mo->z <= 24 * FRACUNIT)
                continue;                   // passable: a walk-over line, an open door
        }
        RL_SegClosest(px, py, li, &cx, &cy);
        if ((cx - px) * (cx - px) + (cy - py) * (cy - py) > 64.0 * 64.0)
            continue;
        rel = atan2(cy - py, cx - px) * 180.0 / M_PI - RL_AngleDeg(mo->angle);
        while (rel > 180.0)
            rel -= 360.0;
        while (rel <= -180.0)
            rel += 360.0;
        if (rel >= -45.0 && rel <= 45.0)
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------
// Path guidance (2026-11-17): path_dist / path_rel on the state line,
// the walking distance and direction to the nearest unvisited room, so
// the env's time penalty and look reward follow corridors around bends
// instead of pointing through walls. Reward shaping only.
//
// The graph: 32-unit cells over the map's bounding box, 8 directions,
// by the GOTO pathfinder's rules (t_action.c): the player fits at a
// cell's centre (P_CheckPosition, and the full height between floor and
// ceiling), never climbs more than 24 units from one cell to the next,
// and the segment between the centres is clear at its quarter points.
// It is built once per level (RL_LevelSetup, so after each LOAD), with
// every door sector's ceiling raised to its open height for the build,
// and each edge remembers the door sector it crosses, if any.
//
// The field: Dijkstra from every cell in an unvisited sector that is
// not a door (a door's own sector never pays, rooms()), over edges whose
// door is open now or one the player can open (a manual door, a locked
// one only with its key). Recomputed only when the visited set, the
// keys or a door's open state changes. Movers other than doors (lifts,
// floors) are taken as they were at the build.
// ---------------------------------------------------------------------

#define RL_CELL      32
#define RL_PATH_AIM  4              // cells ahead along the path: path_rel's aim
#define RL_PATH_AHEAD 6             // cells ahead along the path: path_ahead (training waypoints)
static const int rl_dx[8] = { 1, 1, 0, -1, -1, -1, 0, 1 };
static const int rl_dy[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
static const int rl_cost[8] = { 32, 45, 32, 45, 32, 45, 32, 45 };

static int rl_gw, rl_gh, rl_gx0, rl_gy0;   // grid size; origin, map units
static byte *rl_gedge;          // bit d: the player can step from the cell to neighbour d
static short *rl_gsec;          // the sector at the cell's centre, -1 not standable
static short *rl_gdoor;         // [cell * 8 + d]: the door sector edge d crosses, -1 none
static int *rl_gdist;           // distance to the nearest target, -1 none reachable
static byte *rl_doorok;         // per sector, for the current field: a door the player can pass
static boolean rl_graph_dirty = true;
static sector_t *rl_graph_for;
static unsigned rl_path_sig;
static boolean rl_path_have;

// The player could stand at (x, y): fits, by the pathfinder's rule.
static boolean RL_Stand(mobj_t *mo, fixed_t x, fixed_t y, fixed_t *floor_out)
{
    if (!P_CheckPosition(mo, x, y) || tmceilingz - tmfloorz < mo->height)
        return false;
    *floor_out = tmfloorz;
    return true;
}

static fixed_t RL_CellX(int c) { return (fixed_t) (rl_gx0 + (c % rl_gw) * RL_CELL + RL_CELL / 2) << FRACBITS; }
static fixed_t RL_CellY(int c) { return (fixed_t) (rl_gy0 + (c / rl_gw) * RL_CELL + RL_CELL / 2) << FRACBITS; }

static void RL_BuildGraph(mobj_t *pmo)
{
    int i, c, d, n, minx = 0, miny = 0, maxx = 0, maxy = 0;
    fixed_t *floors, *ceil_saved;
    int t0 = I_GetTimeMS();

    free(rl_gedge); free(rl_gsec); free(rl_gdoor); free(rl_gdist); free(rl_doorok);
    rl_gedge = NULL; rl_gsec = NULL; rl_gdoor = NULL; rl_gdist = NULL; rl_doorok = NULL;
    rl_graph_dirty = false;
    rl_graph_for = sectors;
    rl_path_have = false;
    if (numvertexes <= 0)
        return;
    for (i = 0; i < numvertexes; i++)
    {
        int vx = vertexes[i].x >> FRACBITS, vy = vertexes[i].y >> FRACBITS;
        if (i == 0 || vx < minx) minx = vx;
        if (i == 0 || vx > maxx) maxx = vx;
        if (i == 0 || vy < miny) miny = vy;
        if (i == 0 || vy > maxy) maxy = vy;
    }
    rl_gx0 = minx;
    rl_gy0 = miny;
    rl_gw = (maxx - minx) / RL_CELL + 1;
    rl_gh = (maxy - miny) / RL_CELL + 1;
    n = rl_gw * rl_gh;
    rl_gedge = calloc(n, 1);
    rl_gsec = malloc(n * sizeof(short));
    rl_gdoor = malloc((size_t) n * 8 * sizeof(short));
    rl_gdist = malloc(n * sizeof(int));
    rl_doorok = calloc(numsectors > 0 ? numsectors : 1, 1);
    floors = malloc(n * sizeof(fixed_t));
    ceil_saved = malloc((numsectors > 0 ? numsectors : 1) * sizeof(fixed_t));

    // Every door open for the build: its ceiling at the height it opens to.
    for (i = 0; i < numsectors; i++)
    {
        ceil_saved[i] = sectors[i].ceilingheight;
        if (RL_Passage(&sectors[i]) == RL_DOOR)
        {
            fixed_t open = P_FindLowestCeilingSurrounding(&sectors[i]) - 4 * FRACUNIT;
            if (open > sectors[i].ceilingheight)
                sectors[i].ceilingheight = open;
        }
    }
    for (c = 0; c < n; c++)
    {
        fixed_t x = RL_CellX(c), y = RL_CellY(c);
        rl_gsec[c] = RL_Stand(pmo, x, y, &floors[c])
            ? (short) (R_PointInSubsector(x, y)->sector - sectors) : -1;
    }
    for (c = 0; c < n; c++)
    {
        int cx = c % rl_gw, cy = c / rl_gw;
        for (d = 0; d < 8; d++)
            rl_gdoor[c * 8 + d] = -1;
        if (rl_gsec[c] < 0)
            continue;
        for (d = 0; d < 8; d++)
        {
            int nx = cx + rl_dx[d], ny = cy + rl_dy[d], nc = ny * rl_gw + nx, s;
            fixed_t ax = RL_CellX(c), ay = RL_CellY(c), bx, by, prev = floors[c], f;
            boolean ok = true;
            if (nx < 0 || ny < 0 || nx >= rl_gw || ny >= rl_gh || rl_gsec[nc] < 0)
                continue;
            bx = RL_CellX(nc);
            by = RL_CellY(nc);
            for (s = 1; s <= 4 && ok; s++)
            {
                fixed_t sx = (fixed_t) (((int64_t) ax * (4 - s) + (int64_t) bx * s) / 4);
                fixed_t sy = (fixed_t) (((int64_t) ay * (4 - s) + (int64_t) by * s) / 4);
                sector_t *sec = R_PointInSubsector(sx, sy)->sector;
                if (s < 4 && !RL_Stand(pmo, sx, sy, &f))
                    ok = false;
                else
                {
                    if (s == 4)
                        f = floors[nc];
                    if (f - prev > 24 * FRACUNIT)
                        ok = false;
                    prev = f;
                }
                if (RL_Passage(sec) == RL_DOOR)
                    rl_gdoor[c * 8 + d] = (short) (sec - sectors);
            }
            if (ok)
                rl_gedge[c] |= 1 << d;
        }
    }
    for (i = 0; i < numsectors; i++)
        sectors[i].ceilingheight = ceil_saved[i];
    free(floors);
    free(ceil_saved);
    printf("[RL] path graph: %dx%d cells, %d ms\n", rl_gw, rl_gh, I_GetTimeMS() - t0);
}

// What the field depends on: visited sectors, keys, and which doors are
// open now.
static unsigned RL_PathSignature(void)
{
    player_t *p = &players[consoleplayer];
    unsigned sig = (unsigned) T_VisitedCount() * 2654435761u;
    int i;
    for (i = 0; i < NUMCARDS; i++)
        sig = sig * 31 + (p->cards[i] ? 1 : 0);
    for (i = 0; i < numsectors; i++)
        if (RL_Passage(&sectors[i]) == RL_DOOR
            && sectors[i].ceilingheight - sectors[i].floorheight >= 56 * FRACUNIT)
            sig = sig * 31 + (unsigned) i + 1;
    return sig;
}

// The field: Dijkstra from the targets, over the edges into each cell
// (a binary heap of cells keyed by distance).
static void RL_PathField(void)
{
    int n = rl_gw * rl_gh, i, c, d, hn = 0;
    int *heap = malloc((size_t) n * 8 * sizeof(int) + sizeof(int));
    int *hkey = malloc((size_t) n * 8 * sizeof(int) + sizeof(int));

    // Doors the player can pass: open now, or a manual door it can open.
    for (i = 0; i < numsectors; i++)
        rl_doorok[i] = sectors[i].ceilingheight - sectors[i].floorheight >= 56 * FRACUNIT;
    for (i = 0; i < numlines; i++)
        if (RL_PassageSpecial(lines[i].special) == 1 && lines[i].backsector != NULL
            && RL_HasKeyFor(lines[i].special))
            rl_doorok[lines[i].backsector - sectors] = 1;

#define RL_HPUSH(cell, key) do { \
        int at_ = hn++; \
        while (at_ > 0 && hkey[(at_ - 1) / 2] > (key)) { \
            heap[at_] = heap[(at_ - 1) / 2]; hkey[at_] = hkey[(at_ - 1) / 2]; at_ = (at_ - 1) / 2; } \
        heap[at_] = (cell); hkey[at_] = (key); } while (0)

    for (c = 0; c < n; c++)
    {
        int sec = rl_gsec[c];
        rl_gdist[c] = -1;
        if (sec >= 0 && !T_SectorVisited(sec) && RL_Passage(&sectors[sec]) != RL_DOOR)
        {
            rl_gdist[c] = 0;
            RL_HPUSH(c, 0);
        }
    }
    while (hn > 0)
    {
        int cur = heap[0], key = hkey[0], lastc, lastk, at = 0;
        // Pop.
        hn--;
        lastc = heap[hn];
        lastk = hkey[hn];
        while (2 * at + 1 < hn)
        {
            int ch = 2 * at + 1;
            if (ch + 1 < hn && hkey[ch + 1] < hkey[ch])
                ch++;
            if (hkey[ch] >= lastk)
                break;
            heap[at] = heap[ch];
            hkey[at] = hkey[ch];
            at = ch;
        }
        heap[at] = lastc;
        hkey[at] = lastk;
        if (key > rl_gdist[cur])
            continue;                       // a stale entry
        // Cells with an edge into cur: neighbour p = cur + off[d], edge (d+4)&7.
        for (d = 0; d < 8; d++)
        {
            int px = cur % rl_gw + rl_dx[d], py = cur / rl_gw + rl_dy[d], pc, e, door, nd;
            if (px < 0 || py < 0 || px >= rl_gw || py >= rl_gh)
                continue;
            pc = py * rl_gw + px;
            e = (d + 4) & 7;
            if (!(rl_gedge[pc] & (1 << e)))
                continue;
            door = rl_gdoor[pc * 8 + e];
            if (door >= 0 && !rl_doorok[door])
                continue;
            nd = key + rl_cost[e];
            if (rl_gdist[pc] < 0 || nd < rl_gdist[pc])
            {
                rl_gdist[pc] = nd;
                if (hn < n * 8)
                    RL_HPUSH(pc, nd);
            }
        }
    }
#undef RL_HPUSH
    free(heap);
    free(hkey);
}

// The player can walk straight from where it stands to (x, y): standable
// at the quarter points and the end, never climbing more than 24 units
// between them (the graph's edge rule, from the real position).
static boolean RL_Reach(mobj_t *mo, fixed_t x, fixed_t y)
{
    fixed_t prev = mo->floorz, f;
    int s;
    for (s = 1; s <= 4; s++)
    {
        fixed_t sx = (fixed_t) (((int64_t) mo->x * (4 - s) + (int64_t) x * s) / 4);
        fixed_t sy = (fixed_t) (((int64_t) mo->y * (4 - s) + (int64_t) y * s) / 4);
        if (!RL_Stand(mo, sx, sy, &f) || f - prev > 24 * FRACUNIT)
            return false;
        prev = f;
    }
    return true;
}

// path_dist (map units; -1 when no unvisited room is reachable) and
// path_rel (degrees off the facing, positive left, to the point
// RL_PATH_AIM cells ahead along the path; *rel_ok false when the player
// is already in a target cell or there is no path).
static int rl_ahead_x, rl_ahead_y;     // path_ahead, map units
static boolean rl_ahead_ok;

static int RL_PathQuery(mobj_t *mo, int *rel, boolean *rel_ok)
{
    int px = (mo->x >> FRACBITS) - rl_gx0, py = (mo->y >> FRACBITS) - rl_gy0;
    int gx = px >= 0 ? px / RL_CELL : -1, gy = py >= 0 ? py / RL_CELL : -1;
    int start = -1, k, d, cur;
    double best = -1;

    *rel_ok = false;
    rl_ahead_ok = false;
    if (rl_graph_dirty || rl_graph_for != sectors)
        RL_BuildGraph(mo);
    if (rl_gdist == NULL)
        return -1;
    if (!rl_path_have || RL_PathSignature() != rl_path_sig)
    {
        rl_path_sig = RL_PathSignature();
        rl_path_have = true;
        RL_PathField();
    }
    // Start: of the cells around the player with a path, the one with
    // the least distance plus the way to its centre -- a way the player
    // can walk straight (RL_Reach): a cell across a wall is near but no
    // start.
    for (k = 0; k < 9; k++)
    {
        int cx = gx + k % 3 - 1, cy = gy + k / 3 - 1, c;
        double ex, ey, v;
        if (cx < 0 || cy < 0 || cx >= rl_gw || cy >= rl_gh)
            continue;
        c = cy * rl_gw + cx;
        if (rl_gdist[c] < 0)
            continue;
        ex = (RL_CellX(c) - mo->x) / (double) FRACUNIT;
        ey = (RL_CellY(c) - mo->y) / (double) FRACUNIT;
        v = rl_gdist[c] + sqrt(ex * ex + ey * ey);
        if (best >= 0 && v >= best)
            continue;
        if (!RL_Reach(mo, RL_CellX(c), RL_CellY(c)))
            continue;
        if (best < 0 || v < best)
        {
            best = v;
            start = c;
        }
    }
    if (start < 0)
        return -1;
    // Aim: the farthest of the next RL_PATH_AIM cells down the field the
    // player can walk to in a straight line (RL_Reach), so the aim never
    // cuts a corner into a wall; the next cell when none is.
    {
        int chain[RL_PATH_AHEAD + 1], len = 1;
        chain[0] = start;
        cur = start;
        for (k = 0; k < RL_PATH_AHEAD && rl_gdist[cur] > 0; k++)
        {
            int next = -1, nv = rl_gdist[cur];
            for (d = 0; d < 8; d++)
            {
                int nx = cur % rl_gw + rl_dx[d], ny = cur / rl_gw + rl_dy[d], nc, door;
                if (!(rl_gedge[cur] & (1 << d)))
                    continue;
                nc = ny * rl_gw + nx;
                door = rl_gdoor[cur * 8 + d];
                if (rl_gdist[nc] < 0 || (door >= 0 && !rl_doorok[door]))
                    continue;
                if (rl_gdist[nc] + rl_cost[d] <= nv && (next < 0 || rl_gdist[nc] < rl_gdist[next]))
                    next = nc;
            }
            if (next < 0)
                break;
            cur = next;
            chain[len++] = cur;
        }
        // path_ahead (2026-11-18): the cell RL_PATH_AHEAD down the path,
        // or the last one (the target room's) -- where the training env
        // puts its WAYPOINT, standing in for the VLM's.
        rl_ahead_x = RL_CellX(chain[len - 1]) >> FRACBITS;
        rl_ahead_y = RL_CellY(chain[len - 1]) >> FRACBITS;
        rl_ahead_ok = true;
        if (len > RL_PATH_AIM + 1)
            len = RL_PATH_AIM + 1;
        cur = len > 1 ? chain[1] : start;
        for (k = len - 1; k > 1; k--)
            if (RL_Reach(mo, RL_CellX(chain[k]), RL_CellY(chain[k])))
            {
                cur = chain[k];
                break;
            }
    }
    // Aim at the start cell itself when it is the target (an unvisited
    // room right beside the player), unless the player is on its centre.
    if (cur != start || fabs((RL_CellX(cur) - mo->x) / (double) FRACUNIT)
                        + fabs((RL_CellY(cur) - mo->y) / (double) FRACUNIT) > 8)
    {
        double r = atan2(RL_CellY(cur) / (double) FRACUNIT - mo->y / (double) FRACUNIT,
                         RL_CellX(cur) / (double) FRACUNIT - mo->x / (double) FRACUNIT) * 180.0 / M_PI
                   - RL_AngleDeg(mo->angle);
        while (r > 180.0)
            r -= 360.0;
        while (r <= -180.0)
            r += 360.0;
        *rel = (int) (r < 0 ? r - 0.5 : r + 0.5);
        *rel_ok = true;
    }
    return (int) (best + 0.5);
}

// WAYPOINT RANDOM (2026-11-18, the training env's waypoints): a random
// cell of the path grid in a sector next to the player's, so it leads out
// of the room (in the player's own sector before, 2026-11-18). From the
// part of the player's sector it can walk to without leaving it (a flood
// fill from the player's cell; id's maps share one sector between rooms
// far apart), a second flood fill takes each neighbouring sector it walks
// into, through doors it can open, door sectors and small sectors (stair
// steps, under RL_SMALL_SEC cells) counting as the way into the sector
// behind them, not as the neighbour. The cell is at least min_dist map
// units from the player (any beyond WP reach, 96, when none is), seed
// picks it. False when there is none.
#define RL_SMALL_SEC 6
boolean RL_SectorPoint(unsigned seed, int min_dist, int *x, int *y)
{
    mobj_t *mo = players[consoleplayer].mo;
    int n, sec, start, head = 0, tail = 0, region, count = 0, c, d, pick, pass;
    int *queue, *size, *cand;
    byte *seen;

    if (mo == NULL || gamestate != GS_LEVEL)
        return false;
    if (rl_graph_dirty || rl_graph_for != sectors)
        RL_BuildGraph(mo);
    if (rl_gsec == NULL)
        return false;
    n = rl_gw * rl_gh;
    sec = (int) (mo->subsector->sector - sectors);
    {
        int px = ((mo->x >> FRACBITS) - rl_gx0) / RL_CELL, py = ((mo->y >> FRACBITS) - rl_gy0) / RL_CELL;
        if (px < 0 || py < 0 || px >= rl_gw || py >= rl_gh)
            return false;
        start = py * rl_gw + px;
    }
    if (rl_gsec[start] != sec)
        return false;
    queue = malloc(n * sizeof(int));
    seen = calloc(n, 1);
    size = calloc(numsectors, sizeof(int));
    for (c = 0; c < n; c++)
        if (rl_gsec[c] >= 0)
            size[rl_gsec[c]]++;
#define RL_THROUGH(s) (RL_Passage(&sectors[s]) == RL_DOOR || size[s] < RL_SMALL_SEC)
    queue[tail++] = start;
    seen[start] = 1;
    // Pass 0: the player's part of its sector. Pass 1: from its edge, the
    // neighbours, entering another sector only from a door or small one.
    region = 0;
    for (pass = 0; pass < 2; pass++)
    {
        if (pass == 1)
        {
            region = tail;     // the player's part: queue[0 .. region)
            head = 0;
        }
        while (head < tail)
        {
            int cur = queue[head++];
            for (d = 0; d < 8; d++)
            {
                int nx = cur % rl_gw + rl_dx[d], ny = cur / rl_gw + rl_dy[d], nc, door;
                if (!(rl_gedge[cur] & (1 << d)))
                    continue;
                nc = ny * rl_gw + nx;
                door = rl_gdoor[cur * 8 + d];
                if (seen[nc] || (door >= 0 && !rl_doorok[door]))
                    continue;
                if (pass == 0 ? rl_gsec[nc] != sec
                    : rl_gsec[nc] == sec || (rl_gsec[cur] != sec && rl_gsec[nc] != rl_gsec[cur]
                                             && !RL_THROUGH(rl_gsec[cur])))
                    continue;
                seen[nc] = 1;
                queue[tail++] = nc;
            }
        }
    }
    // The neighbours' cells far enough away (else any beyond reach), then
    // pick one.
    cand = malloc((tail - region + 1) * sizeof(int));
    for (pass = 0; pass < 2 && count == 0; pass++)
    {
        int lim = pass == 0 ? min_dist : 96;
        for (c = region; c < tail; c++)
        {
            double dx = (RL_CellX(queue[c]) - mo->x) / (double) FRACUNIT;
            double dy = (RL_CellY(queue[c]) - mo->y) / (double) FRACUNIT;
            if (!RL_THROUGH(rl_gsec[queue[c]]) && dx * dx + dy * dy >= (double) lim * lim)
                cand[count++] = queue[c];
        }
    }
#undef RL_THROUGH
    if (count > 0)
    {
        pick = cand[(seed * 2654435761u >> 8) % (unsigned) count];
        *x = RL_CellX(pick) >> FRACBITS;
        *y = RL_CellY(pick) >> FRACBITS;
    }
    free(cand);
    free(queue);
    free(seen);
    free(size);
    return count > 0;
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
               "\"kills\":%d,\"sectors\":%d,\"sector\":%d",
               p->health,
               (p->playerstate == PST_DEAD || p->health <= 0) ? "true" : "false",
               D_EquippedDurabilitySum(p),
               mo->x >> FRACBITS, mo->y >> FRACBITS, mo->z >> FRACBITS,
               RL_AngleDeg(mo->angle),
               turnctrl.tp, turnctrl.tp_max, turnctrl.queue_len,
               (int) w, T_KitAmmo(w), T_KitMaxAmmo(w),
               p->killcount, T_VisitedCount(),
               mo->subsector ? (int) (mo->subsector->sector - sectors) : -1);

        if (rl_fast_speed > 0)
            RL_PUT(",\"fast_tics\":%d", rl_fast_tics);

        // door_sectors (2026-11-17): how many of the visited sectors are
        // doors (RL_Passage), so the env pays a door crossing once: for
        // the room behind it, not for the door's own sector too.
        {
            int doors = 0;
            for (i = 0; i < numsectors; i++)
                if (RL_Passage(&sectors[i]) == RL_DOOR && T_SectorVisited(i))
                    doors++;
            RL_PUT(",\"door_sectors\":%d", doors);
        }
        {
            int wx, wy;
            if (D_WaypointGet(&wx, &wy))
                RL_PUT(",\"waypoint\":[%d,%d]", wx, wy);   // WAYPOINT set, not yet reached
        }

        // Key colours held, as a mask (1 blue, 2 yellow, 4 red): a
        // keycard and a skull key of one colour open the same doors.
        RL_PUT(",\"keys\":%d",
               ((p->cards[it_bluecard] || p->cards[it_blueskull]) ? 1 : 0)
               | ((p->cards[it_yellowcard] || p->cards[it_yellowskull]) ? 2 : 0)
               | ((p->cards[it_redcard] || p->cards[it_redskull]) ? 4 : 0));

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

        // Reward shaping only (the policy never sees frontier_dist):
        // distance to the nearest exit line and to the nearest frontier
        // portal, in map units; -1 when there is none.
        {
            double px = mo->x / (double) FRACUNIT, py = mo->y / (double) FRACUNIT;
            double ex = -1, fr = -1;
            for (i = 0; i < numlines; i++)
            {
                const line_t *ld = &lines[i];
                if (RL_IsExitLine(ld))
                {
                    double d = RL_SegDist(px, py, ld);
                    if (ex < 0 || d < ex)
                        ex = d;
                }
                else if (RL_IsFrontier(ld))
                {
                    double d = RL_SegDist(px, py, ld);
                    if (fr < 0 || d < fr)
                        fr = d;
                }
            }
            RL_PUT(",\"exit_dist\":%d,\"frontier_dist\":%d",
                   ex < 0 ? -1 : (int) (ex + 0.5), fr < 0 ? -1 : (int) (fr + 0.5));
        }
        // portal_dist (2026-11-16): to the nearest portal of the player's
        // current sector (RL_IsPortalOf), -1 when it has none: the time
        // penalty's distance. Reward shaping only, like frontier_dist.
        // portal_rel (2026-11-16): the angle (degrees, -180..180, positive
        // to the left) from the player's facing to the closest point of
        // the nearest portal of the current sector into an unvisited
        // sector; left out when the sector has none. The env's look
        // reward. Reward shaping only.
        // Doors first (2026-11-17): both go by the portals through a door
        // or onto a lift (RL_PassagePortal) when the sector has one of the
        // kind asked for, by all its portals otherwise.
        {
            double px = mo->x / (double) FRACUNIT, py = mo->y / (double) FRACUNIT;
            double pd[2] = { -1, -1 }, ud[2] = { -1, -1 };   // [0] all, [1] doors
            const line_t *uline[2] = { NULL, NULL };
            int k;
            sector_t *s = mo->subsector ? mo->subsector->sector : NULL;
            if (s != NULL)
                for (i = 0; i < s->linecount; i++)
                {
                    const line_t *ld = s->lines[i];
                    const sector_t *other;
                    double d;
                    if (!RL_IsPortalOf(ld, s))
                        continue;
                    d = RL_SegDist(px, py, ld);
                    other = ld->frontsector == s ? ld->backsector : ld->frontsector;
                    for (k = 0; k < 2; k++)
                    {
                        if (k == 1 && !RL_PassagePortal(ld))
                            continue;
                        if (pd[k] < 0 || d < pd[k])
                            pd[k] = d;
                        if (!T_SectorVisited(other - sectors) && (ud[k] < 0 || d < ud[k]))
                        {
                            ud[k] = d;
                            uline[k] = ld;
                        }
                    }
                }
            k = pd[1] >= 0 ? 1 : 0;
            RL_PUT(",\"portal_dist\":%d", pd[k] < 0 ? -1 : (int) (pd[k] + 0.5));
            uline[0] = uline[1] != NULL ? uline[1] : uline[0];
            if (uline[0] != NULL)
            {
                double cx, cy, rel;
                RL_SegClosest(px, py, uline[0], &cx, &cy);
                rel = atan2(cy - py, cx - px) * 180.0 / M_PI - RL_AngleDeg(mo->angle);
                while (rel > 180.0)
                    rel -= 360.0;
                while (rel <= -180.0)
                    rel += 360.0;
                RL_PUT(",\"portal_rel\":%d", (int) (rel < 0 ? rel - 0.5 : rel + 0.5));
            }
        }
        {
            // wall_use also when the wall straight ahead is itself a
            // usable line in reach: approached at a slant, a door's
            // closest point can lie more than 45 degrees off the facing.
            int wd = RL_WallDist(mo);
            boolean use = (rl_wall_special && wd >= 0 && wd <= 64) || RL_WallUse(mo);
            RL_PUT(",\"wall_dist\":%d,\"wall_use\":%s", wd, use ? "true" : "false");
            // wall_line (2026-11-17): that wall's line index, for the
            // env's once-per-wall USE reward.
            if (wd >= 0)
                RL_PUT(",\"wall_line\":%d", rl_wall_line);
        }
        if (rlenv && T_InExplore())
        {
            // path_dist / path_rel (2026-11-17): see RL_PathQuery.
            int rel = 0;
            boolean rel_ok;
            int pdist = RL_PathQuery(mo, &rel, &rel_ok);
            RL_PUT(",\"path_dist\":%d", pdist);
            if (rel_ok)
                RL_PUT(",\"path_rel\":%d", rel);
            if (rl_ahead_ok)
                RL_PUT(",\"path_ahead\":[%d,%d]", rl_ahead_x, rl_ahead_y);
        }

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
        // hp_max and hunted (2026-11-19, the env's frontier save checks):
        // hunted is a living monster in the player's sector whose target
        // is the player.
        {
            thinker_t *th;
            boolean hunted = false;
            for (th = thinkercap.next; th != &thinkercap && !hunted; th = th->next)
            {
                mobj_t *m;
                if (th->function.acp1 != (actionf_p1) P_MobjThinker)
                    continue;
                m = (mobj_t *) th;
                hunted = m->player == NULL && m->health > 0 && m->target == mo
                         && !(m->flags & MF_RLHIDDEN)
                         && ((m->flags & MF_COUNTKILL) || m->type == MT_SKULL)
                         && m->subsector->sector == mo->subsector->sector;
            }
            RL_PUT(",\"hp_max\":%d,\"hunted\":%s", D_MaxHealth(p),
                   hunted ? "true" : "false");
        }
        // foe_dist (2026-11-20, the env's range reward): map units to the
        // nearest living monster on screen (line of sight, within the
        // 90-degree view, as monstervis: the policy must see it), woken or
        // not (the policy can't tell which), -1 when none.
        {
            thinker_t *th;
            int best = -1;
            for (th = thinkercap.next; th != &thinkercap; th = th->next)
            {
                mobj_t *m;
                int dist, rel;
                if (th->function.acp1 != (actionf_p1) P_MobjThinker)
                    continue;
                m = (mobj_t *) th;
                if (m->player != NULL || m->health <= 0
                    || (m->flags & (MF_RLHIDDEN | MF_RLPRACTICE))
                    || !((m->flags & MF_COUNTKILL) || m->type == MT_SKULL))
                    continue;
                dist = P_AproxDistance(m->x - mo->x, m->y - mo->y) >> FRACBITS;
                if (dist > 2048 || (best >= 0 && dist >= best))
                    continue;
                rel = RL_AngleDeg(R_PointToAngle2(mo->x, mo->y, m->x, m->y) - mo->angle);
                if ((rel > 45 && rel < 315) || !P_CheckSight(mo, m))
                    continue;
                best = dist;
            }
            RL_PUT(",\"foe_dist\":%d", best);
        }
        // automap: the map is on screen (Tab or the driver's MAP); the
        // env's forced map glances check it before sending MAP.
        RL_PUT(",\"automap\":%s,\"forced_map\":%s", automapactive ? "true" : "false",
               T_ForcedMapFrame() ? "true" : "false");
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


void RL_NotePickup(const char *kind, int hp, boolean practice, int tier)
{
    if (tier >= 0)
        RL_Emit("{\"event\":\"pickup\",\"kind\":\"%s\",\"hp\":%d,\"practice\":%s,\"tier\":%d,\"gametic\":%d}",
                kind, hp, practice ? "true" : "false", tier, gametic);
    else
        RL_Emit("{\"event\":\"pickup\",\"kind\":\"%s\",\"hp\":%d,\"practice\":%s,\"gametic\":%d}",
                kind, hp, practice ? "true" : "false", gametic);
}


// ---------------------------------------------------------------------
// CONSULT practice monsters and the CONSULT command (see d_rlenv.h).
// ---------------------------------------------------------------------

#define RL_SPAWN_MAX     1              // one at a time (4 before 2026-11-17)
#define RL_SPAWN_LIFE    (5 * TICRATE)  // removed this long after spawning
#define RL_SPAWN_TRIES   16             // directions tried, all in view
#define RL_SPAWN_NEAR    128            // spawn distance range, map units
#define RL_SPAWN_FAR     384            // 640 before 2026-11-17: too small at 128x80

#define RL_ITEM_LIFE     (10 * TICRATE)
#define RL_ITEM_NEAR     64
#define RL_ITEM_FAR      256            // farther, an item is a speck at 128x80
#define RL_ITEM_VIEW     40             // degrees off the facing: on screen (90-degree view)

static int rl_spawn_period = -1;        // -rlspawn; -1 unparsed, 0 off
static int rl_item_period = -1;         // -rlitems; -1 unparsed, 0 off
static mobj_t *rl_item;                 // the live practice item
static int rl_item_age;
static uint32_t rl_spawn_rng = 1;
static mobj_t *rl_spawned[RL_SPAWN_MAX];
static int rl_spawned_age[RL_SPAWN_MAX];
static boolean rl_spawn_purge;          // level set up: remove practice monsters from a save
static boolean rl_drill;                // DRILL ON (2026-11-20): an awake monster ahead, always
static mobj_t *rl_drill_mo;             // the drill's live monster
static int rl_drill_lost;               // tics it has been off screen
#define RL_DRILL_NEAR    192
#define RL_DRILL_FAR     768
#define RL_DRILL_VIEW    20
#define RL_DRILL_TRIES   24
#define RL_DRILL_LOST    TICRATE        // off screen this long: replaced (the policy can't turn)
static boolean RL_TryDrillSpawn(mobj_t *pmo);

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
    rl_item = NULL;
    rl_passage_dirty = true;
    rl_graph_dirty = true;
    rl_spawn_purge = true;   // a LOAD unarchives the save's mobjs after this
    rl_drill = false;        // a LOAD ends a drill: the env sends DRILL ON again
    rl_drill_mo = NULL;
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

// Any live practice monster (or item) in the level, tracked or not: a
// spawn waits for none, so there is only ever one of each.
static boolean RL_PracticeExists(boolean monster)
{
    thinker_t *th;
    for (th = thinkercap.next; th != &thinkercap; th = th->next)
    {
        mobj_t *mo = (mobj_t *) th;
        if (th->function.acp1 == (actionf_p1) P_MobjThinker
            && (mo->flags & MF_RLPRACTICE) && mo->health > 0
            && !!(mo->flags & MF_COUNTKILL) == monster)
            return true;
    }
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
    // RL_SPAWN_TRIES directions within 30 degrees of the view, so the
    // monster is always on screen: one beside or behind the player is
    // never a sighting, and WALK refuses to move with a monster within
    // 192 units, seen or not. A point that is blocked or out of sight is
    // walked back toward the player, down to RL_SPAWN_NEAR units, before
    // trying the next direction; no room in view, no spawn this time.
    for (attempt = 0; attempt < RL_SPAWN_TRIES; attempt++)
    {
        int dist = RL_SPAWN_NEAR + (int) (RL_SpawnRand() % (RL_SPAWN_FAR - RL_SPAWN_NEAR + 1));
        int deg = (int) (RL_SpawnRand() % 61) - 30;
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
            mo->flags |= MF_AMBUSH | MF_RLPRACTICE;              // deaf; practice
            rl_spawned[slot] = mo;
            rl_spawned_age[slot] = 0;
            printf("[RL] spawned type %d at (%d,%d), %d units, %d deg\n",
                   (int) type, x >> FRACBITS, y >> FRACBITS, dist, deg);
            T_RefreshTargets();
            return true;
        }
    }
    return false;
}

// A practice item (see d_rlenv.h): a random Diablo item valid at this
// difficulty, from the private RNG. Uniques a quarter of the time
// (2026-11-17): a unique pickup is a CONSULT trigger, so it has to come
// up often enough to learn from.
static int RL_PracticeItemId(void)
{
    static const int weight[NUM_TIERS] = { 30, 25, 10, 10, 25 };
    int tier, i, n, pick, roll = (int) (RL_SpawnRand() % 100);

    for (tier = 0; tier < NUM_TIERS - 1 && roll >= weight[tier]; tier++)
        roll -= weight[tier];
    for (; tier >= 0; tier--)
    {
        for (i = n = 0; i < D_TierCount(tier); i++)
            if (D_GetItemDef(tier, i) && D_GetItemDef(tier, i)->diff_tier <= D_DifficultyTier())
                n++;
        if (n == 0)
            continue;
        pick = (int) (RL_SpawnRand() % (uint32_t) n);
        for (i = 0; i < D_TierCount(tier); i++)
            if (D_GetItemDef(tier, i) && D_GetItemDef(tier, i)->diff_tier <= D_DifficultyTier()
                && pick-- == 0)
                return D_MAKEITEM(tier, i);
    }
    return -1;
}

// The point of a portal the item path aims at: the closest point of the
// line, kept 24 units off its ends (not into a door jamb).
static void RL_PortalPoint(double px, double py, const line_t *ld, double *cx, double *cy)
{
    double ax = ld->v1->x / (double) FRACUNIT, ay = ld->v1->y / (double) FRACUNIT;
    double bx = ld->v2->x / (double) FRACUNIT, by = ld->v2->y / (double) FRACUNIT;
    double dx = bx - ax, dy = by - ay, len = sqrt(dx * dx + dy * dy), t = 0.5, m;
    if (len > 0)
    {
        m = len > 48 ? 24 / len : 0.5;
        t = ((px - ax) * dx + (py - ay) * dy) / (len * len);
        t = t < m ? m : t > 1 - m ? 1 - m : t;
    }
    *cx = ax + t * dx;
    *cy = ay + t * dy;
}

// On the way to a portal and in view (2026-11-17): the item goes on the
// straight line from the player to a portal of the current sector, when
// that portal point is within RL_ITEM_VIEW degrees of the facing (so the
// item is on screen), 64..RL_ITEM_FAR units away, in line of sight and
// on a floor within a step of the player's. The portals are tried in
// order -- unvisited doors and lifts, other unvisited portals, visited
// doors and lifts, other visited portals; nearest first in each -- until
// the item fits. None fits: no spawn this time.
#define RL_ITEM_PORTALS 64
typedef struct
{
    double x, y, d;
    int rank;               // 0 unvisited door, 1 unvisited, 2 visited door, 3 visited
} rl_portal_t;

static boolean RL_PlaceItem(mobj_t *pmo, const rl_portal_t *pt, int id)
{
    int far = (int) pt->d - 16, dist;       // short of the portal line
    if (far > RL_ITEM_FAR)
        far = RL_ITEM_FAR;
    dist = RL_ITEM_NEAR + (int) (RL_SpawnRand() % (uint32_t) (far - RL_ITEM_NEAR + 1));
    for (; dist >= RL_ITEM_NEAR; dist = dist * 3 / 4)
    {
        double px = pmo->x / (double) FRACUNIT, py = pmo->y / (double) FRACUNIT;
        fixed_t x = pmo->x + (fixed_t) ((pt->x - px) / pt->d * dist * FRACUNIT);
        fixed_t y = pmo->y + (fixed_t) ((pt->y - py) / pt->d * dist * FRACUNIT);
        mobj_t *mo = P_SpawnDiabloLootAt(x, y, id);

        if (!P_CheckPosition(mo, x, y) || !P_CheckSight(pmo, mo)
            || abs(mo->floorz - pmo->floorz) > 24 * FRACUNIT)
        {
            P_RemoveMobj(mo);
            continue;
        }
        mo->flags |= MF_RLPRACTICE;
        rl_item = mo;
        rl_item_age = 0;
        printf("[RL] practice item %d at (%d,%d), %d units toward a%s portal %d units off\n",
               id, x >> FRACBITS, y >> FRACBITS, dist, pt->rank < 2 ? "n unvisited" : " visited",
               (int) pt->d);
        RL_Emit("{\"event\":\"item\",\"x\":%d,\"y\":%d,\"gametic\":%d}",
                x >> FRACBITS, y >> FRACBITS, gametic);
        return true;
    }
    return false;
}

static void RL_TrySpawnItem(mobj_t *pmo)
{
    sector_t *sec = pmo->subsector ? pmo->subsector->sector : NULL;
    double px = pmo->x / (double) FRACUNIT, py = pmo->y / (double) FRACUNIT;
    rl_portal_t pts[RL_ITEM_PORTALS], tmp;
    int i, j, n = 0, id;

    if (sec == NULL)
        return;
    for (i = 0; i < sec->linecount && n < RL_ITEM_PORTALS; i++)
    {
        const line_t *ld = sec->lines[i];
        const sector_t *other;
        double cx, cy, d, rel;
        if (!RL_IsPortalOf(ld, sec))
            continue;
        RL_PortalPoint(px, py, ld, &cx, &cy);
        d = sqrt((cx - px) * (cx - px) + (cy - py) * (cy - py));
        if (d < RL_ITEM_NEAR + 16)
            continue;                       // at the portal already
        rel = atan2(cy - py, cx - px) * 180.0 / M_PI - RL_AngleDeg(pmo->angle);
        while (rel > 180.0)
            rel -= 360.0;
        while (rel <= -180.0)
            rel += 360.0;
        if (rel < -RL_ITEM_VIEW || rel > RL_ITEM_VIEW)
            continue;                       // off screen
        other = ld->frontsector == sec ? ld->backsector : ld->frontsector;
        pts[n].x = cx;
        pts[n].y = cy;
        pts[n].d = d;
        pts[n].rank = (T_SectorVisited(other - sectors) ? 2 : 0) + (RL_PassagePortal(ld) ? 0 : 1);
        n++;
    }
    // By rank, then nearest (insertion sort: a sector has few portals).
    for (i = 1; i < n; i++)
        for (j = i; j > 0 && (pts[j].rank < pts[j - 1].rank
                              || (pts[j].rank == pts[j - 1].rank && pts[j].d < pts[j - 1].d)); j--)
        {
            tmp = pts[j];
            pts[j] = pts[j - 1];
            pts[j - 1] = tmp;
        }
    if (n == 0 || (id = RL_PracticeItemId()) < 0)
        return;
    for (i = 0; i < n; i++)
        if (RL_PlaceItem(pmo, &pts[i], id))
            return;
}

void RL_MoveTick(void)
{
    mobj_t *mo = players[consoleplayer].mo;

    if (rl_fast_speed < 0)
    {
        int p = M_CheckParmWithArgs("-rlfast", 1);
        rl_fast_speed = (rlenv && p > 0) ? atoi(myargv[p + 1]) : 0;
        if (rl_fast_speed < 0)
            rl_fast_speed = 0;
    }
    if (rl_fast_speed == 0 || mo == NULL)
    {
        rl_move_mo = NULL;
        return;
    }
    // Only tic-to-tic movement of the same mobj counts (a LOAD or level
    // change makes a new one; a skipped tic leaves a gap).
    if (mo == rl_move_mo && rl_move_tic == gametic - 1)
    {
        double dx = (double) (mo->x - rl_move_x) / FRACUNIT;
        double dy = (double) (mo->y - rl_move_y) / FRACUNIT;
        double d2 = dx * dx + dy * dy;
        if (d2 >= (double) rl_fast_speed * rl_fast_speed && d2 <= 64.0 * 64.0)
            rl_fast_tics++;
    }
    rl_move_mo = mo;
    rl_move_x = mo->x;
    rl_move_y = mo->y;
    rl_move_tic = gametic;
}

void RL_DecisionMade(void)
{
    rl_fast_tics = 0;
}

void RL_SpawnTick(void)
{
    player_t *pl = &players[consoleplayer];
    int i, live = 0;

    if (rl_spawn_period < 0)
    {
        int p = M_CheckParmWithArgs("-rlspawn", 1);
        rl_spawn_period = (rldrive && p > 0) ? atoi(myargv[p + 1]) : 0;
        if (rl_spawn_period < 0)
            rl_spawn_period = 0;
    }
    if (rl_item_period < 0)
    {
        int p = M_CheckParmWithArgs("-rlitems", 1);
        rl_item_period = (rldrive && p > 0) ? atoi(myargv[p + 1]) : 0;
        if (rl_item_period < 0)
            rl_item_period = 0;
    }
    // A level was just set up (a LOAD included): practice monsters and
    // items in the loaded save have lost their timers -- remove them, so
    // a start state never carries one.
    if (rl_spawn_purge && rldrive && gamestate == GS_LEVEL)
    {
        thinker_t *th;
        int purged = 0;
        rl_spawn_purge = false;
        for (th = thinkercap.next; th != &thinkercap; th = th->next)
        {
            mobj_t *mo = (mobj_t *) th;
            if (th->function.acp1 == (actionf_p1) P_MobjThinker
                && (mo->flags & MF_RLPRACTICE) && mo->health > 0)
            {
                P_RemoveMobj(mo);
                purged++;
            }
        }
        if (purged > 0)
        {
            printf("[RL] removed %d practice monster(s)/item(s) from the loaded save\n", purged);
            T_RefreshTargets();
        }
    }

    if (gamestate != GS_LEVEL || pl->mo == NULL || pl->health <= 0 || !T_InExplore())
        return;

    if (rl_item != NULL && !RL_IsLive(rl_item))
        rl_item = NULL;                     // picked up
    if (rl_item != NULL && ++rl_item_age >= RL_ITEM_LIFE)
    {
        printf("[RL] practice item removed (lifetime)\n");
        P_RemoveMobj(rl_item);
        rl_item = NULL;
    }
    if (rl_item_period > 0 && rl_item == NULL && !D_BackpackFull(pl)
        && RL_SpawnRand() % (uint32_t) rl_item_period == 0 && !RL_PracticeExists(false))
        RL_TrySpawnItem(pl->mo);

    if (rl_drill)
    {
        if (rl_drill_mo != NULL && (!RL_IsLive(rl_drill_mo) || rl_drill_mo->health <= 0))
            rl_drill_mo = NULL;
        if (rl_drill_mo != NULL)
        {
            // On screen: in sight within the 90-degree view (as foe_dist).
            mobj_t *m = rl_drill_mo, *pmo = pl->mo;
            int rel = RL_AngleDeg(R_PointToAngle2(pmo->x, pmo->y, m->x, m->y) - pmo->angle);
            boolean seen = (rel <= 45 || rel >= 315) && P_CheckSight(pmo, m);
            rl_drill_lost = seen ? 0 : rl_drill_lost + 1;
            if (rl_drill_lost >= RL_DRILL_LOST)
            {
                P_RemoveMobj(m);
                rl_drill_mo = NULL;
                T_RefreshTargets();
            }
        }
        if (rl_drill_mo == NULL)
            RL_TryDrillSpawn(pl->mo);
    }

    if (rl_spawn_period == 0)
        return;

    // Forget dead or vanished ones; remove those past their lifetime.
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
        if (++rl_spawned_age[i] >= RL_SPAWN_LIFE)
        {
            printf("[RL] practice monster removed (lifetime)\n");
            P_RemoveMobj(mo);
            rl_spawned[i] = NULL;
            T_RefreshTargets();
            continue;
        }
        live++;
    }
    if (live == 0 && RL_SpawnRand() % (uint32_t) rl_spawn_period == 0 && !RL_PracticeExists(true))
        RL_TrySpawn(pl->mo);
}

// Combat drill (DRILL ON, -rlenv, 2026-11-20): one awake monster at a time,
// RL_DRILL_NEAR..RL_DRILL_FAR units ahead within RL_DRILL_VIEW degrees of
// the facing (the drill's policy only strafes, so it must be lined up by
// strafing), in sight, targeting the player and running its see state; a
// new one as soon as it dies. A normal monster, not a practice one: it
// fights, drops loot and counts as a kill.

static boolean RL_TryDrillSpawn(mobj_t *pmo)
{
    static const mobjtype_t types[] = { MT_POSSESSED, MT_SHOTGUY, MT_TROOP, MT_SERGEANT };
    int attempt;

    for (attempt = 0; attempt < RL_DRILL_TRIES; attempt++)
    {
        int dist = RL_DRILL_NEAR + (int) (RL_SpawnRand() % (RL_DRILL_FAR - RL_DRILL_NEAR + 1));
        int deg = (int) (RL_SpawnRand() % (2 * RL_DRILL_VIEW + 1)) - RL_DRILL_VIEW;
        angle_t an = pmo->angle + (angle_t) ((int64_t) deg * (ANG90 / 90));
        mobjtype_t type = types[RL_SpawnRand() % (sizeof(types) / sizeof(types[0]))];

        for (; dist >= RL_DRILL_NEAR; dist = dist * 3 / 4)
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
            mo->angle = R_PointToAngle2(x, y, pmo->x, pmo->y);   // facing the player
            mo->target = pmo;
            P_SetMobjState(mo, mo->info->seestate);
            rl_drill_mo = mo;
            rl_drill_lost = 0;
            T_RefreshTargets();
            return true;
        }
    }
    return false;
}

void RL_Drill(boolean on)
{
    rl_drill = on;
    if (!on)
        rl_drill_mo = NULL;
}

boolean RL_PracticeAcquired(mobj_t *mo)
{
    int si;
    if (!(mo->flags & MF_RLPRACTICE))
        return false;
    si = RL_SpawnedIndex(mo);
    if (si >= 0)
        rl_spawned[si] = NULL;
    printf("[RL] practice monster acquired the player: removed\n");
    P_RemoveMobj(mo);   // only marks the thinker: the caller's thinker walk stays valid
    T_RefreshTargets(); // the target list may hold it
    return true;
}

int rl_player_hits = 0;

void RL_PlayerHit(mobj_t *t, int dealt)
{
    int max = t->info->spawnhealth;
    if (t->flags & MF_COUNTKILL)
        max = max * D_LoopHpMult() / 100;     // as P_SpawnMobj gave it
    rl_player_hits++;
    RL_Emit("{\"event\":\"damage\",\"amount\":%d,\"max\":%d,\"kill\":%s,\"type\":%d,\"gametic\":%d}",
            dealt, max, t->health <= 0 ? "true" : "false", (int) t->type, gametic);
}

boolean rl_hitscan = false;

void RL_PlayerHurt(mobj_t *inflictor, mobj_t *source, int damage)
{
    boolean monster = source && source->player == NULL;
    const char *kind = inflictor && (inflictor->flags & MF_MISSILE) ? "proj"
                     : rl_hitscan ? "hitscan" : monster ? "melee" : "world";
    RL_Emit("{\"event\":\"hurt\",\"amount\":%d,\"monster\":%s,\"kind\":\"%s\",\"gametic\":%d}",
            damage, monster ? "true" : "false", kind, gametic);
}

void RL_Monsters(boolean hide)
{
    thinker_t *th;
    int n = 0;

    for (th = thinkercap.next; th != &thinkercap; th = th->next)
    {
        mobj_t *mo;
        if (th->function.acp1 != (actionf_p1) P_MobjThinker)
            continue;
        mo = (mobj_t *) th;
        if (mo->player != NULL || mo->health <= 0
            || !((mo->flags & MF_COUNTKILL) || mo->type == MT_SKULL))
            continue;
        if (hide && !(mo->flags & MF_RLHIDDEN))
        {
            P_UnsetThingPosition(mo);
            mo->flags |= MF_RLHIDDEN;
            n++;
        }
        else if (!hide && (mo->flags & MF_RLHIDDEN))
        {
            mo->flags &= ~MF_RLHIDDEN;
            P_SetThingPosition(mo);
            n++;
        }
    }
    printf("[RL] MONSTERS %s: %d\n", hide ? "HIDE" : "SHOW", n);
    T_RefreshTargets();
}

// GIVEGUN <seed> (-rlenv, 2026-11-18, the env's rollout start): a
// random normal-tier gun (pistol, shotgun, chaingun, rocket launcher,
// plasma, BFG, super shotgun: no chainsaw) for this difficulty, or
// fists (the weapon slot emptied), evenly among the eight, equipped in the weapon slot at full durability in
// place of whatever was there. The old one is dropped, not stashed: a
// progress save carries the gun, so stashing would fill the backpack
// over the batches. Strength comes only from gear, so when the player
// lacks it for the gun (it would be on fists), strength gear goes on
// too, the most strength first from any tier (set and unique pieces;
// the lowest tier first before), one piece a slot (not the weapon or shield slots), until it is usable;
// a two-handed gun takes the shield off. seed picks it, never the game's
// RNG. kit (GIVEGUN <seed> KIT, 2026-11-20): every other slot emptied
// first, then RL_GiveKit fills the free ones.
// The kit's score for an item in slot s: fists take the most armour in
// the helm, armour and shield slots and the most strength elsewhere (+1
// fist damage per 4); an AD gun the most AD, an AP gun the most AP
// (a flat point counted as 5%).
static int RL_KitScore(const diablo_itemdef_t *g, int s, int mode)
{
    if (mode == 0)
        return (s == ESLOT_HELM || s == ESLOT_ARMOR || s == ESLOT_SHIELD) ? g->armor : g->str;
    return mode == 1 ? g->ad_pct + 5 * g->ad_flat : g->ap_pct + 5 * g->ap_flat;
}

static void RL_GiveKit(player_t *pl, boolean *used, int mode)
{
    int s, t, i;
    for (s = 0; s < NUM_ESLOTS; s++)
    {
        int best = D_NOITEM, best_score = 0;
        if (used[s])
            continue;
        for (t = 0; t < NUM_TIERS; t++)
            for (i = 0; i < D_TierCount(t); i++)
            {
                const diablo_itemdef_t *g = D_GetItemDef(t, i);
                int sc;
                // Never vitality: max HP stays 100 (the frontier's 80% check).
                if (g == NULL || g->consumable || g->vit > 0 || g->diff_tier > D_DifficultyTier()
                    || !(g->slot == s || (s == ESLOT_RING2 && g->slot == ESLOT_RING1)))
                    continue;
                sc = RL_KitScore(g, s, mode);
                if (sc > best_score)
                {
                    best = D_MAKEITEM(t, i);
                    best_score = sc;
                }
            }
        if (best != D_NOITEM)
        {
            const diablo_itemdef_t *g = D_GetItemDef(D_ITEMTIER(best), D_ITEMIDX(best));
            used[s] = true;
            pl->diablo_equipped[s] = best;
            pl->diablo_equipped_dur[s] = D_ItemMaxDurability(g);
            printf("[RL] GIVEGUN KIT: + %s\n", g->name);
        }
    }
}

boolean RL_GiveGun(unsigned seed, boolean kit, boolean ranged)
{
    player_t *pl = &players[consoleplayer];
    int ids[64], n = 0, i, t, id;
    boolean used[NUM_ESLOTS];
    const diablo_itemdef_t *def;

    if (pl->mo == NULL || pl->health <= 0 || gamestate != GS_LEVEL)
        return false;
    if (kit)
        for (i = 0; i < NUM_ESLOTS; i++)
            if (i != ESLOT_WEAPON)
            {
                pl->diablo_equipped[i] = D_NOITEM;
                pl->diablo_equipped_dur[i] = 0;
            }
    for (i = 0; i < D_TierCount(TIER_NORMAL) && n < 64; i++)
    {
        def = D_GetItemDef(TIER_NORMAL, i);
        if (def && def->slot == ESLOT_WEAPON && def->diff_tier <= D_DifficultyTier()
            && def->doomweapon != wp_fist && def->doomweapon != wp_chainsaw
            && def->doomweapon >= 0 && def->doomweapon < NUMWEAPONS)
            ids[n++] = D_MAKEITEM(TIER_NORMAL, i);
    }
    // Fists count as one more choice (2026-11-18): the weapon slot left
    // empty. Not with ranged (2026-11-20, the combat drill).
    if (ranged && n == 0)
        return false;
    i = (int) ((seed * 2654435761u >> 8) % (unsigned) (ranged ? n : n + 1));
    if (i == n)
    {
        pl->diablo_equipped[ESLOT_WEAPON] = D_NOITEM;
        pl->diablo_equipped_dur[ESLOT_WEAPON] = 0;
        if (kit)
        {
            for (i = 0; i < NUM_ESLOTS; i++)
                used[i] = (i == ESLOT_WEAPON);
            RL_GiveKit(pl, used, 0);
        }
        D_RecalcStats((struct player_s *) pl);
        printf("[RL] GIVEGUN: fists\n");
        return true;
    }
    id = ids[i];
    def = D_GetItemDef(D_ITEMTIER(id), D_ITEMIDX(id));
    if (D_IsTwoHandedWeapon(def->doomweapon))
        pl->diablo_equipped[ESLOT_SHIELD] = D_NOITEM;
    pl->diablo_equipped[ESLOT_WEAPON] = id;
    pl->diablo_equipped_dur[ESLOT_WEAPON] = D_ItemMaxDurability(def);
    for (i = 0; i < NUM_ESLOTS; i++)
        used[i] = (i == ESLOT_WEAPON || i == ESLOT_SHIELD);
    // Strength gear until the gun is usable.
    while (D_EquippedSlotStrDisabled((struct player_s *) pl, ESLOT_WEAPON))
    {
        int best = D_NOITEM, best_str = 0;
        for (t = 0; t < NUM_TIERS; t++)
            for (i = 0; i < D_TierCount(t); i++)
            {
                const diablo_itemdef_t *g = D_GetItemDef(t, i);
                if (g && g->str > best_str && !g->consumable && g->slot >= 0 && g->slot < NUM_ESLOTS
                    && !used[g->slot] && g->diff_tier <= D_DifficultyTier())
                {
                    best = D_MAKEITEM(t, i);
                    best_str = g->str;
                }
            }
        if (best == D_NOITEM)
            break;              // no more to give: on fists
        {
            const diablo_itemdef_t *g = D_GetItemDef(D_ITEMTIER(best), D_ITEMIDX(best));
            used[g->slot] = true;
            pl->diablo_equipped[g->slot] = best;
            pl->diablo_equipped_dur[g->slot] = D_ItemMaxDurability(g);
            printf("[RL] GIVEGUN: + %s (STR %d)\n", g->name, g->str);
        }
    }
    if (kit)
        RL_GiveKit(pl, used, T_KitForWeapon(def->doomweapon)->ap_weapon ? 2 : 1);
    D_RecalcStats((struct player_s *) pl);
    printf("[RL] GIVEGUN: %s%s\n", def->name,
           D_EquippedSlotStrDisabled((struct player_s *) pl, ESLOT_WEAPON) ? " (not enough strength)" : "");
    return true;
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
        si = RL_SpawnedIndex(mo);
        if (si < 0)
            continue;       // only practice monsters, never the map's (under -rlenv too
                            // since 2026-11-18: the policy fights them)
        // A practice monster goes in or out of sight (2026-11-17).
        if (si < 0 && !P_CheckSight(pl->mo, mo) && !(combat && mo->target == pl->mo))
            continue;
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

