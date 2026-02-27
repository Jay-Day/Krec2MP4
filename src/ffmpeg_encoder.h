#pragma once
#include <cstdint>
#include <string>

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
