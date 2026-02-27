# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build

```bash
# Configure (first time only)
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build all targets
cmake --build build --config Release
```

Outputs in `build/Release/`:
- `Krec2MP4.exe` — CLI tool (console subsystem)
- `Krec2MP4_GUI.exe` — Win32 GUI (windows subsystem)
- `AudioCapturePlugin.dll` — mupen64plus audio plugin for PCM capture

No tests. No linter. Verify by building successfully and running a conversion.

## Runtime Dependencies

The built executables expect these alongside them:
- `Core/mupen64plus.dll` — mupen64plus core (RMG-K build with `set_pif_sync_callback` extension)
- `Plugin/` — mupen64plus-video-GLideN64.dll, mupen64plus-rsp-hle.dll, RMG-Input.dll
- `Data/` — mupen64plus.ini (ROM database), GLideN64.ini
- `ffmpeg.exe` — for video encoding
- `AudioCapturePlugin.dll` — built by this project

## Architecture

Converts N64 Kaillera replay files (.krec) to MP4 by running headless mupen64plus emulation with input injection.

### Two executables, one shared library

`Krec2MP4Lib` (static library) contains all conversion logic. Both `main.cpp` (CLI) and `gui_main.cpp` (Win32 GUI) link against it. The GUI runs conversion on a worker thread, communicating via `WM_APP_*` messages.

### Conversion pipeline (`converter.cpp:convert_one`)

1. **Parse krec** — `krec_parser.cpp` reads KRC0/KRC1 format, extracts input frames into flat buffer
2. **Init emulator** — `emulator.cpp` loads core DLL, resolves API functions, loads plugins, patches GLideN64.ini
3. **Configure deterministic replay** — `emulator.cpp:apply_deterministic_settings` sets core config to match RMG-K kaillera mode exactly; `configure_controllers_for_replay` marks controllers as present via RMG-Input config
4. **Register callbacks** — PIF sync callback (`pif_replay.cpp`) for input injection, VI frame callback (`frame_capture.cpp`) for screen capture
5. **Execute** — `emulator.cpp:execute` blocks until emulation stops. During execution:
   - Each VI interrupt fires `frame_capture_callback` which reads the screen via OpenGL, flips vertically, pipes RGB24 to FFmpeg
   - Each PIF sync fires `pif_replay_callback` which writes krec controller input into PIF RAM
   - Audio plugin (`audio_capture_plugin.cpp`) captures raw PCM to a temp file
6. **Mux** — FFmpeg combines temp video + temp audio into final MP4 with `-itsscale` for A/V sync correction

### Key subsystems

- **PIF replay** (`pif_replay.cpp`) — Injects controller input from krec data at the PIF RAM level. Handles JCMD_STATUS, CONTROLLER_READ, PAK_READ/WRITE. Uses per-frame sync flag to consume exactly one input per emulator frame.
- **Video extension** (`vidext.cpp`) — Headless SDL3+OpenGL implementation. Creates a hidden window, uses `glFinish()` instead of buffer swap, vsync disabled.
- **FFmpeg encoder** (`ffmpeg_encoder.cpp`) — Spawns FFmpeg as child process, pipes raw RGB24 frames to stdin. On Windows uses `CreateProcess` + pipe handles; encoder opened lazily on first frame to use actual render dimensions.
- **Audio capture** (`audio_capture_plugin.cpp`) — Standalone DLL implementing mupen64plus audio plugin API. Writes raw s16le stereo PCM. Byte-swaps N64's big-endian 32-bit audio words to little-endian interleaved stereo.

## Critical Implementation Details

### Krec frame delay records
Kaillera recordings contain 0-length 0x12 records at the start (frame delay period). The parser must insert zero bytes for these to maintain frame alignment. Without this, the entire input stream shifts by N frames, causing desync mid-replay.

### Deterministic settings
Must exactly match RMG-K kaillera config: `RandomizeInterrupt=false`, `R4300Emulator=2`, `CountPerOp=0`, `CountPerOpDenomPot=0`, `SiDmaDuration=-1`, `DisableExtraMem=false`, `DisableSaveFileLoading=true`. Controller profiles must have `PluggedIn=true` so `process_controller_command` doesn't short-circuit.

### GLideN64 config
GLideN64 ignores the mupen64plus config API and reads its own `GLideN64.ini` file. Resolution, MSAA, and anisotropic filtering are patched directly in the `[User]` section of that file during `Emulator::init`.

### A/V sync
Video is encoded at a fixed FPS (default 60) but N64 NTSC runs at ~59.94. The mux step uses `-itsscale` computed from actual audio duration vs video duration to correct drift.

### Speed factor
Frame capture sets `M64CORE_SPEED_FACTOR=500` (5x) on the first frame for faster conversion. The speed limiter remains active (not disabled) to maintain emulation stability.

## Related Repositories

- **RMG-K** (`C:\Users\JD\repos\RMG-K`) — The mupen64plus frontend with kaillera netplay support. Our core DLL and plugins come from here.
- **n02-rmg** (`C:\Users\JD\repos\n02-rmg`) — Kaillera client library with recording/playback. Contains the player module that reads krec files during RMG-K playback.
