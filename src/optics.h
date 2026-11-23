#ifndef WITN_OPTICS_H
#define WITN_OPTICS_H
#include <windows.h>
#include <stdint.h>

typedef BOOL (*OpticsRedirect)(unsigned char *, unsigned char *, uintptr_t);

BOOL optics_install(unsigned char *base, OpticsRedirect redirect);
void optics_configure(BOOL enabled, float distance, float aperture, float lens_mm, unsigned strength,
                      float near_plane, float far_plane);
void optics_grade(float gain, float contrast, float saturation, float red, float green, float blue,
                  float noise, BOOL active);
BOOL optics_grade_supported(void);
unsigned optics_frame_id(void);
const char *optics_shader_name(void);
float optics_scene_scale(void);
void optics_clean_motion(BOOL clean);
float optics_focus_center(const float eye[3], const float forward[3], float max_distance);

#endif
