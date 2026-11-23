#ifndef WITN_FRAMING_H
#define WITN_FRAMING_H

typedef struct {
    int left, top, right, bottom;
} PhotoCropRect;
static const struct {
    const char *name;
    int horizontal, vertical;
} frames[] = {{"Viewport", 0, 0}, {"3:2", 3, 2}, {"4:3", 4, 3},   {"1:1", 1, 1},
              {"16:9", 16, 9},    {"5:4", 5, 4}, {"21:9", 21, 9}, {"65:24", 65, 24},
              {"Spread 89:35", 89, 35}};
enum { FRAME_COUNT = sizeof frames / sizeof *frames, FRAME_MAX_DIMENSION = 16384,
       FRAME_MAX_SCALE = 4, FRAME_TIMED_LONG_EDGE = 3840 };

typedef struct {
    int width, height;
} PhotoSize;

static inline int frame_scale_fits(int viewport_width, int viewport_height, int scale, int timed) {
    if (scale < 1 || scale > FRAME_MAX_SCALE)
        return 0;
    int limit = timed ? FRAME_MAX_DIMENSION : FRAME_MAX_DIMENSION / scale;
    return viewport_width > 0 && viewport_width <= limit && viewport_height > 0 && viewport_height <= limit;
}

/* Scale the saved crop after selecting the gate in physical viewport pixels.
   Timed spread shots retain whole 89:35 units when the accumulator is capped. */
static inline PhotoSize frame_output(int crop_width, int crop_height, int scale, int timed) {
    if (crop_width < 1 || crop_width > FRAME_MAX_DIMENSION || crop_height < 1 ||
        crop_height > FRAME_MAX_DIMENSION || scale < 1 || scale > FRAME_MAX_SCALE)
        return (PhotoSize){0};
    int longest = crop_width > crop_height ? crop_width : crop_height;
    if (timed && longest > FRAME_TIMED_LONG_EDGE) {
        int shortest = crop_width < crop_height ? crop_width : crop_height;
        int cap = FRAME_TIMED_LONG_EDGE;
        if (longest * 35 == shortest * 89)
            cap -= cap % 89;
        crop_width = (crop_width * cap + longest / 2) / longest;
        crop_height = (crop_height * cap + longest / 2) / longest;
        if (crop_width < 1)
            crop_width = 1;
        if (crop_height < 1)
            crop_height = 1;
    }
    return (PhotoSize){crop_width * scale, crop_height * scale};
}

static int gcd(int a, int b) {
    while (b) {
        int c = a % b;
        a = b;
        b = c;
    }
    return a;
}

static inline PhotoCropRect frame_crop(int width, int height, int frame, int portrait) {
    PhotoCropRect r = {0, 0, width, height};
    if (width <= 0 || height <= 0 || frame < 0 || frame >= FRAME_COUNT)
        return r;
    int a = frames[frame].horizontal, b = frames[frame].vertical;
    if (!a) {
        if (!portrait)
            return r;
        int divisor = gcd(width, height);
        a = width / divisor;
        b = height / divisor;
    }
    if (portrait) {
        int swap = a;
        a = b;
        b = swap;
    }
    int sx = width / a, sy = height / b;
    int scale = sx < sy ? sx : sy;
    int w, h;
    if (scale) {
        w = a * scale;
        h = b * scale;
    } else if ((long long)width * b > (long long)height * a) {
        /* A resized window can have a prime dimension; fit to a single pixel. */
        h = height;
        w = (int)((long long)h * a / b);
        if (w < 1)
            w = 1;
    } else {
        w = width;
        h = (int)((long long)w * b / a);
        if (h < 1)
            h = 1;
    }
    r.left = (width - w) / 2;
    r.top = (height - h) / 2;
    r.right = r.left + w;
    r.bottom = r.top + h;
    return r;
}

/* Map the shutter's physical edges to the lower-resolution overlay. Quantizing
   the aspect ratio again in the overlay would change the composition. */
static inline PhotoCropRect frame_preview(int width, int height, int source_w, int source_h, int frame,
                                          int portrait) {
    PhotoCropRect r = frame_crop(source_w, source_h, frame, portrait);
    if (source_w < 1 || source_h < 1 || width < 1 || height < 1)
        return (PhotoCropRect){0};
    r.left = (int)(((long long)r.left * width + source_w / 2) / source_w);
    r.right = (int)(((long long)r.right * width + source_w / 2) / source_w);
    r.top = (int)(((long long)r.top * height + source_h / 2) / source_h);
    r.bottom = (int)(((long long)r.bottom * height + source_h / 2) / source_h);
    return r;
}
#endif
