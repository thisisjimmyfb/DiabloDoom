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
#include "d_progress.h"
#include "p_saveg.h"
#include "g_game.h"
#include "m_misc.h"

// Progress file location: repo root (persistent across VM recycles).
// The path is relative to the current working directory; the game is
// typically launched from the repo root or we resolve it below.
#define PROGRESS_FILENAME "progress.dat"

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
static int progress_tic_count = 0;
static boolean progress_initialized = false;

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
static void ProgressGetPath(char *buf, size_t buflen)
{
    const char *home = getenv("HOME");
    if (home != NULL)
        M_snprintf(buf, buflen, "%s/workspace/DiabloDoom/" PROGRESS_FILENAME, home);
    else
        M_snprintf(buf, buflen, "./" PROGRESS_FILENAME);
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

boolean D_ProgressInit(void)
{
    char path[512];
    int fd;
    struct stat st;
    boolean file_exists;

    if (progress_initialized)
        return false;

    // Only enable on Muse VMs, not on local machines
    if (!D_OnMuseVM())
    {
        printf("[PROGRESS] not on Muse VM, persistence disabled\n");
        return false;
    }

    ProgressGetPath(path, sizeof(path));
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

    // Ensure the header is flushed (mmap will handle the data pages)
    msync(progress_hdr, PROGRESS_HEADER_SIZE, MS_ASYNC);

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

void D_ProgressTick(void)
{
    if (!progress_initialized)
        return;

    progress_tic_count++;
    if (progress_tic_count >= PROGRESS_WRITE_INTERVAL)
    {
        progress_tic_count = 0;
        printf("[PROGRESS] tick: attempting save (gamestate=%d)\n", gamestate);
        D_ProgressSaveNow();
    }
}
