#pragma once

// Function pointer types for the audio capture plugin's custom exports.
// Resolved via GetProcAddress after loading the DLL.

typedef void (*ptr_audio_capture_set_output)(const char* path);
typedef unsigned int (*ptr_audio_capture_get_frequency)(void);
typedef unsigned long long (*ptr_audio_capture_get_bytes_written)(void);
