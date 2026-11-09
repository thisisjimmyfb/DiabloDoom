// d_replay.c - Instant replay: circular buffer of the last segment
// (525 captured frames). Outputs MP4 (H.264) for browser/Threads
// compatibility.
//
// A/V sync (variable frame rate): every captured frame carries a
// timestamp (I_GetTimeMS, the same SDL_GetTicks clock the audio
// thread runs on) and the audio-ring write position at capture time.
// At save time the audio slice is cut to exactly the video window
// (first frame's audio position through the last frame's), and the
// MP4 video track is written with per-frame durations taken from
// those timestamps instead of one fixed 1/35s duration. Audio is
// never sped up or slowed down: it plays at 44.1kHz, and the video
// track is timed to true capture time, so a segment captured at
// 40 fps plays back at 40 fps and one captured at 25 fps at 25 fps.
// Without this, video assumed 35 fps while audio ran in real time,
// and the two drifted apart mid-segment (about 0.9s in one measured
// clip) even though live play was in sync.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <pthread.h>
#ifdef NO_X264
// No H.264 support: D_ReplaySaveMP4 will report unavailability.
#else
#include <x264.h>
#endif
#ifdef NO_FAAC
// No AAC support: replays will be video-only.
#else
#include <faac.h>
#endif
#include "doomdef.h"
#include "d_replay.h"
#include "../i_timer.h"
#include "i_video.h"
#include "v_video.h"
#include "../tables.h"

void D_EnsureDoomshotsDir(void)
{
    // On Muse VMs: /tmp/doomshots (ephemeral, won't fill persistent storage).
    // On local machines: doomshots/ under the current folder (same place
    // screenshots land, easy to find next to the game).
    // The farm uploads interesting ones before they're lost on recycle.
    struct stat st;
    const char *dir;
    extern boolean D_OnMuseVM(void);
    if (D_OnMuseVM())
        dir = "/tmp/doomshots";
    else
        dir = "doomshots";
    if (stat(dir, &st) != 0)
    {
        if (mkdir(dir, 0755) != 0 && errno != EEXIST)
            printf("[REPLAY] warning: cannot create %s/\n", dir);
    }
}

// Returns the doomshots directory for this machine.
static const char *D_DoomshotsDir(void)
{
    extern boolean D_OnMuseVM(void);
    if (D_OnMuseVM())
        return "/tmp/doomshots";
    return "doomshots";
}

// Find an unused replay filename: /tmp/doomshots/diablodoom_YYYYMMDD_HHMMSS.mp4,
// with a numeric suffix if the timestamp is taken.
void D_ReplayNextFilename(char *buf, size_t bufsz)
{
    struct stat st;
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    int i;
    for (i = 0; i < 100; i++)
    {
        const char *dir = D_DoomshotsDir();
        if (i == 0)
            snprintf(buf, bufsz, "%s/diablodoom_%04d%02d%02d_%02d%02d%02d.mp4",
                     dir,
                     tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                     tm->tm_hour, tm->tm_min, tm->tm_sec);
        else
            snprintf(buf, bufsz, "%s/diablodoom_%04d%02d%02d_%02d%02d%02d_%02d.mp4",
                     dir,
                     tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                     tm->tm_hour, tm->tm_min, tm->tm_sec, i);
        if (stat(buf, &st) != 0)
            return;  // not taken
    }
    // Extremely unlikely: fall back to pid-suffixed name
    snprintf(buf, bufsz, "doomshots/diablodoom_%04d%02d%02d_%02d%02d%02d_%d.mp4",
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec, (int)getpid());
}

static byte *replay_buffer = NULL;
static int replay_head = 0;
static int replay_count = 0;
// Per-frame capture time (ms, I_GetTimeMS) and audio-ring write
// position (int16 samples, interleaved stereo) at capture time,
// indexed the same way as replay_buffer. These are what make the
// saved MP4 variable-frame-rate instead of assumed-35fps.
static uint32_t replay_frame_ts[REPLAY_MAXFRAMES];
static int replay_frame_audio[REPLAY_MAXFRAMES];
static int replay_autocapture = 0;   // -autocapture: save every segment
static volatile int replay_encode_busy = 0;  // an encode job is in flight

void D_ReplaySetAutoCapture(int on)
{
    replay_autocapture = on;
}

// Pending capture-restart request (see D_ReplayRequestRestartOnLoad).
// Set only by the FIFO driver LOAD command; consumed by G_DoLoadGame.
static int replay_restart_requested = 0;

void D_ReplayRequestRestartOnLoad(void)
{
    replay_restart_requested = 1;
}

int D_ReplayTakeRestartRequest(void)
{
    int r = replay_restart_requested;
    replay_restart_requested = 0;
    return r;
}

// Restart continuous capture: drop the in-progress segment so the next
// segment starts clean at the newly loaded world. Called when any load
// completes (G_DoLoadGame) — a load cut must never straddle a segment.
// An encode already in flight is untouched: it holds its own snapshot
// of purely pre-load frames and lands as its own segment. The next
// save fires only once the count rebuilds to a full segment, so the
// audio window at save time is post-load too.
void D_ReplayRestartCapture(void)
{
    replay_count = 0;
    replay_head = 0;
    printf("[REPLAY] capture restarted (load)\n");
}

// Audio ring buffer: 44.1kHz stereo 16-bit PCM, sized to cover a
// full video segment even when capture runs slow. A segment is 525
// frames, which is 15s at 35 fps but 21s at 25 fps, so a 15s ring
// would already have overwritten the first seconds of audio on a
// slow segment. 30s covers capture down to about 17 fps.
#define REPLAY_AUDIO_RATE 44100
#define REPLAY_AUDIO_CHANNELS 2
#define REPLAY_AUDIO_SECS 30
#define REPLAY_AUDIO_SAMPLES (REPLAY_AUDIO_RATE * REPLAY_AUDIO_SECS)
#define REPLAY_AUDIO_BYTES (REPLAY_AUDIO_SAMPLES * REPLAY_AUDIO_CHANNELS * 2)
static int16_t *replay_audio_buf = NULL;
static volatile int replay_audio_write = 0; // write position (samples, mono count)
static int replay_audio_init_done = 0;

void D_ReplayInit(void)
{
    D_EnsureDoomshotsDir();
    if (replay_buffer) return;
    replay_buffer = malloc(REPLAY_MAXFRAMES * REPLAY_FRAME_SIZE);
    if (replay_buffer) {
        memset(replay_buffer, 0, REPLAY_MAXFRAMES * REPLAY_FRAME_SIZE);
        printf("[REPLAY] buffer ready: %d frames\n", REPLAY_MAXFRAMES);
    }
    // Audio ring buffer.
    if (!replay_audio_buf) {
        replay_audio_buf = malloc(REPLAY_AUDIO_BYTES);
        if (replay_audio_buf) {
            memset(replay_audio_buf, 0, REPLAY_AUDIO_BYTES);
            printf("[REPLAY] audio buffer ready: %d secs\n", REPLAY_AUDIO_SECS);
        }
    }
}

// Called from SDL_mixer post-mix hook (audio thread). Copies mixed stereo
// 16-bit audio into the ring buffer. Must be lock-free and fast.
void D_ReplayAudioHook(void *udata, uint8_t *stream, int len)
{
    int16_t *src = (int16_t *)stream;
    int samples = len / 2; // 16-bit samples (stereo interleaved)
    int i;
    (void)udata;
    if (!replay_audio_buf)
        return;
    for (i = 0; i < samples; i++) {
        // Ring buffer holds interleaved stereo samples.
        // write pos is in samples (not frames).
        int pos = replay_audio_write % (REPLAY_AUDIO_SAMPLES * REPLAY_AUDIO_CHANNELS);
        replay_audio_buf[pos] = src[i];
        replay_audio_write++;
    }
}

void D_ReplayTick(void)
{
    extern pixel_t *I_VideoBuffer;
    if (!replay_buffer || !I_VideoBuffer) return;
    // Timestamp first, then copy: the timestamp and audio position
    // describe this frame, and the audio thread can advance the
    // write position at any moment, so read it at capture time.
    replay_frame_ts[replay_head] = (uint32_t)I_GetTimeMS();
    replay_frame_audio[replay_head] = replay_audio_write;
    memcpy(replay_buffer + (replay_head * REPLAY_FRAME_SIZE),
           I_VideoBuffer, REPLAY_FRAME_SIZE);
    replay_head = (replay_head + 1) % REPLAY_MAXFRAMES;
    if (replay_count < REPLAY_MAXFRAMES) replay_count++;

    // Continuous capture: the moment the buffer holds a full segment,
    // queue its encode (async, on the encoder thread) and start the
    // next segment -- D_ReplaySaveMP4 snapshots and clears the ring.
    // If the encoder is still busy with the previous segment, hold:
    // the ring keeps the newest frames (oldest overwritten) and the
    // save fires the tic the encoder frees up, so the segment that
    // lands is always a full 15 seconds.
    if (replay_autocapture && replay_count >= REPLAY_MAXFRAMES
        && !replay_encode_busy)
    {
        char name[256];
        D_ReplayNextFilename(name, sizeof(name));
        D_ReplaySaveMP4(name);
    }
}

int D_ReplayFrameCount(void) { return replay_count; }

typedef struct { byte *data; int len; int cap; } mem_buf_t;
static void buf_append(mem_buf_t *m, const void *data, int size) {
    if (m->len + size > m->cap) {
        m->cap = (m->cap + size) * 2 + 1024;
        m->data = realloc(m->data, m->cap);
    }
    memcpy(m->data + m->len, data, size);
    m->len += size;
}

// Big-endian writers for MP4
static void mp4_w32(FILE *f, unsigned int v) {
    fputc((v>>24)&0xff,f); fputc((v>>16)&0xff,f);
    fputc((v>>8)&0xff,f); fputc(v&0xff,f);
}
static void mp4_w16(FILE *f, unsigned int v) {
    fputc((v>>8)&0xff,f); fputc(v&0xff,f);
}
static void mp4_w64(FILE *f, unsigned long long v) {
    mp4_w32(f, (unsigned int)((v>>32)&0xffffffff));
    mp4_w32(f, (unsigned int)(v&0xffffffff));
}

#ifndef NO_X264
// RGB (packed, 24-bit) -> YUV420P into x264 picture planes (BT.601).
static void rgb_to_yuv420p(byte *rgb, x264_picture_t *pic, int w, int h)
{
    int x, y, dx, dy;
    byte *yp = pic->img.plane[0];
    byte *up = pic->img.plane[1];
    byte *vp = pic->img.plane[2];
    int ys = pic->img.i_stride[0];
    int uvs = pic->img.i_stride[1];
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            int r = rgb[(y*w+x)*3+0];
            int g = rgb[(y*w+x)*3+1];
            int b = rgb[(y*w+x)*3+2];
            int Y = (77*r + 150*g + 29*b) >> 8;
            yp[y*ys+x] = (byte)(Y < 0 ? 0 : (Y > 255 ? 255 : Y));
            if ((x & 1) == 0 && (y & 1) == 0) {
                // 2x2 chroma average (solid color from NN upscale, but average anyway)
                int r2 = 0, g2 = 0, b2 = 0, cnt = 0;
                int U, V;
                for (dy = 0; dy < 2 && y+dy < h; dy++)
                    for (dx = 0; dx < 2 && x+dx < w; dx++) {
                        r2 += rgb[((y+dy)*w+x+dx)*3+0];
                        g2 += rgb[((y+dy)*w+x+dx)*3+1];
                        b2 += rgb[((y+dy)*w+x+dx)*3+2];
                        cnt++;
                    }
                r2 /= cnt; g2 /= cnt; b2 /= cnt;
                U = (((-43*r2 - 84*g2 + 127*b2) >> 8) + 128);
                V = (((127*r2 - 106*g2 - 21*b2) >> 8) + 128);
                up[(y/2)*uvs + (x/2)] = (byte)(U < 0 ? 0 : (U > 255 ? 255 : U));
                vp[(y/2)*uvs + (x/2)] = (byte)(V < 0 ? 0 : (V > 255 ? 255 : V));
            }
        }
    }
}
#endif

#ifndef NO_FAAC
// Encode the 15-second audio ring buffer to raw AAC frames (AAC-LC,
// 44.1kHz stereo, ~128kbps). On success returns the frame count (>0) and
// fills outputs; the caller owns each aframes[i].data, the aframes array,
// and the asc copy. Returns 0 when no audio is available or encoding
// failed (caller should fall back to video-only).
// Main-thread helper: snapshot one window of the audio ring into a
// freshly malloc'd contiguous PCM buffer, oldest -> newest. start_pos
// and end_pos are absolute replay_audio_write values (int16 samples,
// interleaved stereo) taken from the video frames being saved, so
// this window is exactly the video window, not a fixed 15s. Samples
// that have already been overwritten in the ring, or not yet mixed
// (the tail past the current write position), come out as silence.
// Returns NULL when audio capture is not running. Caller owns the
// buffer; *samples_per_chan_out is the per-channel sample count.
// Differences are computed in signed int arithmetic so they stay
// correct across the write counter wrapping.
static int16_t *replay_snapshot_pcm_window(int start_pos, int end_pos,
                                           uint32_t *samples_per_chan_out)
{
    int total = REPLAY_AUDIO_SAMPLES * REPLAY_AUDIO_CHANNELS; // int16 count
    int wpos;
    int count;
    int16_t *pcm;
    int i;

    *samples_per_chan_out = 0;
    if (!replay_audio_buf)
        return NULL;
    count = end_pos - start_pos;
    if (count <= 0)
        return NULL;
    if (count > total) {
        // Window longer than the ring (very slow capture): keep the
        // most recent audio, drop the earliest video frames' audio.
        printf("[REPLAY] audio window %d samples exceeds ring %d, trimming\n",
               count, total);
        start_pos = end_pos - total;
        count = total;
    }
    pcm = malloc((size_t)count * 2);
    if (!pcm)
        return NULL;
    memset(pcm, 0, (size_t)count * 2);
    wpos = replay_audio_write;
    for (i = 0; i < count; i++) {
        int pos = start_pos + i;
        int age = wpos - pos; // 0 = just written, positive = older
        if (age >= 0 && age < total) {
            int idx = pos % total;
            if (idx < 0) idx += total;
            pcm[i] = replay_audio_buf[idx];
        }
        // else: not yet written, or already overwritten -> silence
    }
    *samples_per_chan_out = (uint32_t)(count / REPLAY_AUDIO_CHANNELS);
    return pcm;
}

// Resample an interleaved-stereo PCM snapshot by linear
// interpolation, per channel. in_per_chan is the input per-channel
// frame count; out_per_chan is the target. Returns a fresh malloc'd
// buffer (caller owns) or NULL on failure. Used to stretch audio
// captured at a slower-than-nominal write rate back to the video
// window at 44100 Hz: the SDL audio thread can starve under CPU
// load (game + x264 encode + Xvfb), so the write position advances
// slower than REPLAY_AUDIO_RATE and the snapshot comes out short --
// the AAC track would otherwise be time-compressed and lead the
// picture. Linear interpolation is adequate for game audio.
static int16_t *replay_resample_pcm_linear(const int16_t *pcm,
                                           uint32_t in_per_chan,
                                           uint32_t out_per_chan)
{
    int16_t *out;
    uint32_t j;
    if (!pcm || in_per_chan == 0 || out_per_chan == 0)
        return NULL;
    out = malloc((size_t)out_per_chan * REPLAY_AUDIO_CHANNELS * 2);
    if (!out)
        return NULL;
    if (out_per_chan == 1 || in_per_chan == 1) {
        // Degenerate: fill with the first frame (or silence).
        for (j = 0; j < out_per_chan; j++) {
            out[j * REPLAY_AUDIO_CHANNELS] =
                in_per_chan > 0 ? pcm[0] : 0;
            out[j * REPLAY_AUDIO_CHANNELS + 1] =
                in_per_chan > 0 ? pcm[1] : 0;
        }
        return out;
    }
    for (j = 0; j < out_per_chan; j++) {
        // Input position in per-channel frames, endpoints preserved.
        double pos = (double)j * (double)(in_per_chan - 1)
                     / (double)(out_per_chan - 1);
        uint32_t i0 = (uint32_t)pos;
        uint32_t i1 = i0 + 1;
        double frac = pos - (double)i0;
        int ch;
        if (i1 >= in_per_chan) {
            i1 = in_per_chan - 1;
            frac = 0.0;
        }
        for (ch = 0; ch < REPLAY_AUDIO_CHANNELS; ch++) {
            double s0 = (double)pcm[i0 * REPLAY_AUDIO_CHANNELS + ch];
            double s1 = (double)pcm[i1 * REPLAY_AUDIO_CHANNELS + ch];
            double s = s0 + (s1 - s0) * frac;
            int v = (int)(s + (s >= 0 ? 0.5 : -0.5));
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            out[j * REPLAY_AUDIO_CHANNELS + ch] = (int16_t)v;
        }
    }
    return out;
}

// Piecewise resample: warp the audio snapshot onto the video
// timeline using the per-frame (ts, aud) pairs. The pairs are a
// wall-clock timestamp and an audio write position captured
// together in D_ReplayTick, so the slope between consecutive
// pairs is the TRUE write rate over that frame's interval -- and
// that rate can wander inside a segment (the SDL audio thread
// starves more or less as CPU load changes), so a single constant
// rate fixes the average compression but not the local warp. This
// maps each output frame (uniform 44100 Hz grid over the video
// window) back to its write position through the piecewise-linear
// (ts -> aud) mapping, then linearly interpolates the PCM there.
// pcm is the interleaved-stereo snapshot of write positions
// [audio_start, audio_start + in_per_chan*2); ts/aud are the
// per-frame pairs (n frames); tail_rate is the local rate past
// the last frame's timestamp; true_rate is the average int16/ms,
// used when an interval is degenerate. Returns a fresh malloc'd
// buffer (caller owns) or NULL on failure. Falls back to a
// uniform linear stretch when the pairs are unusable (n < 2,
// timestamps or audio not increasing).
static int16_t *replay_resample_pcm_piecewise(const int16_t *pcm,
                                              uint32_t in_per_chan,
                                              int audio_start,
                                              const uint32_t *ts,
                                              const int *aud,
                                              int n,
                                              double tail_rate,
                                              double true_rate,
                                              uint32_t out_per_chan)
{
    int16_t *out;
    uint32_t j;
    int i; // current interval: ts[i] <= t < ts[i+1], or the tail
    if (!pcm || in_per_chan == 0 || out_per_chan == 0)
        return NULL;
    if (n < 2 || ts[n - 1] <= ts[0] || aud[n - 1] <= aud[0])
        return replay_resample_pcm_linear(pcm, in_per_chan,
                                          out_per_chan);
    out = malloc((size_t)out_per_chan * REPLAY_AUDIO_CHANNELS * 2);
    if (!out)
        return NULL;
    if (out_per_chan == 1) {
        // Single output frame: the window start.
        out[0] = pcm[0];
        out[1] = pcm[1];
        return out;
    }
    i = 0;
    for (j = 0; j < out_per_chan; j++) {
        // Video time of this output frame (ms).
        double t = (double)ts[0]
                   + (double)j * 1000.0 / (double)REPLAY_AUDIO_RATE;
        double p; // write position (interleaved int16)
        double f; // per-channel frame index into pcm
        uint32_t f0;
        double frac;
        int ch;
        // Advance to the interval containing t (t is monotonic).
        // A zero timestamp gap is skipped: ts[i+1] <= t advances.
        while (i + 1 < n - 1 && (double)ts[i + 1] <= t)
            i++;
        if (t >= (double)ts[n - 1]) {
            // Tail: past the last frame's timestamp, use the
            // last interval's local rate.
            p = (double)aud[n - 1]
                + (t - (double)ts[n - 1]) * tail_rate;
        } else if (ts[i + 1] > ts[i] && aud[i + 1] > aud[i]) {
            // Interior interval: piecewise-linear (ts -> aud).
            p = (double)aud[i]
                + (double)(aud[i + 1] - aud[i])
                  * (t - (double)ts[i])
                  / (double)(ts[i + 1] - ts[i]);
        } else {
            // Degenerate interval (stalled audio thread): fall
            // back to the average rate, anchored at this frame.
            p = (double)aud[i] + (t - (double)ts[i]) * true_rate;
        }
        // Sample the snapshot at write position p. The snapshot
        // covers write positions [audio_start, ...); per-channel
        // frame f = (p - audio_start) / 2. Clamp to the snapshot
        // (the tail can run slightly past it when the local rate
        // exceeds the average used for the window end).
        f = (p - (double)audio_start) / (double)REPLAY_AUDIO_CHANNELS;
        if (f < 0.0)
            f = 0.0;
        if (f > (double)(in_per_chan - 1))
            f = (double)(in_per_chan - 1);
        f0 = (uint32_t)f;
        frac = f - (double)f0;
        if (f0 >= in_per_chan - 1) {
            f0 = in_per_chan - 1;
            frac = 0.0;
        }
        for (ch = 0; ch < REPLAY_AUDIO_CHANNELS; ch++) {
            double s0 = (double)pcm[f0 * REPLAY_AUDIO_CHANNELS + ch];
            double s1 = (double)pcm[(f0 + 1 < in_per_chan
                                     ? f0 + 1 : f0)
                                    * REPLAY_AUDIO_CHANNELS + ch];
            double s = s0 + (s1 - s0) * frac;
            int v = (int)(s + (s >= 0 ? 0.5 : -0.5));
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            out[j * REPLAY_AUDIO_CHANNELS + ch] = (int16_t)v;
        }
    }
    return out;
}

// Encode a PCM snapshot to AAC frames. Does NOT free pcm (caller owns).
// samples_per_chan_in is the snapshot's per-channel length (the video
// window's audio length, not a fixed 15s); it is echoed back in
// samples_per_chan_out on success.
static int replay_encode_audio_pcm(int16_t *pcm,
                               uint32_t samples_per_chan_in,
                               mem_buf_t **aframes_out,
                               uint8_t **asc_out, uint32_t *asc_len_out,
                               uint32_t *samples_per_chan_out)
{
    faac_encoder *enc = NULL;
    faac_encoder_info info;
    const uint8_t *asc;
    uint32_t asc_len;
    uint8_t *outbuf = NULL;
    mem_buf_t *aframes = NULL;
    int aframes_cap = 0, aframes_n = 0;
    int i;
    uint32_t samples_per_chan = samples_per_chan_in;
    uint32_t frames_needed;

    *aframes_out = NULL;
    *asc_out = NULL;
    *asc_len_out = 0;
    *samples_per_chan_out = 0;

    if (!pcm || samples_per_chan == 0)
        return 0;

    {
        faac_params params;
        if (faac_params_init(&params, sizeof(params)) != FAAC_OK) {
            printf("[REPLAY] faac_params_init failed\n");
            goto adone;
        }
        params.sample_rate = REPLAY_AUDIO_RATE;
        params.num_channels = REPLAY_AUDIO_CHANNELS;
        params.mpeg_version = FAAC_MPEG4;
        params.object_type = FAAC_OBJ_LOW;   // AAC-LC
        params.bit_rate = 64000;             // per channel -> 128 kbps
        params.input_format = FAAC_INPUT_16BIT;
        params.output_format = FAAC_STREAM_RAW;
        if (faac_encoder_open(&params, &enc) != FAAC_OK || !enc) {
            printf("[REPLAY] faac_encoder_open failed\n");
            goto adone;
        }
        info.struct_size = sizeof(info);
        if (faac_encoder_get_info(enc, &info) != FAAC_OK) {
            printf("[REPLAY] faac_encoder_get_info failed\n");
            goto adone;
        }
        if (faac_encoder_asc(enc, &asc, &asc_len) != FAAC_OK ||
            !asc || asc_len == 0) {
            printf("[REPLAY] faac_encoder_asc failed\n");
            goto adone;
        }
        printf("[REPLAY] AAC: %u Hz %u ch, %u samples/frame, asc %u bytes\n",
               info.sample_rate, REPLAY_AUDIO_CHANNELS,
               info.frame_samples, asc_len);
    }

    outbuf = malloc(info.max_output_bytes);
    frames_needed = (samples_per_chan + info.frame_samples - 1)
                    / info.frame_samples;
    // Room for every encoded frame plus the flush output. The old
    // fixed 700 only covered a 15s window; a slow-capture window
    // can be up to the 30s ring.
    aframes_cap = (int)frames_needed + 16;
    aframes = calloc((size_t)aframes_cap, sizeof(mem_buf_t));
    if (!outbuf || !aframes)
        goto adone;

    {
        uint32_t in_per_frame = info.frame_samples * REPLAY_AUDIO_CHANNELS;
        uint32_t remaining = samples_per_chan * REPLAY_AUDIO_CHANNELS;
        int16_t *ip = pcm;
        uint32_t written;
        for (i = 0; (uint32_t)i < frames_needed; i++) {
            uint32_t take = remaining < in_per_frame ? remaining : in_per_frame;
            if (faac_encoder_encode(enc, ip, take, outbuf,
                                    info.max_output_bytes,
                                    &written) != FAAC_OK) {
                printf("[REPLAY] faac encode failed at frame %d\n", i);
                goto adone;
            }
            if (written > 0) {
                if (aframes_n >= aframes_cap)
                    goto adone; // shouldn't happen
                buf_append(&aframes[aframes_n], outbuf, (int)written);
                aframes_n++;
            }
            ip += take;
            remaining -= take;
        }
        // Flush the encoder.
        for (;;) {
            uint32_t written2;
            if (faac_encoder_encode(enc, NULL, 0, outbuf,
                                    info.max_output_bytes,
                                    &written2) != FAAC_OK)
                break;
            if (written2 == 0 || aframes_n >= aframes_cap)
                break;
            buf_append(&aframes[aframes_n], outbuf, (int)written2);
            aframes_n++;
        }
    }

    if (aframes_n == 0) {
        printf("[REPLAY] AAC produced no frames\n");
        goto adone;
    }

    *asc_out = malloc(asc_len);
    if (!*asc_out)
        goto adone;
    memcpy(*asc_out, asc, asc_len);
    *asc_len_out = asc_len;
    *aframes_out = aframes;
    *samples_per_chan_out = samples_per_chan;
    aframes = NULL; // ownership transferred

    printf("[REPLAY] AAC encoded %d frames\n", aframes_n);

adone:
    if (enc)
        faac_encoder_close(&enc);
    free(outbuf);
    if (aframes) {
        for (i = 0; i < aframes_n; i++)
            free(aframes[i].data);
        free(aframes);
    }
    if (*aframes_out)
        return aframes_n;
    free(*asc_out);
    *asc_out = NULL;
    return 0;
}
#endif

// ------------------------------------------------------------------
// Async replay encode. The game thread snapshots the ring buffers
// (a few memcpy's); a detached worker thread runs the whole encode +
// MP4 mux and rename()s the finished file into place. A stalled encode
// can no longer freeze the tick loop or the FIFO driver.
// ------------------------------------------------------------------

typedef struct {
    byte *video;          // n frames in display order (REPLAY_FRAME_SIZE each)
    int n;
    int16_t *pcm;         // audio snapshot, owned (may be NULL)
    uint32_t *frame_durs; // per-frame durations, movie timescale (90000)
    uint32_t audio_samples_per_chan; // pcm length, per channel
    byte palette[256 * 3];
    char filename[256];
    char tmpname[264];
} replay_job_t;

static void *replay_encode_worker(void *arg)
{
    replay_job_t *job = (replay_job_t *)arg;
    const char *filename = job->filename;
#ifdef NO_X264
    (void)job;
    (void)filename;
    return NULL;
#else
    FILE *f = NULL;
    byte *rgb = NULL, *palette = job->palette;
    mem_buf_t *frames = NULL;
    int *frame_sizes = NULL;
    unsigned long long *chunk_offsets = NULL;
    byte *sps = NULL, *pps = NULL;
    int sps_len = 0, pps_len = 0;
    // AAC audio state (stays zeroed/NULL when NO_FAAC or on failure).
    int has_audio = 0;
    mem_buf_t *aframes = NULL;
    int naframes = 0;
    int *audio_sizes = NULL;
    unsigned long long *audio_offsets = NULL;
    uint8_t *audio_asc = NULL;
    uint32_t audio_asc_len = 0;
    uint32_t audio_samples = 0;      // samples per channel
    unsigned int audio_movie_dur = 0; // audio duration in movie timescale
    x264_t *enc = NULL;
    x264_picture_t pic_in;
    int pic_allocated = 0;
    int i, j, idx, n;
    int timescale = 90000;
    // Video track duration, in movie timescale units, is the sum of
    // the per-frame durations saved with the frames (variable frame
    // rate). x264 still encodes with a nominal 35 fps timebase and
    // frame-index pts; the MP4 stts below carries the true timing,
    // which is what players use for display.
    unsigned int video_movie_dur = 0;

    n = job->n;
    if (job->frame_durs) {
        for (i = 0; i < n; i++)
            video_movie_dur += job->frame_durs[i];
    }
    if (video_movie_dur == 0)
        video_movie_dur = (unsigned int)n * (timescale / REPLAY_FPS);
    printf("[REPLAY] encoding %d frames to H.264 MP4...\n", n);

    rgb = malloc(REPLAY_OUT_WIDTH * REPLAY_OUT_HEIGHT * 3);
    frames = calloc(n, sizeof(mem_buf_t));
    frame_sizes = malloc(n * sizeof(int));
    chunk_offsets = malloc(n * sizeof(unsigned long long));
    if (!rgb || !frames || !frame_sizes || !chunk_offsets || !palette)
        goto fail;

    // Apply the same gamma correction the game uses for display
    {
        extern int usegamma;
        static byte gpal[256*3];
        int g = usegamma;
        if (g < 0) g = 0; if (g > 4) g = 4;
        // Match I_SetPalette: gammatable + clear bottom 2 bits
        for (i = 0; i < 256; i++) {
            gpal[i*3+0] = gammatable[g][palette[i*3+0]] & ~3;
            gpal[i*3+1] = gammatable[g][palette[i*3+1]] & ~3;
            gpal[i*3+2] = gammatable[g][palette[i*3+2]] & ~3;
        }
        palette = gpal;  // use gamma-corrected palette from here on
    }

    // ---- x264 setup: baseline profile, ultrafast, CRF 23 ----
    {
        x264_param_t param;
        x264_nal_t *nals;
        int num_nals;
        if (x264_param_default_preset(&param, "ultrafast", NULL) < 0) {
            printf("[REPLAY] x264_param_default_preset failed\n");
            goto fail;
        }
        if (x264_param_apply_profile(&param, "baseline") < 0) {
            printf("[REPLAY] x264_param_apply_profile failed\n");
            goto fail;
        }
        param.i_width = REPLAY_OUT_WIDTH;
        param.i_height = REPLAY_OUT_HEIGHT;
        param.i_fps_num = REPLAY_FPS;
        param.i_fps_den = 1;
        param.i_csp = X264_CSP_I420;
        param.b_annexb = 0;          // length-prefixed NALs (MP4 style)
        param.b_repeat_headers = 1;  // SPS/PPS before each IDR
        param.i_threads = 0;         // auto-detect: use all cores for faster encode
        param.i_log_level = X264_LOG_WARNING;
        param.rc.i_rc_method = X264_RC_CRF;
        param.rc.f_rf_constant = 23;

        enc = x264_encoder_open(&param);
        if (!enc) {
            printf("[REPLAY] x264_encoder_open failed\n");
            goto fail;
        }

        // Grab SPS/PPS for the avcC box
        if (x264_encoder_headers(enc, &nals, &num_nals) < 0) {
            printf("[REPLAY] x264_encoder_headers failed\n");
            goto fail;
        }
        for (j = 0; j < num_nals; j++) {
            // b_annexb=0: 4-byte BE length prefix, then raw NAL data
            byte *p = (byte *)nals[j].p_payload + 4;
            int len = nals[j].i_payload - 4;
            if (len <= 0) continue;
            if (nals[j].i_type == NAL_SPS) {
                sps = malloc(len); memcpy(sps, p, len); sps_len = len;
            } else if (nals[j].i_type == NAL_PPS) {
                pps = malloc(len); memcpy(pps, p, len); pps_len = len;
            }
        }
        if (!sps || !pps || sps_len < 4) {
            printf("[REPLAY] no SPS/PPS from x264\n");
            goto fail;
        }
    }

    memset(&pic_in, 0, sizeof(pic_in));
    if (x264_picture_alloc(&pic_in, X264_CSP_I420,
                           REPLAY_OUT_WIDTH, REPLAY_OUT_HEIGHT) != 0) {
        printf("[REPLAY] x264_picture_alloc failed\n");
        goto fail;
    }
    pic_allocated = 1;

    for (i = 0; i < n; i++) {
        {
            byte *src = job->video + (size_t)i * REPLAY_FRAME_SIZE;
            int x, y;
            x264_nal_t *nals;
            int num_nals, pts;
            x264_picture_t pic_out;
            // 2x nearest-neighbor upscale with palette conversion
            for (y = 0; y < REPLAY_HEIGHT; y++) {
                for (x = 0; x < REPLAY_WIDTH; x++) {
                    byte p = src[y * REPLAY_WIDTH + x];
                    byte r = palette[p*3+0], g = palette[p*3+1], b = palette[p*3+2];
                    int ox = x * 2, oy = y * 2;
                    int o;
                    o = (oy * REPLAY_OUT_WIDTH + ox) * 3;
                    rgb[o+0]=r; rgb[o+1]=g; rgb[o+2]=b;
                    o = (oy * REPLAY_OUT_WIDTH + ox + 1) * 3;
                    rgb[o+0]=r; rgb[o+1]=g; rgb[o+2]=b;
                    o = ((oy+1) * REPLAY_OUT_WIDTH + ox) * 3;
                    rgb[o+0]=r; rgb[o+1]=g; rgb[o+2]=b;
                    o = ((oy+1) * REPLAY_OUT_WIDTH + ox + 1) * 3;
                    rgb[o+0]=r; rgb[o+1]=g; rgb[o+2]=b;
                }
            }
            rgb_to_yuv420p(rgb, &pic_in, REPLAY_OUT_WIDTH, REPLAY_OUT_HEIGHT);

            pic_in.i_pts = i;
            if (x264_encoder_encode(enc, &nals, &num_nals, &pic_in, &pic_out) < 0) {
                printf("[REPLAY] x264_encoder_encode failed at frame %d\n", i);
                goto fail;
            }
            if (num_nals > 0) {
                pts = (int)pic_out.i_pts;
                if (pts < 0 || pts >= n) pts = i;  // safety fallback
                for (j = 0; j < num_nals; j++)
                    buf_append(&frames[pts], nals[j].p_payload, nals[j].i_payload);
            }
        }
        if ((i % 100) == 0) printf("[REPLAY] %d/%d\n", i, n);
    }

    // Flush delayed frames
    {
        x264_nal_t *nals;
        int num_nals, pts;
        x264_picture_t pic_out;
        while (x264_encoder_delayed_frames(enc)) {
            if (x264_encoder_encode(enc, &nals, &num_nals, NULL, &pic_out) < 0)
                break;
            if (num_nals > 0) {
                pts = (int)pic_out.i_pts;
                if (pts < 0 || pts >= n) continue;
                for (j = 0; j < num_nals; j++)
                    buf_append(&frames[pts], nals[j].p_payload, nals[j].i_payload);
            }
        }
    }

    x264_picture_clean(&pic_in);
    pic_allocated = 0;
    x264_encoder_close(enc);
    enc = NULL;
    free(rgb);
    rgb = NULL;

    // Every frame must have produced output
    for (i = 0; i < n; i++) {
        if (frames[i].len == 0) {
            printf("[REPLAY] error: frame %d produced no data\n", i);
            goto fail;
        }
        frame_sizes[i] = frames[i].len;
    }

    // ---- AAC audio from the ring buffer (video-only fallback on failure)
#ifndef NO_FAAC
    naframes = replay_encode_audio_pcm(job->pcm,
                                       job->audio_samples_per_chan,
                                       &aframes, &audio_asc,
                                       &audio_asc_len, &audio_samples);
    if (naframes > 0 && audio_asc && audio_asc_len > 0) {
        audio_sizes = malloc((size_t)naframes * sizeof(int));
        audio_offsets = malloc((size_t)naframes * sizeof(unsigned long long));
        if (audio_sizes && audio_offsets) {
            has_audio = 1;
            audio_movie_dur = (unsigned int)(
                (unsigned long long)audio_samples *
                (unsigned long long)timescale / REPLAY_AUDIO_RATE);
        }
    }
    if (!has_audio) {
        if (aframes) {
            for (i = 0; i < naframes; i++)
                free(aframes[i].data);
            free(aframes);
            aframes = NULL;
        }
        free(audio_asc);
        audio_asc = NULL;
        free(audio_sizes);
        audio_sizes = NULL;
        free(audio_offsets);
        audio_offsets = NULL;
        naframes = 0;
        audio_asc_len = 0;
        audio_samples = 0;
        printf("[REPLAY] no AAC audio - video-only\n");
    }
#endif

    f = fopen(job->tmpname, "wb");
    if (!f) {
        printf("[REPLAY] cannot open %s\n", filename);
        goto fail;
    }

    // ftyp
    {
        long pos = ftell(f); mp4_w32(f,0); fwrite("ftyp",1,4,f);
        fwrite("isom",1,4,f); mp4_w32(f,0);
        fwrite("isom",1,4,f); fwrite("iso2",1,4,f);
        fwrite("avc1",1,4,f); fwrite("mp41",1,4,f);
        long end = ftell(f); fseek(f,pos,SEEK_SET);
        mp4_w32(f,(unsigned int)(end-pos)); fseek(f,end,SEEK_SET);
    }

    // mdat (write first so we know chunk offsets)
    {
        unsigned long long mdat_size = 16;
        for (i = 0; i < n; i++) mdat_size += frames[i].len;
        if (has_audio)
            for (i = 0; i < naframes; i++)
                mdat_size += (unsigned long long)aframes[i].len;
        mp4_w32(f,1); fwrite("mdat",1,4,f); mp4_w64(f,mdat_size);
        for (i = 0; i < n; i++) {
            chunk_offsets[i] = (unsigned long long)ftell(f);
            fwrite(frames[i].data, 1, frames[i].len, f);
            free(frames[i].data);
            frames[i].data = NULL;
        }
        if (has_audio) {
            for (i = 0; i < naframes; i++) {
                audio_offsets[i] = (unsigned long long)ftell(f);
                audio_sizes[i] = aframes[i].len;
                fwrite(aframes[i].data, 1, aframes[i].len, f);
                free(aframes[i].data);
                aframes[i].data = NULL;
            }
        }
    }
    free(frames);
    frames = NULL;
    if (has_audio) {
        free(aframes);
        aframes = NULL;
    }

    // moov
    {
        long moov_pos = ftell(f); mp4_w32(f,0); fwrite("moov",1,4,f);

        // mvhd
        {
            long pos = ftell(f); mp4_w32(f,0); fwrite("mvhd",1,4,f);
            mp4_w32(f,0); mp4_w32(f,0); mp4_w32(f,0);
            mp4_w32(f,(unsigned int)timescale);
            {
                unsigned int dur = video_movie_dur;
                if (has_audio && audio_movie_dur > dur)
                    dur = audio_movie_dur;
                mp4_w32(f, dur);
            }
            mp4_w32(f,0x00010000); mp4_w16(f,0x0100); mp4_w16(f,0);
            mp4_w32(f,0); mp4_w32(f,0);
            mp4_w32(f,0x00010000); mp4_w32(f,0); mp4_w32(f,0);
            mp4_w32(f,0); mp4_w32(f,0x00010000); mp4_w32(f,0);
            mp4_w32(f,0); mp4_w32(f,0); mp4_w32(f,0x40000000);
            for (i = 0; i < 6; i++) mp4_w32(f,0);
            mp4_w32(f, has_audio ? 3 : 2); // next_track_ID
            long end = ftell(f); fseek(f,pos,SEEK_SET);
            mp4_w32(f,(unsigned int)(end-pos)); fseek(f,end,SEEK_SET);
        }

        // trak
        {
            long trak_pos = ftell(f); mp4_w32(f,0); fwrite("trak",1,4,f);

            // tkhd
            {
                long pos = ftell(f); mp4_w32(f,0); fwrite("tkhd",1,4,f);
                mp4_w32(f,7); mp4_w32(f,0); mp4_w32(f,0);
                mp4_w32(f,1); mp4_w32(f,0);
                mp4_w32(f,video_movie_dur);
                mp4_w32(f,0); mp4_w32(f,0);
                mp4_w16(f,0); mp4_w16(f,0); mp4_w16(f,0); mp4_w16(f,0);
                mp4_w32(f,0x00010000); mp4_w32(f,0); mp4_w32(f,0);
                mp4_w32(f,0); mp4_w32(f,0x00010000); mp4_w32(f,0);
                mp4_w32(f,0); mp4_w32(f,0); mp4_w32(f,0x40000000);
                mp4_w32(f,(unsigned int)(REPLAY_OUT_WIDTH<<16));
                mp4_w32(f,(unsigned int)(REPLAY_OUT_HEIGHT<<16));
                long end = ftell(f); fseek(f,pos,SEEK_SET);
                mp4_w32(f,(unsigned int)(end-pos)); fseek(f,end,SEEK_SET);
            }

            // mdia
            {
                long mdia_pos = ftell(f); mp4_w32(f,0); fwrite("mdia",1,4,f);

                // mdhd
                {
                    long pos = ftell(f); mp4_w32(f,0); fwrite("mdhd",1,4,f);
                    mp4_w32(f,0); mp4_w32(f,0); mp4_w32(f,0);
                    mp4_w32(f,(unsigned int)timescale);
                    mp4_w32(f,video_movie_dur);
                    mp4_w16(f,0x55c4); mp4_w16(f,0);
                    long end = ftell(f); fseek(f,pos,SEEK_SET);
                    mp4_w32(f,(unsigned int)(end-pos)); fseek(f,end,SEEK_SET);
                }

                // hdlr
                {
                    long pos = ftell(f); mp4_w32(f,0); fwrite("hdlr",1,4,f);
                    mp4_w32(f,0); mp4_w32(f,0);
                    fwrite("vide",1,4,f);
                    mp4_w32(f,0); mp4_w32(f,0); mp4_w32(f,0);
                    fwrite("VideoHandler",1,12,f); fputc(0,f);
                    long end = ftell(f); fseek(f,pos,SEEK_SET);
                    mp4_w32(f,(unsigned int)(end-pos)); fseek(f,end,SEEK_SET);
                }

                // minf
                {
                    long minf_pos = ftell(f); mp4_w32(f,0); fwrite("minf",1,4,f);

                    // vmhd
                    {
                        long pos = ftell(f); mp4_w32(f,0); fwrite("vmhd",1,4,f);
                        mp4_w32(f,1); mp4_w16(f,0);
                        mp4_w16(f,0); mp4_w16(f,0); mp4_w16(f,0);
                        long end = ftell(f); fseek(f,pos,SEEK_SET);
                        mp4_w32(f,(unsigned int)(end-pos)); fseek(f,end,SEEK_SET);
                    }

                    // dinf
                    {
                        long dinf_pos = ftell(f); mp4_w32(f,0); fwrite("dinf",1,4,f);
                        {
                            long pos = ftell(f); mp4_w32(f,0); fwrite("dref",1,4,f);
                            mp4_w32(f,0); mp4_w32(f,1);
                            {
                                long p2 = ftell(f); mp4_w32(f,0); fwrite("url ",1,4,f);
                                mp4_w32(f,1);
                                long e2 = ftell(f); fseek(f,p2,SEEK_SET);
                                mp4_w32(f,(unsigned int)(e2-p2)); fseek(f,e2,SEEK_SET);
                            }
                            long end = ftell(f); fseek(f,pos,SEEK_SET);
                            mp4_w32(f,(unsigned int)(end-pos)); fseek(f,end,SEEK_SET);
                        }
                        long end = ftell(f); fseek(f,dinf_pos,SEEK_SET);
                        mp4_w32(f,(unsigned int)(end-dinf_pos)); fseek(f,end,SEEK_SET);
                    }

                    // stbl
                    {
                        long stbl_pos = ftell(f); mp4_w32(f,0); fwrite("stbl",1,4,f);

                        // stsd (avc1 sample entry + avcC)
                        {
                            long pos = ftell(f); mp4_w32(f,0); fwrite("stsd",1,4,f);
                            mp4_w32(f,0); mp4_w32(f,1);
                            {
                                long epos = ftell(f); mp4_w32(f,0); fwrite("avc1",1,4,f);
                                for (i = 0; i < 6; i++) fputc(0,f);
                                mp4_w16(f,1);
                                mp4_w16(f,0); mp4_w16(f,0);
                                mp4_w32(f,0); mp4_w32(f,0); mp4_w32(f,0);
                                mp4_w16(f,REPLAY_OUT_WIDTH); mp4_w16(f,REPLAY_OUT_HEIGHT);
                                mp4_w32(f,0x00480000); mp4_w32(f,0x00480000);
                                mp4_w32(f,0); mp4_w16(f,1);
                                fputc(10,f); fwrite("AVC Coding",1,10,f);
                                for (i = 0; i < 21; i++) fputc(0,f);
                                mp4_w16(f,24); mp4_w16(f,0xffff);
                                // avcC: decoder configuration from SPS/PPS
                                {
                                    long cpos = ftell(f); mp4_w32(f,0); fwrite("avcC",1,4,f);
                                    fputc(1, f);            // configurationVersion
                                    fputc(sps[1], f);       // AVCProfileIndication
                                    fputc(sps[2], f);       // profile_compatibility
                                    fputc(sps[3], f);       // AVCLevelIndication
                                    fputc(0xFF, f);         // lengthSizeMinusOne = 3 (4-byte)
                                    fputc(0xE1, f);         // numOfSequenceParameterSets = 1
                                    mp4_w16(f, (unsigned int)sps_len);
                                    fwrite(sps, 1, sps_len, f);
                                    fputc(1, f);            // numOfPictureParameterSets = 1
                                    mp4_w16(f, (unsigned int)pps_len);
                                    fwrite(pps, 1, pps_len, f);
                                    long cend = ftell(f); fseek(f,cpos,SEEK_SET);
                                    mp4_w32(f,(unsigned int)(cend-cpos)); fseek(f,cend,SEEK_SET);
                                }
                                long eend = ftell(f); fseek(f,epos,SEEK_SET);
                                mp4_w32(f,(unsigned int)(eend-epos)); fseek(f,eend,SEEK_SET);
                            }
                            long end = ftell(f); fseek(f,pos,SEEK_SET);
                            mp4_w32(f,(unsigned int)(end-pos)); fseek(f,end,SEEK_SET);
                        }

                        // stts: variable frame rate. Consecutive
                        // frames sharing a duration are run-length
                        // encoded into one (count, delta) entry, so
                        // a steady-rate segment still costs one entry
                        // and a varying segment costs one per run.
                        {
                            long pos = ftell(f); mp4_w32(f,0); fwrite("stts",1,4,f);
                            mp4_w32(f,0);
                            {
                                int runs = 0, run_start = 0, k;
                                uint32_t fallback = (uint32_t)(timescale / REPLAY_FPS);
                                // Count runs first so the entry count
                                // in the box header is exact.
                                for (k = 1; k <= n; k++) {
                                    uint32_t cur = (k < n && job->frame_durs)
                                        ? job->frame_durs[k] : 0;
                                    uint32_t prev = (job->frame_durs)
                                        ? job->frame_durs[k-1] : fallback;
                                    if (k == n || cur != prev)
                                        runs++;
                                }
                                if (runs < 1) runs = 1;
                                mp4_w32(f,(unsigned int)runs);
                                for (k = 1; k <= n; k++) {
                                    uint32_t cur = (k < n && job->frame_durs)
                                        ? job->frame_durs[k] : 0;
                                    uint32_t prev = (job->frame_durs)
                                        ? job->frame_durs[k-1] : fallback;
                                    if (k == n || cur != prev) {
                                        mp4_w32(f,(unsigned int)(k - run_start));
                                        mp4_w32(f,prev);
                                        run_start = k;
                                    }
                                }
                            }
                            long end = ftell(f); fseek(f,pos,SEEK_SET);
                            mp4_w32(f,(unsigned int)(end-pos)); fseek(f,end,SEEK_SET);
                        }

                        // stsc
                        {
                            long pos = ftell(f); mp4_w32(f,0); fwrite("stsc",1,4,f);
                            mp4_w32(f,0); mp4_w32(f,(unsigned int)n);
                            for (i = 0; i < n; i++) {
                                mp4_w32(f,(unsigned int)(i+1));
                                mp4_w32(f,1); mp4_w32(f,1);
                            }
                            long end = ftell(f); fseek(f,pos,SEEK_SET);
                            mp4_w32(f,(unsigned int)(end-pos)); fseek(f,end,SEEK_SET);
                        }

                        // stsz
                        {
                            long pos = ftell(f); mp4_w32(f,0); fwrite("stsz",1,4,f);
                            mp4_w32(f,0); mp4_w32(f,0);
                            mp4_w32(f,(unsigned int)n);
                            for (i = 0; i < n; i++)
                                mp4_w32(f,(unsigned int)frame_sizes[i]);
                            long end = ftell(f); fseek(f,pos,SEEK_SET);
                            mp4_w32(f,(unsigned int)(end-pos)); fseek(f,end,SEEK_SET);
                        }

                        // co64
                        {
                            long pos = ftell(f); mp4_w32(f,0); fwrite("co64",1,4,f);
                            mp4_w32(f,0); mp4_w32(f,(unsigned int)n);
                            for (i = 0; i < n; i++)
                                mp4_w64(f,chunk_offsets[i]);
                            long end = ftell(f); fseek(f,pos,SEEK_SET);
                            mp4_w32(f,(unsigned int)(end-pos)); fseek(f,end,SEEK_SET);
                        }

                        long end = ftell(f); fseek(f,stbl_pos,SEEK_SET);
                        mp4_w32(f,(unsigned int)(end-stbl_pos)); fseek(f,end,SEEK_SET);
                    }

                    long end = ftell(f); fseek(f,minf_pos,SEEK_SET);
                    mp4_w32(f,(unsigned int)(end-minf_pos)); fseek(f,end,SEEK_SET);
                }

                long end = ftell(f); fseek(f,mdia_pos,SEEK_SET);
                mp4_w32(f,(unsigned int)(end-mdia_pos)); fseek(f,end,SEEK_SET);
            }

            long end = ftell(f); fseek(f,trak_pos,SEEK_SET);
            mp4_w32(f,(unsigned int)(end-trak_pos)); fseek(f,end,SEEK_SET);
        }

        // ---- audio trak (AAC) ----
        if (has_audio)
        {
            long trak_pos = ftell(f); mp4_w32(f,0); fwrite("trak",1,4,f);

            // tkhd (audio: track 2, full volume, no dimensions)
            {
                long pos = ftell(f); mp4_w32(f,0); fwrite("tkhd",1,4,f);
                mp4_w32(f,7); mp4_w32(f,0); mp4_w32(f,0);
                mp4_w32(f,2); mp4_w32(f,0);
                mp4_w32(f,audio_movie_dur);
                mp4_w32(f,0); mp4_w32(f,0);
                mp4_w16(f,0); mp4_w16(f,0); mp4_w16(f,0x0100); mp4_w16(f,0);
                mp4_w32(f,0x00010000); mp4_w32(f,0); mp4_w32(f,0);
                mp4_w32(f,0); mp4_w32(f,0x00010000); mp4_w32(f,0);
                mp4_w32(f,0); mp4_w32(f,0); mp4_w32(f,0x40000000);
                mp4_w32(f,0); mp4_w32(f,0);
                long end = ftell(f); fseek(f,pos,SEEK_SET);
                mp4_w32(f,(unsigned int)(end-pos)); fseek(f,end,SEEK_SET);
            }

            // mdia
            {
                long mdia_pos = ftell(f); mp4_w32(f,0); fwrite("mdia",1,4,f);

                // mdhd (audio timescale)
                {
                    long pos = ftell(f); mp4_w32(f,0); fwrite("mdhd",1,4,f);
                    mp4_w32(f,0); mp4_w32(f,0); mp4_w32(f,0);
                    mp4_w32(f,REPLAY_AUDIO_RATE);
                    mp4_w32(f,audio_samples);
                    mp4_w16(f,0x55c4); mp4_w16(f,0);
                    long end = ftell(f); fseek(f,pos,SEEK_SET);
                    mp4_w32(f,(unsigned int)(end-pos)); fseek(f,end,SEEK_SET);
                }

                // hdlr (soun)
                {
                    long pos = ftell(f); mp4_w32(f,0); fwrite("hdlr",1,4,f);
                    mp4_w32(f,0); mp4_w32(f,0);
                    fwrite("soun",1,4,f);
                    mp4_w32(f,0); mp4_w32(f,0); mp4_w32(f,0);
                    fwrite("SoundHandler",1,12,f); fputc(0,f);
                    long end = ftell(f); fseek(f,pos,SEEK_SET);
                    mp4_w32(f,(unsigned int)(end-pos)); fseek(f,end,SEEK_SET);
                }

                // minf
                {
                    long minf_pos = ftell(f); mp4_w32(f,0); fwrite("minf",1,4,f);

                    // smhd
                    {
                        long pos = ftell(f); mp4_w32(f,0); fwrite("smhd",1,4,f);
                        mp4_w32(f,0); mp4_w16(f,0); mp4_w16(f,0);
                        long end = ftell(f); fseek(f,pos,SEEK_SET);
                        mp4_w32(f,(unsigned int)(end-pos)); fseek(f,end,SEEK_SET);
                    }

                    // dinf
                    {
                        long dinf_pos = ftell(f); mp4_w32(f,0); fwrite("dinf",1,4,f);
                        {
                            long pos = ftell(f); mp4_w32(f,0); fwrite("dref",1,4,f);
                            mp4_w32(f,0); mp4_w32(f,1);
                            {
                                long p2 = ftell(f); mp4_w32(f,0); fwrite("url ",1,4,f);
                                mp4_w32(f,1);
                                long e2 = ftell(f); fseek(f,p2,SEEK_SET);
                                mp4_w32(f,(unsigned int)(e2-p2)); fseek(f,e2,SEEK_SET);
                            }
                            long end = ftell(f); fseek(f,pos,SEEK_SET);
                            mp4_w32(f,(unsigned int)(end-pos)); fseek(f,end,SEEK_SET);
                        }
                        long end = ftell(f); fseek(f,dinf_pos,SEEK_SET);
                        mp4_w32(f,(unsigned int)(end-dinf_pos)); fseek(f,end,SEEK_SET);
                    }

                    // stbl
                    {
                        long stbl_pos = ftell(f); mp4_w32(f,0); fwrite("stbl",1,4,f);

                        // stsd (mp4a sample entry + esds with ASC)
                        {
                            long pos = ftell(f); mp4_w32(f,0); fwrite("stsd",1,4,f);
                            mp4_w32(f,0); mp4_w32(f,1);
                            {
                                long epos = ftell(f); mp4_w32(f,0); fwrite("mp4a",1,4,f);
                                for (i = 0; i < 6; i++) fputc(0,f);
                                mp4_w16(f,1);
                                mp4_w16(f,0); mp4_w16(f,0); mp4_w32(f,0);
                                mp4_w16(f,REPLAY_AUDIO_CHANNELS);
                                mp4_w16(f,16);
                                mp4_w16(f,0); mp4_w16(f,0);
                                mp4_w32(f,(unsigned int)(REPLAY_AUDIO_RATE<<16));
                                // esds: decoder config with AudioSpecificConfig
                                {
                                    long xpos = ftell(f); mp4_w32(f,0); fwrite("esds",1,4,f);
                                    mp4_w32(f,0); // version + flags
                                    {
                                        unsigned int len_dec = 15 + audio_asc_len;
                                        unsigned int len_es = 23 + audio_asc_len;
                                        fputc(0x03, f); fputc((int)len_es, f);
                                        fputc(0x00, f); fputc(0x02, f); // ES_ID = 2
                                        fputc(0x00, f);                // flags
                                        fputc(0x04, f); fputc((int)len_dec, f);
                                        fputc(0x40, f); // objectTypeIndication: MPEG-4 AAC
                                        fputc(0x15, f); // streamType: AudioStream
                                        fputc(0x00, f); fputc(0x00, f); fputc(0x00, f); // bufferSizeDB
                                        mp4_w32(f,128000); // maxBitrate
                                        mp4_w32(f,128000); // avgBitrate
                                        fputc(0x05, f); fputc((int)audio_asc_len, f);
                                        fwrite(audio_asc, 1, audio_asc_len, f);
                                        fputc(0x06, f); fputc(0x01, f); fputc(0x02, f); // SL config
                                    }
                                    long xend = ftell(f); fseek(f,xpos,SEEK_SET);
                                    mp4_w32(f,(unsigned int)(xend-xpos)); fseek(f,xend,SEEK_SET);
                                }
                                long eend = ftell(f); fseek(f,epos,SEEK_SET);
                                mp4_w32(f,(unsigned int)(eend-epos)); fseek(f,eend,SEEK_SET);
                            }
                            long end = ftell(f); fseek(f,pos,SEEK_SET);
                            mp4_w32(f,(unsigned int)(end-pos)); fseek(f,end,SEEK_SET);
                        }

                        // stts: every AAC frame decodes to 1024 samples
                        {
                            long pos = ftell(f); mp4_w32(f,0); fwrite("stts",1,4,f);
                            mp4_w32(f,0); mp4_w32(f,1);
                            mp4_w32(f,(unsigned int)naframes);
                            mp4_w32(f,1024);
                            long end = ftell(f); fseek(f,pos,SEEK_SET);
                            mp4_w32(f,(unsigned int)(end-pos)); fseek(f,end,SEEK_SET);
                        }

                        // stsc: one sample per chunk
                        {
                            long pos = ftell(f); mp4_w32(f,0); fwrite("stsc",1,4,f);
                            mp4_w32(f,0); mp4_w32(f,(unsigned int)naframes);
                            for (i = 0; i < naframes; i++) {
                                mp4_w32(f,(unsigned int)(i+1));
                                mp4_w32(f,1); mp4_w32(f,1);
                            }
                            long end = ftell(f); fseek(f,pos,SEEK_SET);
                            mp4_w32(f,(unsigned int)(end-pos)); fseek(f,end,SEEK_SET);
                        }

                        // stsz
                        {
                            long pos = ftell(f); mp4_w32(f,0); fwrite("stsz",1,4,f);
                            mp4_w32(f,0); mp4_w32(f,0);
                            mp4_w32(f,(unsigned int)naframes);
                            for (i = 0; i < naframes; i++)
                                mp4_w32(f,(unsigned int)audio_sizes[i]);
                            long end = ftell(f); fseek(f,pos,SEEK_SET);
                            mp4_w32(f,(unsigned int)(end-pos)); fseek(f,end,SEEK_SET);
                        }

                        // co64
                        {
                            long pos = ftell(f); mp4_w32(f,0); fwrite("co64",1,4,f);
                            mp4_w32(f,0); mp4_w32(f,(unsigned int)naframes);
                            for (i = 0; i < naframes; i++)
                                mp4_w64(f,audio_offsets[i]);
                            long end = ftell(f); fseek(f,pos,SEEK_SET);
                            mp4_w32(f,(unsigned int)(end-pos)); fseek(f,end,SEEK_SET);
                        }

                        long end = ftell(f); fseek(f,stbl_pos,SEEK_SET);
                        mp4_w32(f,(unsigned int)(end-stbl_pos)); fseek(f,end,SEEK_SET);
                    }

                    long end = ftell(f); fseek(f,minf_pos,SEEK_SET);
                    mp4_w32(f,(unsigned int)(end-minf_pos)); fseek(f,end,SEEK_SET);
                }

                long end = ftell(f); fseek(f,mdia_pos,SEEK_SET);
                mp4_w32(f,(unsigned int)(end-mdia_pos)); fseek(f,end,SEEK_SET);
            }

            long end = ftell(f); fseek(f,trak_pos,SEEK_SET);
            mp4_w32(f,(unsigned int)(end-trak_pos)); fseek(f,end,SEEK_SET);
        }

        long end = ftell(f); fseek(f,moov_pos,SEEK_SET);
        mp4_w32(f,(unsigned int)(end-moov_pos)); fseek(f,end,SEEK_SET);
    }

    free(frame_sizes);
    free(chunk_offsets);
    free(sps);
    free(pps);
    free(audio_sizes);
    free(audio_offsets);
    free(audio_asc);
    fclose(f);
    rename(job->tmpname, filename);
    printf("[REPLAY] saved %s (%d frames, H.264%s MP4)\n",
           filename, n, has_audio ? "+AAC" : "");
    free(job->video);
    free(job->pcm);
    free(job->frame_durs);
    free(job);
    replay_encode_busy = 0;
    return NULL;

fail:
    if (pic_allocated) x264_picture_clean(&pic_in);
    if (enc) x264_encoder_close(enc);
    if (f) fclose(f);
    free(rgb);
    free(sps);
    free(pps);
    if (frames) {
        for (i = 0; i < n; i++) free(frames[i].data);
        free(frames);
    }
    free(frame_sizes);
    free(chunk_offsets);
    if (aframes) {
        for (i = 0; i < naframes; i++) free(aframes[i].data);
        free(aframes);
    }
    free(audio_sizes);
    free(audio_offsets);
    free(audio_asc);
    remove(job->tmpname);
    free(job->video);
    free(job->pcm);
    free(job->frame_durs);
    free(job);
    replay_encode_busy = 0;
    return NULL;
#endif
}

// Game-thread entry point: snapshot the ring buffers and hand the encode
// to a detached worker thread. Returns 0 when the encode was queued,
// 1 when skipped (busy / empty buffer). The finished MP4 appears at
// `filename` atomically (written to <filename>.tmp, renamed on success).
int D_ReplaySaveMP4(const char *filename)
{
#ifdef NO_X264
    printf("[REPLAY] H.264 support not compiled in (no x264)\n");
    return 1;
#else
    replay_job_t *job;
    pthread_t th;
    int n, i;

    if (replay_encode_busy) {
        printf("[REPLAY] encode already in progress, skipping\n");
        return 1;
    }
    n = replay_count;
    if (!replay_buffer || n == 0) {
        printf("[REPLAY] buffer empty\n");
        return 1;
    }
    D_EnsureDoomshotsDir();
    job = (replay_job_t *)calloc(1, sizeof(*job));
    if (!job)
        return 1;
    job->n = n;
    job->video = (byte *)malloc((size_t)n * REPLAY_FRAME_SIZE);
    job->frame_durs = (uint32_t *)malloc((size_t)n * sizeof(uint32_t));
    if (!job->video || !job->frame_durs) {
        free(job->video);
        free(job->frame_durs);
        free(job);
        return 1;
    }
    {
        // Snapshot timestamps and audio positions in display order,
        // then derive per-frame durations (movie timescale 90000,
        // so 1ms = 90 units). Durations come from timestamp
        // differences, so their sum is the true video-window length
        // regardless of rounding on any single frame. A zero gap
        // (two frames in one millisecond) reuses the previous
        // duration; a gap over 5s (process suspend, where audio
        // also stopped) is capped so one frozen frame cannot push
        // the whole track out of sync. The last frame reuses the
        // previous duration, since there is no next timestamp yet.
        uint32_t ts[REPLAY_MAXFRAMES];
        int aud[REPLAY_MAXFRAMES];
        uint32_t prev_dur_ms = (uint32_t)(90000 / REPLAY_FPS / 90);
        for (i = 0; i < n; i++) {
            int idx = (replay_head - n + i + REPLAY_MAXFRAMES * 2) % REPLAY_MAXFRAMES;
            memcpy(job->video + (size_t)i * REPLAY_FRAME_SIZE,
                   replay_buffer + (size_t)idx * REPLAY_FRAME_SIZE,
                   REPLAY_FRAME_SIZE);
            ts[i] = replay_frame_ts[idx];
            aud[i] = replay_frame_audio[idx];
        }
        for (i = 0; i < n; i++) {
            uint32_t dur_ms;
            if (i + 1 < n) {
                int32_t diff = (int32_t)(ts[i + 1] - ts[i]);
                dur_ms = diff > 0 ? (uint32_t)diff : prev_dur_ms;
                if (dur_ms > 5000) dur_ms = 5000;
            } else {
                dur_ms = prev_dur_ms;
            }
            prev_dur_ms = dur_ms;
            job->frame_durs[i] = dur_ms * 90;
        }
#ifndef NO_FAAC
        // Audio window: from the first frame's audio position
        // through the last frame's duration. The audio write
        // position advances at the TRUE rate, not the nominal
        // 88.2 int16/ms: the SDL audio thread can starve under
        // CPU load (game + x264 encode + Xvfb), so derive the
        // rate from the per-frame (ts, aud) pairs instead of
        // assuming 44.1kHz stereo. The pairs are a wall-clock
        // timestamp and a write position captured together in
        // D_ReplayTick, so their slope is the true samples/ms.
        // The rate can wander INSIDE a segment, so a single
        // constant rate fixes the average compression but not
        // the local warp: the snapshot is resampled PIECEWISE,
        // mapping each output frame back to its write position
        // through the piecewise-linear (ts -> aud) mapping, so
        // the audio aligns with the video timeline locally, not
        // just on average. The tail (past the last frame's
        // timestamp) uses the last interval's local rate.
        {
            double true_rate; // int16 samples (interleaved) per ms
            double tail_rate; // local rate over the last interval
            uint32_t video_window_ms;
            int audio_start, audio_end;
            if (n >= 2 && ts[n - 1] > ts[0] && aud[n - 1] > aud[0]) {
                true_rate = (double)(aud[n - 1] - aud[0])
                            / (double)(ts[n - 1] - ts[0]);
            } else {
                true_rate = (double)(REPLAY_AUDIO_RATE
                                     * REPLAY_AUDIO_CHANNELS);
            }
            // Clamp to a sane range: never faster than nominal
            // (the device cannot deliver more), never below 1
            // sample/ms (a stalled thread).
            if (true_rate > (double)(REPLAY_AUDIO_RATE
                                     * REPLAY_AUDIO_CHANNELS))
                true_rate = (double)(REPLAY_AUDIO_RATE
                                     * REPLAY_AUDIO_CHANNELS);
            if (true_rate < 1.0)
                true_rate = 1.0;
            tail_rate = true_rate;
            if (n >= 2 && ts[n - 1] > ts[n - 2]
                && aud[n - 1] > aud[n - 2]) {
                tail_rate = (double)(aud[n - 1] - aud[n - 2])
                            / (double)(ts[n - 1] - ts[n - 2]);
                if (tail_rate > (double)(REPLAY_AUDIO_RATE
                                         * REPLAY_AUDIO_CHANNELS))
                    tail_rate = (double)(REPLAY_AUDIO_RATE
                                         * REPLAY_AUDIO_CHANNELS);
                if (tail_rate < 1.0)
                    tail_rate = 1.0;
            }
            video_window_ms = ts[n - 1] - ts[0] + prev_dur_ms;
            audio_start = aud[0];
            audio_end = aud[n - 1]
                + (int)(prev_dur_ms * tail_rate);
            job->pcm = replay_snapshot_pcm_window(
                audio_start, audio_end, &job->audio_samples_per_chan);
            if (job->pcm && job->audio_samples_per_chan > 0) {
                uint32_t target_per_chan =
                    (uint32_t)(video_window_ms * REPLAY_AUDIO_RATE
                               / 1000 + 0.5);
                if (target_per_chan > 0) {
                    int16_t *warped = replay_resample_pcm_piecewise(
                        job->pcm, job->audio_samples_per_chan,
                        audio_start, ts, aud, n, tail_rate, true_rate,
                        target_per_chan);
                    if (warped) {
                        free(job->pcm);
                        job->pcm = warped;
                        job->audio_samples_per_chan = target_per_chan;
                    }
                }
            }
            printf("[REPLAY] video window %u ms, audio %u samples/chan (rate %.2f)\n",
                   (unsigned int)video_window_ms,
                   job->audio_samples_per_chan, true_rate);
        }
#endif
    }
    {
        extern byte *W_CacheLumpName(const char *name, int tag);
        byte *pal = W_CacheLumpName("PLAYPAL", 1);
        if (pal)
            memcpy(job->palette, pal, 256 * 3);
    }
    snprintf(job->filename, sizeof(job->filename), "%s", filename);
    snprintf(job->tmpname, sizeof(job->tmpname), "%s.tmp", filename);
    // Clear the ring now that the snapshot holds the frames: same net
    // effect as the old post-encode clear, and frames captured during
    // the background encode belong to the next replay.
    replay_count = 0;
    replay_encode_busy = 1;
    if (pthread_create(&th, NULL, replay_encode_worker, job) != 0) {
        replay_encode_busy = 0;
        free(job->video);
        free(job->pcm);
        free(job->frame_durs);
        free(job);
        return 1;
    }
    pthread_detach(th);
    printf("[REPLAY] encoding %d frames in background\n", n);
    return 0;
#endif
}
