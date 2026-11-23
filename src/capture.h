#ifndef WITN_CAPTURE_H
#define WITN_CAPTURE_H

#include <windows.h>
#include <stdint.h>
#include "photo.h"
#include "pose.h"

BOOL capture_install(unsigned char *base, BOOL (*redirect)(unsigned char *, unsigned char *, uintptr_t),
                     void (*log)(const char *));
BOOL capture_request(const WCHAR *path, int frame, BOOL portrait, BOOL want_exr,
                     const PhotoSettings *settings, float seconds, unsigned scale);
void capture_print_camera(Camera *camera);
BOOL capture_print_progress(unsigned *done, unsigned *total);
void capture_cancel(void);
BOOL capture_busy(void);
/* 0 while pending, 1 PNG saved, 2 PNG + EXR, 3 PNG saved without requested EXR,
   -1 error, -2 canceled. Keeps its value until the next accepted shutter press. */
int capture_result(void);
uint32_t capture_meter(int frame, BOOL portrait);
uint32_t capture_meter_result(float *luminance);
float capture_luminance(void);

#endif
