#define WIN32_LEAN_AND_MEAN
#include "studio.h"
#include "folder.h"
#include "framing.h"
#include <xinput.h>
#include <string.h>

#define SHOW_ROWS 9
#define KEY_COLOR RGB(255, 0, 255)
#define PANEL RGB(21, 28, 32)
#define ACCENT RGB(238, 187, 103)
#define WHITE_TEXT RGB(242, 244, 242)
#define SECONDARY RGB(190, 201, 205)

static const char *const reticles[] = {"Optical", "Square", "Circle", "Off"};
enum { COMP_FRAME, COMP_ORIENTATION, COMP_SCALE, COMP_RETICLE, COMP_GRID, COMP_COUNT };
static const char *const composition[COMP_COUNT] = {"Frame", "Orientation", "Capture scale", "Reticle", "Thirds grid"};
static struct {
    int frame, portrait, scale, reticle, grid;
} settings = {.scale = 1};
static int selected;
static BOOL panel, help_visible, finder = TRUE, loaded;
static const WCHAR *ini;

static int smaller(int a, int b) {
    return a < b ? a : b;
}
static int larger(int a, int b) {
    return a > b ? a : b;
}
RECT studio_crop(int width, int height, int frame, BOOL portrait) {
    PhotoCropRect r = frame_crop(width, height, frame, portrait);
    return (RECT){r.left, r.top, r.right, r.bottom};
}

static void load_settings(void) {
    loaded = TRUE;
    ini = folder_ini();
    if (!ini[0])
        return;
    settings.frame = GetPrivateProfileIntW(L"Finder", L"Frame", 0, ini);
    settings.portrait = GetPrivateProfileIntW(L"Finder", L"Portrait", 0, ini) == 1;
    settings.scale = GetPrivateProfileIntW(L"Finder", L"CaptureScale", 1, ini);
    settings.reticle = GetPrivateProfileIntW(L"Finder", L"Reticle", 0, ini);
    settings.grid = GetPrivateProfileIntW(L"Finder", L"Grid", 0, ini) == 1;
    if (settings.frame < 0 || settings.frame >= FRAME_COUNT)
        settings.frame = 0;
    if (settings.scale < 1 || settings.scale > FRAME_MAX_SCALE)
        settings.scale = 1;
    if (settings.reticle < 0 || settings.reticle >= 4)
        settings.reticle = 0;
}

static void change_composition(int row, int direction) {
    static const WCHAR *const keys[] = {L"Frame", L"Portrait", L"CaptureScale", L"Reticle", L"Grid"};
    int *fields[] = {&settings.frame, &settings.portrait, &settings.scale, &settings.reticle, &settings.grid};
    static const int counts[] = {FRAME_COUNT, 2, FRAME_MAX_SCALE, 4, 2};
    int count = counts[row];
    int value = *fields[row] + direction;
    value = row == COMP_SCALE ? smaller(count, larger(1, value)) : (value + count) % count;
    if (value == *fields[row])
        return;
    *fields[row] = value;
    WCHAR digit[2] = {(WCHAR)(L'0' + value), 0};
    if (ini[0])
        WritePrivateProfileStringW(L"Finder", keys[row], digit, ini);
}

BOOL studio_is_open(void) {
    return panel;
}
BOOL studio_finder_visible(void) {
    return finder;
}
int studio_frame(void) {
    return settings.frame;
}
BOOL studio_portrait(void) {
    return settings.portrait;
}
int studio_capture_scale(void) {
    return settings.scale;
}

typedef struct {
    char label[42], value[66];
    BOOL enabled;
} Row;

typedef struct {
    HWND game;
    int frame, reticle, lens, selected, first, total, capture_scale;
    BOOL portrait, grid, panel, help, meter_valid, timed_capture;
    int meter_thirds;
    char status[224], extra_help[160];
    Row rows[SHOW_ROWS];
} Display;

static SRWLOCK lock = SRWLOCK_INIT;
static SRWLOCK paint_lock = SRWLOCK_INIT;
static Display published, drawn;
static unsigned generation;
static LONG worker_started, gpu_active;
static HWND overlay;
static HDC canvas;
static HBITMAP bitmap;
static DWORD *dib_pixels;
static int canvas_w, canvas_h;
static HFONT body_font, title_font;
static int font_scale;
static HINSTANCE module;

void studio_hide(void) {
    AcquireSRWLockExclusive(&lock);
    if (published.game) {
        published.game = NULL;
        generation++;
    }
    ReleaseSRWLockExclusive(&lock);
}

static int scaled(int size) {
    return MulDiv(size, font_scale, 100);
}

void studio_renderer_ready(BOOL ready) {
    InterlockedExchange(&gpu_active, !!ready);
}

BOOL studio_overlay_active(void) {
    AcquireSRWLockShared(&lock);
    HWND game = published.game;
    ReleaseSRWLockShared(&lock);
    return game && GetForegroundWindow() == game && !IsIconic(game);
}

static void prepare_canvas(HDC screen, int w, int h) {
    if (!canvas)
        canvas = CreateCompatibleDC(screen);
    if (canvas && (w != canvas_w || h != canvas_h || !bitmap)) {
        BITMAPINFO info = {0};
        info.bmiHeader.biSize = sizeof info.bmiHeader;
        info.bmiHeader.biWidth = w;
        info.bmiHeader.biHeight = -h;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void *pixels;
        HBITMAP next = CreateDIBSection(canvas, &info, DIB_RGB_COLORS, &pixels, NULL, 0);
        if (next) {
            HBITMAP prior = SelectObject(canvas, next);
            if (bitmap)
                DeleteObject(prior);
            bitmap = next;
            canvas_w = w;
            canvas_h = h;
            dib_pixels = pixels;
        }
    }
    int scale = larger(60, smaller(270, h * 100 / 860));
    if (scale != font_scale || !body_font || !title_font) {
        font_scale = scale;
        if (body_font)
            DeleteObject(body_font);
        if (title_font)
            DeleteObject(title_font);
        body_font = CreateFontW(-scaled(16), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        title_font =
            CreateFontW(-scaled(21), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                        CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    }
}

static void fill(int left, int top, int right, int bottom, COLORREF color) {
    if (right <= left || bottom <= top)
        return;
    RECT r = {left, top, right, bottom};
    SetDCBrushColor(canvas, color);
    FillRect(canvas, &r, GetStockObject(DC_BRUSH));
}

static void stroke(int x1, int y1, int x2, int y2) {
    MoveToEx(canvas, x1, y1, NULL);
    LineTo(canvas, x2, y2);
}

static void brackets(RECT r, int length) {
    for (int y = 0; y < 2; y++)
        for (int x = 0; x < 2; x++) {
            int xx = x ? r.right - 1 : r.left, yy = y ? r.bottom - 1 : r.top;
            stroke(xx + (x ? -length : length), yy, xx, yy);
            stroke(xx, yy, xx, yy + (y ? -length : length));
        }
}

static void ring(int x, int y, int radius, BOOL gaps) {
    if (!gaps) {
        Ellipse(canvas, x - radius, y - radius, x + radius + 1, y + radius + 1);
        return;
    }
    int gap = larger(2, radius / 10);
    Arc(canvas, x - radius, y - radius, x + radius, y + radius, x + radius, y - gap, x + gap, y - radius);
    Arc(canvas, x - radius, y - radius, x + radius, y + radius, x - gap, y - radius, x - radius, y - gap);
    Arc(canvas, x - radius, y - radius, x + radius, y + radius, x - radius, y + gap, x - gap, y + radius);
    Arc(canvas, x - radius, y - radius, x + radius, y + radius, x + gap, y + radius, x + radius, y + gap);
}

static void guides(RECT r) {
    int w = r.right - r.left, h = r.bottom - r.top, shorter = smaller(w, h);
    int cx = r.left + w / 2, cy = r.top + h / 2;
    for (int pass = 0; pass < 2; pass++) {
        HPEN pen =
            CreatePen(PS_SOLID, larger(1, scaled(pass ? 1 : 3)), pass ? RGB(218, 224, 226) : RGB(45, 49, 48));
        HGDIOBJ old_pen = SelectObject(canvas, pen),
                old_brush = SelectObject(canvas, GetStockObject(NULL_BRUSH));
        if (drawn.grid)
            for (int i = 1; i < 3; i++) {
                stroke(r.left + w * i / 3, r.top, r.left + w * i / 3, r.bottom);
                stroke(r.left, r.top + h * i / 3, r.right, r.top + h * i / 3);
            }
        if (drawn.reticle == 0) {
            RECT border = {r.left + scaled(3), r.top + scaled(3), r.right - scaled(3), r.bottom - scaled(3)};
            brackets(border, shorter / 28);
            ring(cx, cy, larger(5, shorter / 10), TRUE);
            SetPixel(canvas, cx, cy, WHITE_TEXT);
        }
        if (drawn.reticle < 2) {
            int radius = larger(3, shorter / (drawn.reticle ? 14 : 42));
            RECT center = {cx - radius, cy - radius * 3 / 4, cx + radius + 1, cy + radius * 3 / 4 + 1};
            brackets(center, larger(2, radius * 2 / 5));
        }
        if (drawn.reticle == 2) {
            ring(cx, cy, larger(4, shorter * 3 / 25), FALSE);
            ring(cx, cy, larger(3, shorter / 11), FALSE);
        }
        SelectObject(canvas, old_pen);
        SelectObject(canvas, old_brush);
        DeleteObject(pen);
    }
    if (drawn.meter_valid && !drawn.panel) {
        int spacing = larger(2, smaller(scaled(10), w / 40)), y = r.top + scaled(18);
        fill(cx - 10 * spacing, y - scaled(12), cx + 10 * spacing + 1, y + scaled(9), PANEL);
        for (int i = -9; i <= 9; i++) {
            int height = scaled(i == 0 ? 10 : (i % 3 ? 4 : 7)), x = cx + i * spacing;
            fill(x, y - height, x + larger(1, scaled(1)), y + 1, WHITE_TEXT);
        }
        int marker = cx + spacing * drawn.meter_thirds;
        fill(marker - scaled(2), y + scaled(2), marker + scaled(2) + 1, y + scaled(6), ACCENT);
    }
}

static void line(const char *text, int x, int y, int right, int height, COLORREF color, BOOL title,
                 BOOL align_right) {
    RECT r = {x, y, right, y + height};
    HGDIOBJ old = SelectObject(canvas, title ? title_font : body_font);
    SetBkMode(canvas, TRANSPARENT);
    SetTextColor(canvas, color);
    DrawTextA(canvas, text, -1, &r,
              DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX |
                  (align_right ? DT_RIGHT : DT_LEFT));
    SelectObject(canvas, old);
}

static void panel_ui(int w, int h, int source_w, int source_h) {
    int margin = scaled(15), pad = scaled(14), row_h = scaled(25);
    int left = margin, top = margin, right = smaller(w - margin, left + scaled(386));
    int count = smaller(SHOW_ROWS, drawn.total - drawn.first);
    int header = scaled(45), footer = scaled(34);
    int bottom = smaller(h - margin, top + header + count * row_h + footer);
    fill(left, top, right, bottom, PANEL);
    fill(left, top, left + scaled(3), top + header, ACCENT);
    line("Photo Studio", left + pad, top, right - pad, header, WHITE_TEXT, TRUE, FALSE);
    int mid = left + (right - left) * 51 / 100;
    for (int i = 0; i < count; i++) {
        int y = top + header + i * row_h;
        Row *row = drawn.rows + i;
        if (drawn.first + i == drawn.selected)
            fill(left + scaled(4), y, right - scaled(4), y + row_h, RGB(66, 59, 46));
        line(row->label, left + pad, y, mid, row_h, row->enabled ? WHITE_TEXT : SECONDARY, FALSE, FALSE);
        const char *value = row->value;
        char scale_text[42];
        if (drawn.first + i == drawn.total - COMP_COUNT + COMP_SCALE) {
            if (frame_scale_fits(source_w, source_h, drawn.capture_scale, drawn.timed_capture)) {
                PhotoCropRect crop = frame_crop(source_w, source_h, drawn.frame, drawn.portrait);
                PhotoSize output = frame_output(crop.right - crop.left, crop.bottom - crop.top,
                                                drawn.capture_scale, drawn.timed_capture);
                wsprintfA(scale_text, "%dx  %dx%d", drawn.capture_scale, output.width, output.height);
            } else {
                wsprintfA(scale_text, "%dx  exceeds %d px", drawn.capture_scale, FRAME_MAX_DIMENSION);
            }
            value = scale_text;
        }
        line(value, mid, y, right - pad, row_h, row->enabled ? WHITE_TEXT : SECONDARY, FALSE, TRUE);
    }
    char footer_text[96];
    wsprintfA(footer_text, "Arrows / D-pad  Adjust     F4  Close     %d / %d", drawn.selected + 1,
              drawn.total);
    line(footer_text, left + pad, bottom - footer, right - pad, footer, SECONDARY, FALSE, FALSE);
}

static void finder_ui(int w, int h) {
    static const char *const help_rows[] = {
        "WASD  Move     Q / E  Lower / rise     Mouse or arrows  Aim",
        "Z / X  Roll     F10  Level     Shift / Ctrl  Fast / fine",
        "F5  Frame     F6  Rotate frame     F7  Lens     [ / ]  Zoom",
        "H  Finder     G  Thirds grid     R  Reticle     F11  Game HUD",
        "Left-click / pad A  PNG     Pad Y  PNG + EXR     Middle-click / RS  Focus",
        "L  Fine focus     Y  DOF strength     Wheel  Focus distance",
        "Shift / Ctrl + wheel  Aperture     Alt + wheel  Shutter     Ctrl + Alt + wheel  ISO"};
    int margin = scaled(15), pad = scaled(12), rh = scaled(23), bottom = h - margin;
    int extra = drawn.help ? (int)(sizeof help_rows / sizeof *help_rows) + (drawn.extra_help[0] != 0) : 0;
    int row_count = 2 + extra + (drawn.status[0] != 0);
    int top = bottom - row_count * rh - 2 * scaled(5), right = smaller(w - margin, margin + scaled(660));
    fill(margin, top, right, bottom, PANEL);
    fill(margin, top, margin + scaled(3), bottom, ACCENT);
    char title[100], frame_text[32];
    frame_label(frame_text, sizeof frame_text, drawn.frame, drawn.portrait);
    wsprintfA(title, "%d mm     %s   %s     F6 Rotate", drawn.lens, frame_text,
              drawn.portrait ? "Portrait" : "Landscape");
    int y = top + scaled(5);
    line(title, margin + pad, y, right - pad, rh, WHITE_TEXT, TRUE, FALSE);
    if (drawn.status[0]) {
        y += rh;
        line(drawn.status, margin + pad, y, right - pad, rh, WHITE_TEXT, FALSE, FALSE);
    }
    y += rh;
    line("F9  PNG     F3  PNG + EXR     F4  Studio     F2  Folder     F1  Controls     F8  Resume",
         margin + pad, y, right - pad, rh, ACCENT, FALSE, FALSE);
    if (drawn.help) {
        for (unsigned i = 0; i < sizeof help_rows / sizeof *help_rows; i++) {
            y += rh;
            line(help_rows[i], margin + pad, y, right - pad, rh, SECONDARY, FALSE, FALSE);
        }
        if (drawn.extra_help[0]) {
            y += rh;
            line(drawn.extra_help, margin + pad, y, right - pad, rh, SECONDARY, FALSE, FALSE);
        }
    }
}

/* Both presentation paths use the same DIB. Holding paint_lock covers its
   allocation, every GDI operation, and consumption by BitBlt or the GPU. */
static BOOL draw_canvas(HDC dc, int w, int h, int source_w, int source_h) {
    prepare_canvas(dc, w, h);
    if (!canvas || !bitmap || canvas_w != w || canvas_h != h)
        return FALSE;
    PhotoCropRect edges = frame_preview(w, h, source_w, source_h, drawn.frame, drawn.portrait);
    RECT crop = {edges.left, edges.top, edges.right, edges.bottom};
    fill(0, 0, w, h, RGB(12, 15, 16));
    fill(crop.left, crop.top, crop.right, crop.bottom, KEY_COLOR);
    guides(crop);
    if (drawn.panel)
        panel_ui(w, h, source_w, source_h);
    else
        finder_ui(w, h);
    return TRUE;
}

BOOL studio_overlay_bitmap(unsigned width, unsigned height, unsigned source_w, unsigned source_h,
                           unsigned *revision, BYTE *rgba, unsigned stride) {
    static unsigned last_source_w, last_source_h;
    if (!revision || !rgba || !width || !height || !source_w || !source_h || width > 16384 ||
        height > 16384 || source_w > 16384 || source_h > 16384 || stride < width * 4)
        return FALSE;
    AcquireSRWLockExclusive(&paint_lock);
    AcquireSRWLockShared(&lock);
    unsigned next = generation;
    BOOL changed =
        published.game && (next != *revision || last_source_w != source_w || last_source_h != source_h);
    if (changed)
        drawn = published;
    ReleaseSRWLockShared(&lock);
    if (changed)
        changed = draw_canvas(NULL, (int)width, (int)height, (int)source_w, (int)source_h);
    if (changed) {
        GdiFlush();
        for (unsigned y = 0; y < height; y++) {
            DWORD *dst = (DWORD *)(rgba + (size_t)y * stride);
            const DWORD *src = dib_pixels + (size_t)y * width;
            for (unsigned x = 0; x < width; x++) {
                DWORD bgr = src[x] & 0xffffff;
                dst[x] =
                    bgr == 0xff00ff ? 0 : (0xff000000 | (bgr >> 16) | (bgr & 0xff00) | ((bgr & 0xff) << 16));
            }
        }
        last_source_w = source_w;
        last_source_h = source_h;
        *revision = next;
    }
    ReleaseSRWLockExclusive(&paint_lock);
    return changed;
}

static void paint(HWND window) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(window, &ps);
    RECT r;
    if (dc && GetClientRect(window, &r) && r.right > 0 && r.bottom > 0) {
        int w = r.right, h = r.bottom;
        AcquireSRWLockExclusive(&paint_lock);
        AcquireSRWLockShared(&lock);
        drawn = published;
        ReleaseSRWLockShared(&lock);
        if (draw_canvas(dc, w, h, w, h))
            BitBlt(dc, 0, 0, w, h, canvas, 0, 0, SRCCOPY);
        ReleaseSRWLockExclusive(&paint_lock);
    }
    EndPaint(window, &ps);
}

static void refresh(HWND window) {
    static unsigned last;
    static RECT previous;
    static BOOL visible;
    AcquireSRWLockShared(&lock);
    BOOL changed = last != generation;
    last = generation;
    HWND game = published.game;
    ReleaseSRWLockShared(&lock);
    RECT area = {0};
    if (InterlockedCompareExchange(&gpu_active, 0, 0) || !game || GetForegroundWindow() != game ||
        IsIconic(game) || !GetClientRect(game, &area) || !area.right || !area.bottom) {
        if (visible) {
            ShowWindow(window, SW_HIDE);
            visible = FALSE;
        }
        return;
    }
    POINT origin = {0};
    if (!ClientToScreen(game, &origin))
        return;
    OffsetRect(&area, origin.x, origin.y);
    if (!visible || !EqualRect(&area, &previous)) {
        SetWindowPos(window, HWND_TOPMOST, area.left, area.top, area.right - area.left,
                     area.bottom - area.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        previous = area;
        visible = TRUE;
        changed = TRUE;
    }
    if (changed)
        InvalidateRect(window, NULL, FALSE);
}

static LRESULT CALLBACK overlay_proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_TIMER:
        refresh(window);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint(window);
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}

static DWORD WINAPI worker(void *unused) {
    (void)unused;
    static const WCHAR classname[] = L"WitnPhotoFinderWnd";
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = overlay_proc;
    wc.hInstance = module;
    wc.lpszClassName = classname;
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return 0;
    overlay = CreateWindowExW(
        WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT, classname,
        L"War in the North Photo Finder", WS_POPUP, 0, 0, 1, 1, NULL, NULL, module, NULL);
    if (!overlay)
        return 0;
    if (!SetLayeredWindowAttributes(overlay, KEY_COLOR, 255, LWA_COLORKEY)) {
        DestroyWindow(overlay);
        return 0;
    }
    SetTimer(overlay, 1, 40, NULL);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    /* The rendering thread also owns this canvas, for the lifetime of the
       DLL. The OS may destroy the fallback window during display changes. */
    return 0;
}

static void start_worker(void) {
    if (InterlockedCompareExchange(&worker_started, 1, 0))
        return;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)(void *)&start_worker, &module);
    HANDLE thread = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    if (thread)
        CloseHandle(thread);
    else
        InterlockedExchange(&worker_started, 0);
}

void studio_tick(HWND game, BOOL active, BOOL (*pressed)(int), WORD pad_edges, int lens_mm,
                 const char *status, const StudioExtra *extra) {
    static ULONGLONG fallback_after;
    if (!active) {
        panel = FALSE;
        fallback_after = 0;
        studio_hide();
        return;
    }
    if (!game) {
        studio_hide();
        return;
    }
    if (!loaded)
        load_settings();
    if (pressed(VK_F4) || (pad_edges & XINPUT_GAMEPAD_LEFT_THUMB))
        panel = !panel;
    if (pressed(VK_F1)) {
        help_visible = !help_visible;
        finder = TRUE;
    }
    if (pressed('H')) {
        finder = !finder;
        if (!finder)
            panel = FALSE;
    }
    if (pressed('G'))
        change_composition(COMP_GRID, 1);
    if (pressed('R'))
        change_composition(COMP_RETICLE, 1);
    if (pressed(VK_F5))
        change_composition(COMP_FRAME, 1);
    if (pressed(VK_F6))
        change_composition(COMP_ORIENTATION, 1);
    if (!finder && !panel) {
        studio_hide();
        return;
    }

    int n = extra ? larger(0, extra->count) : 0, total = n + COMP_COUNT;
    if (selected >= total)
        selected = 0;
    if (panel) {
        BOOL up = pressed(VK_UP), down = pressed(VK_DOWN), left = pressed(VK_LEFT), right = pressed(VK_RIGHT);
        int step =
            (down || (pad_edges & XINPUT_GAMEPAD_DPAD_DOWN)) - (up || (pad_edges & XINPUT_GAMEPAD_DPAD_UP));
        selected = (selected + total + step) % total;
        int dial = (right || (pad_edges & XINPUT_GAMEPAD_DPAD_RIGHT)) -
                   (left || (pad_edges & XINPUT_GAMEPAD_DPAD_LEFT));
        if (dial) {
            if (selected >= n)
                change_composition(selected - n, dial);
            else if (extra->adjust && (!extra->enabled || extra->enabled(selected)))
                extra->adjust(selected, dial);
        }
    }
    Display next = {0};
    next.game = game;
    next.frame = settings.frame;
    next.portrait = settings.portrait;
    next.capture_scale = settings.scale;
    next.timed_capture = extra && extra->timed_capture;
    next.reticle = settings.reticle;
    next.grid = settings.grid;
    next.panel = panel;
    next.help = help_visible;
    next.lens = lens_mm;
    next.selected = selected;
    next.total = total;
    if (extra && extra->meter_valid) {
        float ev = extra->meter_error_ev;
        if (ev >= -100 && ev <= 100) {
            next.meter_valid = TRUE;
            next.meter_thirds = smaller(9, larger(-9, (int)(ev * 3 + (ev < 0 ? -0.5f : 0.5f))));
        }
    }
    if (status)
        lstrcpynA(next.status, status, sizeof next.status);
    if (extra && extra->help)
        lstrcpynA(next.extra_help, extra->help, sizeof next.extra_help);
    if (panel) {
        char frame_text[32];
        frame_label(frame_text, sizeof frame_text, settings.frame, settings.portrait);
        next.first = larger(0, smaller(total - SHOW_ROWS, selected - SHOW_ROWS / 2));
        for (int i = 0; i < SHOW_ROWS && next.first + i < total; i++) {
            Row *row = next.rows + i;
            int index = next.first + i;
            if (index < n) {
                const char *label = extra->label ? extra->label(index) : NULL;
                if (label)
                    lstrcpynA(row->label, label, sizeof row->label);
                if (extra->value)
                    extra->value(index, row->value, sizeof row->value);
                row->value[sizeof row->value - 1] = 0;
                row->enabled = !extra->enabled || extra->enabled(index);
            } else {
                int key = index - n;
                lstrcpynA(row->label, composition[key], sizeof row->label);
                const char *values[] = {frame_text,
                                        settings.portrait ? "Portrait" : "Landscape", "",
                                        reticles[settings.reticle], settings.grid ? "On" : "Off"};
                lstrcpynA(row->value, values[key], sizeof row->value);
                row->enabled = TRUE;
            }
        }
    }
    AcquireSRWLockExclusive(&lock);
    if (memcmp(&published, &next, sizeof next)) {
        published = next;
        generation++;
    }
    ReleaseSRWLockExclusive(&lock);
    /* Give the render thread its first Present before opening a native popup.
       An exclusive fullscreen swap chain can relinquish focus to a popup. */
    if (!InterlockedCompareExchange(&gpu_active, 0, 0) && !worker_started) {
        ULONGLONG now = GetTickCount64();
        if (!fallback_after)
            fallback_after = now + 1000;
        if (now >= fallback_after)
            start_worker();
    }
}
