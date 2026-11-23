#ifndef WITN_POSE_H
#define WITN_POSE_H

typedef struct { float x, y, z; } Vec3;

typedef struct {
    Vec3 position, forward, up;
    float fov;
} Camera;

#endif
