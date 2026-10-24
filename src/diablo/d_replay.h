// d_replay.h - Instant replay buffer: last 15 seconds of gameplay.
#ifndef D_REPLAY_H
#define D_REPLAY_H

#include <stdint.h>

// 15 seconds at 35 FPS (Doom tick rate)
#define REPLAY_FPS 35
#define REPLAY_SECONDS 15
#define REPLAY_MAXFRAMES (REPLAY_FPS * REPLAY_SECONDS)  // 525 frames
#define REPLAY_WIDTH 320
#define REPLAY_HEIGHT 200
#define REPLAY_FRAME_SIZE (REPLAY_WIDTH * REPLAY_HEIGHT)  // paletted, 64KB
// Output is 2x upscaled for better perceived quality
#define REPLAY_OUT_WIDTH 640
#define REPLAY_OUT_HEIGHT 400

// Initialize the replay buffer. Call once at startup.
void D_ReplayInit(void);

// Capture the current framebuffer. Call once per tick.
void D_ReplayTick(void);

// Audio hook for SDL_mixer post-mix. Copies mixed audio into ring buffer.
// Called from audio thread; must be lock-free.
void D_ReplayAudioHook(void *udata, uint8_t *stream, int len);

// Dump the buffer to an MP4 file (H.264 + AAC). Returns 0 on success.
int D_ReplaySaveMP4(const char *filename);

// Ensure the doomshots/ output directory exists.
void D_EnsureDoomshotsDir(void);

// Find an unused replay filename: doomshots/diablodoom_YYYYMMDD_HHMMSS.mp4.
void D_ReplayNextFilename(char *buf, size_t bufsz);

// Number of frames currently in the buffer.
int D_ReplayFrameCount(void);

// Continuous capture (--continuous-capture): when on, every full 15-second
// segment in the buffer is queued for encode automatically (see
// D_ReplayTick), so the whole session lands on disk as consecutive
// timestamped segment MP4s instead of only on-demand captures.
void D_ReplaySetAutoCapture(int on);

// Restart continuous capture: drop the in-progress segment so the next
// segment starts clean at the newly loaded world. Called from
// G_DoLoadGame only when a driver-command LOAD requested it (see
// D_ReplayRequestRestartOnLoad) — a load cut must never straddle a
// segment, but only driver loads need this: a driver load shows no
// UI, so the cut looks abrupt in the video.
void D_ReplayRestartCapture(void);

// Mark that the next load (G_DoLoadGame) must restart the capture.
// Called only by the FIFO driver LOAD command — never by menu loads
// or -loadgame, whose UI already shows the load happening.
void D_ReplayRequestRestartOnLoad(void);

// Take (and clear) a pending restart request. Called by G_DoLoadGame
// at the top, so the request is consumed by exactly the load it was
// made for even if that load fails.
int D_ReplayTakeRestartRequest(void);

#endif
