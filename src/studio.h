#ifndef WITN_STUDIO_H
#define WITN_STUDIO_H

#include <windows.h>
#include <stddef.h>

/* Extra rows precede the finder's composition rows. All callbacks run on
   the caller's game thread. value() writes a NUL-terminated ASCII string. */
typedef struct {
    int count;
    const char *(*label)(int row);
    void (*value)(int row, char *text, size_t size);
    void (*adjust)(int row, int direction);
    BOOL (*enabled)(int row); /* optional, defaults to TRUE */
    BOOL meter_valid;
    float meter_error_ev;
    const char *help; /* optional additional single-line shortcuts */
    BOOL timed_capture; /* selected exposure will use the timed shutter */
} StudioExtra;

void studio_tick(HWND game, BOOL active, BOOL (*pressed)(int), WORD pad_edges, int lens_mm,
                 const char *status, const StudioExtra *extra);
void studio_hide(void); /* hides the overlay when the game loses focus */
BOOL studio_is_open(void);
BOOL studio_finder_visible(void);
int studio_frame(void);
BOOL studio_portrait(void);
int studio_capture_scale(void);
/* Snapshot frame/portrait at shutter press and use the render buffer's actual
   dimensions. The crop is half-open, like a Win32 RECT. */
RECT studio_crop(int width, int height, int frame, BOOL portrait);
/* Render-thread overlay. Upload only when this call returns TRUE. Reset
   revision to zero after allocating a fresh texture or changing its size. */
BOOL studio_overlay_active(void);
BOOL studio_overlay_bitmap(unsigned width, unsigned height, unsigned source_width, unsigned source_height,
                           unsigned *revision, BYTE *rgba, unsigned stride);
void studio_renderer_ready(BOOL ready);
#endif
