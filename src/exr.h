#ifndef WITN_EXR_H
#define WITN_EXR_H
#include "photo.h"
BOOL photo_write_exr(const WCHAR *png_path, const PhotoSettings *settings, const BYTE *scene_rgba, unsigned w,
                     unsigned h, unsigned stride, BOOL bgra);
/* RGB sums in source code units (0–255 per sample), before photographic gain. */
BOOL photo_write_exr_sum(const WCHAR *png_path, const PhotoSettings *settings, const float *rgb_sums,
                         unsigned w, unsigned h, unsigned samples);
/* The callback supplies planar B,G,R in scene units after multiplication
   by gain. Returning FALSE aborts the unpublished file. */
typedef BOOL (*SceneRow)(void *context, unsigned y, float *planes, float gain);
BOOL photo_write_exr_rows(const WCHAR *png, const PhotoSettings *settings, unsigned w, unsigned h,
                          unsigned samples, unsigned scale, SceneRow reader, void *context);
#endif
