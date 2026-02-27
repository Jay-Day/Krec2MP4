#include "ffmpeg_encoder.h"
#include <cstdio>
#include <cstring>
#include <string>

// Build encoder-specific quality/preset flags for FFmpeg
static std::string build_encoder_flags(const std::string& encoder, int crf) {
    if (encoder == "libx264") {
        char buf[128];
        snprintf(buf, sizeof(buf), "-c:v libx264 -preset medium -crf %d -pix_fmt yuv420p", crf);
        return buf;
    }
    if (encoder == "libx265") {
        char buf[128];
        snprintf(buf, sizeof(buf), "-c:v libx265 -preset medium -crf %d -pix_fmt yuv420p", crf);
        return buf;
    }
    if (encoder == "h264_amf") {
        char buf[128];
        snprintf(buf, sizeof(buf), "-c:v h264_amf -quality quality -rc cqp -qp_i %d -qp_p %d -pix_fmt yuv420p", crf, crf);
        return buf;
    }
    if (encoder == "hevc_amf") {
        char buf[128];
        snprintf(buf, sizeof(buf), "-c:v hevc_amf -quality quality -rc cqp -qp_i %d -qp_p %d -pix_fmt yuv420p", crf, crf);
        return buf;
    }
    if (encoder == "h264_nvenc") {
        char buf[128];
        snprintf(buf, sizeof(buf), "-c:v h264_nvenc -preset p7 -rc vbr -cq %d -pix_fmt yuv420p", crf);
        return buf;
    }
    if (encoder == "hevc_nvenc") {
        char buf[128];
        snprintf(buf, sizeof(buf), "-c:v hevc_nvenc -preset p7 -rc vbr -cq %d -pix_fmt yuv420p", crf);
        return buf;
    }
    // Fallback: treat as libx264
    char buf[128];
    snprintf(buf, sizeof(buf), "-c:v libx264 -preset medium -crf %d -pix_fmt yuv420p", crf);
    return buf;
}

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>

static HANDLE s_child_process = nullptr;
static HANDLE s_write_handle = nullptr;

bool FFmpegEncoder::open(const FFmpegConfig& config) {
    frame_width = config.width;
    frame_height = config.height;

    std::string enc_flags = build_encoder_flags(config.encoder, config.crf);

    // Build FFmpeg command line
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
        "\"%s\" -y -f rawvideo -pixel_format rgb24 -video_size %dx%d "
        "-framerate %g -i pipe:0 "
        "%s "
        "\"%s\"",
        config.ffmpeg_path.c_str(),
        config.width, config.height,
        config.fps,
        enc_flags.c_str(),
        config.output_path.c_str());

    fprintf(stderr, "FFmpeg cmd: %s\n", cmd);

    // Create pipe for stdin
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE read_handle = nullptr;
    if (!CreatePipe(&read_handle, &s_write_handle, &sa, 0)) {
        fprintf(stderr, "Error: CreatePipe failed (%lu)\n", GetLastError());
        return false;
    }

    // Don't let child inherit our write end
    SetHandleInformation(s_write_handle, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = read_handle;
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

    PROCESS_INFORMATION pi = {};
    if (!CreateProcessA(nullptr, cmd, nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        fprintf(stderr, "Error: CreateProcess failed (%lu): %s\n", GetLastError(), cmd);
        CloseHandle(read_handle);
        CloseHandle(s_write_handle);
        s_write_handle = nullptr;
        return false;
    }

    // Close handles we don't need
    CloseHandle(read_handle);
    CloseHandle(pi.hThread);
    s_child_process = pi.hProcess;

    // Wrap the write handle in a FILE* for fwrite convenience
    int fd = _open_osfhandle((intptr_t)s_write_handle, 0);
    if (fd == -1) {
        fprintf(stderr, "Error: _open_osfhandle failed\n");
        CloseHandle(s_write_handle);
        s_write_handle = nullptr;
        return false;
    }

    pipe = _fdopen(fd, "wb");
    if (!pipe) {
        fprintf(stderr, "Error: _fdopen failed\n");
        _close(fd);
        s_write_handle = nullptr;
        return false;
    }

    return true;
}

bool FFmpegEncoder::write_frame(const uint8_t* rgb_data, int width, int height) {
    if (!pipe) return false;

    size_t frame_size = (size_t)width * height * 3;
    size_t written = fwrite(rgb_data, 1, frame_size, pipe);
    if (written != frame_size) {
        fprintf(stderr, "Error: failed to write frame to FFmpeg pipe\n");
        return false;
    }

    return true;
}

void FFmpegEncoder::close() {
    if (pipe) {
        fclose(pipe);
        pipe = nullptr;
        s_write_handle = nullptr; // closed by fclose
    }
    if (s_child_process) {
        WaitForSingleObject(s_child_process, 30000);
        CloseHandle(s_child_process);
        s_child_process = nullptr;
    }
}

#else
// POSIX fallback
bool FFmpegEncoder::open(const FFmpegConfig& config) {
    frame_width = config.width;
    frame_height = config.height;

    std::string enc_flags = build_encoder_flags(config.encoder, config.crf);

    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
        "\"%s\" -y -f rawvideo -pixel_format rgb24 -video_size %dx%d "
        "-framerate %g -i pipe:0 "
        "%s "
        "\"%s\"",
        config.ffmpeg_path.c_str(),
        config.width, config.height,
        config.fps,
        enc_flags.c_str(),
        config.output_path.c_str());

    pipe = popen(cmd, "w");
    if (!pipe) {
        fprintf(stderr, "Error: failed to start FFmpeg: %s\n", cmd);
        return false;
    }
    return true;
}

bool FFmpegEncoder::write_frame(const uint8_t* rgb_data, int width, int height) {
    if (!pipe) return false;
    size_t frame_size = (size_t)width * height * 3;
    size_t written = fwrite(rgb_data, 1, frame_size, pipe);
    if (written != frame_size) {
        fprintf(stderr, "Error: failed to write frame to FFmpeg pipe\n");
        return false;
    }
    return true;
}

void FFmpegEncoder::close() {
    if (pipe) {
        pclose(pipe);
        pipe = nullptr;
    }
}
#endif
