// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/octave_shift.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <vector>

using namespace toonval;

namespace {

// A small deterministic generator, so "random" tests never change between runs or platforms.
struct Lcg {
    std::uint32_t state = 12345;
    int next(int bound) {
        state = state * 1664525u + 1013904223u;
        return static_cast<int>((state >> 8) % static_cast<std::uint32_t>(bound));
    }
};

// A C major scale walk on the 61-key keyboard (36..96), the way a student plays it.
constexpr std::array<int, 8> kScale{60, 62, 64, 65, 67, 69, 71, 72};

} // namespace

TEST_CASE("octave shift: nothing played yet is not sure") {
    OctaveShiftDetector d(kRange61);
    ShiftResult r = d.result();
    CHECK(r.status == ShiftStatus::NotSure);
    CHECK(r.offset_semitones == 0);
    CHECK(r.confidence == 0.0);
    CHECK(r.evidence == ShiftEvidence::None);
}

TEST_CASE("octave shift, range only: notes an octave above the keyboard propose -12") {
    OctaveShiftDetector d(kRange61); // 36..96
    // Octave-up button pressed: the player plays 84..96 and the keyboard sends 96..108.
    for (int i = 0; i < 12; ++i) {
        d.observe(97 + (i % 6)); // 97..102, all above 96
    }
    ShiftResult r = d.result();
    CHECK(r.status == ShiftStatus::Shift);
    CHECK(r.offset_semitones == -12);
    CHECK(r.evidence == ShiftEvidence::Range);
    CHECK(r.confidence >= 0.75);
}

TEST_CASE("octave shift, range only: notes below the keyboard propose +12, two octaves +24") {
    {
        OctaveShiftDetector d(kRange61);
        for (int i = 0; i < 12; ++i) {
            d.observe(24 + (i % 6)); // 24..29, below 36
        }
        ShiftResult r = d.result();
        CHECK(r.status == ShiftStatus::Shift);
        CHECK(r.offset_semitones == 12);
    }
    {
        OctaveShiftDetector d(kRange61);
        for (int i = 0; i < 12; ++i) {
            d.observe(10 + (i % 6)); // 10..15
        }
        ShiftResult r = d.result();
        // +24 gives 34..39, which still has notes under 36; +36 gives 46..51: the fewest octaves
        // that bring the notes inside.
        CHECK(r.status == ShiftStatus::Shift);
        CHECK(r.offset_semitones == 36);
    }
}

TEST_CASE("octave shift, range only: a few notes are not enough") {
    OctaveShiftDetector d(kRange61);
    for (int i = 0; i < 5; ++i) {
        d.observe(100 + i);
    }
    CHECK(d.result().status == ShiftStatus::NotSure);
}

TEST_CASE("octave shift, range only: notes inside the range never propose") {
    OctaveShiftDetector d(kRange61);
    for (int i = 0; i < 40; ++i) {
        d.observe(kScale[static_cast<std::size_t>(i) % kScale.size()]);
    }
    ShiftResult r = d.result();
    CHECK(r.status != ShiftStatus::Shift);
    CHECK(r.offset_semitones == 0);
}

TEST_CASE("octave shift, range only: single stray notes do not propose") {
    OctaveShiftDetector d(kRange61);
    for (int i = 0; i < 40; ++i) {
        d.observe(kScale[static_cast<std::size_t>(i) % kScale.size()]);
        if (i % 7 == 6) {
            d.observe(100); // a stray note above the keyboard
        }
        CHECK(d.result().status != ShiftStatus::Shift);
    }
}

TEST_CASE("octave shift, range only: a mixed window is not sure") {
    OctaveShiftDetector d(kRange61);
    // Half the notes above, half below: no single shift fixes both.
    for (int i = 0; i < 12; ++i) {
        d.observe(i % 2 == 0 ? 100 : 20);
    }
    CHECK(d.result().status == ShiftStatus::NotSure);
}

TEST_CASE("octave shift, range only: notes spread over the whole keyboard do not fit one shift") {
    OctaveShiftDetector d(kRange61);
    for (int i = 0; i < 12; ++i) {
        d.observe(80 + i * 4); // 80..124: some inside, most outside, wider than the keyboard
    }
    CHECK(d.result().status != ShiftStatus::Shift);
}

TEST_CASE("octave shift, range only: one shift fits notes of different distances above the range") {
    OctaveShiftDetector d(kRange61);
    // 97 and 108 both land inside with -12 (85 and 96), the smallest shift that fits them.
    for (int i = 0; i < 12; ++i) {
        d.observe(i % 2 == 0 ? 97 : 108);
    }
    ShiftResult r = d.result();
    CHECK(r.status == ShiftStatus::Shift);
    CHECK(r.offset_semitones == -12);
}

TEST_CASE("octave shift: out-of-range MIDI numbers are ignored") {
    OctaveShiftDetector d(kRange61);
    for (int i = 0; i < 20; ++i) {
        d.observe(-5);
        d.observe(200);
    }
    CHECK(d.result().status == ShiftStatus::NotSure);
    CHECK(d.result().notes_considered == 0);
}

TEST_CASE("octave shift, with a song: the played notes are the expected ones, no shift") {
    OctaveShiftDetector d(kRange61);
    for (int i = 0; i < 12; ++i) {
        int e = kScale[static_cast<std::size_t>(i) % kScale.size()];
        std::array<int, 1> expected{e};
        d.observe(e, expected);
    }
    ShiftResult r = d.result();
    CHECK(r.status == ShiftStatus::NoShift);
    CHECK(r.evidence == ShiftEvidence::Song);
    CHECK(r.offset_semitones == 0);
    CHECK(r.confidence >= 0.9);
}

TEST_CASE("octave shift, with a song: a consistent -12 inside the range is found") {
    OctaveShiftDetector d(kRange61);
    // The octave-down button is on: every played note is an octave under the expected one, and
    // still inside the keyboard's range, so range alone could never see it.
    for (int i = 0; i < 12; ++i) {
        int e = kScale[static_cast<std::size_t>(i) % kScale.size()];
        std::array<int, 1> expected{e};
        d.observe(e - 12, expected);
    }
    ShiftResult r = d.result();
    CHECK(r.status == ShiftStatus::Shift);
    CHECK(r.offset_semitones == 12);
    CHECK(r.evidence == ShiftEvidence::Song);
    CHECK(r.confidence >= 0.9);
}

TEST_CASE("octave shift, with a song: a transpose button of +2 semitones is found") {
    OctaveShiftDetector d(kRange61);
    // The scale walk has varied intervals, so +2 is unambiguous.
    for (int i = 0; i < 12; ++i) {
        int e = kScale[static_cast<std::size_t>(i) % kScale.size()];
        std::array<int, 1> expected{e};
        d.observe(e + 2, expected);
    }
    ShiftResult r = d.result();
    CHECK(r.status == ShiftStatus::Shift);
    CHECK(r.offset_semitones == -2);
}

TEST_CASE("octave shift, with a song: a chord's notes all count as expected") {
    OctaveShiftDetector d(kRange61);
    // Triads of C, F, G, Am in turn, played one octave high.
    const std::array<std::array<int, 3>, 4> chords{
        {{60, 64, 67}, {65, 69, 72}, {67, 71, 74}, {69, 72, 76}}};
    for (int round = 0; round < 3; ++round) {
        for (const auto& chord : chords) {
            for (int p : chord) {
                d.observe(p + 12, chord);
            }
        }
    }
    ShiftResult r = d.result();
    CHECK(r.status == ShiftStatus::Shift);
    CHECK(r.offset_semitones == -12);
}

TEST_CASE("octave shift, with a song: random noise gives no proposal") {
    OctaveShiftDetector d(kRange61);
    Lcg rng;
    for (int i = 0; i < 300; ++i) {
        std::array<int, 1> expected{kScale[static_cast<std::size_t>(rng.next(8))]};
        d.observe(40 + rng.next(50), expected);
        CAPTURE(i);
        CHECK(d.result().status != ShiftStatus::Shift);
    }
}

TEST_CASE("octave shift, with a song: a few stray notes do not propose") {
    OctaveShiftDetector d(kRange61);
    for (int i = 0; i < 60; ++i) {
        int e = kScale[static_cast<std::size_t>(i) % kScale.size()];
        std::array<int, 1> expected{e};
        // Every fifth note is a slip an octave low; the rest are right.
        d.observe(i % 5 == 4 ? e - 12 : e, expected);
        CAPTURE(i);
        CHECK(d.result().status != ShiftStatus::Shift);
    }
}

TEST_CASE("octave shift, with a song: a student with 60 percent right notes is not shifted") {
    OctaveShiftDetector d(kRange61);
    Lcg rng;
    for (int i = 0; i < 200; ++i) {
        int e = kScale[static_cast<std::size_t>(i) % kScale.size()];
        std::array<int, 1> expected{e};
        bool right = rng.next(10) < 6;
        d.observe(right ? e : e + 1 + rng.next(10), expected);
        CAPTURE(i);
        CHECK(d.result().status != ShiftStatus::Shift);
    }
}

TEST_CASE("octave shift, with a song: two offsets that explain the notes equally are not sure") {
    // The song asks for C4 and C5 together every time and the player holds C3 for it: +12 and +24
    // match every note, so there is no winner and no guess.
    OctaveShiftDetector d(kRange61);
    const std::array<int, 2> expected{60, 72};
    for (int i = 0; i < 12; ++i) {
        d.observe(48, expected);
    }
    CHECK(d.result().status == ShiftStatus::NotSure);
    CHECK(d.result().offset_semitones == 0);
}

TEST_CASE("octave shift, with a song: an octave chord played right is still no shift") {
    // Played 60 and 72 against the same expected pair: 0 matches every note, +12 and -12 only half.
    OctaveShiftDetector d(kRange61);
    const std::array<int, 2> expected{60, 72};
    for (int i = 0; i < 12; ++i) {
        d.observe(i % 2 == 0 ? 60 : 72, expected);
    }
    CHECK(d.result().status == ShiftStatus::NoShift);
}

TEST_CASE("octave shift, mid-session: a real shift is found once it dominates the window") {
    OctaveShiftDetector d(kRange61);
    int status_before_shift = 0;
    for (int i = 0; i < 24; ++i) {
        int e = kScale[static_cast<std::size_t>(i) % kScale.size()];
        std::array<int, 1> expected{e};
        d.observe(e, expected);
        status_before_shift += d.result().status == ShiftStatus::Shift ? 1 : 0;
    }
    CHECK(status_before_shift == 0);
    CHECK(d.result().status == ShiftStatus::NoShift);

    // The player presses the octave-up button; every note now arrives 12 higher.
    int first_shift_note = -1;
    for (int i = 0; i < 24; ++i) {
        int e = kScale[static_cast<std::size_t>(i) % kScale.size()];
        std::array<int, 1> expected{e};
        d.observe(e + 12, expected);
        if (first_shift_note < 0 && d.result().status == ShiftStatus::Shift) {
            first_shift_note = i + 1;
            CHECK(d.result().offset_semitones == -12);
        }
    }
    // Not at the first note (one note proves nothing), but well before the window is all new.
    CHECK(first_shift_note > 3);
    CHECK(first_shift_note <= kOctaveShiftWindow);
    CHECK(d.result().status == ShiftStatus::Shift);
    CHECK(d.result().offset_semitones == -12);
}

TEST_CASE("octave shift: reset forgets everything, and a new range clears the history") {
    OctaveShiftDetector d(kRange61);
    for (int i = 0; i < 12; ++i) {
        d.observe(100 + (i % 3));
    }
    REQUIRE(d.result().status == ShiftStatus::Shift);
    d.reset();
    CHECK(d.result().status == ShiftStatus::NotSure);
    CHECK(d.result().notes_considered == 0);

    for (int i = 0; i < 12; ++i) {
        d.observe(100 + (i % 3));
    }
    REQUIRE(d.result().status == ShiftStatus::Shift);
    d.set_range(kRange88); // 100..102 are inside an 88-key keyboard
    CHECK(d.result().notes_considered == 0);
    for (int i = 0; i < 12; ++i) {
        d.observe(100 + (i % 3));
    }
    CHECK(d.result().status != ShiftStatus::Shift);
}

TEST_CASE("octave shift: more expected pitches than the buffer holds are cut, not overrun") {
    OctaveShiftDetector d(kRange61);
    std::vector<int> many;
    for (int p = 36; p < 36 + 40; ++p) {
        many.push_back(p);
    }
    for (int i = 0; i < 12; ++i) {
        d.observe(60, many);
    }
    // Must not crash; with 40 candidates the result is whatever it is, but it is a valid status.
    ShiftResult r = d.result();
    CHECK(r.confidence >= 0.0);
    CHECK(r.confidence <= 1.0);
}
