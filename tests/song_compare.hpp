// SPDX-License-Identifier: GPL-3.0-or-later
// Checks that two Songs match field by field, for the writer and recorder round-trip tests.
#pragma once

#include "toonval/song.hpp"

#include <doctest/doctest.h>

namespace toonval::test {

// What a MIDI file keeps: notes (pitch, velocity, ticks, seconds, track, channel, hand, measure),
// the tempo map, time signatures, bars, parts and channel events. Finger is not in a MIDI file.
inline void expect_same_song(const Song& a, const Song& b) {
    REQUIRE(a.tempo.ticks_per_quarter == b.tempo.ticks_per_quarter);
    REQUIRE(a.tempo.changes.size() == b.tempo.changes.size());
    for (std::size_t i = 0; i < a.tempo.changes.size(); ++i) {
        CHECK(a.tempo.changes[i].tick == b.tempo.changes[i].tick);
        CHECK(a.tempo.changes[i].us_per_quarter == b.tempo.changes[i].us_per_quarter);
        CHECK(a.tempo.changes[i].seconds == doctest::Approx(b.tempo.changes[i].seconds));
    }
    REQUIRE(a.time_signatures.size() == b.time_signatures.size());
    for (std::size_t i = 0; i < a.time_signatures.size(); ++i) {
        CHECK(a.time_signatures[i].tick == b.time_signatures[i].tick);
        CHECK(a.time_signatures[i].numerator == b.time_signatures[i].numerator);
        CHECK(a.time_signatures[i].denominator == b.time_signatures[i].denominator);
    }
    REQUIRE(a.measures.size() == b.measures.size());
    for (std::size_t i = 0; i < a.measures.size(); ++i) {
        CHECK(a.measures[i].number == b.measures[i].number);
        CHECK(a.measures[i].start_tick == b.measures[i].start_tick);
        CHECK(a.measures[i].start_s == doctest::Approx(b.measures[i].start_s));
        CHECK(a.measures[i].numerator == b.measures[i].numerator);
        CHECK(a.measures[i].denominator == b.measures[i].denominator);
    }
    CHECK(a.end_tick == b.end_tick);
    CHECK(a.duration_s == doctest::Approx(b.duration_s));

    REQUIRE(a.notes.size() == b.notes.size());
    for (std::size_t i = 0; i < a.notes.size(); ++i) {
        const Note& x = a.notes[i];
        const Note& y = b.notes[i];
        CAPTURE(i);
        CHECK(x.pitch == y.pitch);
        CHECK(x.velocity == y.velocity);
        CHECK(x.start_tick == y.start_tick);
        CHECK(x.end_tick == y.end_tick);
        CHECK(x.start_s == doctest::Approx(y.start_s));
        CHECK(x.duration_s == doctest::Approx(y.duration_s));
        CHECK(x.track == y.track);
        CHECK(x.channel == y.channel);
        CHECK(x.drum == y.drum);
        CHECK(x.hand == y.hand);
        CHECK(x.measure == y.measure);
    }

    REQUIRE(a.parts.size() == b.parts.size());
    for (std::size_t i = 0; i < a.parts.size(); ++i) {
        CHECK(a.parts[i].track == b.parts[i].track);
        CHECK(a.parts[i].channel == b.parts[i].channel);
        CHECK(a.parts[i].name == b.parts[i].name);
        CHECK(a.parts[i].instrument_name == b.parts[i].instrument_name);
        CHECK(a.parts[i].program == b.parts[i].program);
        CHECK(a.parts[i].has_program == b.parts[i].has_program);
        CHECK(a.parts[i].drum == b.parts[i].drum);
        CHECK(a.parts[i].note_count == b.parts[i].note_count);
        CHECK(a.parts[i].hand == b.parts[i].hand);
    }

    REQUIRE(a.events.size() == b.events.size());
    for (std::size_t i = 0; i < a.events.size(); ++i) {
        const ChannelEvent& x = a.events[i];
        const ChannelEvent& y = b.events[i];
        CAPTURE(i);
        CHECK(x.tick == y.tick);
        CHECK(x.time_s == doctest::Approx(y.time_s));
        CHECK(x.track == y.track);
        CHECK(x.channel == y.channel);
        CHECK(x.kind == y.kind);
        CHECK(x.number == y.number);
        CHECK(x.value == y.value);
    }
}

} // namespace toonval::test
