// d_jpeg.h - Minimal baseline JPEG encoder.
//
// Self-contained: no external libraries beyond libc.
// Used for MJPEG video frames (instant replay feature).
//
// Usage:
//   #include "d_jpeg.h"
//   // rgb_data: width*height*3 bytes of 24-bit RGB
//   int rc = D_WriteJPEG("frame.jpg", rgb, 320, 200, 75);

#ifndef D_JPEG_H
#define D_JPEG_H

#include "doomtype.h"

// Write a baseline DCT JPEG file.
//   filename : output path
//   rgb_data : width*height*3 bytes, 24-bit RGB, row-major
//   width    : image width in pixels
//   height   : image height in pixels
//   quality  : 1-100 (75 recommended)
// Returns 0 on success, non-zero on failure.
int D_WriteJPEG(const char *filename, byte *rgb_data, int width, int height,
                int quality);

#endif // D_JPEG_H
