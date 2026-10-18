// d_progress.c - Crash-safe progress persistence via mmap.
//
// Design (per Jimmy 2026-10-10):
// - Progress file lives in ~/workspace/DiabloDoom (persistent across VM recycles)
// - On boot: check for progress file, load automatically if valid
// - If no file: mmap a new one; game state writes go to the mmap'd region
// - Every 120 tics: serialize full game save into the mmap'd buffer
// - Double-buffered slots for atomic writes (crash mid-write is safe)
// - Jimmy handles cron-based restart; this module handles save/load

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "doomtype.h"
#include "doomstat.h"
#include "t_turn.h"
#include "d_progress.h"
#include "p_saveg.h"
#include "g_game.h"
#include "m_misc.h"

// Progress file location: repo root (persistent across VM recycles).
// The path is relative to the current working directory; the game is
// typically launched from the repo root or we resolve it below.
#define PROGRESS_FILENAME "progress.dat"
#define PROGRESS_SLOT_FILENAME_FMT "progress_slot%d.dat"
#define PROGRESS_AUTO_FILENAME_FMT "progress_auto%d.dat"

// Number of user-selectable save slots (1-based for the UI).
#define PROGRESS_NUM_SLOTS 3

// Number of auto-save rotation slots.
#define PROGRESS_NUM_AUTO_SLOTS 3

// 4MB per slot is plenty for a Doom save (typically 100KB-1MB).
#define PROGRESS_SLOT_SIZE (4 * 1024 * 1024)

#define PROGRESS_MAGIC 0x47504444 // "DDPG" (DiabloDoom Progress)
#define PROGRESS_VERSION 1

// Write every 120 tics (~3.4 seconds at 35 tics/sec).
#define PROGRESS_WRITE_INTERVAL 120

typedef struct
{
    unsigned int magic;       // PROGRESS_MAGIC
    unsigned int version;     // PROGRESS_VERSION
    unsigned int active_slot; // 0 or 1: which slot has the latest valid save
    unsigned int checksum[2]; // simple checksum per slot
    unsigned int save_size[2];// bytes used in each slot
    unsigned long long timestamp; // last write (seconds since epoch)
    unsigned char reserved[32];
} progress_header_t;

#define PROGRESS_HEADER_SIZE sizeof(progress_header_t)
#define PROGRESS_FILE_SIZE (PROGRESS_HEADER_SIZE + 2 * PROGRESS_SLOT_SIZE)

static void *progress_map = NULL;
static progress_header_t *progress_hdr = NULL;
static unsigned char *progress_slots[2] = {NULL, NULL};

// Forward declaration.
static void ProgressGetAutoDir(char *buf, size_t buflen);

// Pending player teleport after auto-save load warp.
// Set by D_ProgressLoadAutoSlot, consumed by G_DoLoadLevel.
static boolean autosave_teleport_pending = false;
static int autosave_teleport_x = 0;
static int autosave_teleport_y = 0;
static int autosave_teleport_angle = 0;

void D_AutosaveTeleportCheck(void)
{
    if (!autosave_teleport_pending)
        return;
    autosave_teleport_pending = false;
    {
        extern player_t players[];
        extern int consoleplayer;
        player_t *pl = &players[consoleplayer];
        if (pl->mo)
        {
            extern boolean P_TeleportMove(mobj_t *thing, fixed_t x, fixed_t y);
            fixed_t x = (fixed_t)autosave_teleport_x;
            fixed_t y = (fixed_t)autosave_teleport_y;
            if (P_TeleportMove(pl->mo, x, y))
            {
                pl->mo->angle = (angle_t)autosave_teleport_angle;
                printf("[AUTOSAVE] teleported to (%d,%d)\n",
                       autosave_teleport_x, autosave_teleport_y);
            }
            else
            {
                printf("[AUTOSAVE] teleport blocked at (%d,%d)\n",
                       autosave_teleport_x, autosave_teleport_y);
            }
        }
    }
    // Kill the enemies that were dead when the auto-save was made.
    {
        char dead_path[512];
        char auto_dir[512];
        ProgressGetAutoDir(auto_dir, sizeof(auto_dir));
        M_snprintf(dead_path, sizeof(dead_path), "%s/progress_auto_dead.dat",
                   auto_dir);
        FILE *f = fopen(dead_path, "rb");
        if (f)
        {
            int count;
            if (fread(&count, sizeof(int), 1, f) == 1 && count > 0 &&
                count < 10000)
            {
                extern thinker_t thinkercap;
                extern void *P_MobjThinker;
                int i;
                int killed = 0;
                for (i = 0; i < count; i++)
                {
                    int x, y, type;
                    if (fread(&x, sizeof(int), 1, f) != 1) break;
                    if (fread(&y, sizeof(int), 1, f) != 1) break;
                    if (fread(&type, sizeof(int), 1, f) != 1) break;
                    // Find a living enemy of this type near the position.
                    thinker_t *th;
                    for (th = thinkercap.next; th != &thinkercap;
                         th = th->next)
                    {
                        if (th->function.acp1 != (actionf_p1)P_MobjThinker)
                            continue;
                        mobj_t *mo = (mobj_t *)th;
                        if ((mo->flags & MF_SHOOTABLE) &&
                            mo->health > 0 && (int)mo->type == type)
                        {
                            int dx = (int)mo->x - x;
                            int dy = (int)mo->y - y;
                            if (dx < 0) dx = -dx;
                            if (dy < 0) dy = -dy;
                            if (dx < 64*65536 && dy < 64*65536)
                            {
                                extern void P_KillMobj(mobj_t *source,
                                                       mobj_t *target);
                                P_KillMobj(NULL, mo);
                                killed++;
                                break;
                            }
                        }
                    }
                }
                printf("[AUTOSAVE] re-killed %d/%d dead enemies\n",
                       killed, count);
            }
            fclose(f);
        }
    }
    // Restore player state (HP, armor, ammo, heat) and live enemy HP.
    {
        char state_path[512];
        char auto_dir[512];
        ProgressGetAutoDir(auto_dir, sizeof(auto_dir));
        M_snprintf(state_path, sizeof(state_path),
                   "%s/progress_auto_state.dat", auto_dir);
        FILE *f = fopen(state_path, "rb");
        if (f)
        {
            extern player_t players[];
            extern int consoleplayer;
            player_t *pl = &players[consoleplayer];
            int hp, armor;
            if (fread(&hp, sizeof(int), 1, f) == 1 &&
                fread(&armor, sizeof(int), 1, f) == 1)
            {
                pl->health = hp;
                if (pl->mo)
                    pl->mo->health = hp;
                pl->armortype = armor / 100;
                pl->armorpoints = armor % 100;
                printf("[AUTOSAVE] restored HP=%d armor=%d\n", hp, armor);
            }
            int i;
            for (i = 0; i < NUMAMMO; i++)
            {
                int a;
                if (fread(&a, sizeof(int), 1, f) == 1)
                    pl->ammo[i] = a;
            }
            {
                int heat;
                if (fread(&heat, sizeof(int), 1, f) == 1)
                {
                    extern turnctrl_t turnctrl;
                    turnctrl.heat = heat;
                }
            }
            // Live enemies: restore their HP.
            {
                int count;
                if (fread(&count, sizeof(int), 1, f) == 1 && count > 0 &&
                    count < 10000)
                {
                    extern thinker_t thinkercap;
                    extern void *P_MobjThinker;
                    int i;
                    int restored = 0;
                    for (i = 0; i < count; i++)
                    {
                        int x, y, type, h;
                        if (fread(&x, sizeof(int), 1, f) != 1) break;
                        if (fread(&y, sizeof(int), 1, f) != 1) break;
                        if (fread(&type, sizeof(int), 1, f) != 1) break;
                        if (fread(&h, sizeof(int), 1, f) != 1) break;
                        // Find the enemy and set its HP.
                        thinker_t *th;
                        for (th = thinkercap.next; th != &thinkercap;
                             th = th->next)
                        {
                            if (th->function.acp1 != (actionf_p1)P_MobjThinker)
                                continue;
                            mobj_t *mo = (mobj_t *)th;
                            if ((mo->flags & MF_SHOOTABLE) &&
                                mo->health > 0 && (int)mo->type == type)
                            {
                                int dx = (int)mo->x - x;
                                int dy = (int)mo->y - y;
                                if (dx < 0) dx = -dx;
                                if (dy < 0) dy = -dy;
                                if (dx < 64*65536 && dy < 64*65536)
                                {
                                    mo->health = h;
                                    restored++;
                                    break;
                                }
                            }
                        }
                    }
                    printf("[AUTOSAVE] restored HP for %d/%d live enemies\n",
                           restored, count);
                }
            }
            fclose(f);
        }
    }
}
static int progress_tic_count = 0;
static boolean progress_initialized = false;
static int progress_auto_slot = 0; // next auto-save slot (0..PROGRESS_NUM_AUTO_SLOTS-1)

// Forward declaration
static boolean D_ProgressLoadFromSlot(int slot);

// Simple checksum: XOR of all 4-byte words.
static unsigned int ProgressChecksum(unsigned char *data, unsigned int len)
{
    unsigned int sum = 0;
    unsigned int i;
    unsigned int *words = (unsigned int *)data;
    unsigned int nwords = len / 4;
    for (i = 0; i < nwords; i++)
        sum ^= words[i];
    // Handle trailing bytes
    for (i = nwords * 4; i < len; i++)
        sum ^= ((unsigned int)data[i] << ((i % 4) * 8));
    return sum;
}

// Find the repo root: the progress file lives in ~/workspace/DiabloDoom
// (persistent across VM recycles, per Jimmy 2026-10-10).
// slot = 0 -> progress.dat (legacy/farm default)
// slot >= 1 -> progress_slotN.dat (user-selectable slots)
static void ProgressGetPathForSlot(char *buf, size_t buflen, int slot)
{
    // Use the current working directory (repo root). The game is run
    // from the repo root, so ./progress.dat is the right location on
    // any machine, regardless of where the repo is checked out.
    if (slot <= 0)
        M_snprintf(buf, buflen, "./" PROGRESS_FILENAME);
    else
    {
        char tmp[64];
        M_snprintf(tmp, sizeof(tmp), PROGRESS_SLOT_FILENAME_FMT, slot);
        M_snprintf(buf, buflen, "./%s", tmp);
    }
}

static void ProgressGetPath(char *buf, size_t buflen)
{
    ProgressGetPathForSlot(buf, buflen, 0);
}

static void ProgressGetAutoDir(char *buf, size_t buflen)
{
    // On Muse VMs: ~/workspace/chocolate-doom/savegames/doom.wad/
    // (persistent, survives recycles).
    // On local machines: ~/.local/share/chocolate-doom/savegames/[wad]/
    extern boolean D_OnMuseVM(void);
    if (D_OnMuseVM())
    {
        const char *home = getenv("HOME");
        if (home)
        {
            M_snprintf(buf, buflen,
                       "%s/workspace/chocolate-doom/savegames/doom.wad",
                       home);
            // Create the directory if it doesn't exist.
            {
                char cmd[1024];
                M_snprintf(cmd, sizeof(cmd), "mkdir -p \"%s\"", buf);
                system(cmd);
            }
            return;
        }
        M_snprintf(buf, buflen, ".");
        return;
    }
    {
        const char *home = getenv("HOME");
        if (home)
        {
            // Use the standard savegames directory for the current IWAD.
            // P_SaveGameFile gives us the pattern; extract the dir.
            char save_path[512];
            extern char *P_SaveGameFile(int slot);
            M_StringCopy(save_path, P_SaveGameFile(0), sizeof(save_path));
            // Strip the filename to get the directory.
            char *last_slash = strrchr(save_path, '/');
            if (last_slash)
            {
                *last_slash = '\0';
                M_snprintf(buf, buflen, "%s", save_path);
                return;
            }
        }
        M_snprintf(buf, buflen, ".");
    }
}

// VM detection: this progress mode is only for Muse VMs, not for
// Jimmy's local machine. We check JARVIS_FQDN for the metaaivm.com suffix.
boolean D_OnMuseVM(void)
{
    const char *fqdn = getenv("JARVIS_FQDN");
    if (fqdn != NULL && strstr(fqdn, "metaaivm.com") != NULL)
        return true;
    // Fallback: HATCH_API_SOCKET is also VM-specific
    if (getenv("HATCH_API_SOCKET") != NULL)
        return true;
    return false;
}

boolean D_ProgressInitSlot(int slot)
{
    char path[512];
    int fd;
    struct stat st;
    boolean file_exists;

    if (progress_initialized)
        return false;

    // Progress persistence is now available everywhere (VM and local).
    // slot = 0 -> progress.dat (legacy/farm default)
    // slot >= 1 -> progress_slotN.dat (user-selectable)

    ProgressGetPathForSlot(path, sizeof(path), slot);
    file_exists = (stat(path, &st) == 0);

    fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0)
    {
        printf("[PROGRESS] cannot open %s, persistence disabled\n", path);
        return false;
    }

    // Ensure the file is the right size
    if (!file_exists || st.st_size != PROGRESS_FILE_SIZE)
    {
        if (ftruncate(fd, PROGRESS_FILE_SIZE) < 0)
        {
            printf("[PROGRESS] ftruncate failed, persistence disabled\n");
            close(fd);
            return false;
        }
        // Zero the new file
        // (mmap will show zeros for the extended region)
    }

    progress_map = mmap(NULL, PROGRESS_FILE_SIZE,
                        PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);

    if (progress_map == MAP_FAILED)
    {
        printf("[PROGRESS] mmap failed, persistence disabled\n");
        progress_map = NULL;
        return false;
    }

    progress_hdr = (progress_header_t *)progress_map;
    progress_slots[0] = (unsigned char *)progress_map + PROGRESS_HEADER_SIZE;
    progress_slots[1] = (unsigned char *)progress_map + PROGRESS_HEADER_SIZE
                        + PROGRESS_SLOT_SIZE;

    progress_initialized = true;

    // Check for a valid existing save
    if (file_exists &&
        progress_hdr->magic == PROGRESS_MAGIC &&
        progress_hdr->version == PROGRESS_VERSION &&
        progress_hdr->active_slot < 2)
    {
        int slot = progress_hdr->active_slot;
        unsigned int size = progress_hdr->save_size[slot];
        unsigned int stored_sum = progress_hdr->checksum[slot];

        if (size > 0 && size <= PROGRESS_SLOT_SIZE &&
            ProgressChecksum(progress_slots[slot], size) == stored_sum)
        {
            printf("[PROGRESS] found valid save (%u bytes), loading...\n", size);
            // Load the save from the mmap'd slot
            // (implemented below via fmemopen)
            if (D_ProgressLoadFromSlot(slot))
            {
                printf("[PROGRESS] save loaded successfully\n");
                return true;
            }
            printf("[PROGRESS] load failed, starting fresh\n");
        }
        else
        {
            printf("[PROGRESS] checksum mismatch or invalid size, starting fresh\n");
        }
    }
    else if (file_exists)
    {
        printf("[PROGRESS] no valid header, starting fresh\n");
    }
    else
    {
        printf("[PROGRESS] new progress file created\n");
    }

    // Initialize header for a fresh file
    memset(progress_hdr, 0, PROGRESS_HEADER_SIZE);
    progress_hdr->magic = PROGRESS_MAGIC;
    progress_hdr->version = PROGRESS_VERSION;
    progress_hdr->active_slot = 0;

    return false;
}

// Serialize the current game state into the inactive slot.
void D_ProgressSaveNow(void)
{
    char *buf = NULL;
    size_t bufsize = 0;
    FILE *mem;
    int inactive;
    extern FILE *save_stream;
    extern boolean savegame_error;

    if (!progress_initialized || progress_map == NULL)
        return;

    // Only save when we're actually in a game (not in menus)
    if (gamestate != GS_LEVEL)
        return;

    mem = open_memstream(&buf, &bufsize);
    if (!mem)
        return;

    // Temporarily redirect the save stream to our memory buffer
    {
        FILE *old_stream = save_stream;
        save_stream = mem;
        savegame_error = false;

        // Serialize (same calls as G_DoSaveGame, minus the file handling)
        P_WriteSaveGameHeader("progress");
        P_ArchivePlayers();
        P_ArchiveWorld();
        P_ArchiveThinkers();
        P_ArchiveSpecials();
        P_ArchiveDiablo();
        P_ArchiveTurn();
        P_WriteSaveGameEOF();

        save_stream = old_stream;
    }

    fclose(mem); // buf and bufsize are now set

    if (savegame_error || buf == NULL || bufsize == 0 ||
        bufsize > PROGRESS_SLOT_SIZE)
    {
        printf("[PROGRESS] save failed (error=%d, size=%zu)\n",
               savegame_error, bufsize);
        free(buf);
        return;
    }

    // Write to the inactive slot (double-buffering for atomicity)
    inactive = progress_hdr->active_slot ^ 1;
    memcpy(progress_slots[inactive], buf, bufsize);
    progress_hdr->save_size[inactive] = bufsize;
    progress_hdr->checksum[inactive] = ProgressChecksum(
        progress_slots[inactive], bufsize);
    // Flip the active slot LAST - this is the atomic commit point.
    // If we crash before this, the old slot is still valid.
    progress_hdr->active_slot = inactive;
    progress_hdr->timestamp = time(NULL);

    // Ensure the header AND data are flushed synchronously.
    // MS_ASYNC was losing saves on kill -9 / VM recycle.
    msync(progress_map, PROGRESS_FILE_SIZE, MS_SYNC);

    free(buf);
}

// Load a save from the given slot. Returns true on success.
// We write the slot data to a temp file and use the existing load
// mechanism, which expects a filename.
static boolean D_ProgressLoadFromSlot(int slot)
{
    unsigned int size;
    char tmpname[512];
    FILE *f;

    if (!progress_initialized || progress_map == NULL)
        return false;

    size = progress_hdr->save_size[slot];
    if (size == 0 || size > PROGRESS_SLOT_SIZE)
        return false;

    // Write to a temp file in /tmp (doesn't need to persist)
    M_snprintf(tmpname, sizeof(tmpname), "/tmp/dd_progress_load.dsg");
    f = fopen(tmpname, "wb");
    if (!f)
        return false;
    if (fwrite(progress_slots[slot], 1, size, f) != size)
    {
        fclose(f);
        return false;
    }
    fclose(f);

    // Use the existing load mechanism. We set up the globals
    // that G_DoLoadGame expects.
    {
        extern char savename[256];
        extern int savegameslot;
        extern gameaction_t gameaction;
        extern boolean singledemo;

        M_StringCopy(savename, tmpname, sizeof(savename));
        // Trigger the load on the next game tic
        gameaction = ga_loadgame;
    }

    return true;
}

// Legacy wrapper: init with slot 0 (progress.dat), for the farm and
// any code that doesn't select a slot.
boolean D_ProgressInit(void)
{
    return D_ProgressInitSlot(0);
}

void D_ProgressTick(void)
{
    // Periodic auto-save disabled: saves are event-driven only
    // (before combat via T_EnterCombat, after loot pickup via p_inter.c).
    // Keeping the tick hook as a no-op in case we want timed saves back.
    (void)progress_initialized;
    (void)progress_tic_count;
}

// Auto-save to Doom savegame slots 7-9 (0-indexed 6-8).
// Rotates through the three slots. Called at the same points as
// D_ProgressSaveNow (before combat, after loot pickup).
// Auto-save is progress.dat only — no legacy Doom savegame format.
// The Doom savegame slots 7-9 are not used; progress.dat is the save.
void D_AutoSaveGame(void)
{
    char path[512];
    char tmp[64];

    if (gamestate != GS_LEVEL)
        return;

    if (!progress_initialized || progress_map == NULL)
        return;

    // Write the current progress to the rotating auto-save slot.
    // This is independent of the player's manual slot; auto-saves
    // rotate through progress_auto1/2/3.dat.
    // Store the current level (episode + map) and player position in the
    // header's reserved bytes so loading can restore them.
    // If a new-game warp is pending (gameaction == ga_newgame), use the
    // target map (d_map), not the stale gamemap.
    {
        extern int gamemap;
        extern int gameepisode;
        extern int d_map;
        extern int d_episode;
        extern gameaction_t gameaction;
        int map_to_store = gamemap;
        int episode_to_store = gameepisode;
        if (gameaction == ga_newgame && d_map >= 1 && d_map <= 32)
        {
            map_to_store = d_map;
            episode_to_store = d_episode;
        }
        progress_hdr->reserved[0] = (unsigned char)map_to_store;
        progress_hdr->reserved[1] = (unsigned char)episode_to_store;
        // Store player position (x, y, angle as 32-bit ints).
        {
            extern player_t players[];
            extern int consoleplayer;
            player_t *pl = &players[consoleplayer];
            if (pl->mo)
            {
                int x = (int)pl->mo->x;
                int y = (int)pl->mo->y;
                int angle = (int)pl->mo->angle;
                memcpy(&progress_hdr->reserved[2], &x, 4);
                memcpy(&progress_hdr->reserved[6], &y, 4);
                memcpy(&progress_hdr->reserved[10], &angle, 4);
            }
        }
        printf("[AUTOSAVE] storing level E%dM%d pos=(%d,%d) (gamemap=%d, gameepisode=%d, gameaction=%d, d_map=%d, d_episode=%d)\n",
               episode_to_store, map_to_store,
               (int)progress_hdr->reserved[2], (int)progress_hdr->reserved[6],
               gamemap, gameepisode, (int)gameaction, d_map, d_episode);
    }
    M_snprintf(tmp, sizeof(tmp), PROGRESS_AUTO_FILENAME_FMT,
               progress_auto_slot + 1);
    {
        char auto_dir[512];
        ProgressGetAutoDir(auto_dir, sizeof(auto_dir));
        M_snprintf(path, sizeof(path), "%s/%s", auto_dir, tmp);
    }

    // Copy the entire progress file (header + slots) to the auto slot.
    // Single slot (no rotation) per user preference.
    {
        FILE *f = fopen(path, "wb");
        if (f)
        {
            fwrite(progress_map, 1, PROGRESS_FILE_SIZE, f);
            fclose(f);
            printf("[AUTOSAVE] %s updated (slot %d)\n",
                   tmp, progress_auto_slot + 1);
        }
        else
        {
            printf("[AUTOSAVE] failed to write %s\n", tmp);
        }
    }

    // Save the list of dead enemies (so loading doesn't revive them).
    {
        char dead_path[512];
        char auto_dir[512];
        ProgressGetAutoDir(auto_dir, sizeof(auto_dir));
        M_snprintf(dead_path, sizeof(dead_path), "%s/progress_auto_dead.dat",
                   auto_dir);
        FILE *f = fopen(dead_path, "wb");
        if (f)
        {
            extern thinker_t thinkercap;
            extern void *P_MobjThinker;
            int count = 0;
            thinker_t *th;
            // First pass: count dead shootable enemies.
            for (th = thinkercap.next; th != &thinkercap; th = th->next)
            {
                if (th->function.acp1 != (actionf_p1)P_MobjThinker)
                    continue;
                mobj_t *mo = (mobj_t *)th;
                if ((mo->flags & MF_SHOOTABLE) && mo->health <= 0 &&
                    (mo->flags & MF_COUNTKILL))
                    count++;
            }
            fwrite(&count, sizeof(int), 1, f);
            // Second pass: write positions and types.
            for (th = thinkercap.next; th != &thinkercap; th = th->next)
            {
                if (th->function.acp1 != (actionf_p1)P_MobjThinker)
                    continue;
                mobj_t *mo = (mobj_t *)th;
                if ((mo->flags & MF_SHOOTABLE) && mo->health <= 0 &&
                    (mo->flags & MF_COUNTKILL))
                {
                    int x = (int)mo->x;
                    int y = (int)mo->y;
                    int type = (int)mo->type;
                    fwrite(&x, sizeof(int), 1, f);
                    fwrite(&y, sizeof(int), 1, f);
                    fwrite(&type, sizeof(int), 1, f);
                }
            }
            fclose(f);
            printf("[AUTOSAVE] saved %d dead enemies\n", count);
        }
    }

    // Save player state (HP, armor, ammo) and live enemies.
    {
        char state_path[512];
        char auto_dir[512];
        ProgressGetAutoDir(auto_dir, sizeof(auto_dir));
        M_snprintf(state_path, sizeof(state_path),
                   "%s/progress_auto_state.dat", auto_dir);
        FILE *f = fopen(state_path, "wb");
        if (f)
        {
            extern player_t players[];
            extern int consoleplayer;
            player_t *pl = &players[consoleplayer];
            // Player HP, armor.
            int hp = pl->health;
            int armor = pl->armortype * 100 + pl->armorpoints;
            fwrite(&hp, sizeof(int), 1, f);
            fwrite(&armor, sizeof(int), 1, f);
            // Ammo (all types).
            int i;
            for (i = 0; i < NUMAMMO; i++)
            {
                int a = pl->ammo[i];
                fwrite(&a, sizeof(int), 1, f);
            }
            // Plasma heat (from turn controller).
            {
                extern turnctrl_t turnctrl;
                int heat = turnctrl.heat;
                fwrite(&heat, sizeof(int), 1, f);
            }
            // Live enemies: count, then x, y, type, health.
            {
                extern thinker_t thinkercap;
                extern void *P_MobjThinker;
                int count = 0;
                thinker_t *th;
                for (th = thinkercap.next; th != &thinkercap; th = th->next)
                {
                    if (th->function.acp1 != (actionf_p1)P_MobjThinker)
                        continue;
                    mobj_t *mo = (mobj_t *)th;
                    if ((mo->flags & MF_SHOOTABLE) && mo->health > 0 &&
                        (mo->flags & MF_COUNTKILL) && mo != pl->mo)
                        count++;
                }
                fwrite(&count, sizeof(int), 1, f);
                for (th = thinkercap.next; th != &thinkercap; th = th->next)
                {
                    if (th->function.acp1 != (actionf_p1)P_MobjThinker)
                        continue;
                    mobj_t *mo = (mobj_t *)th;
                    if ((mo->flags & MF_SHOOTABLE) && mo->health > 0 &&
                        (mo->flags & MF_COUNTKILL) && mo != pl->mo)
                    {
                        int x = (int)mo->x;
                        int y = (int)mo->y;
                        int type = (int)mo->type;
                        int h = mo->health;
                        fwrite(&x, sizeof(int), 1, f);
                        fwrite(&y, sizeof(int), 1, f);
                        fwrite(&type, sizeof(int), 1, f);
                        fwrite(&h, sizeof(int), 1, f);
                    }
                }
                printf("[AUTOSAVE] saved player HP=%d armor=%d, %d live enemies\n",
                       hp, armor, count);
            }
            fclose(f);
        }
    }

    // Single slot (no rotation) per user preference.
    // progress_auto_slot stays at 0.
}

// Load Diablo progress from an auto-save slot (1-based: 1..PROGRESS_NUM_AUTO_SLOTS).
// Copies the auto-save file into the active mmap'd progress region, so the
// game continues from the auto-save state. Sets the rotation so the next
// auto-save overwrites the oldest slot, not the one just loaded.
void D_ProgressLoadAutoSlot(int auto_slot)
{
    char path[512];
    char tmp[64];
    FILE *f;
    size_t n;

    if (auto_slot < 1 || auto_slot > PROGRESS_NUM_AUTO_SLOTS)
        return;
    if (!progress_initialized || progress_map == NULL)
        return;

    M_snprintf(tmp, sizeof(tmp), PROGRESS_AUTO_FILENAME_FMT, auto_slot);
    {
        char auto_dir[512];
        ProgressGetAutoDir(auto_dir, sizeof(auto_dir));
        M_snprintf(path, sizeof(path), "%s/%s", auto_dir, tmp);
    }

    f = fopen(path, "rb");
    if (!f)
    {
        printf("[AUTOSAVE] no auto-save in slot %d\n", auto_slot);
        return;
    }
    n = fread(progress_map, 1, PROGRESS_FILE_SIZE, f);
    fclose(f);
    if (n != PROGRESS_FILE_SIZE)
    {
        printf("[AUTOSAVE] auto-save slot %d is corrupt\n", auto_slot);
        return;
    }
    // Re-point the header/slot pointers (mmap base unchanged, but be safe).
    progress_hdr = (progress_header_t *)progress_map;
    progress_slots[0] = (unsigned char *)progress_map + PROGRESS_HEADER_SIZE;
    progress_slots[1] = (unsigned char *)progress_map + PROGRESS_HEADER_SIZE
                        + PROGRESS_SLOT_SIZE;

    // If the auto-save was from a different level, warp there.
    // On the title screen (gamemap == 0), start a new game on the saved
    // map instead of warping.
    // Player position is restored via D_AutosaveTeleportCheck after the
    // level loads.
    {
        extern int gamemap;
        extern int gameepisode;
        int saved_map = (int)progress_hdr->reserved[0];
        int saved_episode = (int)progress_hdr->reserved[1];
        int saved_x, saved_y, saved_angle;
        memcpy(&saved_x, &progress_hdr->reserved[2], 4);
        memcpy(&saved_y, &progress_hdr->reserved[6], 4);
        memcpy(&saved_angle, &progress_hdr->reserved[10], 4);
        printf("[AUTOSAVE] loaded header: map=%d episode=%d pos=(%d,%d) (current E%dM%d)\n",
               saved_map, saved_episode, saved_x, saved_y,
               gameepisode, gamemap);
        // Queue the teleport (happens after level load).
        if (saved_x != 0 || saved_y != 0)
        {
            autosave_teleport_pending = true;
            autosave_teleport_x = saved_x;
            autosave_teleport_y = saved_y;
            autosave_teleport_angle = saved_angle;
        }
        if (saved_map >= 1 && saved_map <= 32)
        {
            extern void G_DeferedInitNew(int skill, int episode, int map);
            extern int gameskill;
            if (saved_episode < 1 || saved_episode > 4)
                saved_episode = gameepisode; // fallback for old saves
            if (gamemap == 0)
            {
                // Title screen: start new game on the saved map.
                printf("[AUTOSAVE] starting new game on E%dM%d\n",
                       saved_episode, saved_map);
                G_DeferedInitNew(gameskill, saved_episode, saved_map);
            }
            else if (saved_map != gamemap || saved_episode != gameepisode)
            {
                printf("[AUTOSAVE] warping from E%dM%d to E%dM%d\n",
                       gameepisode, gamemap, saved_episode, saved_map);
                // Preserve Diablo progress across the warp (it's in the
                // mmap, not reset by G_DeferedInitNew).
                G_DeferedInitNew(gameskill, saved_episode, saved_map);
            }
        }
    }

    // Single slot (no rotation): keep progress_auto_slot at 0.
    printf("[AUTOSAVE] loaded from slot %d\n", auto_slot);
}
