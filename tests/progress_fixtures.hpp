// SPDX-License-Identifier: GPL-3.0-or-later
// Builders for run records shared by the progress tests.
#pragma once

#include "toonval/run_record.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace toonval::testing {

struct RunSpec {
    std::uint64_t song = 1;
    RunMode mode = RunMode::Flow;
    HandFilter hands = HandFilter::Both;
    int speed = 100;
    std::int32_t day = 100;
    // Practised time in minutes.
    double minutes = 5.0;
    // The whole run: `graded` notes of which `missed` were missed. Used when `bars` is empty.
    int graded = 40;
    int missed = 0;
    int longest_combo = 10;
    // Bars, if the test cares about them; the totals above are then derived from these.
    std::vector<RunBar> bars;
    // Makes two otherwise equal runs different records; the default gives every run its own.
    std::int64_t start_utc_s = -1;
};

inline RunRecord make(RunSpec spec) {
    static std::int64_t serial = 0;
    RunRecord r;
    r.song_hash = spec.song;
    r.song_title = "Song " + std::to_string(spec.song);
    r.mode = spec.mode;
    r.hands = spec.hands;
    r.speed_percent = spec.speed;
    r.local_day = spec.day;
    r.start_utc_s = spec.start_utc_s >= 0 ? spec.start_utc_s : spec.day * 86'400LL + ++serial;
    r.end_utc_s = r.start_utc_s + static_cast<std::int64_t>(spec.minutes * 60.0);
    r.duration_ms = static_cast<std::int64_t>(spec.minutes * 60'000.0);
    r.longest_combo = spec.longest_combo;
    int graded = spec.graded;
    int missed = spec.missed;
    if (!spec.bars.empty()) {
        graded = 0;
        missed = 0;
        for (const RunBar& b : spec.bars) {
            graded += b.graded;
            missed += b.missed;
        }
        r.measures = spec.bars;
    }
    r.all = {graded, graded - missed};
    (spec.hands == HandFilter::Left ? r.left : r.right) = r.all;
    r.counts.perfect = graded - missed;
    r.counts.missed = missed;
    return sanitized(r);
}

// A run of a song of `bars` bars with 4 graded notes in each, where bars in `bad` miss `miss` of
// theirs.
inline RunRecord make_bars(std::uint64_t song, int bars, const std::vector<int>& bad, int miss = 3,
                           std::int32_t day = 100, int wrong = 0) {
    RunSpec spec;
    spec.song = song;
    spec.day = day;
    for (int m = 1; m <= bars; ++m) {
        bool is_bad = std::find(bad.begin(), bad.end(), m) != bad.end();
        spec.bars.push_back({m, 4, is_bad ? miss : 0, is_bad ? wrong : 0});
    }
    return make(spec);
}

} // namespace toonval::testing
