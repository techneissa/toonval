// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "toonval/progress.hpp"
#include "toonval/sections.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace toonval {

// Sections proposed for a song that has none (a song with no sections gets
// suggested ones from its trouble spots). Pure functions of the song and the run history.
//
// The proposal tiles the song like real sections do. Where the player's history has trouble spots
// (trouble_plan in progress.hpp), each spot is a Trouble section: bars 12-13 stay bars 12-13. The
// stretches between and around them are cut into Phrase sections at a regular bar count, aligned
// to the music: the count restarts at every change of time signature (a cut always falls on a bar
// start, and a bar of 3/4 or 6/8 counts as one bar like any other), and a pickup bar belongs to
// the first phrase. With no history, or no trouble in it, the whole song is cut into phrases.

enum class SuggestionKind : std::uint8_t {
    Trouble, // a spot from trouble_plan: practise this
    Phrase,  // part of the regular split
};

struct SuggestedSection {
    SuggestionKind kind = SuggestionKind::Phrase;
    int first_measure = 1; // Measure::number, both ends included
    int last_measure = 1;
    double start_s = 0.0;
    double end_s = 0.0;
    // Trouble only: the heat of its hottest bar (0..1) and why bars there go wrong.
    double heat = 0.0;
    TroubleReason reason = TroubleReason::MissedNotes;

    friend bool operator==(const SuggestedSection&, const SuggestedSection&) = default;
};

struct SuggestOptions {
    // Bars per phrase, 1..16 (more is 16): 0 or less picks 4 for songs up to 32 bars and 8 for
    // longer ones. A
    // phrase shorter than half of this joins its neighbour in the same stretch. If the proposal
    // would exceed kMaxSectionsPerSong, the phrase length doubles until it fits.
    int phrase_bars = 0;
    TroubleOptions trouble;
};

inline constexpr int kMaxPhraseBars = 16;
inline constexpr int kShortSongBars = 32; // up to this many bars an automatic phrase is 4 bars

struct SectionSuggestions {
    std::vector<SuggestedSection> sections; // in time order, tiling [0, duration_s]
    bool from_history = false;              // at least one Trouble section
    int phrase_bars = 0;                    // the phrase length used
};

// Empty for a song with fewer than two bars, or one too short to cut. `song_hash` picks the song's
// runs out of `runs`; 0 means no history.
SectionSuggestions suggest_sections(const Song& song, std::span<const RunRecord> runs,
                                    std::uint64_t song_hash, const SuggestOptions& options = {});

// Places the markers of a proposal in `sections` (on the exact bar starts; markers that already
// exist are skipped). With `trouble_only`, only the edges of Trouble sections get a marker, so the
// rest of the song stays in the larger sections around them. Returns how many markers were placed.
int apply_suggestions(SongSections& sections, const Song& song,
                      const SectionSuggestions& suggestions, bool trouble_only = false);

} // namespace toonval
