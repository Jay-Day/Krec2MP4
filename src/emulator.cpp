#include "emulator.h"
#include "vidext.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include <fstream>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#define LOAD_LIB(path) LoadLibraryA(path)
#define GET_PROC(handle, name) GetProcAddress((HMODULE)(handle), name)
#define FREE_LIB(handle) FreeLibrary((HMODULE)(handle))
#else
#include <dlfcn.h>
#define LOAD_LIB(path) dlopen(path, RTLD_NOW)
#define GET_PROC(handle, name) dlsym(handle, name)
#define FREE_LIB(handle) dlclose(handle)
#endif

static bool s_verbose = false;
static EmulatorLogCallback s_emu_log_callback;

void emulator_set_log_callback(EmulatorLogCallback cb) {
    s_emu_log_callback = std::move(cb);
}

static void debug_callback(void* context, int level, const char* message) {
    if (s_verbose || level <= M64MSG_WARNING) {
        if (s_emu_log_callback) {
            char buf[2048];
            const char* level_str = "???";
            switch (level) {
                case M64MSG_ERROR:   level_str = "ERROR"; break;
                case M64MSG_WARNING: level_str = "WARN"; break;
                case M64MSG_INFO:    level_str = "INFO"; break;
                case M64MSG_STATUS:  level_str = "STATUS"; break;
                case M64MSG_VERBOSE: level_str = "VERBOSE"; break;
            }
            snprintf(buf, sizeof(buf), "[M64P %s] %s", level_str, message);
            s_emu_log_callback(level, buf);
        } else {
            const char* level_str = "???";
            switch (level) {
                case M64MSG_ERROR:   level_str = "ERROR"; break;
                case M64MSG_WARNING: level_str = "WARN"; break;
                case M64MSG_INFO:    level_str = "INFO"; break;
                case M64MSG_STATUS:  level_str = "STATUS"; break;
                case M64MSG_VERBOSE: level_str = "VERBOSE"; break;
            }
            fprintf(stderr, "[M64P %s] %s\n", level_str, message);
        }
    }
}

static void state_callback(void* context, m64p_core_param param_type, int new_value) {
    // No-op for headless mode
}

bool Emulator::load_core(const std::string& path) {
    core_handle = LOAD_LIB(path.c_str());
    if (!core_handle) {
        fprintf(stderr, "Error: failed to load core library '%s'\n", path.c_str());
        return false;
    }

    #define RESOLVE(var, type, name) \
        var = (type)GET_PROC(core_handle, name); \
        if (!var) { fprintf(stderr, "Error: failed to resolve '%s'\n", name); return false; }

    RESOLVE(core_startup, ptr_CoreStartup, "CoreStartup");
    RESOLVE(core_shutdown_fn, ptr_CoreShutdown, "CoreShutdown");
    RESOLVE(core_attach_plugin, ptr_CoreAttachPlugin, "CoreAttachPlugin");
    RESOLVE(core_detach_plugin, ptr_CoreDetachPlugin, "CoreDetachPlugin");
    RESOLVE(core_do_command, ptr_CoreDoCommand, "CoreDoCommand");
    RESOLVE(core_override_vidext, ptr_CoreOverrideVidExt, "CoreOverrideVidExt");
    RESOLVE(config_open_section, ptr_ConfigOpenSection, "ConfigOpenSection");
    RESOLVE(config_set_parameter, ptr_ConfigSetParameter, "ConfigSetParameter");

    // Optional RMG-K extension
    set_pif_callback_fn = (ptr_set_pif_sync_callback)GET_PROC(core_handle, "set_pif_sync_callback");
    if (!set_pif_callback_fn) {
        fprintf(stderr, "Warning: 'set_pif_sync_callback' not found - this core may not support krec replay\n");
    }

    #undef RESOLVE
    return true;
}

bool Emulator::load_plugin(const std::string& path, m64p_plugin_type type) {
    int idx = (int)type - 1; // RSP=1, GFX=2, AUDIO=3, INPUT=4 -> 0,1,2,3
    if (idx < 0 || idx > 3) return false;

    m64p_dynlib_handle handle = LOAD_LIB(path.c_str());
    if (!handle) {
        fprintf(stderr, "Error: failed to load plugin '%s'\n", path.c_str());
        return false;
    }

    auto startup = (ptr_PluginStartup)GET_PROC(handle, "PluginStartup");
    auto shutdown = (ptr_PluginShutdown)GET_PROC(handle, "PluginShutdown");
    if (!startup || !shutdown) {
        fprintf(stderr, "Error: plugin '%s' missing PluginStartup/PluginShutdown\n", path.c_str());
        FREE_LIB(handle);
        return false;
    }

    m64p_error ret = startup(core_handle, nullptr, debug_callback);
    if (ret != M64ERR_SUCCESS && ret != M64ERR_ALREADY_INIT) {
        fprintf(stderr, "Error: PluginStartup failed for '%s' (error %d)\n", path.c_str(), ret);
        FREE_LIB(handle);
        return false;
    }

    // For GFX plugin, grab ReadScreen2
    if (type == M64PLUGIN_GFX) {
        read_screen2 = (ptr_ReadScreen2)GET_PROC(handle, "ReadScreen2");
        if (!read_screen2) {
            fprintf(stderr, "Warning: GFX plugin missing ReadScreen2\n");
        }
    }

    plugin_handles[idx] = handle;
    plugin_shutdowns[idx] = shutdown;
    return true;
}

// Patch GLideN64.ini in the data directory with our video settings.
// GLideN64 uses its own INI config format (not the mupen64plus config system).
void Emulator::configure_gliden64() {
    namespace fs = std::filesystem;
    std::string ini_path = (fs::path(data_dir) / "GLideN64.ini").string();

    // Read entire file
    std::ifstream ifs(ini_path);
    if (!ifs.is_open()) {
        fprintf(stderr, "Warning: cannot open '%s' for GLideN64 config\n", ini_path.c_str());
        return;
    }
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(ifs, line)) {
        // Strip trailing \r if present (Windows line endings)
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    ifs.close();

    int factor = res_width / 320;
    if (factor < 1) factor = 1;

    // Patch settings in the [User] section
    bool in_user = false;
    for (auto& l : lines) {
        if (!l.empty() && l[0] == '[') {
            in_user = (l == "[User]");
            continue;
        }
        if (!in_user) continue;

        // Resolution
        if (l.find("frameBufferEmulation\\nativeResFactor=") == 0) {
            l = "frameBufferEmulation\\nativeResFactor=" + std::to_string(factor);
        } else if (l.find("video\\windowedWidth=") == 0) {
            l = "video\\windowedWidth=" + std::to_string(res_width);
        } else if (l.find("video\\windowedHeight=") == 0) {
            l = "video\\windowedHeight=" + std::to_string(res_height);
        }
        // Anti-aliasing (MSAA)
        else if (l.find("video\\multisampling=") == 0) {
            l = "video\\multisampling=" + std::to_string(msaa);
        } else if (l.find("video\\maxMultiSampling=") == 0) {
            l = "video\\maxMultiSampling=" + std::to_string(msaa);
        }
        // Anisotropic filtering
        else if (l.find("texture\\anisotropy=") == 0) {
            l = "texture\\anisotropy=" + std::to_string(aniso);
        } else if (l.find("texture\\maxAnisotropy=") == 0) {
            l = "texture\\maxAnisotropy=" + std::to_string(aniso);
        }
    }

    // Write back
    std::ofstream ofs(ini_path);
    if (!ofs.is_open()) {
        fprintf(stderr, "Warning: cannot write '%s' for GLideN64 config\n", ini_path.c_str());
        return;
    }
    for (const auto& l : lines) {
        ofs << l << "\n";
    }
    ofs.close();
}

bool Emulator::init(const EmulatorConfig& config) {
    verbose = config.verbose;
    s_verbose = config.verbose;
    res_width = config.res_width;
    res_height = config.res_height;
    msaa = config.msaa;
    aniso = config.aniso;

    if (!load_core(config.core_path)) return false;

    // Get absolute data dir path
    data_dir = std::filesystem::absolute(config.data_dir).string();
    std::string config_dir = data_dir; // Use same dir for config

    m64p_error ret = core_startup(0x020001, config_dir.c_str(), data_dir.c_str(),
                                   nullptr, debug_callback, nullptr, state_callback);
    if (ret != M64ERR_SUCCESS) {
        fprintf(stderr, "Error: CoreStartup failed (error %d)\n", ret);
        return false;
    }

    // Override VidExt with our SDL3 headless implementation
    m64p_video_extension_functions vidext = vidext_get_functions();
    ret = core_override_vidext(&vidext);
    if (ret != M64ERR_SUCCESS) {
        fprintf(stderr, "Error: CoreOverrideVidExt failed (error %d)\n", ret);
        return false;
    }

    // Patch GLideN64.ini with our settings (resolution, MSAA, aniso)
    // GLideN64 reads its own INI file, not the mupen64plus config system
    configure_gliden64();

    // Load plugins: GFX, RSP, Audio, Input
    struct { std::string path; m64p_plugin_type type; } plugins[] = {
        {config.plugin_dir + "mupen64plus-video-GLideN64.dll", M64PLUGIN_GFX},
        {config.plugin_dir + "mupen64plus-rsp-hle.dll",        M64PLUGIN_RSP},
        {config.audio_plugin_path.empty()
            ? config.plugin_dir + "RMG-Audio.dll"
            : config.audio_plugin_path,                         M64PLUGIN_AUDIO},
        {config.plugin_dir + "RMG-Input.dll",                  M64PLUGIN_INPUT},
    };

    for (auto& p : plugins) {
        if (!load_plugin(p.path, p.type)) return false;
    }

    return true;
}

bool Emulator::open_rom(const std::string& rom_path) {
    FILE* f = fopen(rom_path.c_str(), "rb");
    if (!f) {
        fprintf(stderr, "Error: cannot open ROM '%s'\n", rom_path.c_str());
        return false;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    std::vector<uint8_t> rom_data(size);
    if (fread(rom_data.data(), 1, size, f) != (size_t)size) {
        fprintf(stderr, "Error: failed to read ROM\n");
        fclose(f);
        return false;
    }
    fclose(f);

    m64p_error ret = core_do_command(M64CMD_ROM_OPEN, (int)size, rom_data.data());
    if (ret != M64ERR_SUCCESS) {
        fprintf(stderr, "Error: M64CMD_ROM_OPEN failed (error %d)\n", ret);
        return false;
    }

    rom_open = true;
    return true;
}

bool Emulator::attach_plugins() {
    m64p_plugin_type types[] = {M64PLUGIN_GFX, M64PLUGIN_AUDIO, M64PLUGIN_INPUT, M64PLUGIN_RSP};
    for (auto type : types) {
        int idx = (int)type - 1;
        if (!plugin_handles[idx]) continue;
        m64p_error ret = core_attach_plugin(type, plugin_handles[idx]);
        if (ret != M64ERR_SUCCESS) {
            fprintf(stderr, "Error: CoreAttachPlugin(%d) failed (error %d)\n", type, ret);
            return false;
        }
    }
    plugins_attached = true;
    return true;
}

void Emulator::apply_deterministic_settings() {
    m64p_handle section;

    // Core settings for deterministic replay (match RMG-K Kaillera config)
    if (config_open_section("Core", &section) == M64ERR_SUCCESS) {
        int val;

        // Disable RandomizeInterrupt
        val = 0;
        config_set_parameter(section, "RandomizeInterrupt", M64TYPE_BOOL, &val);

        // CPU Emulator = 2 (dynamic recompiler)
        val = 2;
        config_set_parameter(section, "R4300Emulator", M64TYPE_INT, &val);

        // CountPerOp = 0
        val = 0;
        config_set_parameter(section, "CountPerOp", M64TYPE_INT, &val);

        // CountPerOpDenomPot = 0
        val = 0;
        config_set_parameter(section, "CountPerOpDenomPot", M64TYPE_INT, &val);

        // SiDmaDuration = -1
        val = -1;
        config_set_parameter(section, "SiDmaDuration", M64TYPE_INT, &val);

        // DisableExtraMem = false (enable 8MB expansion)
        val = 0;
        config_set_parameter(section, "DisableExtraMem", M64TYPE_BOOL, &val);

        // DisableSaveFileLoading = true (fresh saves)
        val = 1;
        config_set_parameter(section, "DisableSaveFileLoading", M64TYPE_BOOL, &val);
    }

    // Mute audio
    if (config_open_section("Audio-SDL", &section) == M64ERR_SUCCESS) {
        int val = 0;
        config_set_parameter(section, "VOLUME_DEFAULT", M64TYPE_INT, &val);
    }

    // GLideN64 settings (resolution, MSAA, aniso) are configured
    // via GLideN64.ini in configure_gliden64(), called during init().
}

void Emulator::configure_controllers_for_replay(int num_players) {
    m64p_handle section;

    // Configure RMG-Input plugin profiles to mark controllers as present.
    // This matches RMG-K kaillera behavior where controllers are connected
    // via netplay registration. Without this, process_controller_command()
    // short-circuits with NoResponse, skipping pak processing side effects.
    for (int i = 0; i < 4; i++) {
        char section_name[128];
        snprintf(section_name, sizeof(section_name),
                 "Rosalie's Mupen GUI - Input Plugin Profile %d", i);

        if (config_open_section(section_name, &section) == M64ERR_SUCCESS) {
            int plugged_in = (i < num_players) ? 1 : 0;
            config_set_parameter(section, "PluggedIn", M64TYPE_BOOL, &plugged_in);

            // No controller pak (matches kaillera behavior)
            // PLUGIN_NONE = 1
            int plugin = 1;
            config_set_parameter(section, "Plugin", M64TYPE_INT, &plugin);
        }
    }
}

void Emulator::set_pif_callback(pif_sync_callback_t callback) {
    if (set_pif_callback_fn) {
        set_pif_callback_fn(callback);
    }
}

void Emulator::set_frame_callback(m64p_frame_callback callback) {
    core_do_command(M64CMD_SET_FRAME_CALLBACK, 0, (void*)callback);
}

m64p_error Emulator::execute() {
    return core_do_command(M64CMD_EXECUTE, 0, nullptr);
}

void Emulator::stop() {
    core_do_command(M64CMD_STOP, 0, nullptr);
}

void Emulator::read_screen(void* dest, int* width, int* height) {
    if (read_screen2) {
        read_screen2(dest, width, height, 0);
    }
}

void Emulator::detach_plugins() {
    if (!plugins_attached) return;
    m64p_plugin_type types[] = {M64PLUGIN_GFX, M64PLUGIN_AUDIO, M64PLUGIN_INPUT, M64PLUGIN_RSP};
    for (auto type : types) {
        core_detach_plugin(type);
    }
    plugins_attached = false;
}

void Emulator::shutdown() {
    detach_plugins();

    if (rom_open) {
        core_do_command(M64CMD_ROM_CLOSE, 0, nullptr);
        rom_open = false;
    }

    // Shutdown plugins
    for (int i = 0; i < 4; i++) {
        if (plugin_shutdowns[i]) {
            plugin_shutdowns[i]();
            plugin_shutdowns[i] = nullptr;
        }
        if (plugin_handles[i]) {
            FREE_LIB(plugin_handles[i]);
            plugin_handles[i] = nullptr;
        }
    }

    if (core_shutdown_fn) {
        core_shutdown_fn();
    }

    if (core_handle) {
        FREE_LIB(core_handle);
        core_handle = nullptr;
    }
}

m64p_dynlib_handle Emulator::get_audio_plugin_handle() const {
    // AUDIO = M64PLUGIN_AUDIO = 3, index = type - 1 = 2
    return plugin_handles[2];
}
