#include "photo.h"
#include <math.h>
#include <stdio.h>
#include <stdint.h>

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))
static const float apertures[] = {1,   1.1, 1.2, 1.4, 1.6, 1.8, 2, 2.2, 2.5, 2.8, 3.2, 3.5, 4,
                                  4.5, 5,   5.6, 6.3, 7.1, 8,   9, 10,  11,  13,  14,  16};
static const float shutters[] = {
    8,          6,           5,           4,           3.2,         2.5,         2,          1.6,
    1.3,        1,           .8,          .6,          .5,          .4,          .3,         .25,
    .2,         1.f / 6,     1.f / 8,     1.f / 10,    1.f / 13,    1.f / 15,    1.f / 20,   1.f / 25,
    1.f / 30,   1.f / 40,    1.f / 50,    1.f / 60,    1.f / 80,    1.f / 100,   1.f / 125,  1.f / 160,
    1.f / 200,  1.f / 250,   1.f / 320,   1.f / 400,   1.f / 500,   1.f / 640,   1.f / 800,  1.f / 1000,
    1.f / 1250, 1.f / 1600,  1.f / 2000,  1.f / 2500,  1.f / 3200,  1.f / 4000,  1.f / 5000, 1.f / 6400,
    1.f / 8000, 1.f / 10000, 1.f / 12500, 1.f / 16000, 1.f / 20000, 1.f / 25000, 1.f / 32000};
static const float isos[] = {100,  125,  160,  200,  250,  320,  400,  500,  640, 800,
                             1000, 1250, 1600, 2000, 2500, 3200, 4000, 5000, 6400};
typedef struct {
    const char *name;
    float sat, contrast, r, g, b;
} Film;
static const Film films[] = {{"Neutral", 1, 1, 1, 1, 1},
                             {"Daylight negative", 1.06, .99, 1.055, 1.01, .94},
                             {"Pastel negative", .76, .89, 1.04, 1, .98},
                             {"Chrome slide", 1.31, 1.13, 1.01, 1.045, 1},
                             {"Color print", 1.17, 1.06, 1.045, .985, 1.01},
                             {"Instant print", .83, .86, 1.05, 1.012, .96},
                             {"Tungsten negative", .88, 1.045, .89, 1.01, 1.14},
                             {"Cross process", 1.16, 1.15, 1.04, 1.065, .91},
                             {"Fine monochrome", 0, .97, 1, 1, 1},
                             {"Silver print", 0, 1.12, 1, 1, 1},
                             {"Hard monochrome", 0, 1.28, 1, 1, 1}};
static const char *modes[] = {"Game", "Manual", "Aperture Priority", "Shutter Priority"};
static const char *labels[] = {"Exposure mode", "Aperture",    "Shutter",     "ISO",
                               "Auto ISO",      "Minimum ISO", "Maximum ISO", "Preferred shutter",
                               "Compensation",  "Film look",   "ISO grain"};
static const int sizes[] = {COUNT(modes),
                            COUNT(apertures),
                            COUNT(shutters),
                            COUNT(isos),
                            2,
                            COUNT(isos),
                            COUNT(isos),
                            COUNT(shutters),
                            31,
                            COUNT(films),
                            2};

void photo_defaults(PhotoSettings *s) {
    *s = (PhotoSettings){{0, 9, 30, 0, 0, 0, 18, 30, 15, 0, 0}, FALSE, -1};
}
static int clampi(int n, int lo, int hi) {
    return n < lo ? lo : n > hi ? hi : n;
}
const char *photo_label(enum PhotoDial d) {
    return labels[d];
}
BOOL photo_enabled(const PhotoSettings *s, enum PhotoDial d) {
    int mode = s->dial[MODE], automatic = s->dial[AUTO_ISO];
    switch (d) {
    case APERTURE:
        return mode != 3;
    case SHUTTER:
        return mode == 1 || mode == 3;
    case ISO:
        return mode ? !automatic : s->dial[GRAIN];
    case AUTO_ISO:
    case COMPENSATION:
        return mode != 0;
    case MIN_ISO:
    case MAX_ISO:
        return mode && automatic;
    case MIN_SHUTTER:
        return mode == 2 && automatic;
    default:
        return TRUE;
    }
}
void photo_adjust(PhotoSettings *s, enum PhotoDial d, int step) {
    if (!photo_enabled(s, d))
        return;
    int value = s->dial[d] + step;
    s->dial[d] = (d == MODE || d == FILM || sizes[d] == 2) ? (value + sizes[d]) % sizes[d]
                                                           : clampi(value, 0, sizes[d] - 1);
    if (d == MIN_ISO && s->dial[d] > s->dial[MAX_ISO])
        s->dial[d] = s->dial[MAX_ISO];
    if (d == MAX_ISO && s->dial[d] < s->dial[MIN_ISO])
        s->dial[d] = s->dial[MIN_ISO];
}
void photo_load(PhotoSettings *s, const WCHAR *ini) {
    photo_defaults(s);
    for (int i = 0; i < PHOTO_DIAL_COUNT; ++i) {
        WCHAR name[20];
        wsprintfW(name, L"Dial%d", i);
        s->dial[i] =
            clampi((int)GetPrivateProfileIntW(L"Photography", name, s->dial[i], ini), 0, sizes[i] - 1);
    }
    if (s->dial[MIN_ISO] > s->dial[MAX_ISO])
        s->dial[MIN_ISO] = s->dial[MAX_ISO];
}
void photo_save(const PhotoSettings *s, const WCHAR *ini) {
    for (int i = 0; i < PHOTO_DIAL_COUNT; ++i) {
        WCHAR name[20], val[12];
        wsprintfW(name, L"Dial%d", i);
        wsprintfW(val, L"%d", s->dial[i]);
        WritePrivateProfileStringW(L"Photography", name, val, ini);
    }
}
static float nearest(const float *table, int first, int last, float value) {
    float best = table[first], distance = 1.e30f;
    for (int i = first; i <= last; i++) {
        float delta = fabsf(log2f(table[i] / fmaxf(1.e-12f, value)));
        if (delta < distance) {
            distance = delta;
            best = table[i];
        }
    }
    return best;
}
Exposure photo_solve(const PhotoSettings *s) {
    const int *d = s->dial;
    int mode = d[MODE], automatic = mode && d[AUTO_ISO];
    float a = apertures[d[APERTURE]], t = shutters[d[SHUTTER]], iso = isos[d[automatic ? MIN_ISO : ISO]];
    float ec = (d[COMPENSATION] - 15) / 3.f;
    BOOL metered = s->luminance > 0;
    float ev = metered ? 10.f + log2f(s->luminance / .18f) : 10.f;
    float light = exp2f(ev - ec);
    if (metered && mode) {
        if (mode == 2) {
            if (automatic) {
                float needed = 100.f * a * a / (light * shutters[d[MIN_SHUTTER]]);
                int i = d[MIN_ISO];
                while (i < d[MAX_ISO] && isos[i] < needed)
                    i++;
                iso = isos[i];
            }
            t = nearest(shutters, 0, COUNT(shutters) - 1, 100.f * a * a / (iso * light));
        } else {
            if (automatic) {
                float lens = mode == 3 ? apertures[0] : a;
                iso = nearest(isos, d[MIN_ISO], d[MAX_ISO], 100.f * lens * lens / (light * t));
            }
            if (mode == 3)
                a = nearest(apertures, 0, COUNT(apertures) - 1, sqrtf(light * t * iso / 100.f));
        }
    }
    float camera_ev = log2f(a * a / t * 100.f / iso), bias = mode == 1 && !automatic ? ec : 0;
    return (Exposure){
        a, t, iso, mode ? exp2f(10.f - camera_ev + bias) : 1, ev - camera_ev + bias - ec, metered && mode};
}
void photo_value(const PhotoSettings *s, enum PhotoDial d, char *out, size_t size) {
    Exposure x = photo_solve(s);
    int n = s->dial[d];
    switch (d) {
    case MODE:
        snprintf(out, size, "%s", modes[n]);
        break;
    case APERTURE:
        snprintf(out, size, "f/%g", (double)x.aperture);
        break;
    case SHUTTER:
    case MIN_SHUTTER: {
        float t = d == SHUTTER ? x.seconds : shutters[n];
        if (t >= .3f)
            snprintf(out, size, "%g s", (double)t);
        else
            snprintf(out, size, "1/%d s", (int)(1.f / t + .5f));
        break;
    }
    case AUTO_ISO:
    case GRAIN:
        snprintf(out, size, "%s", n ? "On" : "Off");
        break;
    case ISO:
    case MIN_ISO:
    case MAX_ISO:
        snprintf(out, size, "%d", d == ISO ? (int)x.iso : (int)isos[n]);
        break;
    case COMPENSATION:
        snprintf(out, size, "%+.1f EV", (n - 15) / 3.0);
        break;
    case FILM:
        snprintf(out, size, "%s", films[n].name);
        break;
    default:
        *out = 0;
    }
}
static float decode(float x) {
    return x <= .04045f ? x / 12.92f : powf((x + .055f) / 1.055f, 2.4f);
}
static float encode(float x) {
    return x <= .0031308f ? 12.92f * x : 1.055f * powf(x, 1.f / 2.4f) - .055f;
}
void photo_develop(const PhotoSettings *s, unsigned char *pixels, unsigned w, unsigned h, unsigned stride,
                   BOOL bgra) {
    if (s->native_grade || (!s->dial[MODE] && !s->dial[FILM] && !s->dial[GRAIN]))
        return;
    const Film *f = &films[s->dial[FILM]];
    Exposure shot = photo_solve(s);
    float lut[256];
    for (int i = 0; i < 256; i++)
        lut[i] = 255.f * encode(decode(i / 255.f) * shot.gain);
    float noise = s->dial[GRAIN] ? 1.5f + log2f(shot.iso / 100.f) * 1.8f : 0;
    uint32_t rng = 971331;
    unsigned r = bgra ? 2 : 0, b = 2 - r;
    for (unsigned y = 0; y < h; y++)
        for (unsigned x = 0; x < w; x++) {
            unsigned char *p = pixels + (size_t)y * stride + 4 * x;
            float red = lut[p[r]], green = lut[p[1]], blue = lut[p[b]];
            float Y = .2126f * red + .7152f * green + .0722f * blue;
            rng = rng * 1664525u + 1013904223u;
            float grain = noise * ((int)(rng >> 16) / 32768.f - 1.f);
            float offset = 127.5f * (1 - f->contrast) + grain + .5f;
            p[r] = clampi((int)((Y + (red - Y) * f->sat) * f->r * f->contrast + offset), 0, 255);
            p[1] = clampi((int)((Y + (green - Y) * f->sat) * f->g * f->contrast + offset), 0, 255);
            p[b] = clampi((int)((Y + (blue - Y) * f->sat) * f->b * f->contrast + offset), 0, 255);
        }
}
float photo_meter_scene(const unsigned char *p, unsigned w, unsigned h, unsigned stride, BOOL bgra) {
    if (!p || !w || !h)
        return -1;
    unsigned sx = w / 48 + 1, sy = h / 32 + 1, count = 0;
    double sum = 0;
    for (unsigned y = 0; y < h; y += sy)
        for (unsigned x = 0; x < w; x += sx) {
            const unsigned char *rgb = p + (size_t)y * stride + x * 4;
            float light = (.2126f * rgb[bgra ? 2 : 0] + .7152f * rgb[1] + .0722f * rgb[bgra ? 0 : 2]) / 255.f;
            sum += logf(fmaxf(light, .0001f));
            count++;
        }
    return expf((float)(sum / count));
}
void photo_grade_options(const PhotoSettings *s, float *contrast, float *sat, float rgb[3], float *noise) {
    const Film *f = &films[s->dial[FILM]];
    *contrast = f->contrast;
    *sat = f->sat;
    rgb[0] = f->r;
    rgb[1] = f->g;
    rgb[2] = f->b;
    *noise = s->dial[GRAIN] ? .065f + .052f * log2f(photo_solve(s).iso / 100.f) : 0;
}
