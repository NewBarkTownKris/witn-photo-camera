#include "exr.h"
#include <stdint.h>
#include <string.h>
#include <wchar.h>

/* Uncompressed, single-part scanline OpenEXR. All supported hosts are little endian. */
typedef struct {
    BYTE bytes[1200];
    size_t used;
} Header;
static void put(Header *h, const void *p, size_t n) {
    memcpy(h->bytes + h->used, p, n);
    h->used += n;
}
static void attr(Header *h, const char *name, const char *type, const void *p, uint32_t n) {
    put(h, name, strlen(name) + 1);
    put(h, type, strlen(type) + 1);
    put(h, &n, 4);
    put(h, p, n);
}
static BOOL write_all(HANDLE f, const void *p, DWORD n) {
    DWORD written = 0;
    return WriteFile(f, p, n, &written, NULL) && written == n;
}
static BOOL write_scene(const WCHAR *png, const PhotoSettings *settings, const BYTE *rgba, const float *sums,
                        unsigned w, unsigned h, unsigned stride, BOOL bgra, unsigned samples, unsigned scale,
                        SceneRow reader, void *context) {
    size_t len = wcslen(png);
    if (len < 5 || len + 5 >= MAX_PATH || !w || !h || w > 16384 || h > 16384 ||
        (!reader && !sums && (!rgba || stride < w * 4)) || !samples)
        return FALSE;
    WCHAR path[MAX_PATH], tmp[MAX_PATH];
    memcpy(path, png, (len + 1) * sizeof(WCHAR));
    if (lstrcmpiW(path + len - 4, L".png"))
        return FALSE;
    lstrcpyW(path + len - 4, L".exr");
    lstrcpyW(tmp, path);
    lstrcatW(tmp, L".tmp");
    HANDLE file = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return FALSE;
    Header header;
    header.used = 0;
    uint32_t prefix[2] = {20000630, 2};
    put(&header, prefix, sizeof prefix);
    BYTE channels[55] = {0};
    for (int i = 0; i < 3; i++) {
        BYTE *channel = channels + 18 * i;
        channel[0] = "BGR"[i];
        channel[2] = 2; /* pixel type FLOAT */
        channel[10] = 1;
        channel[14] = 1;
    }
    attr(&header, "channels", "chlist", channels, sizeof channels);
    BYTE none = 0;
    attr(&header, "compression", "compression", &none, 1);
    int box[4] = {0, 0, (int)w - 1, (int)h - 1};
    attr(&header, "dataWindow", "box2i", box, sizeof box);
    attr(&header, "displayWindow", "box2i", box, sizeof box);
    attr(&header, "lineOrder", "lineOrder", &none, 1);
    float one = 1, center[2] = {0}, chroma[8] = {.640f, .330f, .300f, .600f, .150f, .060f, .3127f, .3290f};
    attr(&header, "pixelAspectRatio", "float", &one, 4);
    attr(&header, "screenWindowCenter", "v2f", center, 8);
    attr(&header, "screenWindowWidth", "float", &one, 4);
    attr(&header, "chromaticities", "chromaticities", chroma, 32);
    static const char space[] = "lin_rec709_scene",
                      source[] = "Snowblind SceneResolveSurface, linear UNORM, 8 bits per channel";
    attr(&header, "colorInteropID", "string", space, sizeof space - 1);
    attr(&header, "witnSource", "string", source, sizeof source - 1);
    char mode[32];
    photo_value(settings, MODE, mode, sizeof mode);
    attr(&header, "witnExposureMode", "string", mode, (uint32_t)strlen(mode));
    Exposure shot = photo_solve(settings);
    if (settings->dial[MODE]) {
        attr(&header, "expTime", "float", &shot.seconds, 4);
        attr(&header, "aperture", "float", &shot.aperture, 4);
        attr(&header, "isoSpeed", "float", &shot.iso, 4);
    }
    if (samples > 1)
        attr(&header, "witnSampleCount", "int", &samples, sizeof samples);
    if (scale > 1)
        attr(&header, "witnCaptureScale", "int", &scale, sizeof scale);
    put(&header, &none, 1);
    DWORD row_bytes = w * 3 * sizeof(float);
    uint64_t *offsets = HeapAlloc(GetProcessHeap(), 0, h * sizeof *offsets);
    float *row = HeapAlloc(GetProcessHeap(), 0, row_bytes);
    BOOL ok = offsets && row && write_all(file, header.bytes, (DWORD)header.used);
    if (ok) {
        for (unsigned y = 0; y < h; y++)
            offsets[y] = header.used + h * 8ull + y * (8ull + row_bytes);
        ok = write_all(file, offsets, h * sizeof *offsets);
    }
    float scene_value[256];
    for (int i = 0; i < 256; i++)
        scene_value[i] = i * (shot.gain / 255.f);
    for (unsigned y = 0; ok && y < h; y++) {
        if (reader) {
            ok = reader(context, y, row, shot.gain / (255.f * samples));
            if (!ok)
                break;
        } else if (sums) {
            float gain = shot.gain / (255.f * samples);
            const float *p = sums + (size_t)y * w * 3;
            for (unsigned x = 0; x < w; ++x, p += 3) {
                row[x] = p[2] * gain;
                row[w + x] = p[1] * gain;
                row[2 * w + x] = p[0] * gain;
            }
        } else {
            const BYTE *p = rgba + (size_t)y * stride;
            for (unsigned x = 0; x < w; x++, p += 4) {
                row[x] = scene_value[p[bgra ? 0 : 2]];
                row[w + x] = scene_value[p[1]];
                row[2 * w + x] = scene_value[p[bgra ? 2 : 0]];
            }
        }
        uint32_t chunk[2] = {y, row_bytes};
        ok = write_all(file, chunk, 8) && write_all(file, row, row_bytes);
    }
    if (row)
        HeapFree(GetProcessHeap(), 0, row);
    if (offsets)
        HeapFree(GetProcessHeap(), 0, offsets);
    if (!CloseHandle(file))
        ok = FALSE;
    if (ok)
        ok = MoveFileW(tmp, path);
    if (!ok)
        DeleteFileW(tmp);
    return ok;
}

BOOL photo_write_exr(const WCHAR *png, const PhotoSettings *settings, const BYTE *rgba, unsigned w,
                     unsigned h, unsigned stride, BOOL bgra) {
    return write_scene(png, settings, rgba, NULL, w, h, stride, bgra, 1, 1, NULL, NULL);
}

BOOL photo_write_exr_sum(const WCHAR *png, const PhotoSettings *settings, const float *rgb_sums, unsigned w,
                         unsigned h, unsigned samples) {
    return write_scene(png, settings, NULL, rgb_sums, w, h, 0, FALSE, samples, 1, NULL, NULL);
}

BOOL photo_write_exr_rows(const WCHAR *png, const PhotoSettings *settings, unsigned w, unsigned h,
                          unsigned samples, unsigned scale, SceneRow reader, void *context) {
    return reader && write_scene(png, settings, NULL, NULL, w, h, 0, FALSE, samples, scale, reader, context);
}
