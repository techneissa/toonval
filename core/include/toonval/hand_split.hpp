// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "toonval/song.hpp"

namespace toonval {

// Hand split for songs whose notes have no hand. The order is:
// track and channel names first (assign_hands_from_names, done by the MIDI loader), then this split
// by pitch for the notes still Unknown. Nothing here changes a Song in place.
//
// What the split promises, for both methods:
// - A note that already has a hand keeps it. Drum notes stay Unknown, and so does nothing else.
// - Notes that start within kChordWindowSeconds of the first note of a group form a chord. A chord
//   goes to one hand as a whole, by where its middle note sits against the split point, unless it
//   is wider than kHandReachSemitones (a major tenth, 16 semitones). Then it is cut once, at the
//   gap between two neighbouring notes whose midpoint is closest to the split point and that leaves
//   both parts within reach (if no cut does, the closest one anyway).
//
// Fixed: the split point decides. A note (or chord) from the split point up is the right hand.
//
// Smart adds two things, measured in semitones and beats so they do not depend on the tempo:
// 1. Registers. Music often has both hands on one side of middle C for a while, or a right hand
//    that dips under it. For each chord the unknown notes from kSplitWindowBeatsBefore beats before
//    it to kSplitWindowBeatsAfter beats after it are fitted with two registers (k-means on their
//    pitches, started at the previous chord's registers, or at the split point +/-6 semitones for
//    the first chord). Starting from the previous registers keeps a passage in the grouping it
//    already had: a repeated figure with a held bass is not regrouped bar by bar. The fit counts
//    only when it looks like two hands: each register holds at least 15% of the window's notes (and
//    two notes) and the register averages are at least 8 semitones apart. Otherwise, for a single
//    line or a narrow passage, the chosen split point decides. A chord belongs to the register it
//    is nearest to (measured by its middle note), with a one semitone pull toward the side of the
//    split point it lies on, so the split point still counts in doubtful cases.
// 2. Voice leading with hysteresis. A chord whose middle note lies within kHysteresisSemitones (3)
//    of the dividing pitch (the midpoint of the registers, or the split point when there are none)
//    continues a line: if one hand's previous chord ended within kFollowSemitones (4) of this
//    chord's nearest note and within kFollowBeats (2) beats before it, that hand takes the chord
//    (the closer line when both qualify). So a right hand melody can dip three semitones under the
//    dividing pitch and come back without changing hands, and a left hand line can climb three
//    above it. A chord further from the dividing pitch goes by its side.
//
// Limits, measured on the two real files in tests/data (see tests/hand_split_test.cpp): pitch alone
// cannot tell hands apart when they interleave. In Bach's C major prelude the right hand's lowest
// arpeggio note sits only a few semitones over the left hand's held notes, so about one note in
// twelve is misjudged. A run that covers more than about an octave and a half within the window
// looks like two registers and is cut in two.
//
// The notes are expected sorted by start tick, as Song::notes always is.

inline constexpr int kMiddleC = 60;
inline constexpr int kHandReachSemitones = 16; // a major tenth
inline constexpr double kChordWindowSeconds = 0.040;
inline constexpr double kSplitWindowBeatsBefore = 2.0;
inline constexpr double kSplitWindowBeatsAfter = 3.0;
inline constexpr int kHysteresisSemitones = 3;
inline constexpr int kFollowSemitones = 4;
inline constexpr double kFollowBeats = 2.0;

enum class SplitMethod : std::uint8_t { Fixed, Smart };

struct HandSplitOptions {
    int split_point = kMiddleC; // 1..127; notes from here up are the right hand
    SplitMethod method = SplitMethod::Smart;
};

// True while some playable (non-drum) note has no hand: the song needs the split, or names.
bool needs_hand_split(const Song& song);

// One hand per note of `song.notes`, same order. Left or Right for every note without a hand and
// not a drum; a note that had a hand keeps it; drum notes are Unknown.
std::vector<Hand> split_hands(const Song& song, const HandSplitOptions& options = {});

// A copy of the song with split_hands() applied to every note, and each part's hand set when all
// its playable notes ended up in one hand.
Song with_split_hands(const Song& song, const HandSplitOptions& options = {});

// How well `assigned` (one hand per note of `truth`) agrees with the hands of `truth`, over the
// notes whose hand is Left or Right in `truth`. Used by the tests against files with named tracks.
struct HandAgreement {
    int compared = 0;
    int matching = 0;
    // 1 when nothing was compared.
    double fraction() const {
        return compared <= 0 ? 1.0 : static_cast<double>(matching) / static_cast<double>(compared);
    }
};
HandAgreement compare_hands(const Song& truth, std::span<const Hand> assigned);

} // namespace toonval
