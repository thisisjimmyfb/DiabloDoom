// d_replay.c - Instant replay: circular buffer of last 15 seconds.
// Outputs MP4 (H.264) for browser/Threads compatibility.
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

// Audio ring buffer: 15 seconds of 44.1kHz stereo 16-bit PCM.
// Written by the SDL_mixer post-mix hook (audio thread), read when saving.
#define REPLAY_AUDIO_RATE 44100
#define REPLAY_AUDIO_CHANNELS 2
#define REPLAY_AUDIO_SECS 15
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
    memcpy(replay_buffer + (replay_head * REPLAY_FRAME_SIZE),
           I_VideoBuffer, REPLAY_FRAME_SIZE);
    replay_head = (replay_head + 1) % REPLAY_MAXFRAMES;
    if (replay_count < REPLAY_MAXFRAMES) replay_count++;
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
// Main-thread helper: snapshot the audio ring into a freshly malloc'd
// contiguous PCM buffer (15s window, oldest -> newest, silence-padded if
// the game has run less than 15s). Returns NULL when audio capture is
// not running. Caller owns the buffer.
static int16_t *replay_snapshot_pcm(void)
{
    int total = REPLAY_AUDIO_SAMPLES * REPLAY_AUDIO_CHANNELS; // int16 count
    int wpos;
    int16_t *pcm;

    if (!replay_audio_buf)
        return NULL;
    wpos = replay_audio_write;
    pcm = malloc(REPLAY_AUDIO_BYTES);
    if (!pcm)
        return NULL;
    memset(pcm, 0, REPLAY_AUDIO_BYTES);
    if (wpos >= total) {
        int start = wpos % total;
        int first = total - start;
        memcpy(pcm, replay_audio_buf + start, (size_t)first * 2);
        memcpy(pcm + first, replay_audio_buf, (size_t)start * 2);
    } else if (wpos > 0) {
        memcpy(pcm + (total - wpos), replay_audio_buf, (size_t)wpos * 2);
    }
    return pcm;
}

// Encode a PCM snapshot to AAC frames. Does NOT free pcm (caller owns).
static int replay_encode_audio_pcm(int16_t *pcm,
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
    uint32_t samples_per_chan = REPLAY_AUDIO_SAMPLES; // 661500
    uint32_t frames_needed;

    *aframes_out = NULL;
    *asc_out = NULL;
    *asc_len_out = 0;
    *samples_per_chan_out = 0;

    if (!pcm)
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
    aframes_cap = 700;
    aframes = calloc((size_t)aframes_cap, sizeof(mem_buf_t));
    if (!outbuf || !aframes)
        goto adone;

    frames_needed = (samples_per_chan + info.frame_samples - 1)
                    / info.frame_samples;
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
    byte palette[256 * 3];
    char filename[256];
    char tmpname[264];
} replay_job_t;

static volatile int replay_encode_busy = 0;

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
    int frame_duration = timescale / REPLAY_FPS;

    n = job->n;
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
    naframes = replay_encode_audio_pcm(job->pcm, &aframes, &audio_asc,
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
                unsigned int dur = (unsigned int)(n * frame_duration);
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
                mp4_w32(f,(unsigned int)(n * frame_duration));
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
                    mp4_w32(f,(unsigned int)(n * frame_duration));
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

                        // stts
                        {
                            long pos = ftell(f); mp4_w32(f,0); fwrite("stts",1,4,f);
                            mp4_w32(f,0); mp4_w32(f,1);
                            mp4_w32(f,(unsigned int)n);
                            mp4_w32(f,(unsigned int)frame_duration);
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
    if (!job->video) {
        free(job);
        return 1;
    }
    for (i = 0; i < n; i++) {
        int idx = (replay_head - n + i + REPLAY_MAXFRAMES * 2) % REPLAY_MAXFRAMES;
        memcpy(job->video + (size_t)i * REPLAY_FRAME_SIZE,
               replay_buffer + (size_t)idx * REPLAY_FRAME_SIZE,
               REPLAY_FRAME_SIZE);
    }
#ifndef NO_FAAC
    job->pcm = replay_snapshot_pcm();
#endif
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
        free(job);
        return 1;
    }
    pthread_detach(th);
    printf("[REPLAY] encoding %d frames in background\n", n);
    return 0;
#endif
}
