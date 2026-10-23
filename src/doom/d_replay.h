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

// Continuous capture (-autocapture): when on, every full 15-second
// segment in the buffer is queued for encode automatically (see
// D_ReplayTick), so the whole session lands on disk as consecutive
// timestamped segment MP4s instead of only on-demand captures.
void D_ReplaySetAutoCapture(int on);

#endif
