#pragma once
#include "emulator.h"

// Returns the video extension function table for SDL3 headless OpenGL.
m64p_video_extension_functions vidext_get_functions();

// Cleanup SDL resources (called at shutdown).
void vidext_shutdown();
