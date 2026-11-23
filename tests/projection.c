/* cc -std=c11 tests/projection.c -lm -o /tmp/witn-projection && /tmp/witn-projection
   The Windows build also replays a queued viewport through the native callback ABI. */
#include <assert.h>
#include "../src/projection.c"

static const float lens[16] = {
    1.4f, 0, 0, 0, 0, 0, 2.2f, 0, 0, 1.001f, 0, -.1001f, 0, 1, 0, 0
};
static void check_samples(unsigned scale, unsigned w, unsigned h) {
    for (unsigned phase = 0; phase < scale * scale; ++phase) {
        float dx, dy, p[16];
        assert(sample_offset(scale, phase, w, h, &dx, &dy));
        memcpy(p, lens, sizeof p);
        shift_projection(p, dx, dy);
        assert(!memcmp(p + 8, lens + 8, 8 * sizeof(float)));
        for (unsigned depth = 1; depth <= 100; depth *= 10) {
            // Project the center ray. The sample it now represents is opposite
            // the geometry translation and must land on this output pixel center.
            double w_clip = p[13] * depth;
            double raster_x = p[1] * depth / w_clip * w * .5;
            double raster_y = -p[5] * depth / w_clip * h * .5;
            assert(fabs((.5 - raster_x) * scale - (phase % scale + .5)) < 1.e-5);
            assert(fabs((.5 - raster_y) * scale - (phase / scale + .5)) < 1.e-5);
        }
    }
}

#ifdef _WIN32
static void prepare_fixture(void *scene, SceneCamera *camera, unsigned count, void *objects,
                            void *bounds, unsigned flags, unsigned multi, unsigned index) {
    assert(scene == NULL && camera != NULL && count == 1 && !objects && !bounds);
    assert(flags == 5 && multi == 6 && index == 7);
}
static void derive_fixture(SceneCamera *camera) { assert(camera != NULL); }
static void composite_fixture(const CompositeCamera *camera, void *target, unsigned pass, unsigned mode) {
    assert(camera && !target && pass == 1 && mode == 2);
}
static void begin_view(SceneCamera *camera) {
    memcpy(camera->projection, lens, sizeof lens);
    prepare_scene(NULL, camera, 1, NULL, NULL, 5, 6, 7);
}
static void submit_view(const SceneCamera *camera, const float p[16]) {
    CompositeCamera packet = {.lens = camera->lens};
    memcpy(packet.projection, p, sizeof packet.projection);
    composite_scene(&packet, NULL, 1, 2);
}
static void check_frame_order(void) {
    SceneCamera camera = {0};
    original_prepare = prepare_fixture;
    original_derive = derive_fixture;
    original_composite = composite_fixture;
    assert(!projection_phase(2, 0, 2560, 1440));
    installed = TRUE;
    uint32_t a = projection_phase(2, 0, 2560, 1440);
    begin_view(&camera);
    memcpy(camera.projection, lens, sizeof lens); // auto-aspect rebuild
    derive_viewport(&camera);
    submit_view(&camera, lens); // previous packet arrived after this one was prepared
    assert(!projection_ready(a));
    submit_view(&camera, camera.projection);
    assert(projection_ready(a) && !projection_ready(a));
    begin_view(&camera);
    uint32_t b = projection_phase(2, 1, 2560, 1440); // between setup and rebuild
    memcpy(camera.projection, lens, sizeof lens);
    derive_viewport(&camera);
    submit_view(&camera, camera.projection);
    assert(!projection_ready(a) && !projection_ready(b));
    camera.lens.fixed_aspect = 1;
    begin_view(&camera); // custom aspect bypasses the second rebuild
    submit_view(&camera, camera.projection);
    assert(projection_ready(b));
    Camera replay = {{1,2,3}, {0,1,0}, {0,0,1}, 1.f};
    uint32_t c = projection_phase_pose(2, 1, 2560, 1440, &replay);
    camera.lens.eye = replay.position;
    camera.lens.target = (Vec3){1,12,3};
    camera.lens.up = replay.up;
    camera.lens.fov = replay.fov;
    begin_view(&camera);
    SceneCamera old_pose = camera;
    old_pose.lens.eye.x += .02f;
    submit_view(&old_pose, camera.projection);
    assert(!projection_ready(c));
    old_pose = camera;
    old_pose.lens.up = (Vec3){.5f,0,.8660254f};
    submit_view(&old_pose, camera.projection);
    assert(!projection_ready(c));
    submit_view(&camera, camera.projection);
    assert(projection_ready(c));
    camera.lens.orthographic = 1;
    begin_view(&camera);
    assert(!memcmp(camera.projection, lens, sizeof lens));
    projection_reset();
    submit_view(&camera, lens);
    assert(!projection_ready(b));
    camera.lens.orthographic = 0;
    begin_view(&camera);
    assert(!memcmp(camera.projection, lens, sizeof lens));
}
#endif

int main(void) {
    float dx, dy;
    assert(!sample_offset(1, 0, 1, 1, &dx, &dy));
    assert(!sample_offset(5, 0, 1, 1, &dx, &dy));
    assert(!sample_offset(2, 4, 1, 1, &dx, &dy));
    assert(!sample_offset(2, 0, 0, 1, &dx, &dy));
    assert(!sample_offset(2, 0, 1, 0, &dx, &dy));
    for (unsigned scale = 2; scale <= 4; ++scale) {
        check_samples(scale, 2203, 1237);
        check_samples(scale, 16384, 16384);
    }
#ifdef _WIN32
    check_frame_order();
#endif
    return 0;
}
