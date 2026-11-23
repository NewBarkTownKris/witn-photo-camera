#include "print_spool.h"
#include <stdint.h>
#include <string.h>
#include <wchar.h>

static HANDLE store_image(const WCHAR *destination, const void *image, DWORD bytes) {
    WCHAR directory[MAX_PATH], temp[MAX_PATH];
    if (!destination || !image || lstrlenW(destination) >= MAX_PATH)
        return NULL;
    lstrcpyW(directory, destination);
    WCHAR *slash = wcsrchr(directory, L'\\');
    if (!slash)
        return NULL;
    slash[1] = 0;
    if (!GetTempFileNameW(directory, L"WPS", 0, temp))
        return NULL;
    HANDLE file = CreateFileW(temp, GENERIC_READ | GENERIC_WRITE, 0, NULL, TRUNCATE_EXISTING,
                              FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        DeleteFileW(temp);
        return NULL;
    }
    DWORD written = 0;
    LARGE_INTEGER beginning = {0};
    if (!WriteFile(file, image, bytes, &written, NULL) || written != bytes ||
        !SetFilePointerEx(file, beginning, NULL, FILE_BEGIN)) {
        CloseHandle(file);
        return NULL;
    }
    return file;
}

BOOL print_store_display(PrintSpool *s, const WCHAR *path, unsigned phase, unsigned w, unsigned h,
                         unsigned scale, const BYTE *bgra) {
    if (scale < 2 || scale > 4 || phase >= scale * scale || !w || !h || w > 16384 / scale ||
        h > 16384 / scale || s->display[phase])
        return FALSE;
    if (!s->source_row) {
        s->width = w;
        s->height = h;
        s->scale = scale;
        s->source_row = HeapAlloc(GetProcessHeap(), 0, w * 3 * sizeof(float));
    }
    if (!s->source_row || s->width != w || s->height != h || s->scale != scale)
        return FALSE;
    s->display[phase] = store_image(path, bgra, w * h * 4);
    return s->display[phase] != NULL;
}

BOOL print_store_scene(PrintSpool *s, const WCHAR *path, unsigned phase, const BYTE *rgba,
                       const float *sums) {
    if (phase >= s->scale * s->scale || !s->display[phase] || s->scene[phase])
        return FALSE;
    s->float_scene[phase] = sums != NULL;
    s->scene[phase] = store_image(path, sums ? (const void *)sums : rgba,
                                  s->width * s->height * (sums ? 3 * sizeof(float) : 4));
    return s->scene[phase] != NULL;
}

static BOOL read_row(HANDLE file, void *data, DWORD size) {
    DWORD read = 0;
    return file && ReadFile(file, data, size, &read, NULL) && read == size;
}

/* Writers consume rows strictly top to bottom, so each phase file is read
   sequentially. Phase zero covers the top-left sample in each pixel cell. */
BOOL print_png_row(PrintSpool *s, unsigned y, BYTE *bgr) {
    unsigned first = (y % s->scale) * s->scale;
    for (unsigned column = 0; column < s->scale; ++column) {
        if (!read_row(s->display[first + column], s->source_row, s->width * 4))
            return FALSE;
        const BYTE *src = s->source_row;
        BYTE *dst = bgr + column * 3;
        for (unsigned x = 0; x < s->width; ++x, src += 4, dst += 3 * s->scale)
            memcpy(dst, src, 3);
    }
    return TRUE;
}

BOOL print_exr_row(void *context, unsigned y, float *planes, float gain) {
    PrintSpool *s = context;
    unsigned first = (y % s->scale) * s->scale, width = s->width * s->scale;
    for (unsigned column = 0; column < s->scale; ++column) {
        unsigned phase = first + column;
        unsigned pixel_size = s->float_scene[phase] ? 3 * sizeof(float) : 4;
        if (!read_row(s->scene[phase], s->source_row, s->width * pixel_size))
            return FALSE;
        for (unsigned x = 0; x < s->width; ++x) {
            unsigned out = x * s->scale + column;
            const BYTE *p = s->source_row + x * pixel_size;
            if (s->float_scene[phase]) {
                const float *rgb = (const float *)p;
                for (unsigned c = 0; c < 3; ++c)
                    planes[c * width + out] = rgb[2 - c] * gain;
            } else {
                for (unsigned c = 0; c < 3; ++c)
                    planes[c * width + out] = p[2 - c] * gain;
            }
        }
    }
    return TRUE;
}

void print_close(PrintSpool *s) {
    for (unsigned i = 0; i < 16; ++i) {
        if (s->display[i])
            CloseHandle(s->display[i]);
        if (s->scene[i])
            CloseHandle(s->scene[i]);
    }
    if (s->source_row)
        HeapFree(GetProcessHeap(), 0, s->source_row);
    memset(s, 0, sizeof *s);
}
