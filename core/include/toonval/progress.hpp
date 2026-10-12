// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "toonval/run_record.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace toonval {

// What the progress and motivation views show, as pure functions over a list of RunRecords.
// Nothing here reads a clock, a file or a database.
//
// The list is the run history oldest first, in the order the runs were saved (the app reads it
// ORDER BY rowid). "Last", "recent" and "older" mean position in the list, never wall time: a Pi
// without a battery clock can start at a wrong date, and a run saved then must not reorder the
// history. Only the day keys (RunRecord::local_day) are compared as days, and `today` is a day key
// the caller supplies, so runs whose day is after `today` (saved while the clock ran ahead) are
// ignored by the functions that take it.
//
// A song is identified by RunRecord::song_hash. A hash of 0 means "no identity" and gives empty
// results; a generated exercise must hash its parameters to get one.
//
// Every function takes the list by span, never changes it, and runs in O(n log n) or better for n
// records (the trouble-spot map reads only the latest runs of the song, so a long history costs a
// single backward scan).

// ---- Duplicates

// Two records are the same run saved twice (an import repeated, a write retried) when they began at
// the same second on the same song with the same mode, hands, speed and transposition.
bool same_run(const RunRecord& a, const RunRecord& b);

// Indices of the records of `runs` that are not a repeat of an earlier one, in list order.
// Functions below that count time or runs ignore repeats the same way.
std::vector<std::size_t> unique_run_indices(std::span<const RunRecord> runs);

// ---- Daily practice

// Practice on one local day.
struct DayPractice {
    std::int32_t day = 0;
    std::int64_t practised_ms = 0;
    int runs = 0;

    double minutes() const { return static_cast<double>(practised_ms) / 60'000.0; }
};

// One entry per day that has a run, days ascending. Time is RunRecord::duration_ms summed.
std::vector<DayPractice> daily_practice(std::span<const RunRecord> runs);

inline constexpr int kMaxRecentDays = 3660;

// `count` entries (0..kMaxRecentDays) for the days ending at `today`, oldest first, with zeros for
// days without practice: the bars of a week or month chart. Days after `today` are left out.
std::vector<DayPractice> recent_days(std::span<const RunRecord> runs, std::int32_t today,
                                     int count);

inline constexpr int kMaxDailyGoalMinutes = 720; // as AppSettings::daily_goal_minutes

struct DailyGoalProgress {
    int goal_minutes = 0; // clamped to 0..720
    bool has_goal = false;
    double minutes_today = 0.0;
    double fraction = 0.0; // minutes_today / goal, at most 1; 0 without a goal
    bool met = false;      // has_goal and the goal's time is reached
};
// Today's minutes against the daily goal (0 turns the goal off).
DailyGoalProgress daily_goal_progress(std::span<const RunRecord> runs, std::int32_t today,
                                      int goal_minutes);

// A day counts toward the streak when its practice adds up to at least `min_practised_ms`.
struct StreakRule {
    // Any practice at all (the default): one millisecond.
    std::int64_t min_practised_ms = 1;

    static StreakRule any_practice() { return StreakRule{}; }
    // The daily goal must be reached. A goal of 0 means no goal, so any practice counts.
    static StreakRule daily_goal(int goal_minutes);
};

struct Streak {
    // Days in a row that count, ending today when today counts and otherwise ending yesterday. So
    // a day that has not been practised yet does not break the streak; it breaks once a whole day
    // passes without a counting day. Calendar days by day key: a gap of one day or more ends it.
    int current = 0;
    int longest = 0;         // the longest such run in the history (days up to today)
    bool today_done = false; // today counts already
    bool at_risk = false;    // current > 0 and today does not count yet: practise to keep it
};
Streak practice_streak(std::span<const RunRecord> runs, std::int32_t today,
                       const StreakRule& rule = {});

// ---- Per-song bests and history

// Runs with fewer graded notes than this never count as a best: a run stopped after two notes is
// not an achievement. (A run of a very short song needs this many notes to count.)
inline constexpr int kMinGradedForBest = 8;

// The best accuracy for one way of playing a song. Modes are kept apart because Wait Mode grades
// every note as played; the hands are what was practised (Both, Left or Right).
struct BestEntry {
    RunMode mode = RunMode::Flow;
    HandFilter hands = HandFilter::Both;
    int speed_percent = 100;
    double accuracy = 0.0; // of the best run, 0..1
    int graded_notes = 0;  // of the best run
    int longest_combo = 0; // of the best run
    std::int32_t day = 0;  // of the best run
    int runs = 0;          // runs with this mode, hands and speed that counted
};
// One entry per (mode, hands, speed) the song was played with, sorted by mode, hands, then speed.
// The best run has the highest accuracy; a tie goes to the run with more graded notes, then to the
// later one.
std::vector<BestEntry> song_bests(std::span<const RunRecord> runs, std::uint64_t song_hash,
                                  int min_graded = kMinGradedForBest);

struct HistoryFilter {
    std::optional<RunMode> mode;
    std::optional<HandFilter> hands;
    int min_graded = 1; // runs with fewer graded notes are left out
};

// One point of the history graph: a run, or several runs merged when there are too many to draw.
struct HistoryPoint {
    std::int32_t day = 0;    // of the last run merged
    double accuracy = 0.0;   // hit / graded over the runs merged, 0..1
    int speed_percent = 100; // the highest speed among the runs merged
    int runs = 1;
    int graded_notes = 0;
};
// The runs of a song oldest first as points, at most `max_points` of them. When there are more
// runs, consecutive runs are merged into `max_points` buckets of nearly equal size (the first
// bucket is the oldest), so the series always covers the whole history and keeps its order.
std::vector<HistoryPoint> history_series(std::span<const RunRecord> runs, std::uint64_t song_hash,
                                         std::size_t max_points, const HistoryFilter& filter = {});

// ---- Change from your last attempt

enum class Trend : std::uint8_t { Better, Same, Worse };

// A change in accuracy of less than this (half a percentage point) is "same".
inline constexpr double kTrendEpsilon = 0.005;

struct AttemptChange {
    bool has_previous =
        false;             // false: the first attempt like this (the fields after the combo are 0)
    double accuracy = 0.0; // of `current`, always set
    int longest_combo = 0; // of `current`, always set
    double previous_accuracy = 0.0;
    double accuracy_delta = 0.0; // accuracy - previous_accuracy
    int previous_longest_combo = 0;
    int combo_delta = 0;
    Trend trend = Trend::Same;
    // Beats every earlier attempt like this one (and the run has kMinGradedForBest notes).
    bool new_best = false;
    std::int32_t previous_day = 0;
};
// Compares `current` with the latest earlier run of the same song, mode, hands and speed. `runs`
// may already contain `current` (it is skipped as a repeat of itself) or not. Runs with no graded
// notes are not attempts.
AttemptChange compare_with_last_attempt(std::span<const RunRecord> runs, const RunRecord& current);

// ---- Trouble spots

struct TroubleOptions {
    // Only the latest runs of the song are read, newest first.
    int max_runs = 30;
    // A run's weight is 0.5 ^ (age / half_life_runs), age being how many runs of the song came
    // after it. Practising a bar in a loop therefore cools it down in a few runs.
    double half_life_runs = 4.0;
    // A bar needs this many graded notes over those runs, or it has no heat (too few to tell).
    int min_attempts = 4;
    // A bar is trouble when its heat is at least this: a quarter of its notes missed, recently.
    double min_heat = 0.25;
    int max_spots = 3;
    int max_loop_bars = 4;
};

// A wrong press counts as this fraction of a missed note in a bar's error rate, so Wait Mode (which
// has no misses) still shows where the player fumbles.
inline constexpr double kWrongPressWeight = 0.5;

enum class TroubleReason : std::uint8_t {
    MissedNotes, // notes not played in time (or at all) outweigh wrong presses
    WrongNotes,  // wrong presses outweigh missed notes
};
std::string_view trouble_reason_text(TroubleReason reason);

// One bar of the trouble-spot map.
struct BarHeat {
    int measure = 1;
    // 0..1: the error rate of the bar, recent runs weighing more: per run
    // min(1, (missed + kWrongPressWeight * wrong) / graded). 0 when `reliable` is false.
    double heat = 0.0;
    int attempts = 0;      // graded notes seen in the bar over the runs read
    bool reliable = false; // attempts >= min_attempts
};

// Something to practise: a bar, or a few adjacent trouble bars, to loop.
struct TroubleSpot {
    int first_measure = 1; // the loop span, both ends included
    int last_measure = 1;
    double heat = 0.0;        // of its hottest bar
    int attempts = 0;         // graded notes seen in its bars over the runs read
    int runs_seen = 0;        // runs read that played any of its bars
    int runs_with_errors = 0; // of those, runs that missed or played a wrong note in it
    TroubleReason reason = TroubleReason::MissedNotes;
};

struct TroublePlan {
    std::vector<TroubleSpot> spots; // hottest first; the first is what to practise today
    std::vector<BarHeat> bars;      // every bar played in the runs read, sorted by measure
    int runs_read = 0;
};
// The bars to practise today for a song. A bar is trouble when it is reliable and its heat is at
// least `min_heat`. The hottest trouble bar starts a spot (ties go to the bar with more attempts,
// then the lower number); the spot grows toward its hotter trouble neighbour, one bar at a time,
// up to `max_loop_bars` (an equally hot pair goes to the later bar); its bars are used up and the
// next hottest unused bar starts the next spot, up to `max_spots`. Nothing is returned when no bar
// is trouble.
TroublePlan trouble_plan(std::span<const RunRecord> runs, std::uint64_t song_hash,
                         const TroubleOptions& options = {});

// To loop a spot, pass its first_measure and last_measure (Measure::number) to measures_span() in
// transport.hpp for the seconds.

} // namespace toonval
