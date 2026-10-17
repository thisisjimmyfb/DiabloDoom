// d_progress.h - Crash-safe progress persistence via mmap.
//
// The game mmaps a progress file in the repo directory (~/workspace/DiabloDoom).
// Every 120 tics the full game save is serialized into the mmap'd buffer.
// The OS flushes dirty pages automatically, so even a SIGKILL leaves a
// recent save on disk. On boot, a valid progress file is loaded automatically.
//
// Layout: [header][slot A][slot B] - double-buffered for atomic writes.

#ifndef D_PROGRESS_H
#define D_PROGRESS_H

// Call once at boot, before the game starts. Mmaps the progress file;
// loads the save if a valid one exists. Returns true if a save was loaded.
// Uses the default slot 0 (progress.dat).
boolean D_ProgressInit(void);

// Same as D_ProgressInit but for a specific user slot:
// slot = 0 -> progress.dat (legacy/farm default)
// slot >= 1 -> progress_slotN.dat (user-selectable, 1..PROGRESS_NUM_SLOTS)
boolean D_ProgressInitSlot(int slot);

// Number of user-selectable save slots.
#define PROGRESS_NUM_SLOTS 3

// Returns true if running on a Muse VM (for gating VM-only features).
boolean D_OnMuseVM(void);

// Call every game tic. Writes the save every 120 tics.
void D_ProgressTick(void);

// Force an immediate write (e.g., before a risky operation).
void D_ProgressSaveNow(void);
void D_AutoSaveGame(void);

// Load Diablo progress from an auto-save slot (1-based).
void D_ProgressLoadAutoSlot(int auto_slot);

#endif
