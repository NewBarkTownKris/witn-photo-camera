/* Run with cc -std=c11 tests/framing.c -o /tmp/witn-framing && /tmp/witn-framing */
#include <assert.h>
#include <string.h>
#include "../src/framing.h"

static void expect(PhotoCropRect rect, int left, int top, int right, int bottom) {
    assert(rect.left == left && rect.top == top);
    assert(rect.right == right && rect.bottom == bottom);
}

static void expect_size(PhotoSize size, int width, int height) {
    assert(size.width == width && size.height == height);
}

int main(void) {
    /* F5 keeps the selected F6 orientation. The UI must describe the same
       width-to-height ratio that the shutter uses in both orientations. */
    const char *expected[][2] = {
        {"Viewport", "Viewport"}, {"3:2", "2:3"}, {"4:3", "3:4"}, {"1:1", "1:1"},
        {"16:9", "9:16"}, {"5:4", "4:5"}, {"21:9", "9:21"}, {"65:24", "24:65"},
        {"Spread 89:35", "Spread 35:89"},
    };
    _Static_assert(sizeof expected / sizeof *expected == FRAME_COUNT, "cover every frame preset");
    for (int frame = 0; frame < FRAME_COUNT; ++frame) {
        for (int portrait = 0; portrait < 2; ++portrait) {
            char label[32];
            frame_label(label, sizeof label, frame, portrait);
            assert(strcmp(label, expected[frame][portrait]) == 0);
            PhotoCropRect crop = frame_crop(3840, 2160, frame, portrait);
            int w = crop.right - crop.left, h = crop.bottom - crop.top;
            assert(frame == 3 ? w == h : (portrait ? h > w : w > h));
        }
    }
    /* 4K 65:24 shutter. Applying the ratio a second time at half resolution
       gives a misleading 3770x1392 guide instead of the real 3835x1416. */
    expect(frame_crop(3840, 2160, 7, 0), 2, 372, 3837, 1788);
    expect(frame_preview(1920, 1080, 3840, 2160, 7, 0), 1, 186, 1919, 894);

    /* Coprime source: the portrait viewport cannot fit even a single exact
       ratio multiple, so its physical crop rounds to the nearest inner pixel. */
    expect(frame_crop(2203, 1237, 0, 1), 754, 0, 1448, 1237);
    expect(frame_preview(1920, 1078, 2203, 1237, 0, 1), 657, 0, 1262, 1078);
    expect(frame_preview(2203, 1237, 2203, 1237, 0, 1), 754, 0, 1448, 1237);

    /* The native-pixel spread gate stays fixed at every print scale. */
    expect(frame_crop(3840, 2160, 8, 0), 6, 327, 3833, 1832);
    expect(frame_preview(1920, 1080, 3840, 2160, 8, 0), 3, 164, 1917, 916);
    expect(frame_crop(3840, 2160, 8, 1), 1500, 12, 2340, 2148);
    for (int scale = 1; scale <= 4; ++scale) {
        expect_size(frame_output(3827, 1505, scale, 0), 3827 * scale, 1505 * scale);
        expect_size(frame_output(840, 2136, scale, 0), 840 * scale, 2136 * scale);
        /* A spread exceeding 3840 keeps the same integer ratio after capping. */
        expect_size(frame_output(7654, 3010, scale, 1), 3827 * scale, 1505 * scale);
        expect_size(frame_output(3010, 7654, scale, 1), 1505 * scale, 3827 * scale);
    }
    expect_size(frame_output(5120, 2133, 4, 1), 15360, 6400);
    expect_size(frame_output(1, 16384, 4, 1), 4, 15360);
    expect_size(frame_output(1920, 1080, 0, 0), 0, 0);
    expect_size(frame_output(0, 1080, 1, 0), 0, 0);
    assert(frame_scale_fits(4096, 2160, 4, 0));
    assert(!frame_scale_fits(4097, 2160, 4, 0));
    assert(!frame_scale_fits(2160, 4097, 4, 0));
    assert(frame_scale_fits(5120, 2880, 4, 1));
    assert(!frame_scale_fits(16385, 2880, 1, 1));
    assert(!frame_scale_fits(0, 2160, 1, 0));
    assert(!frame_scale_fits(2160, 2160, 5, 1));
    return 0;
}
