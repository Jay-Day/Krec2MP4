// Minimal mupen64plus audio plugin that captures raw PCM audio to a file.
// No speaker output. Used by Krec2MP4 for encoding audio into the output MP4.

#include <cstdio>
#include <cstdint>
#include <cstring>

#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#define CALL   __cdecl
#else
#define EXPORT __attribute__((visibility("default")))
#define CALL
#endif

// --- m64p types (minimal subset) ---

typedef void* m64p_dynlib_handle;
typedef enum { M64PLUGIN_AUDIO = 3 } m64p_plugin_type;
typedef enum {
    M64ERR_SUCCESS = 0, M64ERR_NOT_INIT, M64ERR_ALREADY_INIT, M64ERR_INCOMPATIBLE,
    M64ERR_INPUT_ASSERT, M64ERR_INPUT_INVALID, M64ERR_INPUT_NOT_FOUND, M64ERR_NO_MEMORY,
    M64ERR_FILES, M64ERR_INTERNAL, M64ERR_INVALID_STATE, M64ERR_PLUGIN_FAIL,
    M64ERR_SYSTEM_FAIL, M64ERR_UNSUPPORTED, M64ERR_WRONG_TYPE
} m64p_error;

typedef enum { SYSTEM_NTSC = 0, SYSTEM_PAL, SYSTEM_MPAL } m64p_system_type;

typedef struct {
    unsigned char * RDRAM;
    unsigned char * DMEM;
    unsigned char * IMEM;
    unsigned int * MI_INTR_REG;
    unsigned int * AI_DRAM_ADDR_REG;
    unsigned int * AI_LEN_REG;
    unsigned int * AI_CONTROL_REG;
    unsigned int * AI_STATUS_REG;
    unsigned int * AI_DACRATE_REG;
    unsigned int * AI_BITRATE_REG;
    void (*CheckInterrupts)(void);
} AUDIO_INFO;

// --- Plugin state ---

static bool s_init = false;
static AUDIO_INFO s_audio_info = {};
static FILE* s_output_file = nullptr;
static char s_output_path[1024] = {};
static unsigned int s_frequency = 33600;  // default
static unsigned long long s_bytes_written = 0;

// --- Custom exports for main app ---

extern "C" {

EXPORT void CALL audio_capture_set_output(const char* path) {
    strncpy(s_output_path, path, sizeof(s_output_path) - 1);
    s_output_path[sizeof(s_output_path) - 1] = 0;
}

EXPORT unsigned int CALL audio_capture_get_frequency(void) {
    return s_frequency;
}

EXPORT unsigned long long CALL audio_capture_get_bytes_written(void) {
    return s_bytes_written;
}

// --- Standard m64p audio plugin exports ---

EXPORT m64p_error CALL PluginStartup(m64p_dynlib_handle CoreLibHandle,
                                      void* Context,
                                      void (*DebugCallback)(void*, int, const char*)) {
    if (s_init) return M64ERR_ALREADY_INIT;
    s_init = true;
    return M64ERR_SUCCESS;
}

EXPORT m64p_error CALL PluginShutdown(void) {
    if (!s_init) return M64ERR_NOT_INIT;
    s_init = false;
    return M64ERR_SUCCESS;
}

EXPORT m64p_error CALL PluginGetVersion(m64p_plugin_type* PluginType,
                                         int* PluginVersion,
                                         int* APIVersion,
                                         const char** PluginNamePtr,
                                         int* Capabilities) {
    if (PluginType) *PluginType = M64PLUGIN_AUDIO;
    if (PluginVersion) *PluginVersion = 0x010000;
    if (APIVersion) *APIVersion = 0x020000;
    if (PluginNamePtr) *PluginNamePtr = "Krec2MP4 Audio Capture";
    if (Capabilities) *Capabilities = 0;
    return M64ERR_SUCCESS;
}

EXPORT int CALL InitiateAudio(AUDIO_INFO Audio_Info) {
    s_audio_info = Audio_Info;
    fprintf(stderr, "AudioCapture: InitiateAudio called (RDRAM=%p)\n", Audio_Info.RDRAM);
    return 1; // success
}

EXPORT int CALL RomOpen(void) {
    s_bytes_written = 0;
    fprintf(stderr, "AudioCapture: RomOpen called, output='%s'\n", s_output_path);
    if (s_output_path[0]) {
        s_output_file = fopen(s_output_path, "wb");
        if (!s_output_file) {
            fprintf(stderr, "AudioCapture: failed to open '%s'\n", s_output_path);
            return 0;
        }
    }
    return 1;
}

EXPORT void CALL RomClosed(void) {
    fprintf(stderr, "AudioCapture: RomClosed called, bytes_written=%llu\n", s_bytes_written);
    if (s_output_file) {
        fclose(s_output_file);
        s_output_file = nullptr;
    }
}

EXPORT void CALL AiDacrateChanged(int SystemType) {
    fprintf(stderr, "AudioCapture: AiDacrateChanged(SystemType=%d)\n", SystemType);
    unsigned int vi_clock;
    switch (SystemType) {
        default:
        case SYSTEM_NTSC: vi_clock = 48681812; break;
        case SYSTEM_PAL:  vi_clock = 49656530; break;
        case SYSTEM_MPAL: vi_clock = 48628316; break;
    }
    unsigned int dacrate = *s_audio_info.AI_DACRATE_REG;
    s_frequency = vi_clock / (dacrate + 1);
}

EXPORT void CALL AiLenChanged(void) {
    if (!s_output_file || !s_audio_info.RDRAM) return;

    unsigned int addr = *s_audio_info.AI_DRAM_ADDR_REG & 0xFFFFFF;
    unsigned int len = *s_audio_info.AI_LEN_REG;
    if (len == 0) return;

    const uint8_t* src = s_audio_info.RDRAM + addr;

    // N64 audio: big-endian stereo 16-bit samples stored as 32-bit words.
    // On little-endian host, RDRAM contains [Right_Lo, Right_Hi, Left_Lo, Left_Hi]
    // We need S16LE interleaved: [Left_Lo, Left_Hi, Right_Lo, Right_Hi]
    // So swap the two 16-bit halves of each 32-bit word.
    unsigned int num_samples = len / 4;
    for (unsigned int i = 0; i < num_samples; i++) {
        uint8_t out[4];
        out[0] = src[i * 4 + 2]; // Left Lo
        out[1] = src[i * 4 + 3]; // Left Hi
        out[2] = src[i * 4 + 0]; // Right Lo
        out[3] = src[i * 4 + 1]; // Right Hi
        fwrite(out, 1, 4, s_output_file);
    }
    s_bytes_written += (unsigned long long)num_samples * 4;
}

EXPORT void CALL ProcessAList(void) {}
EXPORT void CALL SetSpeedFactor(int percent) {}
EXPORT void CALL VolumeUp(void) {}
EXPORT void CALL VolumeDown(void) {}
EXPORT int  CALL VolumeGetLevel(void) { return 100; }
EXPORT void CALL VolumeSetLevel(int level) {}
EXPORT void CALL VolumeMute(void) {}
EXPORT const char* CALL VolumeGetString(void) { return "100%"; }

} // extern "C"
