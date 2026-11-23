#define WIN32_LEAN_AND_MEAN
#include "projection.h"
#include <math.h>
#include <string.h>

static int sample_offset(unsigned scale, unsigned phase, unsigned width, unsigned height,
                         float *dx, float *dy) {
    if (scale < 2 || scale > 4 || phase >= scale * scale || !width || !height)
        return 0;
    // Each native pixel covers scale x scale output pixels. Move the image so
    // its raster center samples the requested output center (D3D screen Y is down).
    *dx = (1.f - (2.f * (phase % scale) + 1.f) / scale) / width;
    *dy = ((2.f * (phase / scale) + 1.f) / scale - 1.f) / height;
    return 1;
}

static void shift_projection(float p[16], float dx, float dy) {
    // Native matrices multiply column vectors. Offset clip X/Y by clip W
    // to obtain the same subpixel translation at every depth.
    for (unsigned c = 0; c < 4; ++c) {
        p[c] += dx * p[12 + c];
        p[4 + c] += dy * p[12 + c];
    }
}

#ifdef _WIN32
typedef struct {
    float near_plane, far_plane, fov, aspect, zoom;
    Vec3 eye, target, up;
    float depth_override;
    unsigned fixed_aspect, orthographic;
} SceneLens;
typedef struct {
    SceneLens lens;
    float view[12], projection[16];
} SceneCamera;
typedef struct {
    unsigned char viewport[0x24];
    SceneLens lens;
    unsigned char transforms[0xe0 - 0x24 - sizeof(SceneLens)];
    float projection[16];
} CompositeCamera;
_Static_assert(offsetof(SceneCamera, projection) == 0x74, "native scene projection");
_Static_assert(offsetof(CompositeCamera, projection) == 0xe0, "native composite projection");

typedef void (*PrepareScene)(void *, SceneCamera *, unsigned, void *, void *, unsigned, unsigned, unsigned);
typedef void (*DeriveFrustum)(SceneCamera *);
typedef void (*CompositeScene)(const CompositeCamera *, void *, unsigned, unsigned);
static PrepareScene original_prepare;
static DeriveFrustum original_derive;
static CompositeScene original_composite;
static BOOL installed;
static SRWLOCK lock = SRWLOCK_INIT;
static uint32_t next_serial, requested, confirmed;
static float offset_x, offset_y, expected_matrix[16];
static BOOL have_matrix, need_pose;
static Camera wanted_pose;
static SceneCamera *prepared_camera;

static BOOL matches_direction(Vec3 v, Vec3 unit) {
    float length = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
    return length > .001f && fabsf(v.x / length - unit.x) <= .0002f &&
           fabsf(v.y / length - unit.y) <= .0002f && fabsf(v.z / length - unit.z) <= .0002f;
}

static BOOL matches_pose(const SceneLens *lens) {
    if (!need_pose)
        return TRUE;
    Vec3 look = {lens->target.x - lens->eye.x, lens->target.y - lens->eye.y, lens->target.z - lens->eye.z};
    return fabsf(lens->eye.x - wanted_pose.position.x) <= .001f &&
           fabsf(lens->eye.y - wanted_pose.position.y) <= .001f &&
           fabsf(lens->eye.z - wanted_pose.position.z) <= .001f &&
           fabsf(lens->fov - wanted_pose.fov) <= .0001f &&
           matches_direction(look, wanted_pose.forward) && matches_direction(lens->up, wanted_pose.up);
}

static void prepare_scene(void *scene, SceneCamera *camera, unsigned count, void *objects,
                          void *bounds, unsigned flags, unsigned multi, unsigned index) {
    AcquireSRWLockExclusive(&lock);
    BOOL shift = requested && !camera->lens.orthographic && matches_pose(&camera->lens);
    prepared_camera = shift ? camera : NULL;
    if (shift) {
        shift_projection(camera->projection, offset_x, offset_y);
        if (camera->lens.fixed_aspect) {
            memcpy(expected_matrix, camera->projection, sizeof expected_matrix);
            have_matrix = TRUE;
        }
    }
    ReleaseSRWLockExclusive(&lock);
    // This camera is the renderer's stack copy. Refresh its culling planes
    // before the scene consumes them. The gameplay camera retains its own lens.
    if (shift)
        original_derive(camera);
    original_prepare(scene, camera, count, objects, bounds, flags, multi, index);
}

static void derive_viewport(SceneCamera *camera) {
    AcquireSRWLockExclusive(&lock);
    // Automatic-aspect viewports rebuild the projection after scene setup.
    // A phase transition clears prepared_camera so an in-flight old view
    // cannot be acknowledged as part of the new phase.
    if (requested && camera == prepared_camera) {
        shift_projection(camera->projection, offset_x, offset_y);
        memcpy(expected_matrix, camera->projection, sizeof expected_matrix);
        have_matrix = TRUE;
    }
    ReleaseSRWLockExclusive(&lock);
    original_derive(camera);
}

static void composite_scene(const CompositeCamera *camera, void *target, unsigned pass, unsigned mode) {
    // This packet's projection feeds the HDR pass's c29 inverse matrix. Its
    // eye, target, up and FOV distinguish repeated phases at different poses.
    AcquireSRWLockExclusive(&lock);
    uint32_t serial = requested && have_matrix && matches_pose(&camera->lens) &&
                      !memcmp(camera->projection, expected_matrix, sizeof expected_matrix) ? requested : 0;
    ReleaseSRWLockExclusive(&lock);
    original_composite(camera, target, pass, mode);
    AcquireSRWLockExclusive(&lock);
    confirmed = requested == serial ? serial : 0;
    ReleaseSRWLockExclusive(&lock);
}

BOOL projection_install(unsigned char *base, BOOL (*redirect)(unsigned char *, unsigned char *, uintptr_t)) {
    static const unsigned char prepare[] = {0x48, 0x89, 0x44, 0x24, 0x20, 0xe8, 0xcc, 0xb3, 0x0f, 0};
    static const unsigned char derive[] = {0xe8, 0x81, 0x40, 0x80, 0, 0x48, 0x8d, 0x4c, 0x24, 0x50,
                                           0xe8, 0x07, 0x65, 0x6a, 0};
    static const unsigned char composite[] = {0x48, 0x8d, 0x88, 0x40, 0xb7, 0, 0, 0x41, 0x0f, 0x95, 0xc1,
                                              0x48, 0x03, 0xcf, 0xe8, 0x20, 0x42, 0, 0};
    static const unsigned char projection_load[] = {0x45, 0x0f, 0x10, 0x8d, 0xe0, 0, 0, 0};
    if (memcmp(base + 0x10f36a, prepare, sizeof prepare) ||
        memcmp(base + 0x10f48a, derive, sizeof derive) ||
        memcmp(base + 0x7ace6d, composite, sizeof composite) ||
        memcmp(base + 0x7b1b0c, projection_load, sizeof projection_load))
        return FALSE;
    original_prepare = (PrepareScene)(base + 0x20a740);
    original_derive = (DeriveFrustum)(base + 0x7b59a0);
    original_composite = (CompositeScene)(base + 0x7b10a0);
    installed = redirect(base, base + 0x10f36f, (uintptr_t)prepare_scene) &&
                redirect(base, base + 0x10f494, (uintptr_t)derive_viewport) &&
                redirect(base, base + 0x7ace7b, (uintptr_t)composite_scene);
    return installed;
}

BOOL projection_available(void) {
    return installed;
}

uint32_t projection_phase(unsigned scale, unsigned phase, unsigned width, unsigned height) {
    return projection_phase_pose(scale, phase, width, height, NULL);
}

uint32_t projection_phase_pose(unsigned scale, unsigned phase, unsigned width, unsigned height, const Camera *pose) {
    float dx, dy;
    if (!installed || !sample_offset(scale, phase, width, height, &dx, &dy))
        return 0;
    AcquireSRWLockExclusive(&lock);
    if (++next_serial == 0)
        ++next_serial;
    uint32_t serial = requested = next_serial;
    offset_x = dx;
    offset_y = dy;
    need_pose = pose != NULL;
    if (pose)
        wanted_pose = *pose;
    have_matrix = FALSE;
    prepared_camera = NULL;
    confirmed = 0;
    ReleaseSRWLockExclusive(&lock);
    return serial;
}

BOOL projection_ready(uint32_t serial) {
    AcquireSRWLockExclusive(&lock);
    BOOL ready = serial && serial == confirmed && serial == requested;
    if (ready)
        confirmed = 0;
    ReleaseSRWLockExclusive(&lock);
    return ready;
}

void projection_reset(void) {
    AcquireSRWLockExclusive(&lock);
    requested = confirmed = 0;
    have_matrix = FALSE;
    prepared_camera = NULL;
    ReleaseSRWLockExclusive(&lock);
}
#endif
