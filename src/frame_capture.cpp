#include "frame_capture.h"
#include "pif_replay.h"
#include <cstdio>
#include <cstring>
#include <vector>

static Emulator* s_emu = nullptr;
static FFmpegEncoder* s_encoder = nullptr;
static FFmpegConfig s_ff_config;
static bool s_encoder_opened = false;
static int s_captured_frames = 0;
static int s_total_frames = 0;
static bool s_speed_limiter_disabled = false;
static std::vector<uint8_t> s_pixel_buffer;
static std::vector<uint8_t> s_flipped_buffer;
static ProgressCallback s_progress_callback;
static std::atomic<bool>* s_cancel_flag = nullptr;

void frame_capture_init(Emulator* emu, FFmpegEncoder* encoder, const FFmpegConfig& ff_config,
                        int total_frames) {
    s_emu = emu;
    s_encoder = encoder;
    s_ff_config = ff_config;
    s_encoder_opened = false;
    s_captured_frames = 0;
    s_total_frames = total_frames;
    s_speed_limiter_disabled = false;
    s_pixel_buffer.clear();
    s_flipped_buffer.clear();
    s_progress_callback = nullptr;
    s_cancel_flag = nullptr;
}

void frame_capture_set_progress_callback(ProgressCallback cb) {
    s_progress_callback = std::move(cb);
}

void frame_capture_set_cancel_flag(std::atomic<bool>* flag) {
    s_cancel_flag = flag;
}

int frame_capture_count() {
    return s_captured_frames;
}

void frame_capture_callback(unsigned int frame_index) {
    // Check cancel flag
    if (s_cancel_flag && s_cancel_flag->load()) {
        if (s_emu) {
            s_emu->stop();
        }
        return;
    }

    // Set speed factor to 500% on first frame for faster conversion
    // Keep the speed limiter active (just increase the target speed)
    if (!s_speed_limiter_disabled && s_emu) {
        int speed = 500; // 5x speed
        s_emu->core_do_command(M64CMD_CORE_STATE_SET, M64CORE_SPEED_FACTOR, &speed);
        s_speed_limiter_disabled = true;
    }

    // Reset PIF sync flag for next frame
    pif_replay_reset_frame_sync();

    // Check if replay is done
    if (pif_replay_finished()) {
        if (s_emu) {
            s_emu->stop();
        }
        return;
    }

    if (!s_emu || !s_encoder) return;

    // Get screen dimensions first
    int width = 0, height = 0;
    s_emu->read_screen(nullptr, &width, &height);
    if (width <= 0 || height <= 0) return;

    // Open encoder lazily on first frame using actual render dimensions
    if (!s_encoder_opened) {
        if (width != s_ff_config.width || height != s_ff_config.height) {
            fprintf(stderr, "Frame capture: actual render size %dx%d differs from requested %dx%d, adapting.\n",
                    width, height, s_ff_config.width, s_ff_config.height);
        }
        s_ff_config.width = width;
        s_ff_config.height = height;
        if (!s_encoder->open(s_ff_config)) {
            fprintf(stderr, "Error: failed to open FFmpeg encoder at %dx%d\n", width, height);
            s_emu->stop();
            return;
        }
        s_encoder_opened = true;
    }

    // Allocate buffer and capture
    size_t frame_size = (size_t)width * height * 3;
    if (s_pixel_buffer.size() < frame_size) {
        s_pixel_buffer.resize(frame_size);
        s_flipped_buffer.resize(frame_size);
    }

    s_emu->read_screen(s_pixel_buffer.data(), &width, &height);

    // Flip vertically (OpenGL returns bottom-up, FFmpeg expects top-down)
    int stride = width * 3;
    for (int y = 0; y < height; y++) {
        memcpy(s_flipped_buffer.data() + y * stride,
               s_pixel_buffer.data() + (height - 1 - y) * stride,
               stride);
    }

    s_encoder->write_frame(s_flipped_buffer.data(), width, height);
    s_captured_frames++;

    // Report progress
    if (s_progress_callback) {
        s_progress_callback(s_captured_frames, s_total_frames);
    }
}
