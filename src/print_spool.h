#ifndef WITN_PRINT_SPOOL_H
#define WITN_PRINT_SPOOL_H

#include <windows.h>

/* One native-resolution image per spatial phase. Temporary files are removed
   by the OS even if the game exits during a print. */
typedef struct {
    HANDLE display[16], scene[16];
    BOOL float_scene[16];
    unsigned width, height, scale;
    BYTE *source_row;
} PrintSpool;

BOOL print_store_display(PrintSpool *spool, const WCHAR *destination, unsigned phase, unsigned width,
                         unsigned height, unsigned scale, const BYTE *bgra);
BOOL print_store_scene(PrintSpool *spool, const WCHAR *destination, unsigned phase, const BYTE *rgba,
                       const float *rgb_sums);
BOOL print_png_row(PrintSpool *spool, unsigned y, BYTE *bgr);
BOOL print_exr_row(void *context, unsigned y, float *planes, float gain);
void print_close(PrintSpool *spool);

#endif
