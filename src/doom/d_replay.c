// d_replay.c - Instant replay: circular buffer of last 15 seconds.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include "doomdef.h"
#include "d_replay.h"
#include "i_video.h"
#include "v_video.h"

// Ensure the doomshots/ output directory exists.
void D_EnsureDoomshotsDir(void)
{
    struct stat st;
    if (stat("doomshots", &st) != 0)
    {
        if (mkdir("doomshots", 0755) != 0 && errno != EEXIST)
            printf("[REPLAY] warning: cannot create doomshots/\n");
    }
}

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

static byte *replay_buffer = NULL;
static int replay_head = 0;
static int replay_count = 0;

void D_ReplayInit(void)
{
    extern void D_EnsureDoomshotsDir(void);
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

// JPEG encode to memory using stb
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

int D_ReplaySaveAVI(const char *filename)
{
    extern byte *W_CacheLumpName(const char *name, int tag);
    FILE *f;
    byte *rgb, *palette;
    mem_buf_t *jpegs;
    int i, idx, n;
    long movi_pos, riff_pos, hdrl_pos, strl_pos;

    n = replay_count;
    if (!replay_buffer || n == 0) {
        printf("[REPLAY] buffer empty\n");
        return 1;
    }
    printf("[REPLAY] encoding %d frames...\n", n);

    rgb = malloc(REPLAY_FRAME_SIZE * 3);
    jpegs = calloc(n, sizeof(mem_buf_t));
    palette = W_CacheLumpName("PLAYPAL", 1);
    if (!rgb || !jpegs || !palette) return 1;

    for (i = 0; i < n; i++) {
        idx = (replay_head - n + i + REPLAY_MAXFRAMES*2) % REPLAY_MAXFRAMES;
        byte *src = replay_buffer + idx * REPLAY_FRAME_SIZE;
        int p;
        for (p = 0; p < REPLAY_FRAME_SIZE; p++) {
            rgb[p*3+0] = palette[src[p]*3+0];
            rgb[p*3+1] = palette[src[p]*3+1];
            rgb[p*3+2] = palette[src[p]*3+2];
        }
        jpegs[i].cap = 65536;
        jpegs[i].data = malloc(jpegs[i].cap);
        jpegs[i].len = 0;
        stbi_write_jpg_to_func(mem_write, &jpegs[i],
                               REPLAY_WIDTH, REPLAY_HEIGHT, 3, rgb, 75);
        if ((i % 100) == 0) printf("[REPLAY] %d/%d\n", i, n);
    }
    free(rgb);

    f = fopen(filename, "wb");
    if (!f) return 1;

    // RIFF AVI header (simplified MJPEG)
    fwrite("RIFF", 1, 4, f);
    riff_pos = ftell(f); { int z=0; fwrite(&z,4,1,f); }
    fwrite("AVI ", 1, 4, f);
    fwrite("LIST", 1, 4, f);
    hdrl_pos = ftell(f); { int z=0; fwrite(&z,4,1,f); }
    fwrite("hdrl", 1, 4, f);
    fwrite("avih", 1, 4, f); { int s=56; fwrite(&s,4,1,f); }
    {
        int us = 1000000/REPLAY_FPS, z=0, one=1, w=REPLAY_WIDTH, h=REPLAY_HEIGHT;
        fwrite(&us,4,1,f); fwrite(&z,4,1,f); fwrite(&z,4,1,f); fwrite(&z,4,1,f);
        fwrite(&n,4,1,f); fwrite(&z,4,1,f); fwrite(&one,4,1,f); fwrite(&z,4,1,f);
        fwrite(&w,4,1,f); fwrite(&h,4,1,f);
        fwrite(&z,4,1,f); fwrite(&z,4,1,f); fwrite(&z,4,1,f); fwrite(&z,4,1,f);
    }
    fwrite("LIST", 1, 4, f);
    strl_pos = ftell(f); { int z=0; fwrite(&z,4,1,f); }
    fwrite("strl", 1, 4, f);
    fwrite("strh", 1, 4, f); { int s=56; fwrite(&s,4,1,f); }
    fwrite("vidsMJPG", 1, 8, f);
    {
        int z=0, one=1, fps=REPLAY_FPS, buf=REPLAY_FRAME_SIZE*3;
        short rc[4] = {0,0,REPLAY_WIDTH,REPLAY_HEIGHT};
        fwrite(&z,4,1,f); fwrite(&z,4,1,f); fwrite(&z,4,1,f);
        fwrite(&one,4,1,f); fwrite(&fps,4,1,f); fwrite(&z,4,1,f);
        fwrite(&n,4,1,f); fwrite(&buf,4,1,f);
        fwrite(&z,4,1,f); fwrite(&z,4,1,f);
        fwrite(rc,2,4,f);
    }
    fwrite("strf", 1, 4, f); { int s=40; fwrite(&s,4,1,f); }
    {
        int bs=40, w=REPLAY_WIDTH, h=REPLAY_HEIGHT, z=0, img=w*h*3;
        short pl=1, bpp=24;
        fwrite(&bs,4,1,f); fwrite(&w,4,1,f); fwrite(&h,4,1,f);
        fwrite(&pl,2,1,f); fwrite(&bpp,2,1,f);
        fwrite("MJPG",1,4,f); fwrite(&img,4,1,f);
        fwrite(&z,4,1,f); fwrite(&z,4,1,f); fwrite(&z,4,1,f); fwrite(&z,4,1,f);
    }
    { long e=ftell(f); int s=(int)(e-strl_pos-4);
      fseek(f,strl_pos,SEEK_SET); fwrite(&s,4,1,f); fseek(f,e,SEEK_SET); }
    { long e=ftell(f); int s=(int)(e-hdrl_pos-4);
      fseek(f,hdrl_pos,SEEK_SET); fwrite(&s,4,1,f); fseek(f,e,SEEK_SET); }

    fwrite("LIST", 1, 4, f);
    movi_pos = ftell(f); { int z=0; fwrite(&z,4,1,f); }
    fwrite("movi", 1, 4, f);
    long movi_data = ftell(f);

    long *offsets = malloc(n * sizeof(long));
    int *lens = malloc(n * sizeof(int));
    for (i = 0; i < n; i++) {
        offsets[i] = ftell(f) - movi_data;
        lens[i] = jpegs[i].len;
        fwrite("00dc",1,4,f);
        fwrite(&jpegs[i].len,4,1,f);
        fwrite(jpegs[i].data,1,jpegs[i].len,f);
        if (jpegs[i].len & 1) fputc(0,f);
        free(jpegs[i].data);
    }
    { long e=ftell(f); int s=(int)(e-movi_pos-4);
      fseek(f,movi_pos,SEEK_SET); fwrite(&s,4,1,f); fseek(f,e,SEEK_SET); }

    // idx1
    fwrite("idx1",1,4,f);
    { int s=n*16; fwrite(&s,4,1,f); }
    for (i = 0; i < n; i++) {
        fwrite("00dc",1,4,f);
        { int fl=0x10; fwrite(&fl,4,1,f); }
        { int o=(int)offsets[i]; fwrite(&o,4,1,f); }
        fwrite(&lens[i],4,1,f);
    }
    free(offsets);
    free(lens);
    { long e=ftell(f); int s=(int)(e-8);
      fseek(f,riff_pos,SEEK_SET); fwrite(&s,4,1,f); }
    fclose(f);
    free(jpegs);
    printf("[REPLAY] saved %s\n", filename);
    return 0;
}
