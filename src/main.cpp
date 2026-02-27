#include "converter.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <filesystem>

namespace fs = std::filesystem;

static void print_usage(const char* prog) {
    printf("Usage: %s [options] <input.krec>\n\n", prog);
    printf("Convert N64 Kaillera replay recordings (.krec) to MP4 video.\n\n");
    printf("Options:\n");
    printf("  --rom <path>          N64 ROM file (required)\n");
    printf("  --output <path>       Output .mp4 file (default: <input>.mp4)\n");
    printf("  --batch               Process all .krec files in <input> directory\n");
    printf("  --core <path>         mupen64plus core DLL (default: ./Core/mupen64plus.dll)\n");
    printf("  --plugin-dir <path>   Plugin directory (default: ./Plugin/)\n");
    printf("  --data-dir <path>     Data directory (default: ./Data/)\n");
    printf("  --ffmpeg <path>       FFmpeg executable (default: ffmpeg)\n");
    printf("  --fps <value>         Override framerate (default: 60 NTSC / 50 PAL)\n");
    printf("  --resolution <WxH>    Output resolution (default: 640x480)\n");
    printf("  --crf <int>           H.264 quality, lower=better (default: 23)\n");
    printf("  --verbose             Verbose logging\n");
    printf("  --help                Show this help\n");
}

static bool parse_resolution(const char* str, int* w, int* h) {
    return sscanf(str, "%dx%d", w, h) == 2 && *w > 0 && *h > 0;
}

static bool parse_args(int argc, char* argv[], AppConfig& config) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return false;
        } else if (strcmp(argv[i], "--rom") == 0 && i + 1 < argc) {
            config.rom_path = argv[++i];
        } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            config.output_path = argv[++i];
        } else if (strcmp(argv[i], "--batch") == 0) {
            config.batch = true;
        } else if (strcmp(argv[i], "--core") == 0 && i + 1 < argc) {
            config.core_path = argv[++i];
        } else if (strcmp(argv[i], "--plugin-dir") == 0 && i + 1 < argc) {
            config.plugin_dir = argv[++i];
        } else if (strcmp(argv[i], "--data-dir") == 0 && i + 1 < argc) {
            config.data_dir = argv[++i];
        } else if (strcmp(argv[i], "--ffmpeg") == 0 && i + 1 < argc) {
            config.ffmpeg_path = argv[++i];
        } else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
            config.fps = atof(argv[++i]);
        } else if (strcmp(argv[i], "--resolution") == 0 && i + 1 < argc) {
            if (!parse_resolution(argv[++i], &config.res_width, &config.res_height)) {
                fprintf(stderr, "Error: invalid resolution '%s' (expected WxH)\n", argv[i]);
                return false;
            }
        } else if (strcmp(argv[i], "--crf") == 0 && i + 1 < argc) {
            config.crf = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--verbose") == 0) {
            config.verbose = true;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Error: unknown option '%s'\n", argv[i]);
            return false;
        } else {
            config.input_path = argv[i];
        }
    }

    if (config.rom_path.empty()) {
        fprintf(stderr, "Error: --rom is required\n");
        return false;
    }
    if (config.input_path.empty()) {
        fprintf(stderr, "Error: input .krec file or directory is required\n");
        return false;
    }

    return true;
}

int main(int argc, char* argv[]) {
    printf("Krec2MP4 - N64 Kaillera Replay to Video Converter\n\n");

    // Set defaults relative to exe location
    std::string exe_dir = get_exe_dir();

    AppConfig config;
    config.core_path = exe_dir + "Core\\mupen64plus.dll";
    config.plugin_dir = exe_dir + "Plugin\\";
    config.data_dir = exe_dir + "Data\\";
    config.ffmpeg_path = exe_dir + "ffmpeg.exe";

    if (!parse_args(argc, argv, config)) {
        if (config.rom_path.empty() && config.input_path.empty()) {
            print_usage(argv[0]);
        }
        return 1;
    }

    if (!check_ffmpeg(config.ffmpeg_path)) return 1;

    // Collect krec files
    std::vector<std::string> krec_files;

    if (config.batch) {
        if (!fs::is_directory(config.input_path)) {
            fprintf(stderr, "Error: '%s' is not a directory (--batch requires a directory)\n",
                    config.input_path.c_str());
            return 1;
        }
        for (auto& entry : fs::directory_iterator(config.input_path)) {
            if (entry.is_regular_file() && entry.path().extension() == ".krec") {
                krec_files.push_back(entry.path().string());
            }
        }
        if (krec_files.empty()) {
            fprintf(stderr, "Error: no .krec files found in '%s'\n", config.input_path.c_str());
            return 1;
        }
        printf("Found %zu .krec files for batch processing.\n", krec_files.size());
    } else {
        if (!fs::is_regular_file(config.input_path)) {
            fprintf(stderr, "Error: '%s' is not a file\n", config.input_path.c_str());
            return 1;
        }
        krec_files.push_back(config.input_path);
    }

    int success = 0;
    int failed = 0;

    for (size_t i = 0; i < krec_files.size(); i++) {
        std::string output;
        if (config.batch) {
            std::string out_dir = config.output_path.empty()
                ? fs::path(krec_files[i]).parent_path().string()
                : config.output_path;
            fs::path out_file = fs::path(out_dir) / fs::path(krec_files[i]).stem();
            out_file.replace_extension(".mp4");
            output = out_file.string();
        } else {
            output = make_output_path(krec_files[i], config.output_path);
        }

        printf("\n[%zu/%zu] ", i + 1, krec_files.size());
        if (convert_one(krec_files[i], output, config)) {
            success++;
        } else {
            failed++;
        }
    }

    printf("\n=== Summary ===\n");
    printf("Success: %d, Failed: %d, Total: %zu\n", success, failed, krec_files.size());

    return failed > 0 ? 1 : 0;
}
