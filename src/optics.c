#define WIN32_LEAN_AND_MEAN
#include "optics.h"
#include <math.h>
#include <stddef.h>
#include <string.h>

typedef struct {
    float x, y, z, w;
} ShaderRegister;
typedef void (*UploadConstants)(unsigned, const ShaderRegister *, unsigned);
typedef void (*BlurPass)(void *, int, void *, float, float, const float *);
typedef struct {
    float eye[3], delta[3];
} RaySegment;
typedef struct {
    void *ignore_entity;
    unsigned group, mask;
    void *predicate;
    int enabled;
} RayFilter;
typedef struct {
    float hit_position[3];
    unsigned char hit_data[0x28 - 12];
    float fraction;
    unsigned char remaining_hits[0x540 - 0x2c];
    unsigned count, unused, capacity;
} RayResult;
_Static_assert(offsetof(RayResult, count) == 0x540, "ray collector layout");
typedef int (*WorldRaycast)(void *, const RaySegment *, const RayFilter *, RayResult *);

static UploadConstants upload;
static BlurPass original_blur;
static ShaderRegister *registers;
static WorldRaycast raycast;
static unsigned char **physics_manager;
static SRWLOCK lock = SRWLOCK_INIT;
static BOOL installed;
static BOOL blur_installed;
static volatile LONG rendered_frames;
static void **bound_shader, **shader_entries;
static volatile LONG last_variant = -1;
static volatile LONG scene_scale_bits = 0x3f800000; // full resolution before the first composite
static volatile LONG far_clip_bits = 0x461c4000;    // native camera's 10,000-unit default
static struct {
    BOOL focus, grade, clean_motion;
    float distance, transition, tolerance, blur_size;
    float red, green, blue, contrast, saturation, noise;
} settings;

float optics_focus_center(const float eye[3], const float forward[3], float max_distance) {
    if (!raycast || !physics_manager || !*physics_manager)
        return 0;
    // The engine's closest-hit overload dereferences both world handles.
    void ***world = (void ***)(*physics_manager + 0x18);
    if (!*world || !**world)
        return 0;
    float length = sqrtf(forward[0] * forward[0] + forward[1] * forward[1] + forward[2] * forward[2]);
    if (!(length > 0.5f && length < 2.0f))
        return 0;
    if (!(max_distance > 0.01f)) {
        LONG bits = InterlockedCompareExchange(&far_clip_bits, 0, 0);
        memcpy(&max_distance, &bits, sizeof max_distance);
    }
    max_distance = fminf(max_distance, 10000.0f);
    RaySegment ray;
    for (unsigned i = 0; i < 3; ++i) {
        ray.eye[i] = eye[i];
        ray.delta[i] = forward[i] * (max_distance / length);
    }
    RayResult result = {0};
    result.capacity = 1;
    // The camera collision query at RVA 0x778a07 initializes both halves of
    // the Havok filter info to 0xffff. A null filter packs both halves as zero.
    const RayFilter filter = {NULL, 0xffff, 0xffff, NULL, 1};
    if (!raycast(world, &ray, &filter, &result) || result.count != 1 ||
        !(result.fraction > 0.0f && result.fraction < 1.0f))
        return 0;
    return result.fraction * max_distance;
}

void optics_configure(BOOL enabled, float distance, float aperture, float lens_mm, unsigned strength,
                      float near_plane, float far_plane) {
    // Camera nodes may inherit clipping; c33 carries the resolved values.
    (void)near_plane;
    (void)far_plane;
    AcquireSRWLockExclusive(&lock);
    settings.focus = enabled && distance > 0 && aperture > 0 && lens_mm > 0 && strength > 0;
    if (settings.focus) {
        float lens = lens_mm * 0.001f;
        settings.distance = fmaxf(distance, lens * 1.05f);
        // For small displacements around the focus plane the circle of confusion
        // increases as f^2 * delta / (N * subjectDistance * (subjectDistance - f)).
        // Approximate that slope with the Snowblind shader's symmetric depth ramp.
        float distance_per_metre_of_coc =
            aperture * settings.distance * (settings.distance - lens) / (lens * lens * strength);
        settings.tolerance = 0.000030f * distance_per_metre_of_coc; // full frame, 30 microns
        settings.transition = fmaxf(0.001f, 0.000150f * distance_per_metre_of_coc);
        float infinity_coc = lens * lens * strength / (aperture * (settings.distance - lens));
        // The separable filter operates at half resolution. Its widest native
        // kernel uses nine bilinear taps (17 texels) in each direction.
        settings.blur_size = fminf(12.0f, fmaxf(1.0f, infinity_coc * 1920.0f / 0.036f));
    }
    ReleaseSRWLockExclusive(&lock);
}

void optics_grade(float gain, float contrast, float saturation, float red, float green, float blue,
                  float noise, BOOL active) {
    AcquireSRWLockExclusive(&lock);
    settings.grade = active && gain >= 0.0f && contrast >= 0.0f && saturation >= 0.0f;
    settings.red = red * gain;
    settings.green = green * gain;
    settings.blue = blue * gain;
    settings.contrast = contrast;
    settings.saturation = saturation;
    settings.noise = fminf(1.0f, fmaxf(0.0f, noise));
    ReleaseSRWLockExclusive(&lock);
}

BOOL optics_grade_supported(void) {
    return installed && optics_frame_id() != 0;
}

unsigned optics_frame_id(void) {
    return (unsigned)InterlockedCompareExchange(&rendered_frames, 0, 0);
}

const char *optics_shader_name(void) {
    static const char *const variants[] = {"PostProcessHDR", "PostProcessHDRWithBlur",
                                           "PostProcessHDRWithBlurNoDistortion",
                                           "PostProcessHDRNoDistortion"};
    LONG variant = InterlockedCompareExchange(&last_variant, 0, 0);
    return variant >= 0 && variant < 4 ? variants[variant] : "unobserved";
}

float optics_scene_scale(void) {
    LONG bits = InterlockedCompareExchange(&scene_scale_bits, 0, 0);
    float scale;
    memcpy(&scale, &bits, sizeof scale);
    return scale;
}

void optics_clean_motion(BOOL clean) {
    AcquireSRWLockExclusive(&lock);
    settings.clean_motion = clean;
    ReleaseSRWLockExclusive(&lock);
}

static void photo_blur(void *source, int horizontal, void *target, float size, float brightness,
                       const float *viewport) {
    AcquireSRWLockShared(&lock);
    if (blur_installed && settings.focus) {
        size = settings.blur_size;
        brightness = 1.0f;
    }
    ReleaseSRWLockShared(&lock);
    original_blur(source, horizontal, target, size, brightness, viewport);
}

// The game has just rebuilt the HDR composite's constants. This call uploads
// c33 (near plane, depth range, clock, motion scale) immediately before drawing.
static void apply_post_constants(unsigned index, const ShaderRegister *misc, unsigned count) {
    void *shader = *bound_shader;
    for (int i = 0; i < 4; ++i) {
        if (shader && shader_entries[(0x1c + i) * 2] == shader) {
            InterlockedExchange(&last_variant, i);
            break;
        }
    }
    ShaderRegister frame_constants = *misc;
    LONG bits;
    float scale = registers[2].w;
    if (!(scale > 0.0f && scale <= 1.0f))
        scale = 1.0f;
    memcpy(&bits, &scale, sizeof bits);
    InterlockedExchange(&scene_scale_bits, bits);
    float far_clip = misc->x + misc->y;
    if (misc->y > 0.001f && far_clip > 0.01f && far_clip < 1.e7f) {
        memcpy(&bits, &far_clip, sizeof bits);
        InterlockedExchange(&far_clip_bits, bits);
    }
    AcquireSRWLockShared(&lock);
    // In the motion variant this is the multiplier on velocity-dependent
    // sampling offsets. Zero keeps every sample at the original position.
    if (settings.clean_motion)
        frame_constants.w = 0.0f;
    upload(index, &frame_constants, count);
    if (settings.focus && misc->y > 0.001f) {
        ShaderRegister focus = {fminf(1.0f, fmaxf(0.0f, (settings.distance - misc->x) / misc->y)),
                                settings.transition / misc->y, settings.tolerance / settings.transition,
                                1.0f};
        ShaderRegister blur = registers[2];
        blur.x = 1.0f;
        blur.y = 0.0f;
        upload(1, &focus, 1);
        upload(2, &blur, 1);
    }
    if (settings.grade) {
        ShaderRegister tint = registers[14];
        tint.x *= settings.red;
        tint.y *= settings.green;
        tint.z *= settings.blue;
        upload(14, &tint, 1);

        ShaderRegister curve = registers[28];
        curve.x = (1.0f + curve.x) * settings.contrast - 1.0f;
        curve.y = (1.0f + curve.y) * settings.saturation - 1.0f;
        upload(28, &curve, 1);
        if (settings.noise > 0.0f) {
            ShaderRegister noise = registers[15];
            // The shader multiplies color by noiseTexture * x + y.
            // Center additional grain at unity and preserve level-authored grain.
            noise.x += settings.noise;
            noise.y -= settings.noise * 0.5f;
            upload(15, &noise, 1);
        }
    }
    InterlockedIncrement(&rendered_frames);
    ReleaseSRWLockShared(&lock);
}

BOOL optics_install(unsigned char *base, OpticsRedirect redirect) {
    static const unsigned char ray_bytes[] = {0x4d, 0x85, 0xc9, 0x74, 0x15, 0x41, 0x83, 0xb9, 0x48, 0x05};
    static const unsigned char post_call[] = {0xe8, 0x45, 0x92, 0xff, 0xff};
    static const unsigned char blur_x_call[] = {0xe8, 0x37, 0x11, 0x00, 0x00};
    static const unsigned char blur_y_call[] = {0xe8, 0x0a, 0x11, 0x00, 0x00};
    static const unsigned char copy_offset[] = {0x48, 0x81, 0xc1, 0x01, 0x01, 0x00, 0x00};
    static const unsigned char reg_base[] = {0x48, 0x8d, 0x05, 0xb2, 0x35, 0x59, 0x01};
    if (!memcmp(base + 0x1c3000, ray_bytes, sizeof ray_bytes)) {
        raycast = (WorldRaycast)(base + 0x1c3000);
        physics_manager = (unsigned char **)(base + 0x1c679f0);
    }
    if (memcmp(base + 0x7b1c46, post_call, sizeof post_call) ||
        memcmp(base + 0x7c1aeb, copy_offset, sizeof copy_offset) ||
        memcmp(base + 0x7c1b87, reg_base, sizeof reg_base))
        return FALSE;
    // UploadConstants copies into &staging[(0x101 + registerIndex) * 16].
    registers = (ShaderRegister *)(base + 0x1d55140 + 0x1010);
    // SetPixelShader(0x1c..0x1f) selects the composite variant and records
    // its handle in the renderer's bound shader cache.
    bound_shader = (void **)(base + 0xb72290);
    shader_entries = (void **)(base + 0x1d5c4c8);
    upload = (UploadConstants)(base + 0x7aae90);
    original_blur = (BlurPass)(base + 0x7adf30);
    installed = redirect(base, base + 0x7b1c46, (uintptr_t)apply_post_constants);
    if (installed && !memcmp(base + 0x7acdf4, blur_x_call, sizeof blur_x_call) &&
        !memcmp(base + 0x7ace21, blur_y_call, sizeof blur_y_call)) {
        // Only override kernel parameters when both axes were installed. A
        // single successful redirection otherwise forwards untouched arguments.
        blur_installed = redirect(base, base + 0x7acdf4, (uintptr_t)photo_blur) &&
                         redirect(base, base + 0x7ace21, (uintptr_t)photo_blur);
    }
    return installed;
}
