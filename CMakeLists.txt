cmake_minimum_required(VERSION 3.20)
project(Krec2MP4 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Fetch SDL3 automatically if not installed
include(FetchContent)
FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG        release-3.2.8
    GIT_SHALLOW    TRUE
)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(SDL3)

find_package(OpenGL REQUIRED)

# --- Shared static library ---
add_library(Krec2MP4Lib STATIC
    src/converter.cpp
    src/krec_parser.cpp
    src/emulator.cpp
    src/vidext.cpp
    src/pif_replay.cpp
    src/frame_capture.cpp
    src/ffmpeg_encoder.cpp
)

target_include_directories(Krec2MP4Lib PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(Krec2MP4Lib PUBLIC
    SDL3::SDL3-static
    OpenGL::GL
)

# --- CLI executable ---
add_executable(Krec2MP4
    src/main.cpp
    res/app.rc
)

target_link_libraries(Krec2MP4 PRIVATE
    Krec2MP4Lib
)

# --- Audio capture plugin DLL ---
add_library(AudioCapturePlugin SHARED
    src/audio_capture_plugin.cpp
)

# Output the DLL next to the executables
set_target_properties(AudioCapturePlugin PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Debug"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# --- GUI executable (Windows only) ---
if(WIN32)
    add_executable(Krec2MP4_GUI WIN32
        src/gui_main.cpp
        res/app.rc
    )

    target_link_libraries(Krec2MP4_GUI PRIVATE
        Krec2MP4Lib
        comctl32
        ole32
    )
endif()
