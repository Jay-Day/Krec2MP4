#include "pif_replay.h"
#include <cstdio>
#include <cstring>

// Joybus command constants
enum {
    JCMD_STATUS = 0x00,
    JCMD_CONTROLLER_READ = 0x01,
    JCMD_PAK_READ = 0x02,
    JCMD_PAK_WRITE = 0x03,
    JCMD_RESET = 0xFF
};

static const KrecData* s_krec = nullptr;
static int s_input_frame_index = 0;
static bool s_synced_this_frame = false;
static bool s_replay_finished = false;

// Cached input for current frame (up to 4 players)
static uint32_t s_cached_input[4] = {};
static int s_cached_num_players = 0;

void pif_replay_init(const KrecData* krec) {
    s_krec = krec;
    s_input_frame_index = 0;
    s_synced_this_frame = false;
    s_replay_finished = false;
    s_cached_num_players = 0;
    memset(s_cached_input, 0, sizeof(s_cached_input));
}

void pif_replay_reset_frame_sync() {
    s_synced_this_frame = false;
}

bool pif_replay_finished() {
    return s_replay_finished;
}

int pif_replay_current_frame() {
    return s_input_frame_index;
}

void pif_replay_callback(struct pif* pif) {
    if (!s_krec || s_replay_finished) return;

    int num_players = s_krec->header.num_players;
    if (num_players < 1) num_players = 1;
    if (num_players > 4) num_players = 4;

    // Check if channel 0 has a controller read command
    bool is_controller_read = (pif->channels[0].tx &&
                                pif->channels[0].tx_buf[0] == JCMD_CONTROLLER_READ &&
                                pif->channels[0].rx_buf != nullptr);

    // Only consume a new input frame once per emulator frame
    if (is_controller_read && !s_synced_this_frame) {
        s_synced_this_frame = true;

        // Check if we have more input frames
        int bytes_per_frame = num_players * 4;
        int offset = s_input_frame_index * bytes_per_frame;

        if (offset + bytes_per_frame <= (int)s_krec->input_data.size()) {
            // Read input for each player from the krec data
            s_cached_num_players = num_players;
            for (int i = 0; i < num_players; i++) {
                const uint8_t* src = s_krec->input_data.data() + offset + i * 4;
                memcpy(&s_cached_input[i], src, 4);
            }
            s_input_frame_index++;
        } else {
            // No more input frames - replay is done
            s_replay_finished = true;
            s_cached_num_players = 0;
            memset(s_cached_input, 0, sizeof(s_cached_input));
            return;
        }
    }

    // Write cached input to PIF channels for all players
    for (int i = 0; i < num_players; i++) {
        if (!pif->channels[i].tx || pif->channels[i].rx == nullptr) continue;

        // Clear error bits to show controller as connected
        *pif->channels[i].rx &= ~0xC0;

        uint8_t cmd = pif->channels[i].tx_buf[0];

        if (cmd == JCMD_STATUS || cmd == JCMD_RESET) {
            // Controller detection - return standard N64 controller type
            if (pif->channels[i].rx_buf != nullptr) {
                uint16_t type = 0x0500; // JDT_JOY_ABS_COUNTERS | JDT_JOY_PORT
                pif->channels[i].rx_buf[0] = (uint8_t)(type >> 0);
                pif->channels[i].rx_buf[1] = (uint8_t)(type >> 8);
                pif->channels[i].rx_buf[2] = 0; // No pak
            }
        } else if (cmd == JCMD_CONTROLLER_READ) {
            // Write the cached controller input
            if (pif->channels[i].rx_buf != nullptr && i < s_cached_num_players) {
                pif->channels[i].rx_buf[0] = (s_cached_input[i] >> 24) & 0xFF;
                pif->channels[i].rx_buf[1] = (s_cached_input[i] >> 16) & 0xFF;
                pif->channels[i].rx_buf[2] = (s_cached_input[i] >> 8) & 0xFF;
                pif->channels[i].rx_buf[3] = s_cached_input[i] & 0xFF;
            }
        } else if (cmd == JCMD_PAK_READ) {
            // No controller pak
            if (pif->channels[i].rx_buf != nullptr) {
                pif->channels[i].rx_buf[32] = 255;
            }
        } else if (cmd == JCMD_PAK_WRITE) {
            if (pif->channels[i].rx_buf != nullptr) {
                pif->channels[i].rx_buf[0] = 255;
            }
        }
    }
}
