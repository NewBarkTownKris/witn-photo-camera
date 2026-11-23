/* Windows: compile this fixture together with src/print_spool.c. */
#include "../src/print_spool.h"
#include <assert.h>
#include <string.h>

enum { WIDTH = 5, HEIGHT = 3, MAX_SCALE = 4 };

static void check_spool(const WCHAR *path, unsigned scale) {
    PrintSpool spool = {0};
    BYTE rgba[WIDTH * HEIGHT * 4], bgra[sizeof rgba];
    float sums[WIDTH * HEIGHT * 3];
    for (unsigned phase = 0; phase < scale * scale; ++phase) {
        for (unsigned i = 0; i < WIDTH * HEIGHT; ++i) {
            BYTE r = 128 + phase, g = 32 + i, b = 1 + phase + i;
            memcpy(&rgba[i * 4], (BYTE[]){r, g, b, 255}, 4);
            memcpy(&bgra[i * 4], (BYTE[]){b, g, r, 255}, 4);
            for (unsigned c = 0; c < 3; ++c)
                sums[i * 3 + c] = rgba[i * 4 + c] + .25f;
        }
        assert(print_store_display(&spool, path, phase, WIDTH, HEIGHT, scale, bgra));
        assert(!print_store_display(&spool, path, phase, WIDTH, HEIGHT, scale, bgra));
        assert(print_store_scene(&spool, path, phase, rgba, phase % 2 ? sums : NULL));
    }
    for (unsigned y = 0; y < HEIGHT * scale; ++y) {
        BYTE bgr[WIDTH * MAX_SCALE * 3];
        float planes[WIDTH * MAX_SCALE * 3];
        assert(print_png_row(&spool, y, bgr));
        assert(print_exr_row(&spool, y, planes, .5f));
        for (unsigned x = 0; x < WIDTH * scale; ++x) {
            unsigned phase = x % scale + (y % scale) * scale;
            unsigned i = x / scale + (y / scale) * WIDTH;
            BYTE expected[] = {1 + phase + i, 32 + i, 128 + phase};
            assert(!memcmp(bgr + x * 3, expected, 3));
            for (unsigned c = 0; c < 3; ++c)
                assert(planes[c * WIDTH * scale + x] == (expected[c] + (phase % 2 ? .25f : 0)) * .5f);
        }
    }
    print_close(&spool);
    assert(!spool.source_row && !spool.display[0] && !spool.scene[0]);
}

int main(void) {
    WCHAR path[MAX_PATH];
    DWORD length = GetTempPathW(MAX_PATH, path);
    assert(length && length + 12 < MAX_PATH);
    lstrcatW(path, L"fixture.png");
    for (unsigned scale = 2; scale <= MAX_SCALE; ++scale)
        check_spool(path, scale);
    return 0;
}
