// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "toonval/keyboard.hpp"

namespace toonval {

// Octave-shift detection. When the player presses the keyboard's own octave or
// transpose buttons, every note arrives shifted. The detector watches the latest note-ons and, when
// they agree on one shift, says so; when they do not, it says NotSure and proposes nothing.
//
// Two kinds of evidence, both pure and deterministic (no clock, no randomness):
//
// 1. Range. The notes land outside the keyboard profile's range. With the last window of notes:
//    - at least kOctaveShiftMinRangeNotes notes were seen, and
//    - at least 75% of them lie outside the range, and
//    - one whole-octave offset (a multiple of 12, within +/-36) brings at least 90% of them inside.
//    Of the offsets that do, the one with the smallest magnitude wins, so a note 100 on a 61-key
//    keyboard (36..96) proposes -12, never -24: the evidence asks for no more than the fewest
//    octaves that make the notes fit. Confidence is the share of notes inside after the shift.
//
// 2. Song. While practising a known song the caller passes, with each note-on, the pitches the song
//    expects around now (the notes the evaluator is waiting for or has just passed). Over the last
//    window of notes that came with an expectation (at least kOctaveShiftMinSongNotes), the
//    detector counts for every offset k in -36..+36 how many played notes satisfy "pitch + k is
//    expected".
//    - The winner must explain at least 60% of those notes AND beat the runner-up (the second best
//      offset, k = 0 included) by at least 25% of them. A tie, or two offsets that explain the
//      notes equally (for example the song asks for C4 and C5 together), is NotSure.
//    - A winner of k = 0 is NoShift. Any other winner is Shift with that offset.
//    A student who plays 60% right notes and the rest wrong shows k = 0 at 60% and every other
//    offset near zero, so slips never read as a shift; one stray octave in five stays NoShift.
//
// Song evidence is used when it gives a decision; otherwise the range evidence decides. The
// result's `offset_semitones` is what to ADD to an arriving pitch to get the key the player meant
// (the keyboard sent 12 too high: -12). It is 0 unless the status is Shift.
//
// A caller that proposes a shift and sees it accepted should reset() the detector, so the old
// notes do not argue for the shift a second time; one that sees it dismissed owns the "don't ask
// again" policy.

inline constexpr int kOctaveShiftWindow = 12;       // note-ons remembered
inline constexpr int kOctaveShiftMinRangeNotes = 6; // before range evidence can decide
inline constexpr int kOctaveShiftMinSongNotes = 8;  // before song evidence can decide
inline constexpr int kOctaveShiftMaxOffset = 36;    // three octaves either way
inline constexpr int kOctaveShiftMaxExpected = 16;  // expected pitches kept per note-on
inline constexpr double kOctaveShiftOutsideShare = 0.75;
inline constexpr double kOctaveShiftFitShare = 0.90;
inline constexpr double kOctaveShiftSongShare = 0.60;
inline constexpr double kOctaveShiftSongMargin = 0.25;

enum class ShiftStatus : std::uint8_t {
    NotSure, // too little evidence, or evidence that disagrees: propose nothing
    NoShift, // the song evidence says the keys are right (offset 0)
    Shift,   // propose `offset_semitones`
};

enum class ShiftEvidence : std::uint8_t { None, Range, Song };

struct ShiftResult {
    ShiftStatus status = ShiftStatus::NotSure;
    int offset_semitones = 0; // add to the arriving pitch; 0 unless status is Shift
    double confidence = 0.0;  // 0..1, the share of notes the decision rests on; 0 when NotSure
    ShiftEvidence evidence = ShiftEvidence::None;
    int notes_considered = 0; // note-ons in the window
};

class OctaveShiftDetector {
  public:
    explicit OctaveShiftDetector(KeyRange range = kRange61) : range_(range) {}

    // A new keyboard profile forgets the notes seen on the old one.
    void set_range(KeyRange range);
    const KeyRange& range() const { return range_; }
    void reset();

    // A note-on as it arrived (0..127; anything else is ignored). The second form also passes what
    // the song expects around now; more than kOctaveShiftMaxExpected pitches keep the first ones.
    void observe(int pitch);
    void observe(int pitch, std::span<const int> expected);

    ShiftResult result() const;

  private:
    struct Entry {
        std::uint8_t pitch = 0;
        std::uint8_t expected_count = 0;
        std::array<std::uint8_t, kOctaveShiftMaxExpected> expected{};
    };

    ShiftResult song_result() const;
    ShiftResult range_result() const;
    const Entry& at(int i) const; // 0 = oldest of the remembered notes

    KeyRange range_;
    std::array<Entry, kOctaveShiftWindow> ring_{};
    int head_ = 0; // next slot to write
    int count_ = 0;
};

} // namespace toonval
