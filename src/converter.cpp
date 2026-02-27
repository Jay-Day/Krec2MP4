#include "converter.h"
#include "audio_capture.h"
#include "krec_parser.h"
#include "emulator.h"
#include "pif_replay.h"
#include "frame_capture.h"
#include "ffmpeg_encoder.h"
#include "vidext.h"

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

// --- Callback state ---
static LogCallback s_log_callback;
static ProgressCallback s_progress_callback;
static std::atomic<bool>* s_cancel_flag = nullptr;

void converter_set_log_callback(LogCallback cb) {
    s_log_callback = std::move(cb);
}

void converter_set_progress_callback(ProgressCallback cb) {
    s_progress_callback = std::move(cb);
}

void converter_set_cancel_flag(std::atomic<bool>* flag) {
    s_cancel_flag = flag;
}

void converter_log(int level, const char* fmt, ...) {
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (s_log_callback) {
        s_log_callback(level, buf);
    } else {
        if (level <= LOG_WARNING) {
            fprintf(stderr, "%s\n", buf);
        } else {
            printf("%s\n", buf);
        }
    }
}

std::string get_exe_dir() {
#ifdef _WIN32
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string s(path);
    auto pos = s.find_last_of("\\/");
    return (pos != std::string::npos) ? s.substr(0, pos + 1) : ".\\";
#else
    return "./";
#endif
}

bool check_ffmpeg(const std::string& ffmpeg_path) {
#ifdef _WIN32
    // Run FFmpeg silently with CREATE_NO_WINDOW
    std::string cmd = "\"" + ffmpeg_path + "\" -version";

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE read_handle = nullptr, write_handle = nullptr;
    if (!CreatePipe(&read_handle, &write_handle, &sa, 0)) {
        converter_log(LOG_ERROR, "Error: FFmpeg not found at '%s'", ffmpeg_path.c_str());
        return false;
    }
    SetHandleInformation(read_handle, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = write_handle;
    si.hStdError = write_handle;

    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessA(nullptr, (LPSTR)cmd.c_str(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(write_handle);

    if (!ok) {
        CloseHandle(read_handle);
        converter_log(LOG_ERROR, "Error: FFmpeg not found at '%s'", ffmpeg_path.c_str());
        converter_log(LOG_ERROR, "Install FFmpeg or specify path with --ffmpeg");
        return false;
    }

    // Read some output to confirm it works
    char buf[256];
    DWORD bytes_read;
    bool got_output = false;
    while (ReadFile(read_handle, buf, sizeof(buf) - 1, &bytes_read, nullptr) && bytes_read > 0) {
        got_output = true;
    }
    CloseHandle(read_handle);

    WaitForSingleObject(pi.hProcess, 5000);
    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (!got_output || exit_code != 0) {
        converter_log(LOG_ERROR, "Error: FFmpeg not found at '%s'", ffmpeg_path.c_str());
        converter_log(LOG_ERROR, "Install FFmpeg or specify path with --ffmpeg");
        return false;
    }
    return true;
#else
    std::string cmd = "\"" + ffmpeg_path + "\" -version 2>&1";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) {
        converter_log(LOG_ERROR, "Error: FFmpeg not found at '%s'", ffmpeg_path.c_str());
        converter_log(LOG_ERROR, "Install FFmpeg or specify path with --ffmpeg");
        return false;
    }
    char buf[256];
    bool got_output = false;
    while (fgets(buf, sizeof(buf), p)) {
        got_output = true;
    }
    int ret = pclose(p);
    if (!got_output || ret != 0) {
        converter_log(LOG_ERROR, "Error: FFmpeg not found at '%s'", ffmpeg_path.c_str());
        converter_log(LOG_ERROR, "Install FFmpeg or specify path with --ffmpeg");
        return false;
    }
    return true;
#endif
}

std::string make_output_path(const std::string& input_path, const std::string& output_path) {
    if (!output_path.empty()) return output_path;
    fs::path p(input_path);
    p.replace_extension(".mp4");
    return p.string();
}

// Mux video + raw audio into final MP4 using FFmpeg
static bool mux_video_audio(const std::string& ffmpeg_path,
                             const std::string& video_path,
                             const std::string& audio_path,
                             unsigned int audio_freq,
                             unsigned long long audio_bytes,
                             int frames_captured,
                             double encode_fps,
                             const std::string& output_path) {
    // Calculate scale factor to sync video timestamps with actual audio duration.
    // The video was encoded at a fixed FPS (e.g. 60) but the N64's actual rate
    // may differ slightly (~59.94 for NTSC). -itsscale adjusts video timestamps
    // so they match the audio duration exactly, preventing drift.
    double audio_duration = (double)audio_bytes / ((double)audio_freq * 4.0); // stereo s16le = 4 bytes/sample
    double video_duration = (double)frames_captured / encode_fps;
    double itsscale = (audio_duration > 0 && video_duration > 0)
        ? audio_duration / video_duration : 1.0;

    converter_log(LOG_INFO, "A/V sync: video=%.3fs audio=%.3fs scale=%.6f",
                  video_duration, audio_duration, itsscale);

    char cmd[4096];
    snprintf(cmd, sizeof(cmd),
        "\"%s\" -y -itsscale %g -i \"%s\" -f s16le -ar %u -ac 2 -i \"%s\" "
        "-c:v copy -c:a aac -b:a 192k -shortest \"%s\"",
        ffmpeg_path.c_str(),
        itsscale,
        video_path.c_str(),
        audio_freq,
        audio_path.c_str(),
        output_path.c_str());

    converter_log(LOG_VERBOSE, "Mux cmd: %s", cmd);

#ifdef _WIN32
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE read_handle = nullptr, write_handle = nullptr;
    if (!CreatePipe(&read_handle, &write_handle, &sa, 0)) {
        converter_log(LOG_ERROR, "Error: failed to create pipe for FFmpeg mux");
        return false;
    }
    SetHandleInformation(read_handle, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = write_handle;
    si.hStdError = write_handle;

    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessA(nullptr, cmd, nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(write_handle);

    if (!ok) {
        CloseHandle(read_handle);
        converter_log(LOG_ERROR, "Error: failed to run FFmpeg mux command (%lu)", GetLastError());
        return false;
    }

    // Read and log FFmpeg output
    char buf[512];
    DWORD bytes_read;
    std::string line_buf;
    while (ReadFile(read_handle, buf, sizeof(buf) - 1, &bytes_read, nullptr) && bytes_read > 0) {
        buf[bytes_read] = 0;
        line_buf += buf;
        // Process complete lines
        size_t pos;
        while ((pos = line_buf.find('\n')) != std::string::npos) {
            std::string line = line_buf.substr(0, pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) converter_log(LOG_INFO, "[FFmpeg mux] %s", line.c_str());
            line_buf = line_buf.substr(pos + 1);
        }
    }
    if (!line_buf.empty()) converter_log(LOG_INFO, "[FFmpeg mux] %s", line_buf.c_str());
    CloseHandle(read_handle);

    WaitForSingleObject(pi.hProcess, 60000);
    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return exit_code == 0;
#else
    std::string full_cmd = std::string(cmd) + " 2>&1";
    FILE* p = popen(full_cmd.c_str(), "r");
    if (!p) {
        converter_log(LOG_ERROR, "Error: failed to run FFmpeg mux command");
        return false;
    }
    char buf[512];
    while (fgets(buf, sizeof(buf), p)) {
        size_t len = strlen(buf);
        if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = 0;
        if (buf[0]) converter_log(LOG_INFO, "[FFmpeg mux] %s", buf);
    }
    int ret = pclose(p);
    return ret == 0;
#endif
}

bool convert_one(const std::string& krec_path, const std::string& output_path,
                 const AppConfig& config) {
    converter_log(LOG_INFO, "--- Converting: %s ---", krec_path.c_str());
    converter_log(LOG_INFO, "Output: %s", output_path.c_str());

    if (s_cancel_flag && s_cancel_flag->load()) {
        converter_log(LOG_WARNING, "Cancelled.");
        return false;
    }

    // Parse krec
    KrecData krec;
    if (!krec_parse(krec_path, krec)) return false;

    double fps = config.fps;
    if (fps <= 0) fps = 60.0;
    krec_print_info(krec, fps);

    if (krec.total_input_frames == 0) {
        converter_log(LOG_ERROR, "Error: no input frames in krec file");
        return false;
    }

    // Temp file paths for two-pass mux
    std::string temp_video = output_path + ".tmp_v.mp4";
    std::string temp_audio = output_path + ".tmp_a.raw";

    // Find audio capture plugin DLL next to the executable
    std::string audio_plugin_path = get_exe_dir() + "AudioCapturePlugin.dll";

    // Initialize emulator with audio capture plugin
    Emulator emu;
    EmulatorConfig emu_config;
    emu_config.core_path = config.core_path;
    emu_config.plugin_dir = config.plugin_dir;
    emu_config.data_dir = config.data_dir;
    emu_config.verbose = config.verbose;
    emu_config.res_width = config.res_width;
    emu_config.res_height = config.res_height;
    emu_config.msaa = config.msaa;
    emu_config.aniso = config.aniso;
    emu_config.audio_plugin_path = audio_plugin_path;

    converter_log(LOG_INFO, "Initializing emulator...");
    if (!emu.init(emu_config)) {
        converter_log(LOG_ERROR, "Error: emulator initialization failed");
        return false;
    }

    // Configure audio capture plugin
    auto audio_handle = emu.get_audio_plugin_handle();
    ptr_audio_capture_set_output set_output_fn = nullptr;
    ptr_audio_capture_get_frequency get_freq_fn = nullptr;
    ptr_audio_capture_get_bytes_written get_bytes_fn = nullptr;

    if (audio_handle) {
        set_output_fn = (ptr_audio_capture_set_output)GetProcAddress(
            (HMODULE)audio_handle, "audio_capture_set_output");
        get_freq_fn = (ptr_audio_capture_get_frequency)GetProcAddress(
            (HMODULE)audio_handle, "audio_capture_get_frequency");
        get_bytes_fn = (ptr_audio_capture_get_bytes_written)GetProcAddress(
            (HMODULE)audio_handle, "audio_capture_get_bytes_written");
    }

    if (set_output_fn) {
        set_output_fn(temp_audio.c_str());
        converter_log(LOG_INFO, "Audio capture enabled.");
    } else {
        converter_log(LOG_WARNING, "Warning: audio capture plugin not available, output will have no audio.");
    }

    converter_log(LOG_INFO, "Opening ROM...");
    if (!emu.open_rom(config.rom_path)) {
        emu.shutdown();
        return false;
    }

    // Configure controllers as present before attaching plugins.
    // RMG-Input reads PluggedIn from config during InitiateControllers().
    // Without this, Controllers[].Present=0 and process_controller_command()
    // short-circuits, causing desync vs RMG-K kaillera where controllers are present.
    emu.configure_controllers_for_replay(krec.header.num_players);

    if (!emu.attach_plugins()) {
        emu.shutdown();
        return false;
    }

    emu.apply_deterministic_settings();

    // Setup FFmpeg encoder config (video only, to temp file)
    // Encoder is opened lazily on first frame to match actual render dimensions
    FFmpegEncoder encoder;
    FFmpegConfig ff_config;
    ff_config.ffmpeg_path = config.ffmpeg_path;
    ff_config.output_path = temp_video;
    ff_config.width = config.res_width;
    ff_config.height = config.res_height;
    ff_config.fps = fps;
    ff_config.crf = config.crf;
    ff_config.encoder = config.encoder;

    converter_log(LOG_INFO, "Requested resolution: %dx%d @ %g fps, CRF %d",
                  ff_config.width, ff_config.height, ff_config.fps, ff_config.crf);

    // Setup PIF replay
    pif_replay_init(&krec);
    emu.set_pif_callback(pif_replay_callback);

    // Setup frame capture (encoder opened lazily on first frame)
    frame_capture_init(&emu, &encoder, ff_config,
                       krec.total_input_frames);
    if (s_progress_callback) {
        frame_capture_set_progress_callback(s_progress_callback);
    }
    if (s_cancel_flag) {
        frame_capture_set_cancel_flag(s_cancel_flag);
    }
    emu.set_frame_callback(frame_capture_callback);

    converter_log(LOG_INFO, "Running emulation (%d input frames)...", krec.total_input_frames);
    m64p_error ret = emu.execute();

    int frames_captured = frame_capture_count();
    converter_log(LOG_INFO, "Emulation finished. Captured %d frames.", frames_captured);

    // Close encoder and emulator (this also closes audio capture file via RomClosed)
    encoder.close();

    // Get audio info before shutdown
    unsigned int audio_freq = 33600;
    unsigned long long audio_bytes = 0;
    if (get_freq_fn) audio_freq = get_freq_fn();
    if (get_bytes_fn) audio_bytes = get_bytes_fn();

    converter_log(LOG_INFO, "Audio capture: %llu bytes, frequency: %u Hz",
                  audio_bytes, audio_freq);

    emu.shutdown();

    if (s_cancel_flag && s_cancel_flag->load()) {
        converter_log(LOG_WARNING, "Conversion cancelled.");
        fs::remove(temp_video);
        fs::remove(temp_audio);
        return false;
    }

    if (frames_captured <= 0) {
        converter_log(LOG_WARNING, "Warning: no frames were captured");
        fs::remove(temp_video);
        fs::remove(temp_audio);
        return false;
    }

    // Mux video + audio into final output
    if (audio_bytes > 0) {
        converter_log(LOG_INFO, "Muxing video + audio (sample rate: %u Hz)...", audio_freq);
        // Signal muxing phase to progress callback
        if (s_progress_callback) s_progress_callback(-1, 0);
        if (!mux_video_audio(config.ffmpeg_path, temp_video, temp_audio, audio_freq,
                             audio_bytes, frames_captured, fps, output_path)) {
            converter_log(LOG_ERROR, "Error: FFmpeg mux failed, keeping video-only output.");
            fs::rename(temp_video, output_path);
        }
    } else {
        converter_log(LOG_INFO, "No audio captured, keeping video-only output.");
        fs::rename(temp_video, output_path);
    }

    // Cleanup temp files
    fs::remove(temp_video);
    fs::remove(temp_audio);

    converter_log(LOG_INFO, "Output saved to: %s", output_path.c_str());
    return true;
}
