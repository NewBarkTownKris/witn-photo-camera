#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xinput.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "camera.h"
#include "capture.h"
#include "studio.h"
#include "folder.h"
#include "optics.h"
#include "projection.h"

_Static_assert(sizeof(GameView) == 0x44, "GameView layout");
_Static_assert(offsetof(GameView, eye) == 0x14, "GameView position");
_Static_assert(offsetof(GameView, up) == 0x2c, "GameView orientation");

typedef void (*UpdateCamera)(void *, float, GameView *);
typedef int (*UpdateFrame)(float);
typedef void (*SetPaused)(int, unsigned);
typedef DWORD(WINAPI *ReadPad)(DWORD, XINPUT_STATE *);

static UpdateCamera original_update;
static UpdateFrame original_frame;
static SetPaused set_paused;
static const unsigned photo_pause_reason = 29;
static BOOL owns_pause;
static volatile DWORD *viewport_render_flags;
static const DWORD gui_viewport_flags = 0x1f0;
static DWORD owned_gui_flags;
static BOOL hide_hud = TRUE;
static ReadPad read_pad;
static Camera camera;
static BOOL active;
static BOOL held[256];
static SRWLOCK mouse_lock = SRWLOCK_INIT;
typedef enum { MOUSE_CAPTURED, MOUSE_WAIT_RELEASE, MOUSE_WAIT_CLICK } MouseCapture;
static struct {
    HWND window, watched_window;
    DWORD started;
    MouseCapture capture;
    BOOL relative, minimized;
    int64_t dx, dy;
} look_input;
static POINT previous_pointer;
static RECT previous_bounds;
static WORD previous_buttons;
static BOOL back_armed;
static ULONGLONG last_tick;
static int lens_preset = -1;
static const float lenses[] = {16, 24, 35, 50, 85, 135, 200, 600};
static HMODULE real_dbghelp;

static void log_event(const char *message) {
    OutputDebugStringA(message);
    WCHAR path[MAX_PATH];
    DWORD length = GetTempPathW(MAX_PATH, path);
    static const WCHAR name[] = L"WITNPhotoCamera.log";
    if (!length || length + sizeof name / sizeof *name > MAX_PATH)
        return;
    memcpy(path + length, name, sizeof name);
    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(file, message, (DWORD)strlen(message), &written, NULL);
        CloseHandle(file);
    }
}

static void steam_screenshot(void) {
    HMODULE steam = GetModuleHandleW(L"steam_api64.dll");
    if (!steam)
        return;
    typedef void *(*GetScreenshots)(void);
    typedef void (*Trigger)(void *);
    GetScreenshots get = (GetScreenshots)GetProcAddress(steam, "SteamAPI_SteamScreenshots_v003");
    Trigger trigger = (Trigger)GetProcAddress(steam, "SteamAPI_ISteamScreenshots_TriggerScreenshot");
    void *screenshots = get ? get() : NULL;
    if (screenshots && trigger)
        trigger(screenshots);
}

static BOOL down(int key) {
    return (GetAsyncKeyState(key) & 0x8000) != 0;
}

static BOOL pressed(int key) {
    BOOL current = down(key);
    BOOL edge = current && !held[key];
    held[key] = current;
    return edge;
}

static float axis(SHORT value, int deadzone) {
    int magnitude = value < 0 ? -(int)value : value;
    if (magnitude <= deadzone)
        return 0.0f;
    float fraction = (float)(magnitude - deadzone) / (32768 - deadzone);
    return value < 0 ? -fraction * fraction : fraction * fraction;
}

static HWND game_window(void) {
    HWND window = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    return pid == GetCurrentProcessId() ? window : NULL;
}

static void release_mouse(void) {
    AcquireSRWLockExclusive(&mouse_lock);
    look_input.window = NULL;
    ReleaseSRWLockExclusive(&mouse_lock);
}

static void suspend_mouse(void) {
    AcquireSRWLockExclusive(&mouse_lock);
    look_input.window = NULL;
    look_input.capture = MOUSE_WAIT_RELEASE;
    ReleaseSRWLockExclusive(&mouse_lock);
}

static BOOL mouse_buttons(BOOL can_look, BOOL *shutter, BOOL *focus) {
    *shutter = pressed(VK_LBUTTON);
    *focus = pressed(VK_MBUTTON);
    AcquireSRWLockExclusive(&mouse_lock);
    if (look_input.capture != MOUSE_CAPTURED) {
        // A button used to return to the game can still be down on the
        // first focused frame. Wait until both are up before accepting a click.
        if (!look_input.minimized && !held[VK_LBUTTON] && !held[VK_MBUTTON])
            look_input.capture = MOUSE_WAIT_CLICK;
        else if (can_look && !look_input.minimized && look_input.capture == MOUSE_WAIT_CLICK &&
                 (*shutter || *focus))
            look_input.capture = MOUSE_CAPTURED;
        *shutter = *focus = FALSE;
    }
    BOOL waiting = look_input.capture != MOUSE_CAPTURED;
    ReleaseSRWLockExclusive(&mouse_lock);
    return waiting;
}

static void mouse_delta(HWND window, float *yaw, float *pitch) {
    AcquireSRWLockExclusive(&mouse_lock);
    if (look_input.minimized || look_input.capture != MOUSE_CAPTURED) {
        ReleaseSRWLockExclusive(&mouse_lock);
        return;
    }
    BOOL first = window != look_input.window;
    if (first) {
        look_input.window = window;
        look_input.started = GetTickCount();
        look_input.relative = FALSE;
        look_input.dx = look_input.dy = 0;
    }
    BOOL relative = look_input.relative;
    int64_t dx = look_input.dx, dy = look_input.dy;
    look_input.dx = look_input.dy = 0;
    if (relative) {
        *yaw -= (float)dx * .0024f;
        *pitch -= (float)dy * .0024f;
        ReleaseSRWLockExclusive(&mouse_lock);
        return;
    }
    ReleaseSRWLockExclusive(&mouse_lock);

    // Absolute-only devices and games without raw registration. Rebase on
    // the observed screen position when capture, clipping or monitors change.
    POINT pointer;
    RECT client, bounds, clip;
    if (!GetCursorPos(&pointer) || !GetClientRect(window, &client)) {
        release_mouse();
        return;
    }
    POINT origin = {client.left, client.top}, end = {client.right, client.bottom};
    if (!ClientToScreen(window, &origin) || !ClientToScreen(window, &end)) {
        release_mouse();
        return;
    }
    bounds = (RECT){origin.x, origin.y, end.x, end.y};
    if (GetClipCursor(&clip))
        IntersectRect(&bounds, &bounds, &clip);
    LONG width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
    if (!first && EqualRect(&bounds, &previous_bounds) && PtInRect(&bounds, previous_pointer) &&
        PtInRect(&bounds, pointer)) {
        LONG x = pointer.x - previous_pointer.x, y = pointer.y - previous_pointer.y;
        // Ignore screen-sized cursor relocations from focus/desktop changes.
        AcquireSRWLockShared(&mouse_lock);
        BOOL captured = look_input.window == window && look_input.capture == MOUSE_CAPTURED;
        if (captured && labs(x) <= width / 3 && labs(y) <= height / 3) {
            *yaw -= x * .0024f;
            *pitch -= y * .0024f;
        }
        ReleaseSRWLockShared(&mouse_lock);
        if (captured && (x || y) && width > 8 && height > 8 &&
            (pointer.x <= bounds.left + 1 || pointer.x >= bounds.right - 2 ||
             pointer.y <= bounds.top + 1 || pointer.y >= bounds.bottom - 2) && GetForegroundWindow() == window) {
            if (SetCursorPos(bounds.left + width / 2, bounds.top + height / 2) && !GetCursorPos(&pointer)) {
                release_mouse();
                return;
            }
        }
    }
    previous_pointer = pointer;
    previous_bounds = bounds;
}

static void reset_roll(void) {
    Vec3 world_up = {0, 0, 1};
    Vec3 right = cross(camera.forward, world_up);
    if (dot(right, right) > 0.00001f)
        camera.up = unit(cross(unit(right), camera.forward));
}

enum {
    LENS = PHOTO_DIAL_COUNT,
    FOCUS_DISTANCE,
    FOCUS_MODE,
    FOCUS_STEP,
    DOF,
    STRENGTH,
    MOTION_BLUR,
    OUTPUT_FOLDER,
    ROW_COUNT
};
static PhotoSettings photo;
static BOOL config_loaded, dirty, continuous, af_locked, fine, dof;
static BOOL clean_motion = TRUE;
static int strength;
static float focus_distance, center_distance;
static ULONGLONG af_tick, meter_tick, status_until, shot_ends, save_at;
static BOOL pending_shot, timed_shot, print_shot, exit_requested;
static BOOL awaiting_meter, awaiting_pair;
static ULONGLONG meter_deadline;
typedef struct {
    Camera camera;
    int frame;
    BOOL portrait;
    ULONGLONG tick;
    uint32_t serial;
} MeterPose;
static MeterPose metering, measured;
static BOOL meter_matches(const MeterPose *p, ULONGLONG tick) {
    Vec3 delta = add(p->camera.position, scale(camera.position, -1));
    float tolerance = fminf(.002f, camera.fov * .005f);
    Vec3 aim_delta = add(p->camera.forward, scale(camera.forward, -1));
    Vec3 up_delta = add(p->camera.up, scale(camera.up, -1));
    return p->serial && tick - p->tick <= 1000 && p->frame == studio_frame() &&
           p->portrait == studio_portrait() && dot(delta, delta) < .0001f &&
           fabsf(p->camera.fov - camera.fov) < tolerance &&
           dot(aim_delta, aim_delta) < tolerance * tolerance &&
           dot(up_delta, up_delta) < tolerance * tolerance;
}
static void meter_scene(ULONGLONG tick) {
    if (!photo.dial[MODE]) {
        photo.luminance = -1;
        measured.serial = 0;
        return;
    }
    float luminance;
    if (metering.serial && capture_meter_result(&luminance) == metering.serial) {
        measured = metering;
        metering.serial = 0;
        photo.luminance = luminance;
    }
    if (!metering.serial && (tick >= meter_tick || (awaiting_meter && !meter_matches(&measured, tick)))) {
        uint32_t seq = capture_meter(studio_frame(), studio_portrait());
        if (seq) {
            metering = (MeterPose){camera, studio_frame(), studio_portrait(), tick, seq};
            meter_tick = tick + 350;
        }
    }
}
static char status[180];
static HWND owner;
static HHOOK wheel_hook, focus_hook;
static LONG raw_wheel_count, legacy_wheel_count, raw_wheel_seen;
static int wheel_remainder;
static DWORD wheel_tid;
static BOOL capture_ready;

static void notice(const char *msg) {
    lstrcpynA(status, msg, sizeof status);
    status_until = GetTickCount64() + 3000;
    log_event(msg);
    log_event("\n");
}
static LRESULT CALLBACK focus_events(int code, WPARAM wp, LPARAM lp) {
    if (code >= 0) {
        const CWPSTRUCT *message = (const CWPSTRUCT *)lp;
        if (message->message == WM_ACTIVATEAPP || message->message == WM_ACTIVATE || message->message == WM_SETFOCUS ||
            message->message == WM_KILLFOCUS || message->message == WM_SIZE) {
            AcquireSRWLockExclusive(&mouse_lock);
            if (message->hwnd == look_input.watched_window) {
                BOOL loss = (message->message == WM_ACTIVATEAPP && !message->wParam) ||
                            (message->message == WM_ACTIVATE && LOWORD(message->wParam) == WA_INACTIVE);
                BOOL focus_event = message->message != WM_SIZE;
                if (message->message == WM_SIZE) {
                    BOOL minimized = message->wParam == SIZE_MINIMIZED;
                    focus_event = minimized != look_input.minimized;
                    loss = minimized;
                    look_input.minimized = minimized;
                }
                if (focus_event) {
                    look_input.window = NULL;
                    if (loss || look_input.capture != MOUSE_CAPTURED)
                        look_input.capture = MOUSE_WAIT_RELEASE;
                }
            }
            ReleaseSRWLockExclusive(&mouse_lock);
        }
    }
    return CallNextHookEx(NULL, code, wp, lp);
}

static LRESULT CALLBACK wheel_events(int code, WPARAM wp, LPARAM lp) {
    if (code >= 0 && wp == PM_REMOVE) {
        const MSG *message = (const MSG *)lp;
        if (message->message == WM_INPUT) {
            RAWINPUT input;
            UINT size = sizeof input;
            UINT bytes = GetRawInputData((HRAWINPUT)message->lParam, RID_INPUT, &input, &size,
                                         sizeof(RAWINPUTHEADER));
            if (bytes != (UINT)-1 && bytes >= offsetof(RAWINPUT, data) + sizeof(RAWMOUSE) &&
                input.header.dwType == RIM_TYPEMOUSE) {
                const RAWMOUSE *mouse = &input.data.mouse;
                if (mouse->usButtonFlags & RI_MOUSE_WHEEL) {
                    InterlockedExchange(&raw_wheel_seen, TRUE);
                    InterlockedExchangeAdd(&raw_wheel_count, (SHORT)mouse->usButtonData);
                }
                if (mouse->lLastX || mouse->lLastY) {
                    AcquireSRWLockExclusive(&mouse_lock);
                    if (look_input.window && (LONG)(message->time - look_input.started) > 0) {
                        if (mouse->usFlags & MOUSE_MOVE_ABSOLUTE) {
                            if (look_input.relative)
                                look_input.window = NULL;
                        } else {
                            look_input.relative = TRUE;
                            look_input.dx += mouse->lLastX;
                            look_input.dy += mouse->lLastY;
                        }
                    }
                    ReleaseSRWLockExclusive(&mouse_lock);
                }
            }
        } else if (message->message == WM_MOUSEWHEEL) {
            InterlockedExchangeAdd(&legacy_wheel_count, (SHORT)HIWORD(message->wParam));
        }
    }
    return CallNextHookEx(NULL, code, wp, lp);
}
static int wheel_steps(BOOL consume) {
    LONG raw = InterlockedExchange(&raw_wheel_count, 0);
    LONG legacy = InterlockedExchange(&legacy_wheel_count, 0);
    int delta = InterlockedCompareExchange(&raw_wheel_seen, 0, 0) ? raw : legacy;
    wheel_remainder = consume ? wheel_remainder + delta : 0;
    int steps = wheel_remainder / WHEEL_DELTA;
    wheel_remainder %= WHEEL_DELTA;
    return steps;
}
static void watch_wheel(HWND window) {
    DWORD thread = GetWindowThreadProcessId(window, NULL);
    if (thread == wheel_tid && wheel_hook)
        return;
    if (wheel_hook)
        UnhookWindowsHookEx(wheel_hook);
    if (focus_hook)
        UnhookWindowsHookEx(focus_hook);
    AcquireSRWLockExclusive(&mouse_lock);
    look_input.watched_window = window;
    look_input.minimized = FALSE;
    look_input.capture = MOUSE_CAPTURED;
    ReleaseSRWLockExclusive(&mouse_lock);
    wheel_tid = thread;
    wheel_hook = SetWindowsHookExW(WH_GETMESSAGE, wheel_events, NULL, thread);
    focus_hook = SetWindowsHookExW(WH_CALLWNDPROC, focus_events, NULL, thread);
}
static int lens_mm(void) {
    return (int)(18.f / tanf(camera.fov * .5f) + .5f);
}
static const char *focus_mode(void) {
    return continuous ? "AF-C" : af_locked ? "AF lock" : "Manual";
}
static void mark_dirty(void) {
    dirty = TRUE;
    save_at = GetTickCount64() + 500;
}
static void enable_dof(void) {
    if (!dof) {
        dof = TRUE;
        mark_dirty();
    }
}
static void cycle_lens(int step) {
    int count = sizeof lenses / sizeof lenses[0];
    lens_preset = lens_preset < 0 ? (step > 0 ? 0 : count - 1) : (lens_preset + step + count) % count;
    camera.fov = 2.f * atanf(18.f / lenses[lens_preset]);
    mark_dirty();
}
static void save_config(void) {
    const WCHAR *ini = folder_ini();
    if (!*ini)
        return;
    photo_save(&photo, ini);
    WCHAR val[16];
    wsprintfW(val, L"%d", lens_preset);
    WritePrivateProfileStringW(L"Camera", L"Lens", val, ini);
    wsprintfW(val, L"%d", strength);
    WritePrivateProfileStringW(L"Camera", L"Strength", val, ini);
    WritePrivateProfileStringW(L"Camera", L"DOF", dof ? L"1" : L"0", ini);
    WritePrivateProfileStringW(L"Camera", L"CleanMotion", clean_motion ? L"1" : L"0", ini);
    dirty = FALSE;
}
static const char *row_label(int row) {
    static const char *other[] = {
        "Lens",           "Focus distance", "Focus mode",         "Manual focus step",
        "Depth of field", "DOF strength",   "Camera motion blur", "Screenshot folder"};
    if (row == ISO && !photo.dial[MODE] && photo.dial[GRAIN])
        return "Grain ISO";
    return row < PHOTO_DIAL_COUNT ? photo_label(row) : other[row - PHOTO_DIAL_COUNT];
}
static BOOL row_enabled(int row) {
    if (awaiting_meter || (pending_shot && capture_busy()))
        return FALSE;
    return row < PHOTO_DIAL_COUNT ? photo_enabled(&photo, row) : TRUE;
}
static void row_value(int row, char *out, size_t size) {
    if (row < PHOTO_DIAL_COUNT) {
        photo_value(&photo, row, out, size);
        return;
    }
    switch (row) {
    case LENS:
        snprintf(out, size, "%d mm", lens_mm());
        break;
    case FOCUS_DISTANCE:
        snprintf(out, size, "%.2f m", (double)focus_distance);
        break;
    case FOCUS_MODE:
        snprintf(out, size, "%s", focus_mode());
        break;
    case FOCUS_STEP:
        snprintf(out, size, "%s", fine ? "1%" : "20%");
        break;
    case DOF:
        snprintf(out, size, "%s", dof ? "On" : "Off");
        break;
    case STRENGTH:
        snprintf(out, size, "%dx", 1 << strength);
        break;
    case MOTION_BLUR:
        snprintf(out, size, "%s", clean_motion ? "Clean" : "Game");
        break;
    default:
        snprintf(out, size, "Choose folder");
        break;
    }
}
static void row_adjust(int row, int step) {
    if (!row_enabled(row))
        return;
    if (row < PHOTO_DIAL_COUNT) {
        photo_adjust(&photo, row, step);
        mark_dirty();
        return;
    }
    switch (row) {
    case LENS:
        cycle_lens(step);
        break;
    case FOCUS_DISTANCE:
        continuous = af_locked = FALSE;
        enable_dof();
        focus_distance = fmaxf(fmaxf(.05f, lens_mm() * .00105f),
                               fminf(10000, focus_distance * powf(fine ? 1.01f : 1.2f, (float)step)));
        break;
    case FOCUS_MODE:
        continuous = !continuous;
        af_locked = !continuous;
        enable_dof();
        af_tick = 0;
        break;
    case FOCUS_STEP:
        fine = !fine;
        break;
    case DOF:
        dof = !dof;
        if (!dof)
            continuous = FALSE;
        mark_dirty();
        break;
    case STRENGTH:
        strength = (strength + step + 4) % 4;
        mark_dirty();
        break;
    case MOTION_BLUR:
        clean_motion = !clean_motion;
        mark_dirty();
        break;
    case OUTPUT_FOLDER:
        folder_choose(owner);
        break;
    }
}
static void release_photo(void) {
    if ((timed_shot || print_shot) && capture_busy())
        capture_cancel();
    timed_shot = print_shot = FALSE;
    exit_requested = FALSE;
    awaiting_meter = FALSE;
    back_armed = FALSE;
    if (wheel_hook) {
        UnhookWindowsHookEx(wheel_hook);
        wheel_hook = NULL;
        wheel_tid = 0;
    }
    if (focus_hook) {
        UnhookWindowsHookEx(focus_hook);
        focus_hook = NULL;
    }
    active = FALSE;
    release_mouse();
    if (dirty)
        save_config();
    optics_configure(FALSE, 0, 0, 0, 0, 0, 0);
    optics_grade(1, 1, 1, 1, 1, 1, 0, FALSE);
    optics_clean_motion(FALSE);
    studio_tick(NULL, FALSE, NULL, 0, 0, NULL, NULL);
    log_event("Camera disabled.\n");
}
static void request_exit(void) {
    if (pending_shot && !timed_shot && !print_shot && capture_busy()) {
        exit_requested = TRUE;
        return;
    }
    release_photo();
}
static void shutter(BOOL pair, ULONGLONG tick) {
    if (capture_busy())
        return;
    if (!capture_ready) {
        notice("Photo renderer unavailable - using Steam screenshot");
        steam_screenshot();
        return;
    }
    Exposure shot = photo_solve(&photo);
    if ((photo.dial[MODE] >= 2 || (photo.dial[MODE] && photo.dial[AUTO_ISO])) &&
        (photo.luminance <= 0 || !meter_matches(&measured, tick))) {
        if (!awaiting_meter) {
            awaiting_meter = TRUE;
            awaiting_pair = pair;
            meter_deadline = tick + 2000;
            notice("Metering the selected frame");
        }
        return;
    }
    awaiting_meter = FALSE;
    WCHAR path[MAX_PATH];
    if (!folder_photo_path(path)) {
        notice("Cannot write to the selected photo folder");
        return;
    }
    photo.native_grade = optics_grade_supported();
    float seconds = photo.dial[MODE] && shot.seconds >= 1.f / 15.f ? shot.seconds : 0;
    unsigned scale = (unsigned)studio_capture_scale();
    if (scale > 1 && !projection_available()) {
        notice("Print projection unavailable on this build");
        return;
    }
    if (!capture_request(path, studio_frame(), studio_portrait(), pair, &photo, seconds, scale)) {
        notice("Shutter unavailable");
        return;
    }
    timed_shot = seconds > 0;
    print_shot = scale > 1;
    shot_ends = tick + (ULONGLONG)(seconds * 1000);
    pending_shot = TRUE;
    char details[200];
    snprintf(details, sizeof details, "Shutter: %d mm, %.2f m, f/%g, %g s, ISO %d, crop %d%s, %ux, %s.\n",
             lens_mm(), (double)focus_distance, (double)shot.aperture, (double)shot.seconds, (int)shot.iso,
             studio_frame(), studio_portrait() ? " portrait" : "", scale, optics_shader_name());
    log_event(details);
    notice(pair ? "Capturing PNG + scene EXR" : "Capturing PNG");
}
static void poll_shot(void) {
    if (!pending_shot)
        return;
    if (capture_busy()) {
        if (exit_requested && GetTickCount64() > shot_ends + 8000) {
            log_event("Photo writer is still busy; releasing the gameplay pause.\n");
            release_photo();
        }
        return;
    }
    pending_shot = FALSE;
    timed_shot = print_shot = FALSE;
    int result = capture_result();
    const char *msg = result == 1    ? "PNG saved"
                      : result == 2  ? "PNG + scene EXR saved"
                      : result == 3  ? "PNG saved - scene EXR unavailable"
                      : result == -2 ? "Exposure canceled"
                                     : "Photo write failed";
    notice(msg);
    if (exit_requested)
        release_photo();
}
static BOOL no_press(int key) {
    (void)key;
    return FALSE;
}
static void update_finder(HWND window, WORD edges, ULONGLONG tick, BOOL locked, BOOL mouse_waiting) {
    Exposure exposure = photo_solve(&photo);
    char readout[180], shutter_text[28], mode[32];
    photo_value(&photo, SHUTTER, shutter_text, sizeof shutter_text);
    photo_value(&photo, MODE, mode, sizeof mode);
    if (photo.dial[MODE])
        snprintf(readout, sizeof readout, "%s   f/%g   %s   ISO %d   %s %.2f m%s", mode,
                 (double)exposure.aperture, shutter_text, (int)exposure.iso, focus_mode(),
                 (double)focus_distance, dof ? "   DOF" : "");
    else
        snprintf(readout, sizeof readout, "Game exposure   f/%g   %s %.2f m%s", (double)exposure.aperture,
                 focus_mode(), (double)focus_distance, dof ? "   DOF" : "");
    if (dof && center_distance > 0) {
        size_t used = strlen(readout);
        float err = center_distance - focus_distance;
        snprintf(readout + used, sizeof readout - used, "   %.2f m %s", (double)center_distance,
                 fabsf(err) < fmaxf(.001f, .00003f * exposure.aperture * focus_distance *
                                               fmaxf(0.f, focus_distance - lens_mm() * .001f) /
                                               (lens_mm() * lens_mm() * .000001f * (1 << strength)))
                     ? "aligned"
                 : err > 0 ? "farther"
                           : "nearer");
    }
    unsigned done = 0, total = 0;
    if (timed_shot && locked && tick < shot_ends)
        snprintf(readout, sizeof readout, "Exposure open   %.1f s remaining", (shot_ends - tick) * .001);
    else if (print_shot && locked && capture_print_progress(&done, &total)) {
        if (total && done < total)
            snprintf(readout, sizeof readout, "Print samples   %u / %u     F8  Cancel", done, total);
        else
            lstrcpyA(readout, "Developing print photograph");
    } else if (timed_shot && locked)
        lstrcpyA(readout, "Developing photograph");
    else if (tick < status_until)
        lstrcpynA(readout, status, sizeof readout);
    if (mouse_waiting && !locked && !studio_is_open()) {
        size_t used = strlen(readout);
        snprintf(readout + used, sizeof readout - used, "   Click to resume mouse look");
    }
    StudioExtra rows = {
        ROW_COUNT,         row_label,
        row_value,         row_adjust,
        row_enabled,       exposure.metered,
        exposure.error_ev, "N / T  Focus / AF-C     B  DOF     U / I  Aperture     J / K  Focus distance",
        photo.dial[MODE] && exposure.seconds >= 1.f / 15.f};
    studio_tick(window, TRUE, locked ? no_press : pressed, locked ? 0 : edges, lens_mm(), readout, &rows);
}

static void move_camera(HWND window, const XINPUT_STATE *pad, WORD buttons, float dt) {
    const XINPUT_GAMEPAD *g = &pad->Gamepad;
    float yaw = (down(VK_LEFT) - down(VK_RIGHT) - axis(g->sThumbRX, 8689)) * 1.6f * dt;
    float pitch = (down(VK_UP) - down(VK_DOWN) + axis(g->sThumbRY, 8689)) * 1.6f * dt;
    if (!down(VK_MENU))
        mouse_delta(window, &yaw, &pitch);
    else
        release_mouse();
    float sensitivity = fminf(1.f, camera.fov / 1.05f), speed = 6.f * dt;
    if (down(VK_CONTROL))
        speed *= .2f;
    if (down(VK_SHIFT))
        speed *= 4.f;
    float vertical = down('E') - down('Q') + !!(buttons & XINPUT_GAMEPAD_RIGHT_SHOULDER) -
                     !!(buttons & XINPUT_GAMEPAD_LEFT_SHOULDER);
    camera_move(&camera, yaw * sensitivity, pitch * sensitivity, (down('Z') - down('X')) * dt,
                (down('W') - down('S') + axis(g->sThumbLY, 7849)) * speed,
                (down('D') - down('A') + axis(g->sThumbLX, 7849)) * speed, vertical * speed);
    if (pressed(VK_F10))
        reset_roll();
}

static void apply_photo_camera(GameView *view) {
    Camera render_camera = camera;
    capture_print_camera(&render_camera);
    camera_apply(&render_camera, view);
}

static void update_camera(void *controller, float elapsed, GameView *view) {
    original_update(controller, elapsed, view);
    static BOOL observed;
    if (!observed) {
        observed = TRUE;
        log_event("Camera update reached.\n");
    }
    if (!read_pad) {
        HMODULE xinput = GetModuleHandleW(L"xinput1_4.dll");
        if (xinput)
            read_pad = (ReadPad)GetProcAddress(xinput, "XInputGetState");
    }
    HWND window = game_window();
    ULONGLONG tick = GetTickCount64();
    float dt = fminf(.05f, (float)(tick - last_tick) * .001f);
    last_tick = tick;
    if (!window || folder_busy()) {
        if (active && !window)
            suspend_mouse();
        else
            release_mouse();
        if (active) {
            apply_photo_camera(view);
            studio_hide();
        }
        return;
    }
    XINPUT_STATE pad = {0};
    BOOL pad_available = read_pad && read_pad(0, &pad) == ERROR_SUCCESS;
    WORD buttons = pad.Gamepad.wButtons, edges = buttons & ~previous_buttons;
    BOOL back_down = (buttons & XINPUT_GAMEPAD_BACK) != 0;
    BOOL resume = active && back_armed && !back_down && buttons == 0 && pad_available;
    if (edges & XINPUT_GAMEPAD_BACK)
        back_armed = active;
    if (!back_down || (buttons & ~XINPUT_GAMEPAD_BACK))
        back_armed = FALSE;
    if (back_down || (previous_buttons & XINPUT_GAMEPAD_BACK))
        edges = 0;
    previous_buttons = buttons;
    if (resume) {
        request_exit();
        return;
    }
    if (!active && pressed(VK_F8) && camera_begin(&camera, view)) {
        owner = window;
        if (!config_loaded) {
            const WCHAR *ini = folder_ini();
            if (*ini) {
                photo_load(&photo, ini);
                lens_preset = (int)GetPrivateProfileIntW(L"Camera", L"Lens", -1, ini);
                if (lens_preset < 0 || lens_preset >= 8)
                    lens_preset = -1;
                strength = (int)GetPrivateProfileIntW(L"Camera", L"Strength", 0, ini);
                if (strength < 0 || strength > 3)
                    strength = 0;
                dof = GetPrivateProfileIntW(L"Camera", L"DOF", 0, ini) == 1;
                clean_motion = GetPrivateProfileIntW(L"Camera", L"CleanMotion", 1, ini) == 1;
            } else
                photo_defaults(&photo);
            config_loaded = TRUE;
        }
        photo.luminance = -1;
        photo.native_grade = optics_grade_supported();
        focus_distance = 3;
        center_distance = 0;
        continuous = af_locked = FALSE;
        fine = FALSE;
        if (lens_preset >= 0)
            camera.fov = 2.f * atanf(18.f / lenses[lens_preset]);
        metering.serial = measured.serial = 0;
        awaiting_meter = FALSE;
        af_tick = meter_tick = 0;
        status_until = 0;
        active = TRUE;
        held[VK_LBUTTON] = down(VK_LBUTTON);
        held[VK_MBUTTON] = down(VK_MBUTTON);
        edges = 0;
        release_mouse();
        watch_wheel(window);
        InterlockedExchange(&raw_wheel_seen, FALSE);
        wheel_steps(FALSE);
        log_event("Camera enabled.\n");
    }
    if (!active)
        return;
    owner = window;
    BOOL locked = awaiting_meter || (pending_shot && capture_busy());
    BOOL can_move = !locked || (timed_shot && tick < shot_ends);
    BOOL click, middle;
    BOOL mouse_waiting = mouse_buttons(can_move && !studio_is_open() && !down(VK_MENU), &click, &middle);
    update_finder(window, edges, tick, locked, mouse_waiting);
    BOOL panel = studio_is_open();
    if (!panel && can_move) {
        move_camera(window, &pad, buttons, dt);
    } else
        release_mouse();
    if (locked)
        wheel_steps(FALSE);
    if (!locked) {
        if (pressed(VK_F2) && folder_choose(window)) {
            release_mouse();
            apply_photo_camera(view);
            studio_hide();
            return;
        }
        if (pressed(VK_F7))
            cycle_lens(1);
        if (pressed('U'))
            row_adjust(APERTURE, -1);
        if (pressed('I'))
            row_adjust(APERTURE, 1);
        if (pressed('L'))
            row_adjust(FOCUS_STEP, 1);
        if (pressed('J'))
            row_adjust(FOCUS_DISTANCE, -1);
        if (pressed('K'))
            row_adjust(FOCUS_DISTANCE, 1);
        if (pressed('T'))
            row_adjust(FOCUS_MODE, 1);
        if (pressed('B'))
            row_adjust(DOF, 1);
        if (pressed('Y'))
            row_adjust(STRENGTH, 1);
        if (pressed('V'))
            row_adjust(MOTION_BLUR, 1);
        int steps = wheel_steps(!panel);
        if (steps && !panel) {
            int dial = down(VK_MENU)                        ? (down(VK_CONTROL) ? ISO : SHUTTER)
                       : down(VK_SHIFT) || down(VK_CONTROL) ? APERTURE
                                                            : FOCUS_DISTANCE;
            if (dial == APERTURE)
                steps = -steps;
            for (int i = 0; i < abs(steps) && i < 20; i++)
                row_adjust(dial, steps > 0 ? 1 : -1);
        }
        int zoom_step = down(VK_OEM_6) - down(VK_OEM_4);
        if (!panel && zoom_step) {
            if (lens_preset >= 0) {
                lens_preset = -1;
                mark_dirty();
            }
            camera.fov = fmaxf(.03f, fminf(2.6f, camera.fov + zoom_step * dt));
        }
        BOOL focus_once = pressed('N') || (edges & XINPUT_GAMEPAD_RIGHT_THUMB) ||
                          (!panel && middle && studio_finder_visible());
        if (focus_once || ((continuous || dof) && tick >= af_tick)) {
            float xyz[3] = {camera.position.x, camera.position.y, camera.position.z};
            float forward[3] = {camera.forward.x, camera.forward.y, camera.forward.z};
            center_distance = optics_focus_center(xyz, forward, view->far_plane);
            if (center_distance > 0 && (continuous || focus_once))
                focus_distance = center_distance;
            af_tick = tick + 150;
            if (focus_once) {
                if (center_distance > 0) {
                    continuous = FALSE;
                    af_locked = TRUE;
                    enable_dof();
                }
                char msg[80];
                if (center_distance > 0)
                    snprintf(msg, sizeof msg, "Focus locked at %.2f m", (double)focus_distance);
                else
                    lstrcpyA(msg, "No center hit - holding focus");
                notice(msg);
            }
        }
    }
    apply_photo_camera(view);
    if (!pending_shot)
        meter_scene(tick);
    optics_clean_motion(clean_motion);
    Exposure exposure = photo_solve(&photo);
    optics_configure(dof, focus_distance, exposure.aperture, (float)lens_mm(), 1u << strength,
                     view->near_plane, view->far_plane);
    float contrast, saturation, rgb[3], noise;
    photo_grade_options(&photo, &contrast, &saturation, rgb, &noise);
    optics_grade(exposure.gain, contrast, saturation, rgb[0], rgb[1], rgb[2], noise,
                 photo.dial[MODE] || photo.dial[FILM] || photo.dial[GRAIN]);
    if (dirty && tick >= save_at)
        save_config();
    BOOL want_pair = pressed(VK_F3) || (edges & XINPUT_GAMEPAD_Y);
    BOOL want_png =
        pressed(VK_F9) || (edges & XINPUT_GAMEPAD_A) || (!panel && click && studio_finder_visible());
    if (awaiting_meter) {
        if (tick >= meter_deadline) {
            awaiting_meter = FALSE;
            notice("Scene meter unavailable");
        } else if (photo.luminance > 0 && meter_matches(&measured, tick))
            shutter(awaiting_pair, tick);
    } else if (!locked && (want_pair || want_png))
        shutter(want_pair, tick);
}

static void update_hud(void) {
    DWORD flags = *viewport_render_flags;
    if (active && hide_hud) {
        owned_gui_flags |= gui_viewport_flags & ~flags;
        flags |= gui_viewport_flags;
    } else {
        flags &= ~owned_gui_flags;
        owned_gui_flags = 0;
    }
    if (flags != *viewport_render_flags)
        *viewport_render_flags = flags;
}

static int update_frame(float scale) {
    poll_shot();
    if (active && !folder_busy() && game_window() && pressed(VK_F8))
        request_exit();
    if (owns_pause != active) {
        owns_pause = active;
        set_paused(owns_pause, photo_pause_reason);
        log_event(owns_pause ? "Photo pause requested.\n" : "Photo pause released.\n");
    }
    if (active && !folder_busy() && game_window() && !capture_busy() && pressed(VK_F11))
        hide_hud = !hide_hud;
    update_hud();
    return original_frame(scale);
}

static BOOL redirect_call(unsigned char *base, unsigned char *call, uintptr_t callback) {
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    uintptr_t step = info.dwAllocationGranularity;
    uintptr_t start = (uintptr_t)base & ~(step - 1);
    unsigned char *relay = NULL;
    for (uintptr_t offset = step; offset < 0x40000000; offset += step) {
        relay = VirtualAlloc((void *)(start + offset), 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (relay)
            break;
    }
    if (!relay)
        return FALSE;

    // jmp qword ptr [rip] followed by the full 64-bit DLL address.
    static const unsigned char jump[] = {0xff, 0x25, 0, 0, 0, 0};
    memcpy(relay, jump, sizeof jump);
    memcpy(relay + sizeof jump, &callback, sizeof callback);
    DWORD old;
    if (!VirtualProtect(relay, 4096, PAGE_EXECUTE_READ, &old) ||
        !FlushInstructionCache(GetCurrentProcess(), relay, 4096) ||
        !VirtualProtect(call, 5, PAGE_EXECUTE_READWRITE, &old)) {
        VirtualFree(relay, 0, MEM_RELEASE);
        return FALSE;
    }
    int32_t displacement = (int32_t)((uintptr_t)relay - ((uintptr_t)call + 5));
    memcpy(call + 1, &displacement, sizeof displacement);
    FlushInstructionCache(GetCurrentProcess(), call, 5);
    DWORD ignored;
    VirtualProtect(call, 5, old, &ignored);
    return TRUE;
}

static BOOL install_hook(unsigned char *base) {
    unsigned char *camera_call = base + 0x45cb23;
    unsigned char *frame_call = base + 0x0475b8;
    static const unsigned char camera_bytes[] = {0xe8, 0xb8, 0xd5, 0x24, 0x00};
    static const unsigned char frame_bytes[] = {0xe8, 0x53, 0xaa, 0x3e, 0x00};
    static const unsigned char pause_bytes[] = {0x8b, 0xc1, 0x8b, 0xca, 0xba, 0x01, 0, 0, 0};
    static const unsigned char gui_bytes[] = {0x89, 0x0d, 0xc2, 0x22, 0x9e, 0x01, 0xc3};
    if (memcmp(camera_call, camera_bytes, sizeof camera_bytes) ||
        memcmp(frame_call, frame_bytes, sizeof frame_bytes) ||
        memcmp(base + 0x2c4810, pause_bytes, sizeof pause_bytes) ||
        memcmp(base + 0x2c5300, gui_bytes, sizeof gui_bytes))
        return FALSE;
    original_update = (UpdateCamera)(base + 0x6aa0e0);
    original_frame = (UpdateFrame)(base + 0x432010);
    set_paused = (SetPaused)(base + 0x2c4810);
    // DisableGuiViewportRendering reserves bits 4–8 for the GUI viewports.
    viewport_render_flags = (volatile DWORD *)(base + 0x1ca75c8);
    if (!redirect_call(base, frame_call, (uintptr_t)update_frame))
        return FALSE;
    if (!redirect_call(base, camera_call, (uintptr_t)update_camera))
        return FALSE;
    log_event(projection_install(base, redirect_call) ? "Print projection installed.\n" :
                                                        "Print projection unavailable.\n");
    capture_ready = capture_install(base, redirect_call, log_event);
    log_event(capture_ready ? "Photo renderer installed.\n" : "Photo renderer unavailable.\n");
    log_event(optics_install(base, redirect_call) ? "Optics installed.\n" : "Optics unavailable.\n");
    return TRUE;
}

BOOL WINAPI proxy_dump(HANDLE process, DWORD pid, HANDLE file, int type, void *exception, void *streams,
                       void *callback) {
    typedef BOOL(WINAPI * Dump)(HANDLE, DWORD, HANDLE, int, void *, void *, void *);
    if (!real_dbghelp) {
        WCHAR path[MAX_PATH];
        UINT size = GetSystemDirectoryW(path, MAX_PATH);
        static const WCHAR suffix[] = L"\\dbghelp.dll";
        if (!size || size + sizeof suffix / sizeof *suffix > MAX_PATH)
            return FALSE;
        memcpy(path + size, suffix, sizeof suffix);
        real_dbghelp = LoadLibraryW(path);
    }
    Dump dump = real_dbghelp ? (Dump)GetProcAddress(real_dbghelp, "MiniDumpWriteDump") : NULL;
    return dump && dump != proxy_dump && dump(process, pid, file, type, exception, streams, callback);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        HMODULE executable = GetModuleHandleW(NULL);
        WCHAR path[MAX_PATH];
        DWORD length = GetModuleFileNameW(executable, path, MAX_PATH);
        if (length < 8 || length >= MAX_PATH || lstrcmpiW(path + length - 8, L"witn.exe"))
            return TRUE;
        unsigned char *base = (unsigned char *)executable;
        IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
        IMAGE_NT_HEADERS *pe = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
        if (pe->OptionalHeader.SizeOfImage != WITN_IMAGE_SIZE ||
            pe->FileHeader.TimeDateStamp != WITN_TIMESTAMP)
            return TRUE;
        HMODULE xinput = GetModuleHandleW(L"xinput1_4.dll");
        if (xinput)
            read_pad = (ReadPad)GetProcAddress(xinput, "XInputGetState");
        log_event(install_hook(base) ? "Camera hook installed.\n" : "Camera hook rejected.\n");
    }
    return TRUE;
}
