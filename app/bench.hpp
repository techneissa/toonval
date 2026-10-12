// SPDX-License-Identifier: GPL-3.0-or-later
// `toonval --bench`: draws a synthetic free-play scene (rising notes, chords, held notes, sparks)
// with no MIDI and prints frame-time statistics per note style (each frame's CPU and GPU work, up
// to a one-pixel read-back, without the wait for the display) and the effects level the
// auto-degrade reached. Also renders exact screenshots (`--screenshot`).
#pragma once

#include "toonval/look.hpp"

#include <string>
#include <vector>

namespace toonval::app {

struct BenchOptions {
    std::vector<NoteStyle> styles; // run each in turn; empty means the look's own style
    int notes = 200;               // notes on screen at once, about
    double seconds = 5.0;          // measured time per style, after a short warm-up
    int width = 1920;              // pixels drawn
    int height = 1080;
    bool exact_window = false;     // --window: draw straight into a window of exactly that size
    bool degrade = true;           // run the EffectsGovernor; off measures full effects only
    // Screenshot mode: step a fixed 60 Hz clock to `at_seconds` and save that frame as a PNG.
    std::string screenshot;
    double at_seconds = 3.0;
};

// Returns the process exit code.
int run_bench(const BenchOptions& options, const LookSettings& look);

} // namespace toonval::app
