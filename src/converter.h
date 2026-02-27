#pragma once
#include <string>
#include <functional>
#include <atomic>

// Callback types for GUI integration
using LogCallback = std::function<void(int level, const char* msg)>;
using ProgressCallback = std::function<void(int current_frame, int total_frames)>;

// Log levels (reuse mupen64plus levels for consistency)
enum {
    LOG_ERROR = 1,
    LOG_WARNING = 2,
    LOG_INFO = 3,
    LOG_STATUS = 4,
    LOG_VERBOSE = 5
};

// Set callbacks (nullptr to restore default stdout/stderr behavior)
void converter_set_log_callback(LogCallback cb);
void converter_set_progress_callback(ProgressCallback cb);
void converter_set_cancel_flag(std::atomic<bool>* flag);

// Log function used internally; routes through callback if set
void converter_log(int level, const char* fmt, ...);

struct AppConfig {
    std::string rom_path;
    std::string input_path;   // .krec file or directory (batch)
    std::string output_path;  // output file or directory
    std::string core_path;    // resolved in main()
    std::string plugin_dir;
    std::string data_dir;
    std::string ffmpeg_path;
    double fps = 0; // 0 = auto-detect
    int res_width = 640;
    int res_height = 480;
    int crf = 23;
    int msaa = 0;       // 0=off, 2, 4, 8, 16
    int aniso = 0;      // 0=off, 2, 4, 8, 16
    std::string encoder = "libx264"; // FFmpeg codec name
    bool batch = false;
    bool verbose = false;
};

// Get the directory containing the executable
std::string get_exe_dir();

// Check if FFmpeg is available at the given path
bool check_ffmpeg(const std::string& ffmpeg_path);

// Generate output path from input path (replaces .krec with .mp4)
std::string make_output_path(const std::string& input_path, const std::string& output_path);

// Convert a single .krec file to .mp4. Returns true on success.
bool convert_one(const std::string& krec_path, const std::string& output_path,
                 const AppConfig& config);
