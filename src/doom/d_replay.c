// d_replay.c - Instant replay: circular buffer of last 15 seconds.
// Outputs MP4 (MJPEG) for Threads compatibility.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include "doomdef.h"
#include "d_replay.h"
#include "i_video.h"
#include "v_video.h"
#include "../tables.h"

void D_EnsureDoomshotsDir(void)
{
    struct stat st;
    if (stat("doomshots", &st) != 0)
    {
        if (mkdir("doomshots", 0755) != 0 && errno != EEXIST)
            printf("[REPLAY] warning: cannot create doomshots/\n");
    }
}

// Find an unused replay filename: doomshots/diablodoom_YYYYMMDD_HHMMSS.mp4,
// with a numeric suffix if the timestamp is taken.
void D_ReplayNextFilename(char *buf, size_t bufsz)
{
    struct stat st;
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    int i;
    for (i = 0; i < 100; i++)
    {
        if (i == 0)
            snprintf(buf, bufsz, "doomshots/diablodoom_%04d%02d%02d_%02d%02d%02d.mp4",
                     tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                     tm->tm_hour, tm->tm_min, tm->tm_sec);
        else
            snprintf(buf, bufsz, "doomshots/diablodoom_%04d%02d%02d_%02d%02d%02d_%02d.mp4",
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

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

static byte *replay_buffer = NULL;
static int replay_head = 0;
static int replay_count = 0;

void D_ReplayInit(void)
{
    D_EnsureDoomshotsDir();
    if (replay_buffer) return;
    replay_buffer = malloc(REPLAY_MAXFRAMES * REPLAY_FRAME_SIZE);
    if (replay_buffer) {
        memset(replay_buffer, 0, REPLAY_MAXFRAMES * REPLAY_FRAME_SIZE);
        printf("[REPLAY] buffer ready: %d frames\n", REPLAY_MAXFRAMES);
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
static void mem_write(void *ctx, void *data, int size) {
    mem_buf_t *m = (mem_buf_t*)ctx;
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

int D_ReplaySaveMP4(const char *filename)
{
    extern byte *W_CacheLumpName(const char *name, int tag);
    FILE *f;
    byte *rgb, *palette;
    mem_buf_t *jpegs;
    int *frame_sizes;
    unsigned long long *chunk_offsets;
    int i, idx, n;
    int timescale = 90000;
    int frame_duration = timescale / REPLAY_FPS;

    D_EnsureDoomshotsDir();

    n = replay_count;
    if (!replay_buffer || n == 0) {
        printf("[REPLAY] buffer empty\n");
        return 1;
    }
    printf("[REPLAY] encoding %d frames to MP4...\n", n);

    rgb = malloc(REPLAY_OUT_WIDTH * REPLAY_OUT_HEIGHT * 3);
    jpegs = calloc(n, sizeof(mem_buf_t));
    frame_sizes = malloc(n * sizeof(int));
    chunk_offsets = malloc(n * sizeof(unsigned long long));
    palette = W_CacheLumpName("PLAYPAL", 1);
    if (!rgb || !jpegs || !frame_sizes || !chunk_offsets || !palette) return 1;

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

    for (i = 0; i < n; i++) {
        idx = (replay_head - n + i + REPLAY_MAXFRAMES*2) % REPLAY_MAXFRAMES;
        byte *src = replay_buffer + idx * REPLAY_FRAME_SIZE;
        int x, y;
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
        jpegs[i].cap = 65536;
        jpegs[i].data = malloc(jpegs[i].cap);
        jpegs[i].len = 0;
        stbi_write_jpg_to_func(mem_write, &jpegs[i],
                               REPLAY_OUT_WIDTH, REPLAY_OUT_HEIGHT, 3, rgb, 95);
        frame_sizes[i] = jpegs[i].len;
        if ((i % 100) == 0) printf("[REPLAY] %d/%d\n", i, n);
    }
    free(rgb);

    f = fopen(filename, "wb");
    if (!f) return 1;

    // ftyp
    {
        long pos = ftell(f); mp4_w32(f,0); fwrite("ftyp",1,4,f);
        fwrite("isom",1,4,f); mp4_w32(f,0);
        fwrite("isom",1,4,f); fwrite("iso2",1,4,f); fwrite("mp41",1,4,f);
        long end = ftell(f); fseek(f,pos,SEEK_SET);
        mp4_w32(f,(unsigned int)(end-pos)); fseek(f,end,SEEK_SET);
    }

    // mdat (write first so we know chunk offsets)
    {
        unsigned long long mdat_size = 16;
        for (i = 0; i < n; i++) mdat_size += jpegs[i].len;
        mp4_w32(f,1); fwrite("mdat",1,4,f); mp4_w64(f,mdat_size);
        for (i = 0; i < n; i++) {
            chunk_offsets[i] = (unsigned long long)ftell(f);
            fwrite(jpegs[i].data, 1, jpegs[i].len, f);
            free(jpegs[i].data);
        }
    }
    free(jpegs);

    // moov
    {
        long moov_pos = ftell(f); mp4_w32(f,0); fwrite("moov",1,4,f);

        // mvhd
        {
            long pos = ftell(f); mp4_w32(f,0); fwrite("mvhd",1,4,f);
            mp4_w32(f,0); mp4_w32(f,0); mp4_w32(f,0);
            mp4_w32(f,(unsigned int)timescale);
            mp4_w32(f,(unsigned int)(n * frame_duration));
            mp4_w32(f,0x00010000); mp4_w16(f,0x0100); mp4_w16(f,0);
            mp4_w32(f,0); mp4_w32(f,0);
            mp4_w32(f,0x00010000); mp4_w32(f,0); mp4_w32(f,0);
            mp4_w32(f,0); mp4_w32(f,0x00010000); mp4_w32(f,0);
            mp4_w32(f,0); mp4_w32(f,0); mp4_w32(f,0x40000000);
            for (i = 0; i < 6; i++) mp4_w32(f,0);
            mp4_w32(f,2);
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

                        // stsd (jpeg sample entry)
                        {
                            long pos = ftell(f); mp4_w32(f,0); fwrite("stsd",1,4,f);
                            mp4_w32(f,0); mp4_w32(f,1);
                            {
                                long epos = ftell(f); mp4_w32(f,0); fwrite("jpeg",1,4,f);
                                for (i = 0; i < 6; i++) fputc(0,f);
                                mp4_w16(f,1);
                                mp4_w16(f,0); mp4_w16(f,0);
                                mp4_w32(f,0); mp4_w32(f,0); mp4_w32(f,0);
                                mp4_w16(f,REPLAY_OUT_WIDTH); mp4_w16(f,REPLAY_OUT_HEIGHT);
                                mp4_w32(f,0x00480000); mp4_w32(f,0x00480000);
                                mp4_w32(f,0); mp4_w16(f,1);
                                fputc(9,f); fwrite("PhotoJPEG",1,9,f);
                                for (i = 0; i < 22; i++) fputc(0,f);
                                mp4_w16(f,24); mp4_w16(f,0xffff);
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

        long end = ftell(f); fseek(f,moov_pos,SEEK_SET);
        mp4_w32(f,(unsigned int)(end-moov_pos)); fseek(f,end,SEEK_SET);
    }

    free(frame_sizes);
    free(chunk_offsets);
    fclose(f);
    printf("[REPLAY] saved %s (%d frames, MP4)\n", filename, n);
    return 0;
}
