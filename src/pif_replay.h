#pragma once
#include "emulator.h"
#include "krec_parser.h"

// Initialize the PIF replay system with parsed krec data.
void pif_replay_init(const KrecData* krec);

// The PIF sync callback to register with the core.
// Injects krec input data into PIF channels each frame.
void pif_replay_callback(struct pif* pif);

// Reset the synced-this-frame flag (called from frame callback).
void pif_replay_reset_frame_sync();

// Check if replay has finished (all input frames consumed).
bool pif_replay_finished();

// Get the current input frame index (for progress reporting).
int pif_replay_current_frame();
