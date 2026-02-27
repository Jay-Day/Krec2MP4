#include "vidext.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <cstdio>

static SDL_Window* s_window = nullptr;
static SDL_GLContext s_gl_context = nullptr;
static bool s_initialized = false;

// GL attribute storage
static int s_gl_doublebuffer = 1;
static int s_gl_depth_size = 24;
static int s_gl_red_size = 8;
static int s_gl_green_size = 8;
static int s_gl_blue_size = 8;
static int s_gl_alpha_size = 8;
static int s_gl_swap_interval = 0;
static int s_gl_multisample_buffers = 0;
static int s_gl_multisample_samples = 0;
static int s_gl_major = 3;
static int s_gl_minor = 3;
static int s_gl_profile = M64P_GL_CONTEXT_PROFILE_COMPATIBILITY;

static m64p_error VidExt_Init(void) {
    if (s_initialized) return M64ERR_ALREADY_INIT;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "VidExt: SDL_Init(VIDEO) failed: %s\n", SDL_GetError());
        return M64ERR_SYSTEM_FAIL;
    }

    s_initialized = true;
    return M64ERR_SUCCESS;
}

static m64p_error VidExt_InitWithRenderMode(m64p_render_mode mode) {
    if (mode == M64P_RENDER_VULKAN) {
        return M64ERR_UNSUPPORTED;
    }
    return VidExt_Init();
}

static m64p_error VidExt_Quit(void) {
    if (s_gl_context) {
        SDL_GL_DestroyContext(s_gl_context);
        s_gl_context = nullptr;
    }
    if (s_window) {
        SDL_DestroyWindow(s_window);
        s_window = nullptr;
    }
    s_initialized = false;
    return M64ERR_SUCCESS;
}

static m64p_error VidExt_ListModes(m64p_2d_size* sizes, int* num) {
    return M64ERR_UNSUPPORTED;
}

static m64p_error VidExt_ListRates(m64p_2d_size size, int* num_rates, int* rates) {
    return M64ERR_UNSUPPORTED;
}

static m64p_error VidExt_SetMode(int width, int height, int bpp, int screen_mode, int flags) {
    // Set GL attributes before window creation
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, s_gl_doublebuffer);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, s_gl_depth_size);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, s_gl_red_size);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, s_gl_green_size);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, s_gl_blue_size);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, s_gl_alpha_size);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, s_gl_multisample_buffers);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, s_gl_multisample_samples);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, s_gl_major);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, s_gl_minor);

    SDL_GLProfile sdl_profile;
    switch (s_gl_profile) {
        case M64P_GL_CONTEXT_PROFILE_CORE:
            sdl_profile = SDL_GL_CONTEXT_PROFILE_CORE;
            break;
        case M64P_GL_CONTEXT_PROFILE_ES:
            sdl_profile = SDL_GL_CONTEXT_PROFILE_ES;
            break;
        default:
            sdl_profile = SDL_GL_CONTEXT_PROFILE_COMPATIBILITY;
            break;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, sdl_profile);

    // Destroy existing window if resizing
    if (s_gl_context) {
        SDL_GL_DestroyContext(s_gl_context);
        s_gl_context = nullptr;
    }
    if (s_window) {
        SDL_DestroyWindow(s_window);
        s_window = nullptr;
    }

    // Create hidden window for headless rendering
    s_window = SDL_CreateWindow("Krec2MP4", width, height,
                                SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!s_window) {
        fprintf(stderr, "VidExt: SDL_CreateWindow failed: %s\n", SDL_GetError());
        // Fallback: try minimized instead of hidden
        s_window = SDL_CreateWindow("Krec2MP4", width, height,
                                    SDL_WINDOW_OPENGL | SDL_WINDOW_MINIMIZED);
        if (!s_window) {
            fprintf(stderr, "VidExt: SDL_CreateWindow fallback also failed: %s\n", SDL_GetError());
            return M64ERR_SYSTEM_FAIL;
        }
    }

    s_gl_context = SDL_GL_CreateContext(s_window);
    if (!s_gl_context) {
        fprintf(stderr, "VidExt: SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(s_window);
        s_window = nullptr;
        return M64ERR_SYSTEM_FAIL;
    }

    SDL_GL_MakeCurrent(s_window, s_gl_context);
    SDL_GL_SetSwapInterval(0); // No vsync for max speed

    return M64ERR_SUCCESS;
}

static m64p_error VidExt_SetModeWithRate(int w, int h, int rate, int bpp, int mode, int flags) {
    return M64ERR_UNSUPPORTED;
}

static m64p_function VidExt_GLGetProc(const char* proc) {
    return (m64p_function)SDL_GL_GetProcAddress(proc);
}

static m64p_error VidExt_GLSetAttr(m64p_GLattr attr, int value) {
    switch (attr) {
        case M64P_GL_DOUBLEBUFFER:          s_gl_doublebuffer = value; break;
        case M64P_GL_DEPTH_SIZE:            s_gl_depth_size = value; break;
        case M64P_GL_RED_SIZE:              s_gl_red_size = value; break;
        case M64P_GL_GREEN_SIZE:            s_gl_green_size = value; break;
        case M64P_GL_BLUE_SIZE:             s_gl_blue_size = value; break;
        case M64P_GL_ALPHA_SIZE:            s_gl_alpha_size = value; break;
        case M64P_GL_SWAP_CONTROL:          s_gl_swap_interval = 0; break; // always 0 for headless
        case M64P_GL_MULTISAMPLEBUFFERS:    s_gl_multisample_buffers = value; break;
        case M64P_GL_MULTISAMPLESAMPLES:    s_gl_multisample_samples = value; break;
        case M64P_GL_CONTEXT_MAJOR_VERSION: s_gl_major = value; break;
        case M64P_GL_CONTEXT_MINOR_VERSION: s_gl_minor = value; break;
        case M64P_GL_CONTEXT_PROFILE_MASK:  s_gl_profile = value; break;
        default: break;
    }
    return M64ERR_SUCCESS;
}

static m64p_error VidExt_GLGetAttr(m64p_GLattr attr, int* value) {
    switch (attr) {
        case M64P_GL_DOUBLEBUFFER:          *value = s_gl_doublebuffer; break;
        case M64P_GL_BUFFER_SIZE:           *value = s_gl_red_size + s_gl_green_size + s_gl_blue_size + s_gl_alpha_size; break;
        case M64P_GL_DEPTH_SIZE:            *value = s_gl_depth_size; break;
        case M64P_GL_RED_SIZE:              *value = s_gl_red_size; break;
        case M64P_GL_GREEN_SIZE:            *value = s_gl_green_size; break;
        case M64P_GL_BLUE_SIZE:             *value = s_gl_blue_size; break;
        case M64P_GL_ALPHA_SIZE:            *value = s_gl_alpha_size; break;
        case M64P_GL_SWAP_CONTROL:          *value = s_gl_swap_interval; break;
        case M64P_GL_MULTISAMPLEBUFFERS:    *value = s_gl_multisample_buffers; break;
        case M64P_GL_MULTISAMPLESAMPLES:    *value = s_gl_multisample_samples; break;
        case M64P_GL_CONTEXT_MAJOR_VERSION: *value = s_gl_major; break;
        case M64P_GL_CONTEXT_MINOR_VERSION: *value = s_gl_minor; break;
        case M64P_GL_CONTEXT_PROFILE_MASK:  *value = s_gl_profile; break;
        default: break;
    }
    return M64ERR_SUCCESS;
}

static m64p_error VidExt_GLSwapBuf(void) {
    // For headless capture, just ensure rendering is complete
    // Don't actually swap - we capture via ReadScreen2 in the frame callback
    glFinish();
    return M64ERR_SUCCESS;
}

static m64p_error VidExt_SetCaption(const char* title) {
    return M64ERR_SUCCESS;
}

static m64p_error VidExt_ToggleFS(void) {
    return M64ERR_SUCCESS;
}

static m64p_error VidExt_ResizeWindow(int w, int h) {
    return M64ERR_SUCCESS;
}

static uint32_t VidExt_GLGetDefaultFramebuffer(void) {
    return 0;
}

static m64p_error VidExt_VKGetSurface(void** surface, void* instance) {
    return M64ERR_UNSUPPORTED;
}

static m64p_error VidExt_VKGetInstanceExtensions(const char** extensions[], uint32_t* num) {
    return M64ERR_UNSUPPORTED;
}

m64p_video_extension_functions vidext_get_functions() {
    m64p_video_extension_functions funcs = {};
    funcs.Functions = 17;
    funcs.VidExtFuncInit = VidExt_Init;
    funcs.VidExtFuncInitWithRenderMode = VidExt_InitWithRenderMode;
    funcs.VidExtFuncQuit = VidExt_Quit;
    funcs.VidExtFuncListModes = VidExt_ListModes;
    funcs.VidExtFuncListRates = VidExt_ListRates;
    funcs.VidExtFuncSetMode = VidExt_SetMode;
    funcs.VidExtFuncSetModeWithRate = VidExt_SetModeWithRate;
    funcs.VidExtFuncGLGetProc = VidExt_GLGetProc;
    funcs.VidExtFuncGLSetAttr = VidExt_GLSetAttr;
    funcs.VidExtFuncGLGetAttr = VidExt_GLGetAttr;
    funcs.VidExtFuncGLSwapBuf = VidExt_GLSwapBuf;
    funcs.VidExtFuncSetCaption = VidExt_SetCaption;
    funcs.VidExtFuncToggleFS = VidExt_ToggleFS;
    funcs.VidExtFuncResizeWindow = VidExt_ResizeWindow;
    funcs.VidExtFuncGLGetDefaultFramebuffer = VidExt_GLGetDefaultFramebuffer;
    funcs.VidExtFuncVKGetSurface = VidExt_VKGetSurface;
    funcs.VidExtFuncVKGetInstanceExtensions = VidExt_VKGetInstanceExtensions;
    return funcs;
}

void vidext_shutdown() {
    VidExt_Quit();
    SDL_Quit();
}
