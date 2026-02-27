#pragma once
#include <cstdint>
#include <string>
#include <functional>

// Forward declarations matching mupen64plus API types
extern "C" {

// --- m64p_types (subset needed for our tool) ---
typedef void* m64p_handle;
typedef void (*m64p_function)(void);
typedef void (*m64p_frame_callback)(unsigned int FrameIndex);

typedef enum { M64TYPE_INT = 1, M64TYPE_FLOAT, M64TYPE_BOOL, M64TYPE_STRING } m64p_type;
typedef enum { M64MSG_ERROR = 1, M64MSG_WARNING, M64MSG_INFO, M64MSG_STATUS, M64MSG_VERBOSE } m64p_msg_level;

typedef enum {
    M64ERR_SUCCESS = 0, M64ERR_NOT_INIT, M64ERR_ALREADY_INIT, M64ERR_INCOMPATIBLE,
    M64ERR_INPUT_ASSERT, M64ERR_INPUT_INVALID, M64ERR_INPUT_NOT_FOUND, M64ERR_NO_MEMORY,
    M64ERR_FILES, M64ERR_INTERNAL, M64ERR_INVALID_STATE, M64ERR_PLUGIN_FAIL,
    M64ERR_SYSTEM_FAIL, M64ERR_UNSUPPORTED, M64ERR_WRONG_TYPE
} m64p_error;

typedef enum {
    M64PLUGIN_NULL = 0, M64PLUGIN_RSP = 1, M64PLUGIN_GFX, M64PLUGIN_AUDIO, M64PLUGIN_INPUT, M64PLUGIN_CORE
} m64p_plugin_type;

typedef enum { M64EMU_STOPPED = 1, M64EMU_RUNNING, M64EMU_PAUSED } m64p_emu_state;
typedef enum { M64VIDEO_NONE = 1, M64VIDEO_WINDOWED, M64VIDEO_FULLSCREEN } m64p_video_mode;

typedef enum {
    M64CORE_EMU_STATE = 1, M64CORE_VIDEO_MODE, M64CORE_SAVESTATE_SLOT, M64CORE_SPEED_FACTOR,
    M64CORE_SPEED_LIMITER, M64CORE_VIDEO_SIZE, M64CORE_AUDIO_VOLUME, M64CORE_AUDIO_MUTE,
    M64CORE_INPUT_GAMESHARK, M64CORE_STATE_LOADCOMPLETE, M64CORE_STATE_SAVECOMPLETE,
    M64CORE_SCREENSHOT_CAPTURED,
} m64p_core_param;

typedef enum {
    M64CMD_NOP = 0, M64CMD_ROM_OPEN, M64CMD_ROM_CLOSE, M64CMD_ROM_GET_HEADER,
    M64CMD_ROM_GET_SETTINGS, M64CMD_EXECUTE, M64CMD_STOP, M64CMD_PAUSE, M64CMD_RESUME,
    M64CMD_CORE_STATE_QUERY, M64CMD_STATE_LOAD, M64CMD_STATE_SAVE, M64CMD_STATE_SET_SLOT,
    M64CMD_SEND_SDL_KEYDOWN, M64CMD_SEND_SDL_KEYUP, M64CMD_SET_FRAME_CALLBACK,
    M64CMD_TAKE_NEXT_SCREENSHOT, M64CMD_CORE_STATE_SET, M64CMD_READ_SCREEN, M64CMD_RESET,
    M64CMD_ADVANCE_FRAME, M64CMD_SET_MEDIA_LOADER, M64CMD_NETPLAY_INIT,
    M64CMD_NETPLAY_CONTROL_PLAYER, M64CMD_NETPLAY_GET_VERSION, M64CMD_NETPLAY_CLOSE,
    M64CMD_PIF_OPEN, M64CMD_ROM_SET_SETTINGS, M64CMD_DISK_OPEN, M64CMD_DISK_CLOSE
} m64p_command;

typedef enum {
    M64P_GL_DOUBLEBUFFER = 1, M64P_GL_BUFFER_SIZE, M64P_GL_DEPTH_SIZE,
    M64P_GL_RED_SIZE, M64P_GL_GREEN_SIZE, M64P_GL_BLUE_SIZE, M64P_GL_ALPHA_SIZE,
    M64P_GL_SWAP_CONTROL, M64P_GL_MULTISAMPLEBUFFERS, M64P_GL_MULTISAMPLESAMPLES,
    M64P_GL_CONTEXT_MAJOR_VERSION, M64P_GL_CONTEXT_MINOR_VERSION, M64P_GL_CONTEXT_PROFILE_MASK
} m64p_GLattr;

typedef enum { M64P_GL_CONTEXT_PROFILE_CORE, M64P_GL_CONTEXT_PROFILE_COMPATIBILITY, M64P_GL_CONTEXT_PROFILE_ES } m64p_GLContextType;
typedef enum { M64P_RENDER_OPENGL = 0, M64P_RENDER_VULKAN } m64p_render_mode;
typedef enum { M64VIDEOFLAG_SUPPORT_RESIZING = 1 } m64p_video_flags;

typedef struct { unsigned int uiWidth; unsigned int uiHeight; } m64p_2d_size;

typedef struct {
    unsigned int Functions;
    m64p_error    (*VidExtFuncInit)(void);
    m64p_error    (*VidExtFuncQuit)(void);
    m64p_error    (*VidExtFuncListModes)(m64p_2d_size*, int*);
    m64p_error    (*VidExtFuncListRates)(m64p_2d_size, int*, int*);
    m64p_error    (*VidExtFuncSetMode)(int, int, int, int, int);
    m64p_error    (*VidExtFuncSetModeWithRate)(int, int, int, int, int, int);
    m64p_function (*VidExtFuncGLGetProc)(const char*);
    m64p_error    (*VidExtFuncGLSetAttr)(m64p_GLattr, int);
    m64p_error    (*VidExtFuncGLGetAttr)(m64p_GLattr, int*);
    m64p_error    (*VidExtFuncGLSwapBuf)(void);
    m64p_error    (*VidExtFuncSetCaption)(const char*);
    m64p_error    (*VidExtFuncToggleFS)(void);
    m64p_error    (*VidExtFuncResizeWindow)(int, int);
    uint32_t      (*VidExtFuncGLGetDefaultFramebuffer)(void);
    m64p_error    (*VidExtFuncInitWithRenderMode)(m64p_render_mode);
    m64p_error    (*VidExtFuncVKGetSurface)(void**, void*);
    m64p_error    (*VidExtFuncVKGetInstanceExtensions)(const char**[], uint32_t*);
} m64p_video_extension_functions;

typedef void (*ptr_DebugCallback)(void* Context, int level, const char* message);
typedef void (*ptr_StateCallback)(void* Context, m64p_core_param param_type, int new_value);

// PIF structures for input injection
struct pif_channel {
    void* jbd;
    const void* ijbd;
    uint8_t* tx;
    uint8_t* tx_buf;
    uint8_t* rx;
    uint8_t* rx_buf;
};

struct pif {
    uint8_t* base;
    uint8_t* ram;
    struct pif_channel channels[5]; // PIF_CHANNELS_COUNT = 5 in actual header
};

typedef void (*pif_sync_callback_t)(struct pif*);

} // extern "C"

// Mupen64plus core function pointer types
#ifdef _WIN32
#include <windows.h>
typedef HMODULE m64p_dynlib_handle;
#else
typedef void* m64p_dynlib_handle;
#endif

typedef m64p_error (*ptr_CoreStartup)(int, const char*, const char*, void*, ptr_DebugCallback, void*, ptr_StateCallback);
typedef m64p_error (*ptr_CoreShutdown)(void);
typedef m64p_error (*ptr_CoreAttachPlugin)(m64p_plugin_type, m64p_dynlib_handle);
typedef m64p_error (*ptr_CoreDetachPlugin)(m64p_plugin_type);
typedef m64p_error (*ptr_CoreDoCommand)(m64p_command, int, void*);
typedef m64p_error (*ptr_CoreOverrideVidExt)(m64p_video_extension_functions*);
typedef m64p_error (*ptr_ConfigOpenSection)(const char*, m64p_handle*);
typedef m64p_error (*ptr_ConfigSetParameter)(m64p_handle, const char*, m64p_type, const void*);
typedef int        (*ptr_ConfigGetParamInt)(m64p_handle, const char*);
typedef m64p_error (*ptr_ConfigSetDefaultInt)(m64p_handle, const char*, int, const char*);
typedef m64p_error (*ptr_ConfigSetDefaultBool)(m64p_handle, const char*, int, const char*);
typedef m64p_error (*ptr_ConfigSetDefaultString)(m64p_handle, const char*, const char*, const char*);

// Plugin function types
typedef m64p_error (*ptr_PluginStartup)(m64p_dynlib_handle, void*, ptr_DebugCallback);
typedef m64p_error (*ptr_PluginShutdown)(void);

// RMG-K extension
typedef void (*ptr_set_pif_sync_callback)(pif_sync_callback_t);

// GFX plugin ReadScreen2 type
typedef void (*ptr_ReadScreen2)(void* dest, int* width, int* height, int front);

using EmulatorLogCallback = std::function<void(int level, const char* msg)>;

// Set a log callback for emulator debug messages. nullptr restores default stderr output.
void emulator_set_log_callback(EmulatorLogCallback cb);

struct EmulatorConfig {
    std::string core_path = "./Core/mupen64plus.dll";
    std::string plugin_dir = "./Plugin/";
    std::string rom_path;
    std::string data_dir = "./Data/";
    std::string audio_plugin_path; // optional override (empty = use RMG-Audio from plugin_dir)
    int res_width = 640;
    int res_height = 480;
    int msaa = 0;       // 0=off, 2, 4, 8, 16
    int aniso = 0;      // 0=off, 2, 4, 8, 16
    bool verbose = false;
};

class Emulator {
public:
    bool init(const EmulatorConfig& config);
    bool open_rom(const std::string& rom_path);
    bool attach_plugins();
    void apply_deterministic_settings();
    void configure_controllers_for_replay(int num_players);
    void set_pif_callback(pif_sync_callback_t callback);
    void set_frame_callback(m64p_frame_callback callback);
    m64p_error execute(); // blocks until emulation stops
    void stop();
    void read_screen(void* dest, int* width, int* height);
    void shutdown();
    m64p_dynlib_handle get_audio_plugin_handle() const;

    ptr_CoreDoCommand core_do_command = nullptr;

private:
    bool load_core(const std::string& path);
    bool load_plugin(const std::string& path, m64p_plugin_type type);
    void detach_plugins();

    m64p_dynlib_handle core_handle = nullptr;
    m64p_dynlib_handle plugin_handles[4] = {};
    ptr_PluginShutdown plugin_shutdowns[4] = {};

    ptr_CoreStartup core_startup = nullptr;
    ptr_CoreShutdown core_shutdown_fn = nullptr;
    ptr_CoreAttachPlugin core_attach_plugin = nullptr;
    ptr_CoreDetachPlugin core_detach_plugin = nullptr;
    ptr_CoreOverrideVidExt core_override_vidext = nullptr;
    ptr_ConfigOpenSection config_open_section = nullptr;
    ptr_ConfigSetParameter config_set_parameter = nullptr;
    ptr_set_pif_sync_callback set_pif_callback_fn = nullptr;
    ptr_ReadScreen2 read_screen2 = nullptr;

    bool verbose = false;
    bool rom_open = false;
    bool plugins_attached = false;
    std::string data_dir;
    int res_width = 640;
    int res_height = 480;
    int msaa = 0;
    int aniso = 0;

    void configure_gliden64();
};
