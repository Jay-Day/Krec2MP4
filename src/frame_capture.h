#pragma once
#include "emulator.h"
#include "ffmpeg_encoder.h"
#include <functional>
#include <atomic>

using ProgressCallback = std::function<void(int current_frame, int total_frames)>;

// Initialize frame capture with a reference to the emulator and encoder.
// The encoder is opened lazily on the first frame using actual render dimensions.
// total_frames is the expected number of input frames (for progress reporting).
void frame_capture_init(Emulator* emu, FFmpegEncoder* encoder, const FFmpegConfig& ff_config,
                        int total_frames = 0);

// Set a progress callback (called each captured frame).
void frame_capture_set_progress_callback(ProgressCallback cb);

// Set a cancel flag (checked each frame; stops emulation when set).
void frame_capture_set_cancel_flag(std::atomic<bool>* flag);

// The VI frame callback registered with the core.
void frame_capture_callback(unsigned int frame_index);

// Get the number of frames captured so far.
int frame_capture_count();
