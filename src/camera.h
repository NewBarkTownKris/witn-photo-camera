#ifndef WITN_CAMERA_H
#define WITN_CAMERA_H

#include <math.h>

#include "pose.h"
typedef struct {
    float near_plane, far_plane, fov, aspect, zoom;
    Vec3 eye, target, up;
    float depth_override, focal_length;
    unsigned extra;
} GameView;


static Vec3 add(Vec3 a, Vec3 b) { return (Vec3){a.x+b.x, a.y+b.y, a.z+b.z}; }
static Vec3 scale(Vec3 v, float k) { return (Vec3){v.x*k, v.y*k, v.z*k}; }
static float dot(Vec3 a, Vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static Vec3 cross(Vec3 a, Vec3 b) {
    return (Vec3){a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x};
}
static Vec3 unit(Vec3 v) { return scale(v, 1.0f/sqrtf(dot(v,v))); }
static Vec3 rotate(Vec3 v, Vec3 axis, float angle) {
    return add(add(scale(v,cosf(angle)),scale(cross(axis,v),sinf(angle))),
               scale(axis,dot(axis,v)*(1.0f-cosf(angle))));
}

static int camera_begin(Camera *camera, const GameView *view) {
    Vec3 look = add(view->target, scale(view->eye,-1.0f));
    Vec3 right = cross(look, view->up);
    float look_squared = dot(look,look);
    float right_squared = dot(right,right);
    if (!(look_squared > 0.0001f && look_squared < 1.0e14f &&
          right_squared > 0.0001f && right_squared < 1.0e14f &&
          view->fov > 0.01f && view->fov < 3.10f)) return 0;
    camera->position = view->eye;
    camera->forward = unit(look);
    camera->up = unit(cross(unit(right),camera->forward));
    /* Snowblind's projection multiplies cot(HFOV/2) by zoom. Keep the
       entry framing while representing the detached lens as effective HFOV. */
    camera->fov = view->zoom > .001f && view->zoom < 1000.f
        ? 2.f * atanf(tanf(view->fov * .5f) / view->zoom) : view->fov;
    return 1;
}

static void camera_move(Camera *camera, float yaw, float pitch, float roll,
                        float forward, float rightward, float vertical) {
    camera->forward = rotate(camera->forward,camera->up,yaw);
    Vec3 right = unit(cross(camera->forward,camera->up));
    camera->forward = unit(rotate(camera->forward,right,pitch));
    camera->up = unit(rotate(cross(right,camera->forward),camera->forward,roll));
    right = cross(camera->forward,camera->up);
    camera->position = add(camera->position,
        add(add(scale(camera->forward,forward),scale(right,rightward)),
            scale(camera->up,vertical)));
}

static void camera_apply(const Camera *camera, GameView *view) {
    view->eye = camera->position;
    view->target = add(camera->position,scale(camera->forward,10.0f));
    view->up = camera->up;
    view->fov = camera->fov;
    view->zoom = 1.f;
}

#endif
