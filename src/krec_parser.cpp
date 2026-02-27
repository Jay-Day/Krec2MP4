#include "krec_parser.h"
#include <cstdio>
#include <cstring>
#include <ctime>

bool krec_parse(const std::string& path, KrecData& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        fprintf(stderr, "Error: cannot open '%s'\n", path.c_str());
        return false;
    }

    fseek(f, 0, SEEK_END);
    long file_len = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_len < 400) {
        fprintf(stderr, "Error: file too short (%ld bytes)\n", file_len);
        fclose(f);
        return false;
    }

    std::vector<uint8_t> buf(file_len);
    if (fread(buf.data(), 1, file_len, f) != (size_t)file_len) {
        fprintf(stderr, "Error: failed to read file\n");
        fclose(f);
        return false;
    }
    fclose(f);

    // Check magic
    char magic[5] = {};
    memcpy(magic, buf.data(), 4);
    bool is_krc1 = (strcmp(magic, "KRC1") == 0);
    bool is_krc0 = (strcmp(magic, "KRC0") == 0);

    if (!is_krc1 && !is_krc0) {
        fprintf(stderr, "Error: invalid magic '%s' (expected KRC0 or KRC1)\n", magic);
        return false;
    }

    uint32_t header_size = is_krc1 ? 400 : 272;
    if ((uint32_t)file_len < header_size) {
        fprintf(stderr, "Error: file too short for header\n");
        return false;
    }

    memcpy(out.header.magic, magic, 5);

    // App name at offset 4, 128 bytes
    memcpy(out.header.app_name, buf.data() + 4, 128);
    out.header.app_name[127] = 0;

    // Game name at offset 132, 128 bytes
    memcpy(out.header.game_name, buf.data() + 132, 128);
    out.header.game_name[127] = 0;

    // Timestamp at offset 260
    memcpy(&out.header.timestamp, buf.data() + 260, 4);

    // Player number at offset 264
    memcpy(&out.header.player_number, buf.data() + 264, 4);

    // Num players at offset 268
    memcpy(&out.header.num_players, buf.data() + 268, 4);

    // Player names at offset 272 (KRC1 only), 4 x 32 bytes
    memset(out.header.player_names, 0, sizeof(out.header.player_names));
    if (is_krc1) {
        for (int i = 0; i < 4; i++) {
            memcpy(out.header.player_names[i], buf.data() + 272 + i * 32, 32);
            out.header.player_names[i][31] = 0;
        }
    }

    // Parse records after header - extract input frames
    int num_players = out.header.num_players;
    if (num_players < 1) num_players = 1;
    if (num_players > 4) num_players = 4;

    out.input_data.clear();
    out.total_input_frames = 0;
    out.delay_frames = 0;

    int bytes_per_frame = num_players * 4;
    bool in_delay = true; // Track initial delay period

    uint8_t* scan = buf.data() + header_size;
    uint8_t* end = buf.data() + file_len;

    while (scan + 1 < end) {
        uint8_t type = *scan++;

        if (type == 0x12) {
            // Input frame: type(1) + length(2) + data(length)
            if (scan + 2 > end) break;
            uint16_t rlen;
            memcpy(&rlen, scan, 2);
            scan += 2;
            if (rlen > 0) {
                if (scan + rlen > end) break;
                in_delay = false;
                // Append the raw input data (4 bytes per player)
                out.input_data.insert(out.input_data.end(), scan, scan + rlen);
                scan += rlen;
            } else {
                // Zero-length record: kaillera frame delay entry.
                // Insert zero bytes to maintain frame alignment.
                // In RMG-K, these frames get zero input (callback returns early).
                if (in_delay) out.delay_frames++;
                out.input_data.insert(out.input_data.end(), bytes_per_frame, 0);
            }
            out.total_input_frames++;
        } else if (type == 0x14) {
            // Drop: null-terminated nick + 4 bytes player number
            while (scan < end && *scan != 0) scan++;
            if (scan < end) scan++; // skip null
            scan += 4; // player number
        } else if (type == 0x08) {
            // Chat: two null-terminated strings
            while (scan < end && *scan != 0) scan++;
            if (scan < end) scan++;
            while (scan < end && *scan != 0) scan++;
            if (scan < end) scan++;
        } else {
            break; // unknown record type
        }
    }

    return true;
}

void krec_print_info(const KrecData& data, double fps) {
    printf("=== Krec File Info ===\n");
    printf("Format:    %s\n", data.header.magic);
    printf("App:       %s\n", data.header.app_name);
    printf("Game:      %s\n", data.header.game_name);

    time_t t = (time_t)data.header.timestamp;
    struct tm* tm = localtime(&t);
    if (tm) {
        char timebuf[64];
        strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", tm);
        printf("Date:      %s\n", timebuf);
    }

    printf("Player #:  %d\n", data.header.player_number);
    printf("Players:   %d\n", data.header.num_players);
    for (int i = 0; i < data.header.num_players && i < 4; i++) {
        if (data.header.player_names[i][0]) {
            printf("  P%d:      %s\n", i + 1, data.header.player_names[i]);
        }
    }
    printf("Frames:    %d\n", data.total_input_frames);
    if (data.delay_frames > 0) {
        printf("Delay:     %d frames (kaillera frame delay)\n", data.delay_frames);
    }

    int total_sec = (int)(data.total_input_frames / fps);
    printf("Duration:  %d:%02d (at %.0f fps)\n", total_sec / 60, total_sec % 60, fps);
    printf("Input data: %zu bytes\n", data.input_data.size());
}
