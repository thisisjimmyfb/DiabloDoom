//
// Copyright(C) 1993-1996 Id Software, Inc.
// Copyright(C) 1993-2008 Raven Software
// Copyright(C) 2005-2014 Simon Howard
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//	Gamma correction LUT stuff.
//	Functions to draw patches (by post) directly to screen.
//	Functions to blit a block to the screen.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>

#include "i_system.h"

#include "doomtype.h"

#include "deh_str.h"
#include "i_input.h"
#include "i_swap.h"
#include "i_video.h"
#include "m_bbox.h"
#include "m_misc.h"
#include "v_video.h"
#include "w_wad.h"
#include "z_zone.h"

#include "config.h"
#ifdef HAVE_LIBPNG
#include <png.h>
#endif

// TODO: There are separate RANGECHECK defines for different games, but this
// is common code. Fix this.
#define RANGECHECK

// Blending table used for fuzzpatch, etc.
// Only used in Heretic/Hexen

byte *tinttable = NULL;

// villsa [STRIFE] Blending table used for Strife
byte *xlatab = NULL;

// The screen buffer that the v_video.c code draws to.

static pixel_t *dest_screen = NULL;

int dirtybox[4]; 

// haleyjd 08/28/10: clipping callback function for patches.
// This is needed for Chocolate Strife, which clips patches to the screen.
static vpatchclipfunc_t patchclip_callback = NULL;

//
// V_MarkRect 
// 
void V_MarkRect(int x, int y, int width, int height) 
{ 
    // If we are temporarily using an alternate screen, do not 
    // affect the update box.

    if (dest_screen == I_VideoBuffer)
    {
        M_AddToBox (dirtybox, x, y); 
        M_AddToBox (dirtybox, x + width-1, y + height-1); 
    }
} 
 

//
// V_CopyRect 
// 
void V_CopyRect(int srcx, int srcy, pixel_t *source,
                int width, int height,
                int destx, int desty)
{ 
    pixel_t *src;
    pixel_t *dest;
 
#ifdef RANGECHECK 
    if (srcx < 0
     || srcx + width > SCREENWIDTH
     || srcy < 0
     || srcy + height > SCREENHEIGHT 
     || destx < 0
     || destx + width > SCREENWIDTH
     || desty < 0
     || desty + height > SCREENHEIGHT)
    {
        I_Error ("Bad V_CopyRect");
    }
#endif 

    V_MarkRect(destx, desty, width, height); 
 
    src = source + SCREENWIDTH * srcy + srcx; 
    dest = dest_screen + SCREENWIDTH * desty + destx; 

    for ( ; height>0 ; height--) 
    { 
        memcpy(dest, src, width * sizeof(*dest));
        src += SCREENWIDTH; 
        dest += SCREENWIDTH; 
    } 
} 
 
//
// V_SetPatchClipCallback
//
// haleyjd 08/28/10: Added for Strife support.
// By calling this function, you can setup runtime error checking for patch 
// clipping. Strife never caused errors by drawing patches partway off-screen.
// Some versions of vanilla DOOM also behaved differently than the default
// implementation, so this could possibly be extended to those as well for
// accurate emulation.
//
void V_SetPatchClipCallback(vpatchclipfunc_t func)
{
    patchclip_callback = func;
}

//
// V_DrawPatch
// Masks a column based masked pic to the screen. 
//

void V_DrawPatch(int x, int y, patch_t *patch)
{ 
    int count;
    int col;
    column_t *column;
    pixel_t *desttop;
    pixel_t *dest;
    byte *source;
    int w;

    y -= SHORT(patch->topoffset);
    x -= SHORT(patch->leftoffset);

    // haleyjd 08/28/10: Strife needs silent error checking here.
    if(patchclip_callback)
    {
        if(!patchclip_callback(patch, x, y))
            return;
    }

#ifdef RANGECHECK
    if (x < 0
     || x + SHORT(patch->width) > SCREENWIDTH
     || y < 0
     || y + SHORT(patch->height) > SCREENHEIGHT)
    {
        I_Error("Bad V_DrawPatch");
    }
#endif

    V_MarkRect(x, y, SHORT(patch->width), SHORT(patch->height));

    col = 0;
    desttop = dest_screen + y * SCREENWIDTH + x;

    w = SHORT(patch->width);

    for ( ; col<w ; x++, col++, desttop++)
    {
        column = (column_t *)((byte *)patch + LONG(patch->columnofs[col]));

        // step through the posts in a column
        while (column->topdelta != 0xff)
        {
            source = (byte *)column + 3;
            dest = desttop + column->topdelta*SCREENWIDTH;
            count = column->length;

            while (count--)
            {
                *dest = *source++;
                dest += SCREENWIDTH;
            }
            column = (column_t *)((byte *)column + column->length + 4);
        }
    }
}

//
// V_DrawPatchFlipped
// Masks a column based masked pic to the screen.
// Flips horizontally, e.g. to mirror face.
//

void V_DrawPatchFlipped(int x, int y, patch_t *patch)
{
    int count;
    int col; 
    column_t *column; 
    pixel_t *desttop;
    pixel_t *dest;
    byte *source; 
    int w; 
 
    y -= SHORT(patch->topoffset); 
    x -= SHORT(patch->leftoffset); 

    // haleyjd 08/28/10: Strife needs silent error checking here.
    if(patchclip_callback)
    {
        if(!patchclip_callback(patch, x, y))
            return;
    }

#ifdef RANGECHECK 
    if (x < 0
     || x + SHORT(patch->width) > SCREENWIDTH
     || y < 0
     || y + SHORT(patch->height) > SCREENHEIGHT)
    {
        I_Error("Bad V_DrawPatchFlipped");
    }
#endif

    V_MarkRect (x, y, SHORT(patch->width), SHORT(patch->height));

    col = 0;
    desttop = dest_screen + y * SCREENWIDTH + x;

    w = SHORT(patch->width);

    for ( ; col<w ; x++, col++, desttop++)
    {
        column = (column_t *)((byte *)patch + LONG(patch->columnofs[w-1-col]));

        // step through the posts in a column
        while (column->topdelta != 0xff )
        {
            source = (byte *)column + 3;
            dest = desttop + column->topdelta*SCREENWIDTH;
            count = column->length;

            while (count--)
            {
                *dest = *source++;
                dest += SCREENWIDTH;
            }
            column = (column_t *)((byte *)column + column->length + 4);
        }
    }
}



//
// V_DrawPatchDirect
// Draws directly to the screen on the pc. 
//

void V_DrawPatchDirect(int x, int y, patch_t *patch)
{
    V_DrawPatch(x, y, patch); 
} 

//
// V_DrawTLPatch
//
// Masks a column based translucent masked pic to the screen.
//

void V_DrawTLPatch(int x, int y, patch_t * patch)
{
    int count, col;
    column_t *column;
    pixel_t *desttop, *dest;
    byte *source;
    int w;

    y -= SHORT(patch->topoffset);
    x -= SHORT(patch->leftoffset);

    if (x < 0
     || x + SHORT(patch->width) > SCREENWIDTH 
     || y < 0
     || y + SHORT(patch->height) > SCREENHEIGHT)
    {
        I_Error("Bad V_DrawTLPatch");
    }

    col = 0;
    desttop = dest_screen + y * SCREENWIDTH + x;

    w = SHORT(patch->width);
    for (; col < w; x++, col++, desttop++)
    {
        column = (column_t *) ((byte *) patch + LONG(patch->columnofs[col]));

        // step through the posts in a column

        while (column->topdelta != 0xff)
        {
            source = (byte *) column + 3;
            dest = desttop + column->topdelta * SCREENWIDTH;
            count = column->length;

            while (count--)
            {
                *dest = tinttable[*dest + ((*source++) << 8)];
                dest += SCREENWIDTH;
            }
            column = (column_t *) ((byte *) column + column->length + 4);
        }
    }
}

//
// V_DrawXlaPatch
//
// villsa [STRIFE] Masks a column based translucent masked pic to the screen.
//

void V_DrawXlaPatch(int x, int y, patch_t * patch)
{
    int count, col;
    column_t *column;
    pixel_t *desttop, *dest;
    byte *source;
    int w;

    y -= SHORT(patch->topoffset);
    x -= SHORT(patch->leftoffset);

    if(patchclip_callback)
    {
        if(!patchclip_callback(patch, x, y))
            return;
    }

    col = 0;
    desttop = dest_screen + y * SCREENWIDTH + x;

    w = SHORT(patch->width);
    for(; col < w; x++, col++, desttop++)
    {
        column = (column_t *) ((byte *) patch + LONG(patch->columnofs[col]));

        // step through the posts in a column

        while(column->topdelta != 0xff)
        {
            source = (byte *) column + 3;
            dest = desttop + column->topdelta * SCREENWIDTH;
            count = column->length;

            while(count--)
            {
                *dest = xlatab[*dest + ((*source) << 8)];
                source++;
                dest += SCREENWIDTH;
            }
            column = (column_t *) ((byte *) column + column->length + 4);
        }
    }
}

//
// V_DrawAltTLPatch
//
// Masks a column based translucent masked pic to the screen.
//

void V_DrawAltTLPatch(int x, int y, patch_t * patch)
{
    int count, col;
    column_t *column;
    pixel_t *desttop, *dest;
    byte *source;
    int w;

    y -= SHORT(patch->topoffset);
    x -= SHORT(patch->leftoffset);

    if (x < 0
     || x + SHORT(patch->width) > SCREENWIDTH
     || y < 0
     || y + SHORT(patch->height) > SCREENHEIGHT)
    {
        I_Error("Bad V_DrawAltTLPatch");
    }

    col = 0;
    desttop = dest_screen + y * SCREENWIDTH + x;

    w = SHORT(patch->width);
    for (; col < w; x++, col++, desttop++)
    {
        column = (column_t *) ((byte *) patch + LONG(patch->columnofs[col]));

        // step through the posts in a column

        while (column->topdelta != 0xff)
        {
            source = (byte *) column + 3;
            dest = desttop + column->topdelta * SCREENWIDTH;
            count = column->length;

            while (count--)
            {
                *dest = tinttable[((*dest) << 8) + *source++];
                dest += SCREENWIDTH;
            }
            column = (column_t *) ((byte *) column + column->length + 4);
        }
    }
}

//
// V_DrawShadowedPatch
//
// Masks a column based masked pic to the screen.
//

void V_DrawShadowedPatch(int x, int y, patch_t *patch)
{
    int count, col;
    column_t *column;
    pixel_t *desttop, *dest;
    byte *source;
    pixel_t *desttop2, *dest2;
    int w;

    y -= SHORT(patch->topoffset);
    x -= SHORT(patch->leftoffset);

    if (x < 0
     || x + SHORT(patch->width) > SCREENWIDTH
     || y < 0
     || y + SHORT(patch->height) > SCREENHEIGHT)
    {
        I_Error("Bad V_DrawShadowedPatch");
    }

    col = 0;
    desttop = dest_screen + y * SCREENWIDTH + x;
    desttop2 = dest_screen + (y + 2) * SCREENWIDTH + x + 2;

    w = SHORT(patch->width);
    for (; col < w; x++, col++, desttop++, desttop2++)
    {
        column = (column_t *) ((byte *) patch + LONG(patch->columnofs[col]));

        // step through the posts in a column

        while (column->topdelta != 0xff)
        {
            source = (byte *) column + 3;
            dest = desttop + column->topdelta * SCREENWIDTH;
            dest2 = desttop2 + column->topdelta * SCREENWIDTH;
            count = column->length;

            while (count--)
            {
                *dest2 = tinttable[((*dest2) << 8)];
                dest2 += SCREENWIDTH;
                *dest = *source++;
                dest += SCREENWIDTH;

            }
            column = (column_t *) ((byte *) column + column->length + 4);
        }
    }
}

//
// Load tint table from TINTTAB lump.
//

void V_LoadTintTable(void)
{
    tinttable = W_CacheLumpName("TINTTAB", PU_STATIC);
}

//
// V_LoadXlaTable
//
// villsa [STRIFE] Load xla table from XLATAB lump.
//

void V_LoadXlaTable(void)
{
    xlatab = W_CacheLumpName("XLATAB", PU_STATIC);
}

//
// V_DrawBlock
// Draw a linear block of pixels into the view buffer.
//

void V_DrawBlock(int x, int y, int width, int height, pixel_t *src)
{ 
    pixel_t *dest;
 
#ifdef RANGECHECK 
    if (x < 0
     || x + width >SCREENWIDTH
     || y < 0
     || y + height > SCREENHEIGHT)
    {
	I_Error ("Bad V_DrawBlock");
    }
#endif 
 
    V_MarkRect (x, y, width, height); 
 
    dest = dest_screen + y * SCREENWIDTH + x; 

    while (height--) 
    { 
	memcpy (dest, src, width * sizeof(*dest));
	src += width; 
	dest += SCREENWIDTH; 
    } 
} 

void V_DrawFilledBox(int x, int y, int w, int h, int c)
{
    pixel_t *buf, *buf1;
    int x1, y1;

    // Clip to the frame buffer. Callers can pass origins on or past
    // the screen edges (e.g. UI elements that follow the mouse), and
    // an unclipped write runs past I_VideoBuffer into the heap chunk
    // that follows the screen surface -- the corruption then only
    // surfaces when that chunk is freed (crash on quit, 2026-10-22).
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SCREENWIDTH)  w = SCREENWIDTH - x;
    if (y + h > SCREENHEIGHT) h = SCREENHEIGHT - y;
    if (w <= 0 || h <= 0)
        return;

    buf = I_VideoBuffer + SCREENWIDTH * y + x;

    for (y1 = 0; y1 < h; ++y1)
    {
        buf1 = buf;

        for (x1 = 0; x1 < w; ++x1)
        {
            *buf1++ = c;
        }

        buf += SCREENWIDTH;
    }
}

void V_DrawBitmapGlyph(int x, int y, const byte *glyph, int w, int h,
                       pixel_t colour)
{
    int row, col, bitpos = 0;

    if (glyph == NULL || w <= 0 || h <= 0)
        return;
    for (row = 0; row < h; row++)
    {
        for (col = 0; col < w; col++, bitpos++)
        {
            int dx = x + col, dy = y + row;
            if ((glyph[bitpos / 8] & (1 << (bitpos % 8))) == 0)
                continue;
            if (dx < 0 || dx >= SCREENWIDTH || dy < 0 || dy >= SCREENHEIGHT)
                continue;
            dest_screen[dy * SCREENWIDTH + dx] = colour;
        }
    }
}

void V_DrawHorizLine(int x, int y, int w, int c)
{
    pixel_t *buf;
    int x1;

    // Clip to the frame buffer (see V_DrawFilledBox).
    if (y < 0 || y >= SCREENHEIGHT)
        return;
    if (x < 0) { w += x; x = 0; }
    if (x + w > SCREENWIDTH) w = SCREENWIDTH - x;
    if (w <= 0)
        return;

    buf = I_VideoBuffer + SCREENWIDTH * y + x;

    for (x1 = 0; x1 < w; ++x1)
    {
        *buf++ = c;
    }
}

void V_DrawVertLine(int x, int y, int h, int c)
{
    pixel_t *buf;
    int y1;

    // Clip to the frame buffer (see V_DrawFilledBox).
    if (x < 0 || x >= SCREENWIDTH)
        return;
    if (y < 0) { h += y; y = 0; }
    if (y + h > SCREENHEIGHT) h = SCREENHEIGHT - y;
    if (h <= 0)
        return;

    buf = I_VideoBuffer + SCREENWIDTH * y + x;

    for (y1 = 0; y1 < h; ++y1)
    {
        *buf = c;
        buf += SCREENWIDTH;
    }
}

void V_DrawBox(int x, int y, int w, int h, int c)
{
    V_DrawHorizLine(x, y, w, c);
    V_DrawHorizLine(x, y+h-1, w, c);
    V_DrawVertLine(x, y, h, c);
    V_DrawVertLine(x+w-1, y, h, c);
}

//
// Draw a "raw" screen (lump containing raw data to blit directly
// to the screen)
//
 
void V_DrawRawScreen(pixel_t *raw)
{
    memcpy(dest_screen, raw, SCREENWIDTH * SCREENHEIGHT * sizeof(*dest_screen));
}

//
// V_Init
// 
void V_Init (void) 
{ 
    // no-op!
    // There used to be separate screens that could be drawn to; these are
    // now handled in the upper layers.
}

// Set the buffer that the code draws to.

void V_UseBuffer(pixel_t *buffer)
{
    dest_screen = buffer;
}

// Restore screen buffer to the i_video screen buffer.

void V_RestoreBuffer(void)
{
    dest_screen = I_VideoBuffer;
}

//
// SCREEN SHOTS
//

typedef PACKED_STRUCT (
{
    char		manufacturer;
    char		version;
    char		encoding;
    char		bits_per_pixel;

    unsigned short	xmin;
    unsigned short	ymin;
    unsigned short	xmax;
    unsigned short	ymax;
    
    unsigned short	hres;
    unsigned short	vres;

    unsigned char	palette[48];
    
    char		reserved;
    char		color_planes;
    unsigned short	bytes_per_line;
    unsigned short	palette_type;
    
    char		filler[58];
    unsigned char	data;		// unbounded
}) pcx_t;


//
// WritePCXfile
//

void WritePCXfile(char *filename, pixel_t *data,
                  int width, int height,
                  byte *palette)
{
    int		i;
    int		length;
    pcx_t*	pcx;
    byte*	pack;
	
    pcx = Z_Malloc (width*height*2+1000, PU_STATIC, NULL);

    pcx->manufacturer = 0x0a;		// PCX id
    pcx->version = 5;			// 256 color
    pcx->encoding = 1;			// uncompressed
    pcx->bits_per_pixel = 8;		// 256 color
    pcx->xmin = 0;
    pcx->ymin = 0;
    pcx->xmax = SHORT(width-1);
    pcx->ymax = SHORT(height-1);
    pcx->hres = SHORT(1);
    pcx->vres = SHORT(1);
    memset (pcx->palette,0,sizeof(pcx->palette));
    pcx->reserved = 0;                  // PCX spec: reserved byte must be zero
    pcx->color_planes = 1;		// chunky image
    pcx->bytes_per_line = SHORT(width);
    pcx->palette_type = SHORT(2);	// not a grey scale
    memset (pcx->filler,0,sizeof(pcx->filler));

    // pack the image
    pack = &pcx->data;
	
    for (i=0 ; i<width*height ; i++)
    {
	if ( (*data & 0xc0) != 0xc0)
	    *pack++ = *data++;
	else
	{
	    *pack++ = 0xc1;
	    *pack++ = *data++;
	}
    }
    
    // write the palette
    *pack++ = 0x0c;	// palette ID byte
    for (i=0 ; i<768 ; i++)
	*pack++ = *palette++;
    
    // write output file
    length = pack - (byte *)pcx;
    M_WriteFile (filename, pcx, length);

    Z_Free (pcx);
}

#ifdef HAVE_LIBPNG
//
// WritePNGfile
//

static void error_fn(png_structp p, png_const_charp s)
{
    printf("libpng error: %s\n", s);
}

static void warning_fn(png_structp p, png_const_charp s)
{
    printf("libpng warning: %s\n", s);
}

void WritePNGfile(char *filename, pixel_t *data,
                  int width, int height,
                  byte *palette)
{
    png_structp ppng;
    png_infop pinfo;
    png_colorp pcolor;
    FILE *handle;
    int i, j;
    int w_factor, h_factor;
    byte *rowbuf;

    if (aspect_ratio_correct == 1)
    {
        // scale up to accommodate aspect ratio correction
        w_factor = 5;
        h_factor = 6;

        width *= w_factor;
        height *= h_factor;
    }
    else
    {
        w_factor = 1;
        h_factor = 1;
    }

    handle = M_fopen(filename, "wb");
    if (!handle)
    {
        return;
    }

    ppng = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL,
                                   error_fn, warning_fn);
    if (!ppng)
    {
        fclose(handle);
        return;
    }

    pinfo = png_create_info_struct(ppng);
    if (!pinfo)
    {
        fclose(handle);
        png_destroy_write_struct(&ppng, NULL);
        return;
    }

    png_init_io(ppng, handle);

    png_set_IHDR(ppng, pinfo, width, height,
                 8, PNG_COLOR_TYPE_PALETTE, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);

    pcolor = malloc(sizeof(*pcolor) * 256);
    if (!pcolor)
    {
        fclose(handle);
        png_destroy_write_struct(&ppng, &pinfo);
        return;
    }

    for (i = 0; i < 256; i++)
    {
        pcolor[i].red   = *(palette + 3 * i);
        pcolor[i].green = *(palette + 3 * i + 1);
        pcolor[i].blue  = *(palette + 3 * i + 2);
    }

    png_set_PLTE(ppng, pinfo, pcolor, 256);
    free(pcolor);

    png_write_info(ppng, pinfo);

    rowbuf = malloc(width);

    if (rowbuf)
    {
        for (i = 0; i < SCREENHEIGHT; i++)
        {
            // expand the row 5x
            for (j = 0; j < SCREENWIDTH; j++)
            {
                memset(rowbuf + j * w_factor, *(data + i*SCREENWIDTH + j), w_factor);
            }

            // write the row 6 times
            for (j = 0; j < h_factor; j++)
            {
                png_write_row(ppng, rowbuf);
            }
        }

        free(rowbuf);
    }

    png_write_end(ppng, pinfo);
    png_destroy_write_struct(&ppng, &pinfo);
    fclose(handle);
}
#endif

//
// Async screenshot queue (defined below V_ScreenShotFile).
static void ss_enqueue(const char *filename);
static boolean ss_filename_queued(const char *name);

// V_ScreenShot
//

char last_screenshot_name[128] = "";

void V_ScreenShot(const char *format)
{
    int i;
    char lbmname[128];
    const char *ext;
    
    // find a file name to save it to

#ifdef HAVE_LIBPNG
    if (png_screenshots)
    {
        ext = "png";
    }
    else
#endif
    {
        ext = "pcx";
    }

    for (i=0; i<=99; i++)
    {
        M_snprintf(lbmname, sizeof(lbmname), format, i, ext);

        // A queued-but-not-yet-written file counts as taken, or two
        // screenshots in the same second could pick the same name.
        if (!M_FileExists(lbmname) && !ss_filename_queued(lbmname))
        {
            break;      // file doesn't exist
        }
    }

    if (i == 100)
    {
#ifdef HAVE_LIBPNG
        if (png_screenshots)
        {
            I_Error ("V_ScreenShot: Couldn't create a PNG");
        }
        else
#endif
        {
            I_Error ("V_ScreenShot: Couldn't create a PCX");
        }
    }

    // Async: copy + queue; the worker thread writes the file. The
    // HUD message below now means "capture queued" (the ~23ms gap
    // before the file lands is imperceptible).
    ss_enqueue(lbmname);
    // Remember the filename for the HUD message
    M_StringCopy(last_screenshot_name, lbmname, sizeof(last_screenshot_name));
}

// V_ScreenShotFile: same framebuffer capture as V_ScreenShot, but
// writes exactly the filename given (no auto-numbering). Used by
// SURVEY v2 (Jimmy, 2026-10-26): one sweep dumps 72 frames named
// survey_<ts>_01.png ... _72.png, in rotation order, so the worker
// reviews the dumped frames instead of hunting video segments.
void V_ScreenShotFile(const char *filename)
{
    // Async: copy + queue; the worker thread writes the file.
    // Used by the SURVEY frame dump (one call per sweep step).
    ss_enqueue(filename);
    M_StringCopy(last_screenshot_name, filename, sizeof(last_screenshot_name));
}

// ------------------------------------------------------------------
// Async screenshot queue (Jimmy, 2026-10-26). Screenshots used to
// encode synchronously in the game thread (~23ms per PNG), which
// stretched a 36-frame SURVEY sweep from ~1.03s of game time to
// ~1.16s+ wall-clock (and v2's 72-frame sweep to 3.77s). The encode
// now runs on one dedicated PNG worker thread -- NOT the replay
// video thread, so a video burst can never starve screenshots or
// vice versa -- while the game thread only does a fast framebuffer
// copy per screenshot.
//
// Correctness story: the worker NEVER reads the live I_VideoBuffer.
// The game thread memcpys the buffer (~256KB at 640x400 8-bit, a
// fraction of a millisecond -- ~500x faster than a 28ms tic) into a
// heap entry at capture time, so the copy always beats the next
// render. The worker encodes from that copy at its own pace. The
// palette is copied per entry too (768 bytes, trivial), so the
// worker never touches the WAD cache from its thread.
//
// Queue policy: bounded (SCREENSHOT_QUEUE_CAP). When full, the game
// thread BLOCKS on a condition variable until a slot frees -- it
// NEVER drops. Dropping oldest/newest would lose a sweep step,
// which is exactly the sampling gap the SURVEY dump was built to
// kill. Under load this degrades gracefully to the old synchronous
// behavior: correct, just slower. A SURVEY burst (36 frames over
// ~1s, worker drains ~43 frames/s) keeps the queue shallow; the cap
// only binds under pathological load.
//
// Flush semantics: V_ScreenshotQueueEmpty() is true only when no
// entry is queued AND the worker is not mid-encode, so a "frames
// ready" log line gated on it means every file is actually on disk.
// V_ScreenshotFlush() blocks until empty; the driver QUIT handler
// calls it before exiting, so a final screenshot queued just
// before QUIT still lands. The worker is detached and never exits
// on its own -- it dies with the process after a QUIT flush (queue
// empty, nothing in flight). Menu/error exits do not flush; only
// the driver QUIT path guarantees pending frames.
//
// Fallbacks (never lose a frame, never wedge): if the worker thread
// cannot start, or an entry cannot be allocated, the game thread
// writes that frame synchronously itself (the old behavior). A
// failed encode still pops its entry and continues -- one bad file
// must never stall the queue behind it.
// ------------------------------------------------------------------

#define SCREENSHOT_QUEUE_CAP 16

typedef struct
{
    char filename[256];
    pixel_t *pixels;      // SCREENWIDTH*SCREENHEIGHT copy, owned
    byte palette[256 * 3];
    boolean use_png;      // png_screenshots snapshotted at queue time
} screenshot_job_t;

static screenshot_job_t *ss_queue[SCREENSHOT_QUEUE_CAP];
static int ss_q_head = 0;
static int ss_q_count = 0;
static int ss_worker_busy = 0;
static int ss_worker_started = 0;
static int ss_worker_failed = 0;
static pthread_mutex_t ss_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ss_not_empty = PTHREAD_COND_INITIALIZER;
static pthread_cond_t ss_not_full = PTHREAD_COND_INITIALIZER;
static pthread_cond_t ss_empty = PTHREAD_COND_INITIALIZER;

static void ss_write_job(screenshot_job_t *job)
{
#ifdef HAVE_LIBPNG
    if (job->use_png)
    {
        WritePNGfile(job->filename, job->pixels,
                     SCREENWIDTH, SCREENHEIGHT, job->palette);
        return;
    }
#endif
    WritePCXfile(job->filename, job->pixels,
                 SCREENWIDTH, SCREENHEIGHT, job->palette);
}

static void *ss_worker(void *arg)
{
    (void) arg;
    for (;;)
    {
        screenshot_job_t *job;
        pthread_mutex_lock(&ss_mutex);
        while (ss_q_count == 0)
            pthread_cond_wait(&ss_not_empty, &ss_mutex);
        job = ss_queue[ss_q_head];
        ss_q_head = (ss_q_head + 1) % SCREENSHOT_QUEUE_CAP;
        ss_q_count--;
        ss_worker_busy = 1;
        pthread_cond_signal(&ss_not_full);
        pthread_mutex_unlock(&ss_mutex);

        // Encode OUTSIDE the queue lock: holding it across a ~23ms
        // encode would block the game thread on the mutex and
        // defeat the whole point of async.
        ss_write_job(job);
        free(job->pixels);
        free(job);

        pthread_mutex_lock(&ss_mutex);
        ss_worker_busy = 0;
        if (ss_q_count == 0)
            pthread_cond_signal(&ss_empty);
        pthread_mutex_unlock(&ss_mutex);
    }
    return NULL;
}

// True when a filename is already queued (not yet written). The
// timestamped picker must treat queued names as taken, or two
// screenshots in the same second could pick the same NN and the
// second would overwrite the first.
static boolean ss_filename_queued(const char *name)
{
    int i;
    boolean found = false;
    pthread_mutex_lock(&ss_mutex);
    for (i = 0; i < ss_q_count; i++)
    {
        if (!strcmp(ss_queue[(ss_q_head + i) % SCREENSHOT_QUEUE_CAP]->filename,
                    name))
        {
            found = true;
            break;
        }
    }
    pthread_mutex_unlock(&ss_mutex);
    return found;
}

// Game-thread enqueue: fast copy, then push. Never called with the
// queue lock held. Falls back to a synchronous write (old behavior)
// when the worker cannot start or an entry cannot be allocated.
static void ss_enqueue(const char *filename)
{
    screenshot_job_t *job;
    pixel_t *pixels;
    byte *pal;

    job = (screenshot_job_t *) malloc(sizeof(*job));
    pixels = (pixel_t *) malloc(SCREENWIDTH * SCREENHEIGHT * sizeof(pixel_t));
    if (!job || !pixels)
    {
        // Allocation failure: write synchronously from the live
        // buffer (old behavior) rather than lose the frame. The
        // palette pointer goes straight to the writer, as before.
        free(job);
        free(pixels);
        pal = W_CacheLumpName(DEH_String("PLAYPAL"), PU_CACHE);
#ifdef HAVE_LIBPNG
        if (png_screenshots)
            WritePNGfile((char *) filename, I_VideoBuffer,
                         SCREENWIDTH, SCREENHEIGHT, pal);
        else
#endif
            WritePCXfile((char *) filename, I_VideoBuffer,
                         SCREENWIDTH, SCREENHEIGHT, pal);
        return;
    }

    M_StringCopy(job->filename, filename, sizeof(job->filename));
    memcpy(pixels, I_VideoBuffer,
           SCREENWIDTH * SCREENHEIGHT * sizeof(pixel_t));
    job->pixels = pixels;
    pal = W_CacheLumpName(DEH_String("PLAYPAL"), PU_CACHE);
    if (pal)
        memcpy(job->palette, pal, 256 * 3);
    else
        memset(job->palette, 0, sizeof(job->palette));
#ifdef HAVE_LIBPNG
    job->use_png = png_screenshots;
#else
    job->use_png = false;
#endif

    pthread_mutex_lock(&ss_mutex);
    if (!ss_worker_started && !ss_worker_failed)
    {
        pthread_t th;
        if (pthread_create(&th, NULL, ss_worker, NULL) == 0)
        {
            pthread_detach(th);
            ss_worker_started = 1;
        }
        else
            ss_worker_failed = 1;
    }
    if (ss_worker_failed)
    {
        pthread_mutex_unlock(&ss_mutex);
        // Worker never started: synchronous write, old behavior.
        ss_write_job(job);
        free(job->pixels);
        free(job);
        return;
    }
    // Backpressure: block (mutex released while waiting) until a
    // slot frees. Never drop -- a dropped sweep step is a gap.
    while (ss_q_count >= SCREENSHOT_QUEUE_CAP)
        pthread_cond_wait(&ss_not_full, &ss_mutex);
    ss_queue[(ss_q_head + ss_q_count) % SCREENSHOT_QUEUE_CAP] = job;
    ss_q_count++;
    pthread_cond_signal(&ss_not_empty);
    pthread_mutex_unlock(&ss_mutex);
}

// Non-blocking: true only when nothing is queued AND the worker is
// not mid-encode, i.e. every requested file is actually on disk.
int V_ScreenshotQueueEmpty(void)
{
    int empty;
    pthread_mutex_lock(&ss_mutex);
    empty = (ss_q_count == 0 && !ss_worker_busy);
    pthread_mutex_unlock(&ss_mutex);
    return empty;
}

// Blocking: wait until the queue drains. Called by the driver QUIT
// handler before exiting. Returns at once when nothing is queued
// (worker never started, sync fallback, or already drained).
void V_ScreenshotFlush(void)
{
    pthread_mutex_lock(&ss_mutex);
    while (!(ss_q_count == 0 && !ss_worker_busy))
        pthread_cond_wait(&ss_empty, &ss_mutex);
    pthread_mutex_unlock(&ss_mutex);
}

#define MOUSE_SPEED_BOX_WIDTH  120
#define MOUSE_SPEED_BOX_HEIGHT 9
#define MOUSE_SPEED_BOX_X (SCREENWIDTH - MOUSE_SPEED_BOX_WIDTH - 10)
#define MOUSE_SPEED_BOX_Y 15

//
// V_DrawMouseSpeedBox
//

static void DrawAcceleratingBox(int speed)
{
    int red, white, yellow;
    int original_speed;
    int redline_x;
    int linelen;

    red = I_GetPaletteIndex(0xff, 0x00, 0x00);
    white = I_GetPaletteIndex(0xff, 0xff, 0xff);
    yellow = I_GetPaletteIndex(0xff, 0xff, 0x00);

    // Calculate the position of the red threshold line when calibrating
    // acceleration.  This is 1/3 of the way along the box.

    redline_x = MOUSE_SPEED_BOX_WIDTH / 3;

    if (speed >= mouse_threshold)
    {
        // Undo acceleration and get back the original mouse speed
        original_speed = speed - mouse_threshold;
        original_speed = (int) (original_speed / mouse_acceleration);
        original_speed += mouse_threshold;

        linelen = (original_speed * redline_x) / mouse_threshold;
    }
    else
    {
        linelen = (speed * redline_x) / mouse_threshold;
    }

    // Horizontal "thermometer"
    if (linelen > MOUSE_SPEED_BOX_WIDTH - 1)
    {
        linelen = MOUSE_SPEED_BOX_WIDTH - 1;
    }

    if (linelen < redline_x)
    {
        V_DrawHorizLine(MOUSE_SPEED_BOX_X + 1,
                        MOUSE_SPEED_BOX_Y + MOUSE_SPEED_BOX_HEIGHT / 2,
                        linelen, white);
    }
    else
    {
        V_DrawHorizLine(MOUSE_SPEED_BOX_X + 1,
                        MOUSE_SPEED_BOX_Y + MOUSE_SPEED_BOX_HEIGHT / 2,
                        redline_x, white);
        V_DrawHorizLine(MOUSE_SPEED_BOX_X + redline_x,
                        MOUSE_SPEED_BOX_Y + MOUSE_SPEED_BOX_HEIGHT / 2,
                        linelen - redline_x, yellow);
    }

    // Draw acceleration threshold line
    V_DrawVertLine(MOUSE_SPEED_BOX_X + redline_x, MOUSE_SPEED_BOX_Y + 1,
                   MOUSE_SPEED_BOX_HEIGHT - 2, red);
}

// Highest seen mouse turn speed. We scale the range of the thermometer
// according to this value, so that it never exceeds the range. Initially
// this is set to a 1:1 setting where 1 pixel = 1 unit of speed.
static int max_seen_speed = MOUSE_SPEED_BOX_WIDTH - 1;

static void DrawNonAcceleratingBox(int speed)
{
    int white;
    int linelen;

    white = I_GetPaletteIndex(0xff, 0xff, 0xff);

    if (speed > max_seen_speed)
    {
        max_seen_speed = speed;
    }

    // Draw horizontal "thermometer":
    linelen = speed * (MOUSE_SPEED_BOX_WIDTH - 1) / max_seen_speed;

    V_DrawHorizLine(MOUSE_SPEED_BOX_X + 1,
                    MOUSE_SPEED_BOX_Y + MOUSE_SPEED_BOX_HEIGHT / 2,
                    linelen, white);
}

void V_DrawMouseSpeedBox(int speed)
{
    int bgcolor, bordercolor, black;

    // If the mouse is turned off, don't draw the box at all.
    if (!usemouse)
    {
        return;
    }

    // Get palette indices for colors for widget. These depend on the
    // palette of the game being played.

    bgcolor = I_GetPaletteIndex(0x77, 0x77, 0x77);
    bordercolor = I_GetPaletteIndex(0x55, 0x55, 0x55);
    black = I_GetPaletteIndex(0x00, 0x00, 0x00);

    // Calculate box position

    V_DrawFilledBox(MOUSE_SPEED_BOX_X, MOUSE_SPEED_BOX_Y,
                    MOUSE_SPEED_BOX_WIDTH, MOUSE_SPEED_BOX_HEIGHT, bgcolor);
    V_DrawBox(MOUSE_SPEED_BOX_X, MOUSE_SPEED_BOX_Y,
              MOUSE_SPEED_BOX_WIDTH, MOUSE_SPEED_BOX_HEIGHT, bordercolor);
    V_DrawHorizLine(MOUSE_SPEED_BOX_X + 1, MOUSE_SPEED_BOX_Y + 4,
                    MOUSE_SPEED_BOX_WIDTH - 2, black);

    // If acceleration is used, draw a box that helps to calibrate the
    // threshold point.
    if (mouse_threshold > 0 && fabs(mouse_acceleration - 1) > 0.01)
    {
        DrawAcceleratingBox(speed);
    }
    else
    {
        DrawNonAcceleratingBox(speed);
    }
}

