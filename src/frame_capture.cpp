#include "frame_capture.h"
#include "pif_replay.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

// GL function pointers for PBO operations
typedef void (APIENTRYP PFNGLGENBUFFERSPROC)(GLsizei n, GLuint* buffers);
typedef void (APIENTRYP PFNGLDELETEBUFFERSPROC)(GLsizei n, const GLuint* buffers);
typedef void (APIENTRYP PFNGLBINDBUFFERPROC)(GLenum target, GLuint buffer);
typedef void (APIENTRYP PFNGLBUFFERDATAPROC)(GLenum target, GLsizeiptr size, const void* data, GLenum usage);
typedef void* (APIENTRYP PFNGLMAPBUFFERPROC)(GLenum target, GLenum access);
typedef GLboolean (APIENTRYP PFNGLUNMAPBUFFERPROC)(GLenum target);

static PFNGLGENBUFFERSPROC glGenBuffers_fn = nullptr;
static PFNGLDELETEBUFFERSPROC glDeleteBuffers_fn = nullptr;
static PFNGLBINDBUFFERPROC glBindBuffer_fn = nullptr;
static PFNGLBUFFERDATAPROC glBufferData_fn = nullptr;
static PFNGLMAPBUFFERPROC glMapBuffer_fn = nullptr;
static PFNGLUNMAPBUFFERPROC glUnmapBuffer_fn = nullptr;

#ifndef GL_PIXEL_PACK_BUFFER
#define GL_PIXEL_PACK_BUFFER 0x88EB
#endif
#ifndef GL_STREAM_READ
#define GL_STREAM_READ 0x88E1
#endif
#ifndef GL_READ_ONLY
#define GL_READ_ONLY 0x88B8
#endif

static Emulator* s_emu = nullptr;
static FFmpegEncoder* s_encoder = nullptr;
static FFmpegConfig s_ff_config;
static bool s_encoder_opened = false;
static int s_captured_frames = 0;
static int s_total_frames = 0;
static bool s_speed_limiter_disabled = false;
static ProgressCallback s_progress_callback;
static std::atomic<bool>* s_cancel_flag = nullptr;

// PBO double-buffering state
static GLuint s_pbo[2] = {0, 0};
static int s_pbo_index = 0;
static bool s_pbo_initialized = false;
static bool s_pbo_has_data = false;
static int s_pbo_width = 0;
static int s_pbo_height = 0;

// Encoding worker thread — flips + writes frames off the emulation thread
static std::thread s_encode_thread;
static std::mutex s_encode_mutex;
static std::condition_variable s_encode_cv;
static std::condition_variable s_encode_done_cv;
static std::vector<uint8_t> s_staging_buffer;   // raw pixels from PBO (bottom-up)
static std::vector<uint8_t> s_flipped_buffer;   // flipped pixels (top-down)
static bool s_encode_has_work = false;
static bool s_encode_shutdown = false;
static int s_encode_width = 0;
static int s_encode_height = 0;

static void encode_worker() {
    while (true) {
        std::unique_lock<std::mutex> lock(s_encode_mutex);
        s_encode_cv.wait(lock, [] { return s_encode_has_work || s_encode_shutdown; });

        if (s_encode_shutdown && !s_encode_has_work) break;

        int width = s_encode_width;
        int height = s_encode_height;
        int stride = width * 3;
        size_t frame_size = (size_t)stride * height;

        if (s_flipped_buffer.size() < frame_size)
            s_flipped_buffer.resize(frame_size);

        // Flip vertically
        for (int y = 0; y < height; y++) {
            memcpy(s_flipped_buffer.data() + y * stride,
                   s_staging_buffer.data() + (height - 1 - y) * stride, stride);
        }

        s_encode_has_work = false;
        lock.unlock();
        s_encode_done_cv.notify_one();

        // Write to FFmpeg (outside lock so emulation thread can continue)
        s_encoder->write_frame(s_flipped_buffer.data(), width, height);
        s_captured_frames++;

        if (s_progress_callback) {
            s_progress_callback(s_captured_frames, s_total_frames);
        }
    }
}

static void start_encode_thread() {
    s_encode_shutdown = false;
    s_encode_has_work = false;
    s_encode_thread = std::thread(encode_worker);
}

static void stop_encode_thread() {
    {
        std::lock_guard<std::mutex> lock(s_encode_mutex);
        s_encode_shutdown = true;
    }
    s_encode_cv.notify_one();
    if (s_encode_thread.joinable())
        s_encode_thread.join();
}

// Wait for the encode thread to finish its current frame
static void wait_for_encode() {
    std::unique_lock<std::mutex> lock(s_encode_mutex);
    s_encode_done_cv.wait(lock, [] { return !s_encode_has_work; });
}

static bool init_pbo_functions() {
    glGenBuffers_fn = (PFNGLGENBUFFERSPROC)SDL_GL_GetProcAddress("glGenBuffers");
    glDeleteBuffers_fn = (PFNGLDELETEBUFFERSPROC)SDL_GL_GetProcAddress("glDeleteBuffers");
    glBindBuffer_fn = (PFNGLBINDBUFFERPROC)SDL_GL_GetProcAddress("glBindBuffer");
    glBufferData_fn = (PFNGLBUFFERDATAPROC)SDL_GL_GetProcAddress("glBufferData");
    glMapBuffer_fn = (PFNGLMAPBUFFERPROC)SDL_GL_GetProcAddress("glMapBuffer");
    glUnmapBuffer_fn = (PFNGLUNMAPBUFFERPROC)SDL_GL_GetProcAddress("glUnmapBuffer");

    return glGenBuffers_fn && glDeleteBuffers_fn && glBindBuffer_fn &&
           glBufferData_fn && glMapBuffer_fn && glUnmapBuffer_fn;
}

static void init_pbos(int width, int height) {
    size_t size = (size_t)width * height * 3;
    glGenBuffers_fn(2, s_pbo);
    for (int i = 0; i < 2; i++) {
        glBindBuffer_fn(GL_PIXEL_PACK_BUFFER, s_pbo[i]);
        glBufferData_fn(GL_PIXEL_PACK_BUFFER, size, nullptr, GL_STREAM_READ);
    }
    glBindBuffer_fn(GL_PIXEL_PACK_BUFFER, 0);
    s_pbo_width = width;
    s_pbo_height = height;
    s_pbo_index = 0;
    s_pbo_has_data = false;
    s_pbo_initialized = true;

    s_staging_buffer.resize(size);
    start_encode_thread();
}

static void cleanup_pbos() {
    if (s_pbo_initialized) {
        stop_encode_thread();
        glDeleteBuffers_fn(2, s_pbo);
        s_pbo[0] = s_pbo[1] = 0;
        s_pbo_initialized = false;
        s_pbo_has_data = false;
    }
}

// Map PBO, copy to staging buffer, hand off to encode thread
static void process_pbo_async(int pbo_idx, int width, int height) {
    // Wait for encode thread to finish previous frame before overwriting staging buffer
    wait_for_encode();

    glBindBuffer_fn(GL_PIXEL_PACK_BUFFER, s_pbo[pbo_idx]);
    void* ptr = glMapBuffer_fn(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
    if (ptr) {
        size_t frame_size = (size_t)width * height * 3;
        memcpy(s_staging_buffer.data(), ptr, frame_size);
        glUnmapBuffer_fn(GL_PIXEL_PACK_BUFFER);

        // Signal encode thread
        {
            std::lock_guard<std::mutex> lock(s_encode_mutex);
            s_encode_width = width;
            s_encode_height = height;
            s_encode_has_work = true;
        }
        s_encode_cv.notify_one();
    }
    glBindBuffer_fn(GL_PIXEL_PACK_BUFFER, 0);
}

void frame_capture_init(Emulator* emu, FFmpegEncoder* encoder, const FFmpegConfig& ff_config,
                        int total_frames) {
    s_emu = emu;
    s_encoder = encoder;
    s_ff_config = ff_config;
    s_encoder_opened = false;
    s_captured_frames = 0;
    s_total_frames = total_frames;
    s_speed_limiter_disabled = false;
    // Keep buffers allocated across batch runs to avoid reallocation
    s_progress_callback = nullptr;
    s_cancel_flag = nullptr;
    s_pbo_initialized = false;
    s_pbo_has_data = false;
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

void frame_capture_flush() {
    if (s_pbo_initialized && s_pbo_has_data) {
        int prev = 1 - s_pbo_index;
        process_pbo_async(prev, s_pbo_width, s_pbo_height);
    }
    // Wait for final encode to complete before cleanup
    wait_for_encode();
    cleanup_pbos();
}

void frame_capture_callback(unsigned int frame_index) {
    // Check cancel flag
    if (s_cancel_flag && s_cancel_flag->load()) {
        if (s_emu) {
            cleanup_pbos();
            s_emu->stop();
        }
        return;
    }

    // Disable speed limiter on first frame for maximum conversion speed
    if (!s_speed_limiter_disabled && s_emu) {
        int limiter = 0; // 0 = off
        s_emu->core_do_command(M64CMD_CORE_STATE_SET, M64CORE_SPEED_LIMITER, &limiter);
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

    // Get screen dimensions
    int width = 0, height = 0;
    s_emu->read_screen(nullptr, &width, &height);
    if (width <= 0 || height <= 0) return;

    // Open encoder lazily on first frame
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

    // Initialize PBOs + encode thread on first frame
    if (!s_pbo_initialized) {
        if (!init_pbo_functions()) {
            fprintf(stderr, "Warning: PBO functions not available, falling back to sync readback\n");
            size_t frame_size = (size_t)width * height * 3;
            std::vector<uint8_t> pixel_buffer(frame_size);
            s_emu->read_screen(pixel_buffer.data(), &width, &height);
            if (s_flipped_buffer.size() < frame_size) s_flipped_buffer.resize(frame_size);
            int stride = width * 3;
            for (int y = 0; y < height; y++) {
                memcpy(s_flipped_buffer.data() + y * stride,
                       pixel_buffer.data() + (height - 1 - y) * stride, stride);
            }
            s_encoder->write_frame(s_flipped_buffer.data(), width, height);
            s_captured_frames++;
            if (s_progress_callback) s_progress_callback(s_captured_frames, s_total_frames);
            return;
        }
        init_pbos(width, height);
    }

    // --- PBO double-buffered async readback + threaded encode ---

    // 1. If previous frame is ready, hand it to the encode thread
    if (s_pbo_has_data) {
        int prev = 1 - s_pbo_index;
        process_pbo_async(prev, s_pbo_width, s_pbo_height);
    }

    // 2. Start async readback of current frame into current PBO
    glBindBuffer_fn(GL_PIXEL_PACK_BUFFER, s_pbo[s_pbo_index]);
    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
    glBindBuffer_fn(GL_PIXEL_PACK_BUFFER, 0);

    s_pbo_has_data = true;
    s_pbo_index = 1 - s_pbo_index;
}
