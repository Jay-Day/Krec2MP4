#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct KrecHeader {
    char magic[5];         // "KRC1" + null
    char app_name[128];
    char game_name[128];
    uint32_t timestamp;
    int32_t player_number;
    int32_t num_players;
    char player_names[4][32];
};

struct KrecData {
    KrecHeader header;
    // Flat array of input frames. Each frame is num_players * 4 bytes.
    std::vector<uint8_t> input_data;
    int total_input_frames;
    int delay_frames;  // Number of initial 0-length records (kaillera frame delay)
};

// Parse a .krec file into KrecData. Returns true on success.
bool krec_parse(const std::string& path, KrecData& out);

// Print krec metadata to stdout.
void krec_print_info(const KrecData& data, double fps);
