// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/pitch.hpp"

#include <cmath>
#include <limits>

#include <doctest/doctest.h>

using namespace toonval;

TEST_CASE("black keys follow the piano pattern") {
    // C C# D D# E F F# G G# A A# B
    const bool expected[12] = {false, true,  false, true,  false, false,
                               true,  false, true,  false, true,  false};
    for (int pc = 0; pc < 12; ++pc) {
        CHECK(is_black_key(60 + pc) == expected[pc]);
        CHECK(is_black_key(24 + pc) == expected[pc]);
    }
}

TEST_CASE("octaves use scientific pitch notation") {
    CHECK(octave(60) == 4);
    CHECK(octave(59) == 3);
    CHECK(octave(21) == 0);  // A0, lowest piano key
    CHECK(octave(108) == 8); // C8, highest piano key
    CHECK(octave(0) == -1);
}

TEST_CASE("note names") {
    CHECK(note_name(60) == "C4");
    CHECK(note_name(61) == "C#4");
    CHECK(note_name(61, true) == "Db4");
    CHECK(note_name(21) == "A0");
    CHECK(note_name(36) == "C2");
    CHECK(note_name(96) == "C7");
    CHECK(note_name(-1).empty());
    CHECK(note_name(128).empty());
}

TEST_CASE("frequencies round-trip through note numbers") {
    CHECK(frequency_hz(69) == doctest::Approx(440.0));
    CHECK(frequency_hz(60) == doctest::Approx(261.6256).epsilon(1e-6));
    CHECK(frequency_hz(21) == doctest::Approx(27.5));
    CHECK(frequency_hz(69, 442.0) == doctest::Approx(442.0));

    for (int note = 21; note <= 108; ++note) {
        CHECK(note_from_frequency(frequency_hz(note)) == doctest::Approx(note));
    }
    CHECK(note_from_frequency(0.0) < 0.0);
}

TEST_CASE("note_from_frequency reports no note for unusable input") {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    CHECK(note_from_frequency(-440.0) < 0.0);
    CHECK(note_from_frequency(nan) < 0.0);
    CHECK(note_from_frequency(inf) < 0.0);
    CHECK(note_from_frequency(-inf) < 0.0);
    CHECK(note_from_frequency(440.0, nan) < 0.0);
    CHECK(note_from_frequency(440.0, inf) < 0.0);
    CHECK(note_from_frequency(440.0, -inf) < 0.0);
    CHECK(note_from_frequency(440.0, 0.0) < 0.0);
    CHECK(note_from_frequency(440.0, -440.0) < 0.0);
    // Sub-audio frequencies below MIDI 0 are also negative.
    CHECK(note_from_frequency(1.0) < 0.0);
    CHECK_FALSE(std::isnan(note_from_frequency(nan)));
}

TEST_CASE("sharp and flat names for all 12 pitch classes") {
    const char* sharps[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    const char* flats[12] = {"C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"};
    for (int pc = 0; pc < 12; ++pc) {
        CHECK(note_name(60 + pc) == std::string(sharps[pc]) + "4");
        CHECK(note_name(60 + pc, true) == std::string(flats[pc]) + "4");
    }
}

TEST_CASE("negative octaves and the top note") {
    for (int n = 0; n < 12; ++n) {
        CHECK(octave(n) == -1);
    }
    CHECK(octave(12) == 0);
    CHECK(note_name(0) == "C-1");
    CHECK(note_name(1) == "C#-1");
    CHECK(note_name(1, true) == "Db-1");
    CHECK(note_name(11) == "B-1");
    CHECK(note_name(127) == "G9");
    CHECK(note_name(127, true) == "G9");
    CHECK(octave(127) == 9);
}

TEST_CASE("a custom A4 reference works in both directions") {
    CHECK(frequency_hz(69, 442.0) == doctest::Approx(442.0));
    CHECK(frequency_hz(81, 442.0) == doctest::Approx(884.0));
    CHECK(frequency_hz(60, 442.0) == doctest::Approx(442.0 * std::exp2(-9.0 / 12.0)));
    CHECK(note_from_frequency(442.0, 442.0) == doctest::Approx(69.0));
    CHECK(note_from_frequency(440.0, 442.0) < 69.0);
    for (int note = 0; note <= 127; ++note) {
        CHECK(note_from_frequency(frequency_hz(note, 442.0), 442.0) == doctest::Approx(note));
    }
}
