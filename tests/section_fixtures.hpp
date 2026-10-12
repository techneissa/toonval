// SPDX-License-Identifier: GPL-3.0-or-later
// Songs for the section tests: plain bars with no notes in them to read.
#pragma once

#include "toonval/song.hpp"
#include "toonval/song_builder.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <random>

namespace toonval::testing {

// `bars` bars of numerator/denominator at `bpm`, one note across all of them. At 120 BPM in 4/4 a
// bar lasts exactly 2 s, so bar n starts at 2 * (n - 1): whole numbers, comparable exactly.
inline Song bars_song(int bars, int bpm = 120, int numerator = 4, int denominator = 4) {
    Song song;
    song.tempo = make_tempo_map(480, bpm);
    song.time_signatures = {TimeSignature{0, static_cast<std::uint8_t>(numerator),
                                          static_cast<std::uint8_t>(denominator)}};
    const std::int64_t bar_ticks = 480LL * 4 * numerator / denominator;
    Note n;
    n.start_tick = 0;
    n.end_tick = bar_ticks * bars;
    n.pitch = 60;
    song.notes.push_back(n);
    REQUIRE(finish_song(song));
    REQUIRE(static_cast<int>(song.measures.size()) == bars);
    return song;
}

// Like bars_song, but with a one-beat pickup bar in front (bar number 0), then `bars` bars of 4/4.
inline Song pickup_song(int bars) {
    Song song;
    song.tempo = make_tempo_map(480, 120);
    song.time_signatures = {TimeSignature{0, 1, 4}, TimeSignature{480, 4, 4}};
    Note n;
    n.end_tick = 480 + 1920LL * bars;
    song.notes.push_back(n);
    REQUIRE(finish_song(song));
    REQUIRE(song.measures.front().number == 0);
    return song;
}

// `first` bars of 4/4 followed by `second` bars of 3/4, at 120 BPM.
inline Song meter_change_song(int first, int second) {
    Song song;
    song.tempo = make_tempo_map(480, 120);
    song.time_signatures = {TimeSignature{0, 4, 4}, TimeSignature{1920LL * first, 3, 4}};
    Note n;
    n.end_tick = 1920LL * first + 1440LL * second;
    song.notes.push_back(n);
    REQUIRE(finish_song(song));
    return song;
}

// A song of 3 to 60 bars, with a random meter and a tempo that changes once in half of them.
inline Song random_song(std::mt19937_64& rng) {
    static constexpr int kMeters[][2] = {{4, 4}, {3, 4}, {6, 8}, {2, 4}, {5, 4}};
    const auto* meter = kMeters[rng() % 5];
    const int bars = 3 + static_cast<int>(rng() % 58);
    const int bpm = 40 + static_cast<int>(rng() % 200);
    Song song;
    song.tempo = make_tempo_map(480, bpm);
    if (rng() % 2 == 0) {
        TempoChange change;
        change.tick = 480LL * 4 * meter[0] / meter[1] * (1 + static_cast<int>(rng() % 2));
        change.us_per_quarter = us_per_quarter_from_bpm(40 + static_cast<double>(rng() % 200));
        song.tempo.changes.push_back(change);
    }
    song.time_signatures = {
        TimeSignature{0, static_cast<std::uint8_t>(meter[0]), static_cast<std::uint8_t>(meter[1])}};
    Note n;
    n.end_tick = 480LL * 4 * meter[0] / meter[1] * bars;
    song.notes.push_back(n);
    REQUIRE(finish_song(song));
    return song;
}

} // namespace toonval::testing
