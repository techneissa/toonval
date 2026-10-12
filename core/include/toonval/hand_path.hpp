// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "toonval/run_record.hpp"
#include "toonval/sections.hpp"
#include "toonval/song.hpp"

#include <cstdint>
#include <span>
#include <string>

namespace toonval {

// "The usual path is right hand first, then left, then both, and the results screen remembers each
// separately". This is that path as a small state machine over
// the run history, for a whole song or for one section. It keeps no state of its own: the stage
// follows from the runs (RunRecord::hands says which hand a run practised), so the same history
// always gives the same stage, and an undone or deleted run moves it back.
//
// A run counts for a hand when it practised exactly that hand (HandFilter) and graded at least
// `min_graded` notes inside the scope (the section's bars, or the whole song). Its accuracy there
// is 1 - (missed + kWrongPressWeight * wrong) / graded, the error rate the trouble-spot map uses,
// so a Wait Mode run (which has no misses) still shows fumbling. A hand is mastered when a run at
// or above the goal speed reaches `mastery_accuracy`.

enum class PathStage : std::uint8_t {
    RightHand,
    LeftHand,
    BothHands,
    Done, // every hand of the scope is mastered
};

struct HandPathOptions {
    double mastery_accuracy = 0.9;    // 0..1
    double slow_down_below = 0.75;    // a last run under this accuracy asks for a slower speed
    int hand_goal_speed_percent = 80; // one hand is mastered at this speed
    int both_goal_speed_percent = 100;
    int start_speed_percent = 60; // where a hand starts
    int speed_step_percent = 10;  // how much a good run raises the speed, and a bad one lowers it
    int min_graded = 4;           // notes graded in the scope for a run to count
    int recent_runs = 10;         // only the latest runs of each hand are read
};

// What to read. For a section, give its bars (Section::first_measure / last_measure); the song's
// notes tell which hands play there.
struct HandPathScope {
    bool whole_song = true;
    int first_measure = 0; // Measure::number, both ends included; used unless whole_song
    int last_measure = 0;
    bool has_right = true; // the scope has right-hand notes
    bool has_left = true;  // and left-hand notes
};

// The scope of a section of `song`: its bars, and the hands that have notes in its time span
// (drum parts and notes of unknown hand do not count). Null section: the whole song.
HandPathScope scope_of_section(const Song& song, const Section* section);

// One hand's standing in the scope.
struct HandProgress {
    HandFilter hands = HandFilter::Both;
    bool skipped = false;       // the scope has no notes for this hand (or it is not a stage)
    int runs = 0;               // runs counted
    double best_accuracy = 0.0; // of those, 0..1
    int best_speed_percent = 0; // the highest speed with a run at mastery accuracy; 0 for none
    int last_speed_percent = 0; // the latest counted run
    double last_accuracy = 0.0;
    bool mastered = false;
};

struct HandPathStatus {
    PathStage stage = PathStage::RightHand;
    HandProgress right;
    HandProgress left;
    HandProgress both;
};

HandPathStatus hand_path(std::span<const RunRecord> runs, std::uint64_t song_hash,
                         const HandPathScope& scope = {}, const HandPathOptions& options = {});

enum class PathAdvice : std::uint8_t {
    Start,          // not practised yet: begin at the start speed
    RaiseSpeed,     // the last run was accurate: go faster
    KeepPracticing, // close: another run at the same speed
    SlowDown,       // the last run was shaky: slower
    AllDone,        // nothing left on the path
};

struct PathSuggestion {
    PathAdvice advice = PathAdvice::Start;
    HandFilter hands = HandFilter::Right; // what to practise next
    int speed_percent = 60;               // 25..150 in steps of 5; 0 for AllDone
    PathStage stage = PathStage::RightHand;
    // The stage just before this one was finished by the history (a hand mastered): "right hand
    // done, try the left". Set only with Start on a later stage.
    bool moved_on = false;
};

PathSuggestion next_practice(const HandPathStatus& status, const HandPathOptions& options = {});

// A short line for the results screen, in English: "Right hand at 80%: done. Try the left hand at
// 60%.", "Left hand: last run 82% at 70%. Try 80%.", "Everything on this path is done."
std::string path_hint(const HandPathStatus& status, const PathSuggestion& suggestion);

} // namespace toonval
