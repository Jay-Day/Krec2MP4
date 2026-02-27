#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct EncoderInfo {
    const wchar_t* label;
    const char* codec;
    bool hw;  // true = needs hardware probe
};

// Returns the subset of known encoders available on this system.
// CPU encoders are always included; GPU encoders are tested by running
// a quick FFmpeg encode and checking the exit code.
std::vector<EncoderInfo> probe_available_encoders(const std::string& ffmpeg_path);

struct FFmpegConfig {
    std::string ffmpeg_path = "ffmpeg";
    std::string output_path;
    std::string encoder = "libx264";  // FFmpeg -c:v codec name
    int width = 640;
    int height = 480;
    double fps = 60.0;
    int crf = 23;
};

class FFmpegEncoder {
public:
    bool open(const FFmpegConfig& config);
    bool write_frame(const uint8_t* rgb_data, int width, int height);
    void close();
    bool is_open() const { return pipe != nullptr; }

private:
    FILE* pipe = nullptr;
    int frame_width = 0;
    int frame_height = 0;
};
