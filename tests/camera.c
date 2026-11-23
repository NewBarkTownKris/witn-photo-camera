#include <assert.h>
#include "../src/camera.h"

int main(void) {
    GameView original = {.fov = 1.18f, .zoom = 1.7f, .eye = {4, 6, 8}, .target = {4, 16, 8}, .up = {0, 0, 1}};
    Camera camera;
    assert(camera_begin(&camera, &original));
    float game_projection = original.zoom / tanf(original.fov * .5f);
    camera_apply(&camera, &original);
    assert(original.zoom == 1);
    assert(fabsf(1.f / tanf(original.fov * .5f) - game_projection) < .00001f);
    camera.fov = 2.f * atanf(18.f / 135.f);
    camera_move(&camera, .2f, .12f, .3f, 5.f, 1.f, -2.f);
    camera_apply(&camera, &original);
    assert(fabsf(18.f / tanf(original.fov * .5f) - 135.f) < .001f);
    assert(fabsf(dot(camera.forward, camera.up)) < .00001f);
    assert(dot(camera.forward, camera.forward) > .99999f);
    return 0;
}
