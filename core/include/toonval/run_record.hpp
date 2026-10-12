// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "toonval/evaluator.hpp"
#include "toonval/note_window.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace toonval {

// One finished practice run, as plain values the app can keep in SQLite (columns, or the blob that
// serialize_run_record() makes). The progress functions in progress.hpp read lists of these.
//
// Days. Core has no time zone database. The caller decides which local calendar day a run belongs
// to and passes it as `local_day`: the number of days since 1970-01-01 of the local date (see
// local_day_from_utc for a fixed offset). A run belongs to the day it started on, so one that
// crosses midnight is not split. Because the key comes from the caller, a 23 or 25 hour day (DST)
// is still one day, and a run's wall times are never used to find its day again.
//
// Every field has a range; sanitized() is the one place that knows them. make_run_record, the
// serializer and the parser all go through it, so a record that came from a damaged file cannot
// reach the progress functions out of range.

enum class RunMode : std::uint8_t { Flow, Wait };

inline constexpr std::size_t kMaxRunTitleBytes = 256;
inline constexpr int kMaxRunCount = 1'000'000; // any note count, combo or pass count
inline constexpr std::int64_t kMaxRunDurationMs = 24LL * 60 * 60 * 1000;
inline constexpr std::int64_t kMaxRunUtcSeconds = 1LL << 40; // +/- (about 35,000 years)
inline constexpr std::int32_t kMaxRunDay = 3'000'000;        // +/- (about 8,200 years)
inline constexpr int kMaxRunMeasureNumber = 100'000;
inline constexpr std::size_t kMaxRunMeasures = 20'000;

struct RunCounts {
    int perfect = 0;
    int good = 0;
    int early = 0;
    int late = 0;
    int missed = 0;
    int played = 0; // Wait Mode

    friend bool operator==(const RunCounts&, const RunCounts&) = default;
};

struct RunHand {
    int graded = 0; // notes with a final result
    int hit = 0;    // every grade except Missed; never more than `graded`
    // hit / graded, or 1 when nothing was graded.
    double accuracy() const {
        return graded <= 0 ? 1.0 : static_cast<double>(hit) / static_cast<double>(graded);
    }

    friend bool operator==(const RunHand&, const RunHand&) = default;
};

// One bar of the trouble-spot map.
struct RunBar {
    int measure = 1; // Song bar number; a pickup bar is 0
    int graded = 0;
    int missed = 0; // never more than `graded`
    int wrong = 0;  // wrong presses placed in this bar

    friend bool operator==(const RunBar&, const RunBar&) = default;
};

// What the caller knows about the run that the evaluator does not.
struct RunContext {
    std::uint64_t song_hash = 0; // content_hash() of the song file; 0 when unknown
    std::string song_title;
    RunMode mode = RunMode::Flow;
    HandFilter hands = HandFilter::Both;
    int speed_percent = 100;
    int transpose_semitones = 0;
    std::int64_t start_utc_s = 0; // wall time, seconds since 1970 UTC
    std::int64_t end_utc_s = 0;
    std::int32_t local_day = 0; // see above
    // Time spent practising, from a monotonic clock, paused time left out. Kept apart from the wall
    // times so a clock that jumps (a Pi without a battery clock syncs on boot) cannot change it.
    std::int64_t duration_ms = 0;
};

struct RunRecord {
    std::uint64_t song_hash = 0;
    std::string song_title; // <= 256 bytes of UTF-8 without control characters
    RunMode mode = RunMode::Flow;
    HandFilter hands = HandFilter::Both;
    int speed_percent = 100;      // 25 .. 150 in steps of 5
    int transpose_semitones = 0;  // -12 .. 12
    std::int64_t start_utc_s = 0; // +/- kMaxRunUtcSeconds
    std::int64_t end_utc_s = 0;   // never before start_utc_s
    std::int32_t local_day = 0;   // +/- kMaxRunDay
    std::int64_t duration_ms = 0; // 0 .. kMaxRunDurationMs

    RunCounts counts;
    int wrong = 0;      // presses that matched no note
    int unplayable = 0; // notes no key could play
    int longest_combo = 0;
    int clean_passes = 0; // A-B loop passes with no miss, wrong press or seek
    RunHand left;
    RunHand right;
    RunHand unknown; // notes whose hand the file did not say
    RunHand all;     // the notes of the practised hands: what "accuracy" means for the run
    std::vector<RunBar> measures; // sorted by `measure`, one entry per bar

    // Accuracy of the practised notes: Both is `all`, Left and Right their own hand's.
    double accuracy(HandFilter hand) const {
        return hand == HandFilter::Left    ? left.accuracy()
               : hand == HandFilter::Right ? right.accuracy()
                                           : all.accuracy();
    }
    int graded_notes() const { return all.graded; }

    friend bool operator==(const RunRecord&, const RunRecord&) = default;
};

// The record of a run from the evaluator's summary. `passes` are the PassResults taken from the
// evaluator (take_passes); clean ones are counted. The result is sanitized.
RunRecord make_run_record(const EvaluationSummary& summary, const RunContext& context,
                          std::span<const PassResult> passes = {});

// The same record with every field forced into its range: speed snapped to 25..150 by 5, transpose
// -12..12, counts 0..kMaxRunCount with hit <= graded and missed <= graded, the end not before the
// start, duration 0..24 h, the title stripped of control characters (empty when not UTF-8) and cut
// to 256 bytes at a character boundary, bars clamped to 0..kMaxRunMeasureNumber, sorted and merged
// (sums) when repeated. Idempotent.
RunRecord sanitized(const RunRecord& record);

// ---- Day keys

// The local day of a UTC time at a fixed offset (seconds east of UTC): floor((utc + offset) /
// 86400).
std::int32_t local_day_from_utc(std::int64_t utc_s, std::int32_t utc_offset_s);

struct CivilDate {
    int year = 1970;
    int month = 1; // 1..12
    int day = 1;   // 1..31
};
// Days since 1970-01-01 <-> proleptic Gregorian date.
CivilDate civil_from_day(std::int32_t day);
std::int32_t day_from_civil(const CivilDate& date);

// ---- Storage

// Version 1, all integers little-endian, "varint" is LEB128 (7 bits per byte, low first, at most
// 10 bytes), "zigzag" is a varint of (n << 1) ^ (n >> 63):
//
//   "TR", version byte 1
//   song_hash          8 bytes
//   title              varint length, then the bytes
//   mode               1 byte: 0 Flow, 1 Wait
//   hands              1 byte: 0 Both, 1 Left, 2 Right
//   speed_percent      varint
//   transpose          zigzag
//   start_utc_s        zigzag
//   end_utc_s          varint of end - start
//   local_day          zigzag
//   duration_ms        varint
//   counts             6 varints: perfect good early late missed played
//   wrong, unplayable, longest_combo, clean_passes   4 varints
//   left, right, unknown, all    each: varint graded, varint hit
//   bars               varint count, then per bar: varint measure (the first one as it is, the
//                      others as the step from the previous bar), graded, missed, wrong
//
// A later layout bumps the version byte; this build then reports NewerVersion and the app keeps
// the row as it is.
std::string serialize_run_record(const RunRecord& record);

enum class RunParseStatus : std::uint8_t {
    Ok,
    Truncated,    // ends before the record does
    BadMagic,     // not a run record
    NewerVersion, // written by a newer build
    Corrupt,      // a bad enum value, a number too long, too many bars, or bytes after the end
};

// Never throws and never reads outside `bytes`. On Ok the record is sanitized (numbers outside
// their range are clamped, not refused); on any other status `out` is untouched.
RunParseStatus parse_run_record(std::string_view bytes, RunRecord& out);

} // namespace toonval
