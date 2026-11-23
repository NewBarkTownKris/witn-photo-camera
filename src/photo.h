#ifndef WITN_PHOTO_H
#define WITN_PHOTO_H
#include <windows.h>
#include <stddef.h>

enum PhotoDial {
    MODE,
    APERTURE,
    SHUTTER,
    ISO,
    AUTO_ISO,
    MIN_ISO,
    MAX_ISO,
    MIN_SHUTTER,
    COMPENSATION,
    FILM,
    GRAIN,
    PHOTO_DIAL_COUNT
};
typedef struct {
    int dial[PHOTO_DIAL_COUNT]; /* indices in the photographic dial tables */
    BOOL native_grade;          /* the PNG already contains the live grade */
    float luminance;            /* reading locked at shutter release */
} PhotoSettings;

typedef struct {
    float aperture, seconds, iso, gain, error_ev;
    BOOL metered;
} Exposure;

void photo_defaults(PhotoSettings *settings);
void photo_adjust(PhotoSettings *settings, enum PhotoDial dial, int direction);
BOOL photo_enabled(const PhotoSettings *settings, enum PhotoDial dial);
const char *photo_label(enum PhotoDial dial);
void photo_value(const PhotoSettings *settings, enum PhotoDial dial, char *out, size_t size);
Exposure photo_solve(const PhotoSettings *settings);
void photo_load(PhotoSettings *settings, const WCHAR *ini);
void photo_save(const PhotoSettings *settings, const WCHAR *ini);
void photo_develop(const PhotoSettings *settings, unsigned char *pixels, unsigned width, unsigned height,
                   unsigned stride, BOOL bgra);
float photo_meter_scene(const unsigned char *pixels, unsigned w, unsigned h, unsigned stride, BOOL bgra);
void photo_grade_options(const PhotoSettings *s, float *contrast, float *sat, float rgb[3], float *noise);
#endif
