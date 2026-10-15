// d_replay.h - Instant replay buffer: last 15 seconds of gameplay.
#ifndef D_REPLAY_H
#define D_REPLAY_H

// 15 seconds at 35 FPS (Doom tick rate)
#define REPLAY_FPS 35
#define REPLAY_SECONDS 15
#define REPLAY_MAXFRAMES (REPLAY_FPS * REPLAY_SECONDS)  // 525 frames
#define REPLAY_WIDTH 320
#define REPLAY_HEIGHT 200
#define REPLAY_FRAME_SIZE (REPLAY_WIDTH * REPLAY_HEIGHT)  // paletted, 64KB

// Initialize the replay buffer. Call once at startup.
void D_ReplayInit(void);

// Capture the current framebuffer. Call once per tick.
void D_ReplayTick(void);

// Dump the buffer to an MJPEG AVI file. Returns 0 on success.
int D_ReplaySaveAVI(const char *filename);

// Number of frames currently in the buffer.
int D_ReplayFrameCount(void);

#endif
