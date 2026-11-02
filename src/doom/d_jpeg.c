// d_jpeg.c - Minimal baseline JPEG encoder.
//
// Self-contained, no external libraries. Baseline DCT, 4:2:0 chroma
// subsampling, standard Huffman tables (Annex K).
//
// Written for MJPEG video frames; favors simplicity over compression ratio.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "doomtype.h"
#include "d_jpeg.h"

// ---------------------------------------------------------------------------
// Standard quantization tables (Annex K), zigzag order is applied at use.
// ---------------------------------------------------------------------------

static const byte std_lum_qt[64] = {
    16, 11, 10, 16, 24, 40, 51, 61,
    12, 12, 14, 19, 26, 58, 60, 55,
    14, 13, 16, 24, 40, 57, 69, 56,
    14, 17, 22, 29, 51, 87, 80, 62,
    18, 22, 37, 56, 68,109,103, 77,
    24, 35, 55, 64, 81,104,113, 92,
    49, 64, 78, 87,103,121,120,101,
    72, 92, 95, 98,112,100,103, 99
};

static const byte std_chroma_qt[64] = {
    17, 18, 24, 47, 99, 99, 99, 99,
    18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99,
    47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99
};

// Zigzag scan order: zigzag[i] = natural-order index of i-th zigzag coeff.
static const byte zigzag[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

// ---------------------------------------------------------------------------
// Standard Huffman tables, Annex K "bits" + "values" form.
// We build canonical codes at init.
// ---------------------------------------------------------------------------

static const byte bits_dc_lum[16] =
    {0,1,5,1,1,1,1,1,1,0,0,0,0,0,0,0};
static const byte val_dc_lum[12] =
    {0,1,2,3,4,5,6,7,8,9,10,11};

static const byte bits_ac_lum[16] =
    {0,2,1,3,3,2,4,3,5,5,4,4,0,0,1,0x7d};
static const byte val_ac_lum[162] = {
    0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,
    0x21,0x31,0x41,0x06,0x13,0x51,0x61,0x07,
    0x22,0x71,0x14,0x32,0x81,0x91,0xa1,0x08,
    0x23,0x42,0xb1,0xc1,0x15,0x52,0xd1,0xf0,
    0x24,0x33,0x62,0x72,0x82,0x09,0x0a,0x16,
    0x17,0x18,0x19,0x1a,0x25,0x26,0x27,0x28,
    0x29,0x2a,0x34,0x35,0x36,0x37,0x38,0x39,
    0x3a,0x43,0x44,0x45,0x46,0x47,0x48,0x49,
    0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,
    0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,
    0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,
    0x7a,0x83,0x84,0x85,0x86,0x87,0x88,0x89,
    0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,
    0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,
    0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,
    0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,
    0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,
    0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xe1,0xe2,
    0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,
    0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,
    0xf9,0xfa
};

static const byte bits_dc_chroma[16] =
    {0,3,1,1,1,1,1,1,1,1,1,0,0,0,0,0};
static const byte val_dc_chroma[12] =
    {0,1,2,3,4,5,6,7,8,9,10,11};

static const byte bits_ac_chroma[16] =
    {0,2,1,2,4,4,3,4,7,5,4,4,0,1,2,0x77};
static const byte val_ac_chroma[162] = {
    0x00,0x01,0x02,0x03,0x11,0x04,0x05,0x21,
    0x31,0x06,0x12,0x41,0x51,0x07,0x61,0x71,
    0x13,0x22,0x32,0x81,0x08,0x14,0x42,0x91,
    0xa1,0xb1,0xc1,0x09,0x23,0x33,0x52,0xf0,
    0x15,0x62,0x72,0xd1,0x0a,0x16,0x24,0x34,
    0xe1,0x25,0xf1,0x17,0x18,0x19,0x1a,0x26,
    0x27,0x28,0x29,0x2a,0x35,0x36,0x37,0x38,
    0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,
    0x49,0x4a,0x53,0x54,0x55,0x56,0x57,0x58,
    0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,
    0x69,0x6a,0x73,0x74,0x75,0x76,0x77,0x78,
    0x79,0x7a,0x82,0x83,0x84,0x85,0x86,0x87,
    0x88,0x89,0x8a,0x92,0x93,0x94,0x95,0x96,
    0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,
    0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,
    0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,
    0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xd2,
    0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,
    0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,
    0xea,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,
    0xf9,0xfa
};

// Canonical Huffman code tables, indexed by symbol.
typedef struct {
    unsigned short code[256];
    byte size[256];
} huff_table_t;

static huff_table_t h_dc_lum, h_ac_lum, h_dc_chroma, h_ac_chroma;
static int huff_inited = 0;

static void build_huff(huff_table_t *ht, const byte *bits, const byte *vals,
                       int nvals)
{
    unsigned short code = 0;
    int k = 0, len, i;
    memset(ht, 0, sizeof(*ht));
    for (len = 1; len <= 16; len++)
    {
        for (i = 0; i < bits[len - 1]; i++)
        {
            if (k >= nvals)
                return;
            ht->code[vals[k]] = code;
            ht->size[vals[k]] = (byte)len;
            k++;
            code++;
        }
        code <<= 1;
    }
}

static void init_huffman(void)
{
    if (huff_inited)
        return;
    build_huff(&h_dc_lum, bits_dc_lum, val_dc_lum, 12);
    build_huff(&h_ac_lum, bits_ac_lum, val_ac_lum, 162);
    build_huff(&h_dc_chroma, bits_dc_chroma, val_dc_chroma, 12);
    build_huff(&h_ac_chroma, bits_ac_chroma, val_ac_chroma, 162);
    huff_inited = 1;
}

// ---------------------------------------------------------------------------
// Bit writer
// ---------------------------------------------------------------------------

typedef struct {
    FILE *f;
    unsigned int acc;
    int nbits;
} bitwriter_t;

static void bw_init(bitwriter_t *bw, FILE *f)
{
    bw->f = f;
    bw->acc = 0;
    bw->nbits = 0;
}

static void bw_emit(bitwriter_t *bw, unsigned int code, int size)
{
    bw->acc = (bw->acc << size) | code;
    bw->nbits += size;
    while (bw->nbits >= 8)
    {
        int b = (bw->acc >> (bw->nbits - 8)) & 0xFF;
        fputc(b, bw->f);
        if (b == 0xFF)
            fputc(0x00, bw->f); // byte stuffing
        bw->nbits -= 8;
    }
}

static void bw_flush(bitwriter_t *bw)
{
    if (bw->nbits > 0)
    {
        // Pad with 1s per JPEG spec.
        int b = ((bw->acc << (8 - bw->nbits)) | ((1 << (8 - bw->nbits)) - 1)) & 0xFF;
        fputc(b, bw->f);
        if (b == 0xFF)
            fputc(0x00, bw->f);
        bw->nbits = 0;
        bw->acc = 0;
    }
}

// ---------------------------------------------------------------------------
// Forward DCT (AAN fast float DCT)
// ---------------------------------------------------------------------------

static void fdct(float *blk)
{
    float tmp0, tmp1, tmp2, tmp3, tmp4, tmp5, tmp6, tmp7;
    float tmp10, tmp11, tmp12, tmp13;
    float z1, z2, z3, z4, z5, z11, z13;
    float *dataptr;
    int i;

    // Pass 1: process rows.
    dataptr = blk;
    for (i = 0; i < 8; i++)
    {
        tmp0 = dataptr[0] + dataptr[7];
        tmp7 = dataptr[0] - dataptr[7];
        tmp1 = dataptr[1] + dataptr[6];
        tmp6 = dataptr[1] - dataptr[6];
        tmp2 = dataptr[2] + dataptr[5];
        tmp5 = dataptr[2] - dataptr[5];
        tmp3 = dataptr[3] + dataptr[4];
        tmp4 = dataptr[3] - dataptr[4];

        tmp10 = tmp0 + tmp3;
        tmp13 = tmp0 - tmp3;
        tmp11 = tmp1 + tmp2;
        tmp12 = tmp1 - tmp2;

        dataptr[0] = tmp10 + tmp11;
        dataptr[4] = tmp10 - tmp11;

        z1 = (tmp12 + tmp13) * 0.707106781f;
        dataptr[2] = tmp13 + z1;
        dataptr[6] = tmp13 - z1;

        tmp10 = tmp4 + tmp5;
        tmp11 = tmp5 + tmp6;
        tmp12 = tmp6 + tmp7;

        z5 = (tmp10 - tmp12) * 0.382683433f;
        z2 = 0.541196100f * tmp10 + z5;
        z4 = 1.306562965f * tmp12 + z5;
        z3 = tmp11 * 0.707106781f;

        z11 = tmp7 + z3;
        z13 = tmp7 - z3;

        dataptr[5] = z13 + z2;
        dataptr[3] = z13 - z2;
        dataptr[1] = z11 + z4;
        dataptr[7] = z11 - z4;

        dataptr += 8;
    }

    // Pass 2: process columns.
    dataptr = blk;
    for (i = 0; i < 8; i++)
    {
        tmp0 = dataptr[0] + dataptr[56];
        tmp7 = dataptr[0] - dataptr[56];
        tmp1 = dataptr[8] + dataptr[48];
        tmp6 = dataptr[8] - dataptr[48];
        tmp2 = dataptr[16] + dataptr[40];
        tmp5 = dataptr[16] - dataptr[40];
        tmp3 = dataptr[24] + dataptr[32];
        tmp4 = dataptr[24] - dataptr[32];

        tmp10 = tmp0 + tmp3;
        tmp13 = tmp0 - tmp3;
        tmp11 = tmp1 + tmp2;
        tmp12 = tmp1 - tmp2;

        dataptr[0] = tmp10 + tmp11;
        dataptr[32] = tmp10 - tmp11;

        z1 = (tmp12 + tmp13) * 0.707106781f;
        dataptr[16] = tmp13 + z1;
        dataptr[48] = tmp13 - z1;

        tmp10 = tmp4 + tmp5;
        tmp11 = tmp5 + tmp6;
        tmp12 = tmp6 + tmp7;

        z5 = (tmp10 - tmp12) * 0.382683433f;
        z2 = 0.541196100f * tmp10 + z5;
        z4 = 1.306562965f * tmp12 + z5;
        z3 = tmp11 * 0.707106781f;

        z11 = tmp7 + z3;
        z13 = tmp7 - z3;

        dataptr[40] = z13 + z2;
        dataptr[24] = z13 - z2;
        dataptr[8] = z11 + z4;
        dataptr[56] = z11 - z4;

        dataptr++;
    }
}

// ---------------------------------------------------------------------------
// Block encoding
// ---------------------------------------------------------------------------

// Encode one 8x8 block. data[] is 64 samples in natural order, level-shifted
// by -128 already. prev_dc is updated. Returns via bitwriter.
static void encode_block(bitwriter_t *bw, float *fblk, int *prev_dc,
                         const byte *qt, huff_table_t *h_dc,
                         huff_table_t *h_ac)
{
    int zz[64];
    int i, diff, size, nbits, run;
    int eob;

    fdct(fblk);

    // Quantize into zigzag order.
    for (i = 0; i < 64; i++)
    {
        float v = fblk[zigzag[i]] / 8.0f / qt[zigzag[i]];
        zz[i] = (int)(v < 0 ? v - 0.5f : v + 0.5f);
    }

    // DC: differential.
    diff = zz[0] - *prev_dc;
    *prev_dc = zz[0];
    if (diff == 0)
        size = 0;
    else
    {
        int ad = diff < 0 ? -diff : diff;
        size = 0;
        while (ad) { size++; ad >>= 1; }
    }
    bw_emit(bw, h_dc->code[size], h_dc->size[size]);
    if (size)
    {
        nbits = diff < 0 ? diff - 1 + (1 << size) : diff;
        bw_emit(bw, (unsigned int)nbits, size);
    }

    // AC: run-length.
    run = 0;
    eob = 64;
    // Find last nonzero.
    while (eob > 1 && zz[eob - 1] == 0)
        eob--;
    for (i = 1; i < eob; i++)
    {
        if (zz[i] == 0)
        {
            run++;
            continue;
        }
        while (run > 15)
        {
            // ZRL: 0xF0
            bw_emit(bw, h_ac->code[0xF0], h_ac->size[0xF0]);
            run -= 16;
        }
        {
            int v = zz[i], av = v < 0 ? -v : v;
            size = 0;
            while (av) { size++; av >>= 1; }
            {
                int sym = (run << 4) | size;
                bw_emit(bw, h_ac->code[sym], h_ac->size[sym]);
                nbits = v < 0 ? v - 1 + (1 << size) : v;
                bw_emit(bw, (unsigned int)nbits, size);
            }
        }
        run = 0;
    }
    if (eob < 64)
        bw_emit(bw, h_ac->code[0x00], h_ac->size[0x00]); // EOB
    else if (run > 0)
        bw_emit(bw, h_ac->code[0x00], h_ac->size[0x00]);
}

// ---------------------------------------------------------------------------
// JPEG markers
// ---------------------------------------------------------------------------

static void write_u16(FILE *f, unsigned short v)
{
    fputc((v >> 8) & 0xFF, f);
    fputc(v & 0xFF, f);
}

static void write_marker(FILE *f, byte id)
{
    fputc(0xFF, f);
    fputc(id, f);
}

static void write_dqt(FILE *f, const byte *qt_lum, const byte *qt_chroma)
{
    int i;
    write_marker(f, 0xDB); // DQT
    write_u16(f, 2 + 2 * (1 + 64));
    fputc(0x00, f); // 8-bit, table 0 (luminance)
    for (i = 0; i < 64; i++)
        fputc(qt_lum[zigzag[i]], f);
    fputc(0x01, f); // 8-bit, table 1 (chrominance)
    for (i = 0; i < 64; i++)
        fputc(qt_chroma[zigzag[i]], f);
}

static void write_dht(FILE *f, const byte *bits, const byte *vals, int nvals,
                      int tc_th)
{
    write_marker(f, 0xC4); // DHT
    write_u16(f, (unsigned short)(2 + 1 + 16 + nvals));
    fputc(tc_th, f);
    fwrite(bits, 1, 16, f);
    fwrite(vals, 1, nvals, f);
}

// ---------------------------------------------------------------------------
// Main entry point
// ---------------------------------------------------------------------------

int D_WriteJPEG(const char *filename, byte *rgb_data, int width, int height,
                int quality)
{
    FILE *f;
    bitwriter_t bw;
    byte qt_lum[64], qt_chroma[64];
    int i, mx, my, bx, by;
    int mcu_w, mcu_h;
    int prev_dc_y = 0, prev_dc_cb = 0, prev_dc_cr = 0;
    float fblk[64];
    // Padded Y/Cb/Cr planes (4:2:0).
    int pw = (width + 15) & ~15;
    int ph = (height + 15) & ~15;
    byte *plane_y, *plane_cb, *plane_cr;
    int scale;

    if (!filename || !rgb_data || width <= 0 || height <= 0)
        return 1;
    if (quality < 1) quality = 1;
    if (quality > 100) quality = 100;

    init_huffman();

    // IJG quality scaling.
    if (quality < 50)
        scale = 5000 / quality;
    else
        scale = 200 - quality * 2;
    for (i = 0; i < 64; i++)
    {
        int v = (std_lum_qt[i] * scale + 50) / 100;
        qt_lum[i] = (byte)(v < 1 ? 1 : (v > 255 ? 255 : v));
        v = (std_chroma_qt[i] * scale + 50) / 100;
        qt_chroma[i] = (byte)(v < 1 ? 1 : (v > 255 ? 255 : v));
    }

    // Convert RGB -> YCbCr 4:2:0 into padded planes.
    plane_y = (byte *)malloc(pw * ph);
    plane_cb = (byte *)malloc((pw / 2) * (ph / 2));
    plane_cr = (byte *)malloc((pw / 2) * (ph / 2));
    if (!plane_y || !plane_cb || !plane_cr)
    {
        free(plane_y); free(plane_cb); free(plane_cr);
        return 2;
    }
    memset(plane_y, 0, pw * ph);
    memset(plane_cb, 128, (pw / 2) * (ph / 2));
    memset(plane_cr, 128, (pw / 2) * (ph / 2));

    for (my = 0; my < height; my++)
    {
        for (mx = 0; mx < width; mx++)
        {
            byte r = rgb_data[(my * width + mx) * 3 + 0];
            byte g = rgb_data[(my * width + mx) * 3 + 1];
            byte b = rgb_data[(my * width + mx) * 3 + 2];
            // ITU-R BT.601
            int y  = (int)( 0.299f   * r + 0.587f   * g + 0.114f   * b);
            int cb = (int)(-0.16874f * r - 0.33126f * g + 0.5f     * b + 128);
            int cr = (int)( 0.5f     * r - 0.41869f * g - 0.08131f * b + 128);
            plane_y[my * pw + mx] = (byte)(y < 0 ? 0 : (y > 255 ? 255 : y));
            // Box-filter 2x2 downsample for chroma.
            if ((mx & 1) == 0 && (my & 1) == 0)
            {
                int sx, sy, sum_cb = 0, sum_cr = 0, n = 0;
                for (sy = 0; sy < 2 && my + sy < height; sy++)
                    for (sx = 0; sx < 2 && mx + sx < width; sx++)
                    {
                        byte rr = rgb_data[((my+sy) * width + (mx+sx)) * 3 + 0];
                        byte gg = rgb_data[((my+sy) * width + (mx+sx)) * 3 + 1];
                        byte bb = rgb_data[((my+sy) * width + (mx+sx)) * 3 + 2];
                        sum_cb += (int)(-0.16874f * rr - 0.33126f * gg + 0.5f * bb + 128);
                        sum_cr += (int)( 0.5f * rr - 0.41869f * gg - 0.08131f * bb + 128);
                        n++;
                    }
                sum_cb /= n; sum_cr /= n;
                plane_cb[(my/2) * (pw/2) + (mx/2)] =
                    (byte)(sum_cb < 0 ? 0 : (sum_cb > 255 ? 255 : sum_cb));
                plane_cr[(my/2) * (pw/2) + (mx/2)] =
                    (byte)(sum_cr < 0 ? 0 : (sum_cr > 255 ? 255 : sum_cr));
            }
        }
    }
    // Replicate edge pixels into padding.
    for (my = 0; my < ph; my++)
        for (mx = width; mx < pw; mx++)
            plane_y[my * pw + mx] = plane_y[my * pw + width - 1];
    for (my = height; my < ph; my++)
        memcpy(plane_y + my * pw, plane_y + (height - 1) * pw, pw);
    for (my = 0; my < ph / 2; my++)
        for (mx = width / 2; mx < pw / 2; mx++)
        {
            plane_cb[my * (pw/2) + mx] = plane_cb[my * (pw/2) + width/2 - 1];
            plane_cr[my * (pw/2) + mx] = plane_cr[my * (pw/2) + width/2 - 1];
        }
    for (my = height / 2; my < ph / 2; my++)
    {
        memcpy(plane_cb + my * (pw/2), plane_cb + (height/2 - 1) * (pw/2), pw/2);
        memcpy(plane_cr + my * (pw/2), plane_cr + (height/2 - 1) * (pw/2), pw/2);
    }

    f = fopen(filename, "wb");
    if (!f)
    {
        free(plane_y); free(plane_cb); free(plane_cr);
        return 3;
    }

    // SOI
    write_marker(f, 0xD8);

    // APP0 (JFIF)
    write_marker(f, 0xE0);
    write_u16(f, 16);
    fwrite("JFIF\0", 1, 5, f);
    fputc(1, f); fputc(1, f); // version 1.1
    fputc(0, f);              // no density units
    write_u16(f, 1); write_u16(f, 1);
    fputc(0, f); fputc(0, f); // no thumbnail

    // DQT
    write_dqt(f, qt_lum, qt_chroma);

    // SOF0 (baseline)
    write_marker(f, 0xC0);
    write_u16(f, 8 + 3 * 3);
    fputc(8, f); // 8-bit samples
    write_u16(f, (unsigned short)height);
    write_u16(f, (unsigned short)width);
    fputc(3, f); // 3 components
    fputc(1, f); fputc(0x22, f); fputc(0, f); // Y:  id 1, 2x2, QT 0
    fputc(2, f); fputc(0x11, f); fputc(1, f); // Cb: id 2, 1x1, QT 1
    fputc(3, f); fputc(0x11, f); fputc(1, f); // Cr: id 3, 1x1, QT 1

    // DHT x4
    write_dht(f, bits_dc_lum, val_dc_lum, 12, 0x00);
    write_dht(f, bits_ac_lum, val_ac_lum, 162, 0x10);
    write_dht(f, bits_dc_chroma, val_dc_chroma, 12, 0x01);
    write_dht(f, bits_ac_chroma, val_ac_chroma, 162, 0x11);

    // SOS
    write_marker(f, 0xDA);
    write_u16(f, 6 + 2 * 3);
    fputc(3, f);
    fputc(1, f); fputc(0x00, f); // Y:  DC 0, AC 0
    fputc(2, f); fputc(0x11, f); // Cb: DC 1, AC 1
    fputc(3, f); fputc(0x11, f); // Cr: DC 1, AC 1
    fputc(0, f); fputc(63, f); fputc(0, f); // Ss, Se, Ah/Al

    // Scan data: MCU order.
    bw_init(&bw, f);
    mcu_w = pw / 16;
    mcu_h = ph / 16;
    for (my = 0; my < mcu_h; my++)
        for (mx = 0; mx < mcu_w; mx++)
        {
            // 4 Y blocks.
            for (by = 0; by < 2; by++)
                for (bx = 0; bx < 2; bx++)
                {
                    int ox = mx * 16 + bx * 8;
                    int oy = my * 16 + by * 8;
                    int px, py;
                    for (py = 0; py < 8; py++)
                        for (px = 0; px < 8; px++)
                            fblk[py * 8 + px] =
                                (float)plane_y[(oy + py) * pw + (ox + px)] - 128.0f;
                    encode_block(&bw, fblk, &prev_dc_y, qt_lum,
                                 &h_dc_lum, &h_ac_lum);
                }
            // Cb block.
            {
                int ox = mx * 8, oy = my * 8, px, py;
                for (py = 0; py < 8; py++)
                    for (px = 0; px < 8; px++)
                        fblk[py * 8 + px] =
                            (float)plane_cb[(oy + py) * (pw/2) + (ox + px)] - 128.0f;
                encode_block(&bw, fblk, &prev_dc_cb, qt_chroma,
                             &h_dc_chroma, &h_ac_chroma);
            }
            // Cr block.
            {
                int ox = mx * 8, oy = my * 8, px, py;
                for (py = 0; py < 8; py++)
                    for (px = 0; px < 8; px++)
                        fblk[py * 8 + px] =
                            (float)plane_cr[(oy + py) * (pw/2) + (ox + px)] - 128.0f;
                encode_block(&bw, fblk, &prev_dc_cr, qt_chroma,
                             &h_dc_chroma, &h_ac_chroma);
            }
        }
    bw_flush(&bw);

    // EOI
    write_marker(f, 0xD9);

    fclose(f);
    free(plane_y); free(plane_cb); free(plane_cr);
    return 0;
}
