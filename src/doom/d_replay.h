// d_replay.h - Instant replay buffer: last 60 seconds of gameplay.
#ifndef D_REPLAY_H
#define D_REPLAY_H

#include <stdint.h>

// 60 seconds at 35 FPS (Doom tick rate). Long buffer so agents with
// 10-20s vision latency can still capture the moment they reacted to.
#define REPLAY_FPS 35
#define REPLAY_SECONDS 60
#define REPLAY_MAXFRAMES (REPLAY_FPS * REPLAY_SECONDS)  // 2100 frames
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

#endif
