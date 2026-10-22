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
// Rotates through the three slots. Called before combat and after
// loot pickup.
// Auto-save is progress.dat only — no legacy Doom savegame format.
// The Doom savegame slots 7-9 are not used; progress.dat is the save.
void D_AutoSaveGame(void)
{
    char path[512];
    char tmp[64];
    extern gameaction_t gameaction;
    extern boolean g_loading_savegame;

    if (gamestate != GS_LEVEL)
        return;

    if (!progress_initialized || progress_map == NULL)
        return;

    // Don't autosave while a savegame load is pending (ga_loadgame).
    // The load reads the autosave slot; writing to it first would corrupt
    // the pending load with a half-new/half-old state.
    if (gameaction == ga_loadgame)
        return;

    // Don't autosave DURING a load (G_DoLoadGame). The load uses the global
    // save_stream; a reentrant save would reassign it for writing and
    // segfault the in-progress load when it tries to read.
    if (g_loading_savegame)
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
        {
            int stored_x, stored_y;
            memcpy(&stored_x, &progress_hdr->reserved[2], 4);
            memcpy(&stored_y, &progress_hdr->reserved[6], 4);
            printf("[AUTOSAVE] storing level E%dM%d pos=(%d,%d) (gamemap=%d, gameepisode=%d, gameaction=%d, d_map=%d, d_episode=%d)\n",
                   episode_to_store, map_to_store,
                   stored_x >> 16, stored_y >> 16,
                   gamemap, gameepisode, (int)gameaction, d_map, d_episode);
        }
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

    // Auto slots are 0-2 (display 1-3); manual are 3-5 (display 4-6).
    // With SAVEGAMESIZE expanded to 4MB, Diablo data fits.
    // No rotation: auto-save always uses slot 1 (Doom slot 0).
    {
        extern void G_DoSaveGameSlot(int slot, const char *desc);
        G_DoSaveGameSlot(0, "AUTO 1");
        printf("[AUTOSAVE] Doom savegame written to slot 0\n");
    }
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

    // First, try to load the full Doom savegame.
    // Auto slots are 0-2 (display 1-3). auto_slot is 1-based.
    // This restores everything: position, enemies, doors, items.
    {
        extern char *P_SaveGameFile(int slot);
        extern void G_LoadGame(char *name);
        char dsg_path[512];
        int doom_slot = auto_slot - 1; // 1-based to 0-based
        // Copy to local buffer: P_SaveGameFile uses a static buffer
        // that ProgressGetAutoDir overwrites via P_SaveGameFile(0).
        M_StringCopy(dsg_path, P_SaveGameFile(doom_slot), sizeof(dsg_path));
        FILE *dsg = fopen(dsg_path, "rb");
        if (dsg)
        {
            fclose(dsg);
            printf("[AUTOSAVE] loading Doom savegame from slot %d\n",
                   doom_slot);
            // Load the Diablo progress first (gear), then the Doom state.
            // The Doom savegame has the level state; Diablo gear comes
            // from the progress file.
            M_snprintf(tmp, sizeof(tmp), PROGRESS_AUTO_FILENAME_FMT,
                       auto_slot);
            {
                char auto_dir[512];
                ProgressGetAutoDir(auto_dir, sizeof(auto_dir));
                M_snprintf(path, sizeof(path), "%s/%s", auto_dir, tmp);
            }
            f = fopen(path, "rb");
            if (f)
            {
                n = fread(progress_map, 1, PROGRESS_FILE_SIZE, f);
                fclose(f);
                if (n == PROGRESS_FILE_SIZE)
                {
                    progress_hdr = (progress_header_t *)progress_map;
                    progress_slots[0] = (unsigned char *)progress_map +
                                        PROGRESS_HEADER_SIZE;
                    progress_slots[1] = (unsigned char *)progress_map +
                                        PROGRESS_HEADER_SIZE +
                                        PROGRESS_SLOT_SIZE;
                    printf("[AUTOSAVE] Diablo progress loaded\n");
                }
            }
            // Now load the Doom savegame (full level state).
            G_LoadGame(dsg_path);
            printf("[AUTOSAVE] loaded from slot %d (full savegame)\n",
                   auto_slot);
            return;
        }
        // No paired Doom savegame: this auto slot has no world state,
        // and there is deliberately no fallback anymore. The old warp
        // fallback loaded the progress (gear) file alone, started a
        // FRESH level and teleported the player in -- silently
        // resurrecting every monster (Jimmy's report, 2026-10-21:
        // 'saved games are reviving dead monsters'). It bit auto
        // slots 2 and 3 in practice: the auto-save keeps a single
        // slot (slot 1 -- progress_auto1.dat + doomsav0.dsg), so
        // their .dsg partners never exist, while stale
        // progress_auto2/3.dat files from the rotating era made the
        // fake load look real. A load is the full savegame or an
        // honest failure.
        printf("[AUTOSAVE] auto slot %d has no saved world -- load refused\n",
               auto_slot);
        if (gamestate == GS_LEVEL)
        {
            extern player_t players[];
            extern int consoleplayer;
            players[consoleplayer].message =
                "That auto slot has no saved game.";
        }
        return;
    }
}
