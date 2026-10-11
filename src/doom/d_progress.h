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
boolean D_ProgressInit(void);

// Call every game tic. Writes the save every 120 tics.
void D_ProgressTick(void);

// Force an immediate write (e.g., before a risky operation).
void D_ProgressSaveNow(void);

#endif
