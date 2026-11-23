#ifndef WITN_PROJECTION_H
#define WITN_PROJECTION_H

#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
#include "pose.h"
BOOL projection_install(unsigned char *base, BOOL (*redirect)(unsigned char *, unsigned char *, uintptr_t));
BOOL projection_available(void);
uint32_t projection_phase(unsigned scale, unsigned phase, unsigned width, unsigned height);
uint32_t projection_phase_pose(unsigned scale, unsigned phase, unsigned width, unsigned height, const Camera *pose);
// Consumes the matching scene pass. Call once per submitted frame.
BOOL projection_ready(uint32_t serial);
void projection_reset(void);
#endif

#endif
