// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/progress.hpp"

#include "progress_fixtures.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <numeric>
#include <vector>

using namespace toonval;
using toonval::testing::make;
using toonval::testing::RunSpec;

namespace {

// A run of `graded` notes with `missed` of them missed.
RunRecord run(std::uint64_t song, int graded, int missed, HandFilter hands = HandFilter::Both,
              int speed = 100, RunMode mode = RunMode::Flow, std::int32_t day = 100) {
    RunSpec s;
    s.song = song;
    s.graded = graded;
    s.missed = missed;
    s.hands = hands;
    s.speed = speed;
    s.mode = mode;
    s.day = day;
    return make(s);
}

} // namespace

TEST_CASE("bests: no runs, and the song with no identity") {
    std::vector<RunRecord> none;
    CHECK(song_bests(none, 1).empty());
    std::vector<RunRecord> runs = {run(0, 40, 0)};
    CHECK(song_bests(runs, 0).empty());
}

TEST_CASE("bests: the best accuracy per mode, hands and speed") {
    std::vector<RunRecord> runs = {
        run(1, 40, 10, HandFilter::Right, 100), // 75%
        run(1, 40, 4, HandFilter::Right, 100),  // 90%  <- best right hand at 100
        run(1, 40, 8, HandFilter::Right, 100),  // 80%
        run(1, 40, 2, HandFilter::Left, 100),   // 95%
        run(1, 40, 12, HandFilter::Right, 70),  // 70%
        run(1, 40, 20, HandFilter::Both, 100),  // 50%
        run(2, 40, 0, HandFilter::Right, 100),  // another song
    };
    std::vector<BestEntry> b = song_bests(runs, 1);
    REQUIRE(b.size() == 4);
    // Sorted by mode, hands (Both, Left, Right), speed.
    CHECK(b[0].hands == HandFilter::Both);
    CHECK(b[0].accuracy == doctest::Approx(0.5));
    CHECK(b[1].hands == HandFilter::Left);
    CHECK(b[1].accuracy == doctest::Approx(0.95));
    CHECK(b[2].hands == HandFilter::Right);
    CHECK(b[2].speed_percent == 70);
    CHECK(b[2].accuracy == doctest::Approx(0.7));
    CHECK(b[3].hands == HandFilter::Right);
    CHECK(b[3].speed_percent == 100);
    CHECK(b[3].accuracy == doctest::Approx(0.9));
    CHECK(b[3].runs == 3);
    CHECK(b[3].graded_notes == 40);
}

TEST_CASE("bests: Wait and Flow are kept apart") {
    std::vector<RunRecord> runs = {run(1, 40, 0, HandFilter::Both, 100, RunMode::Wait),
                                   run(1, 40, 8, HandFilter::Both, 100, RunMode::Flow)};
    std::vector<BestEntry> b = song_bests(runs, 1);
    REQUIRE(b.size() == 2);
    CHECK(b[0].mode == RunMode::Flow);
    CHECK(b[0].accuracy == doctest::Approx(0.8));
    CHECK(b[1].mode == RunMode::Wait);
    CHECK(b[1].accuracy == doctest::Approx(1.0));
}

TEST_CASE("bests: a run stopped after a few notes is no best") {
    std::vector<RunRecord> runs = {run(1, 3, 0), run(1, 0, 0), run(1, 40, 8)};
    std::vector<BestEntry> b = song_bests(runs, 1);
    REQUIRE(b.size() == 1);
    CHECK(b[0].accuracy == doctest::Approx(0.8));
    CHECK(b[0].runs == 1);
    // The limit is the caller's to choose.
    b = song_bests(runs, 1, 1);
    REQUIRE(b.size() == 1);
    CHECK(b[0].accuracy == doctest::Approx(1.0));
    CHECK(b[0].runs == 2);
    CHECK(song_bests(runs, 1, 1000).empty());
}

TEST_CASE("bests: ties go to the run with more notes, then the later run") {
    RunRecord a = run(1, 40, 4, HandFilter::Both, 100, RunMode::Flow, 10);
    RunRecord b = run(1, 80, 8, HandFilter::Both, 100, RunMode::Flow, 11);
    RunRecord c = run(1, 80, 8, HandFilter::Both, 100, RunMode::Flow, 12);
    std::vector<RunRecord> runs = {a, b, c};
    std::vector<BestEntry> best = song_bests(runs, 1);
    REQUIRE(best.size() == 1);
    CHECK(best[0].graded_notes == 80);
    CHECK(best[0].day == 12);
}

TEST_CASE("bests: a run saved twice counts once") {
    RunRecord r = run(1, 40, 4);
    std::vector<RunRecord> runs = {r, r};
    CHECK(song_bests(runs, 1)[0].runs == 1);
}

TEST_CASE("history: no runs, one run, other songs") {
    std::vector<RunRecord> none;
    CHECK(history_series(none, 1, 50).empty());
    std::vector<RunRecord> runs = {run(2, 40, 4),
                                   run(1, 40, 10, HandFilter::Both, 85, RunMode::Flow, 123)};
    std::vector<HistoryPoint> h = history_series(runs, 1, 50);
    REQUIRE(h.size() == 1);
    CHECK(h[0].day == 123);
    CHECK(h[0].accuracy == doctest::Approx(0.75));
    CHECK(h[0].speed_percent == 85);
    CHECK(h[0].runs == 1);
    CHECK(h[0].graded_notes == 40);
    CHECK(history_series(runs, 0, 50).empty());
    CHECK(history_series(runs, 1, 0).empty());
}

TEST_CASE("history: filters by mode and hands, and keeps the order of the list") {
    std::vector<RunRecord> runs = {
        run(1, 40, 8, HandFilter::Right, 100, RunMode::Flow, 1),
        run(1, 40, 4, HandFilter::Left, 100, RunMode::Flow, 2),
        run(1, 40, 0, HandFilter::Right, 100, RunMode::Wait, 3),
        run(1, 40, 2, HandFilter::Right, 100, RunMode::Flow, 4),
    };
    HistoryFilter f;
    f.hands = HandFilter::Right;
    f.mode = RunMode::Flow;
    std::vector<HistoryPoint> h = history_series(runs, 1, 50, f);
    REQUIRE(h.size() == 2);
    CHECK(h[0].day == 1);
    CHECK(h[1].day == 4);
    CHECK(history_series(runs, 1, 50).size() == 4);
    f = {};
    f.mode = RunMode::Wait;
    CHECK(history_series(runs, 1, 50, f).size() == 1);
    f = {};
    f.min_graded = 41;
    CHECK(history_series(runs, 1, 50, f).empty());
}

TEST_CASE("history: runs with no graded notes are not points") {
    std::vector<RunRecord> runs = {run(1, 0, 0), run(1, 40, 4)};
    CHECK(history_series(runs, 1, 50).size() == 1);
}

TEST_CASE("history: downsampled to at most N points, covering the whole history in order") {
    std::vector<RunRecord> runs;
    for (int i = 0; i < 1000; ++i) {
        // Accuracy rises from 50% to 99.9%; the day rises by one a run.
        runs.push_back(run(1, 100, 50 - i / 20, HandFilter::Both, 50 + (i / 100) * 5, RunMode::Flow,
                           1000 + i));
    }
    for (std::size_t n : {1u, 2u, 7u, 50u, 333u, 999u}) {
        CAPTURE(n);
        std::vector<HistoryPoint> h = history_series(runs, 1, n);
        REQUIRE(h.size() == n);
        int total = 0;
        for (std::size_t i = 0; i < h.size(); ++i) {
            total += h[i].runs;
            CHECK(h[i].runs >= 1000 / static_cast<int>(n));
            CHECK(h[i].runs <= 1000 / static_cast<int>(n) + 1);
            if (i > 0) {
                CHECK(h[i].day > h[i - 1].day);
                CHECK(h[i].accuracy >= h[i - 1].accuracy - 1e-9); // the trend survives merging
            }
        }
        CHECK(total == 1000);
        CHECK(h.back().day == 1999);
    }
    // At or under the limit nothing is merged.
    CHECK(history_series(runs, 1, 1000).size() == 1000);
    CHECK(history_series(runs, 1, 5000).size() == 1000);
    CHECK(history_series(runs, 1, 1000)[999].runs == 1);
}

TEST_CASE("history: a merged point weighs accuracy by notes and shows the top speed") {
    std::vector<RunRecord> runs = {run(1, 100, 0, HandFilter::Both, 60),  // 100 notes, all hit
                                   run(1, 20, 10, HandFilter::Both, 80)}; // 20 notes, half hit
    std::vector<HistoryPoint> h = history_series(runs, 1, 1);
    REQUIRE(h.size() == 1);
    CHECK(h[0].runs == 2);
    CHECK(h[0].graded_notes == 120);
    CHECK(h[0].accuracy == doctest::Approx(110.0 / 120.0));
    CHECK(h[0].speed_percent == 80);
}

TEST_CASE("history and bests over 100,000 runs") {
    std::vector<RunRecord> runs;
    runs.reserve(100'000);
    for (int i = 0; i < 100'000; ++i) {
        RunSpec s;
        s.song = 1 + static_cast<std::uint64_t>(i % 7);
        s.graded = 50;
        s.missed = i % 11;
        s.speed = 50 + (i % 5) * 10;
        s.day = i / 40;
        s.start_utc_s = i;
        runs.push_back(make(s));
    }
    auto t0 = std::chrono::steady_clock::now();
    std::vector<HistoryPoint> h = history_series(runs, 3, 200);
    std::vector<BestEntry> b = song_bests(runs, 3);
    AttemptChange c = compare_with_last_attempt(runs, runs.back());
    auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count();
    CHECK(ms < 10'000);
    REQUIRE(h.size() == 200);
    int total = 0;
    for (const HistoryPoint& p : h) {
        total += p.runs;
    }
    CHECK(total == 14'286); // runs with i % 7 == 2 among 100,000
    CHECK(b.size() == 5);   // five speeds
    CHECK(c.has_previous);
}

// ---- Change from the last attempt

TEST_CASE("change: the first attempt has nothing to compare with") {
    std::vector<RunRecord> none;
    RunRecord now = run(1, 40, 4);
    AttemptChange c = compare_with_last_attempt(none, now);
    CHECK_FALSE(c.has_previous);
    CHECK_FALSE(c.new_best);
    CHECK(c.accuracy_delta == 0.0);
    // One run in the list that is the run itself: still the first.
    std::vector<RunRecord> only = {now};
    CHECK_FALSE(compare_with_last_attempt(only, now).has_previous);
}

TEST_CASE("change: compares with the latest earlier run of the same song, mode, hands and speed") {
    RunRecord older = run(1, 40, 12);
    older.longest_combo = 5;
    RunRecord last = run(1, 40, 8);
    last.longest_combo = 9;
    std::vector<RunRecord> runs = {
        older,
        last,
        run(2, 40, 0),                                       // another song
        run(1, 40, 0, HandFilter::Left),                     // other hands
        run(1, 40, 0, HandFilter::Both, 80),                 // other speed
        run(1, 40, 0, HandFilter::Both, 100, RunMode::Wait), // other mode
        run(1, 0, 0),                                        // nothing graded: not an attempt
    };
    RunRecord now = run(1, 40, 4);
    now.longest_combo = 20;
    AttemptChange c = compare_with_last_attempt(runs, now);
    REQUIRE(c.has_previous);
    CHECK(c.accuracy == doctest::Approx(0.9));
    CHECK(c.previous_accuracy == doctest::Approx(0.8));
    CHECK(c.accuracy_delta == doctest::Approx(0.1));
    CHECK(c.previous_longest_combo == 9);
    CHECK(c.combo_delta == 11);
    CHECK(c.trend == Trend::Better);
    CHECK(c.new_best);
    CHECK(c.previous_day == 100);
}

TEST_CASE("change: skips itself when it is already saved, and finds trends") {
    RunRecord prev = run(1, 40, 4);
    RunRecord now = run(1, 40, 8);
    std::vector<RunRecord> runs = {prev, now};
    AttemptChange c = compare_with_last_attempt(runs, now);
    REQUIRE(c.has_previous);
    CHECK(c.accuracy_delta == doctest::Approx(-0.1));
    CHECK(c.trend == Trend::Worse);
    CHECK_FALSE(c.new_best);

    RunRecord same = run(1, 40, 4);
    std::vector<RunRecord> just_prev = {prev};
    c = compare_with_last_attempt(just_prev, same);
    CHECK(c.trend == Trend::Same);
    CHECK_FALSE(c.new_best); // equal is not better

    // Half a percentage point or less is the same.
    RunRecord a = run(1, 1000, 100);
    RunRecord b = run(1, 1000, 96); // +0.4 points
    std::vector<RunRecord> one = {a};
    CHECK(compare_with_last_attempt(one, b).trend == Trend::Same);
    RunRecord d = run(1, 1000, 90); // +1 point
    CHECK(compare_with_last_attempt(one, d).trend == Trend::Better);
}

TEST_CASE("change: new_best needs the run to beat every earlier attempt") {
    std::vector<RunRecord> runs = {run(1, 40, 2), run(1, 40, 10)};
    // Better than the last attempt (75%) but not than the one before (95%).
    AttemptChange c = compare_with_last_attempt(runs, run(1, 40, 4));
    CHECK(c.trend == Trend::Better);
    CHECK_FALSE(c.new_best);
    // Short runs never set a best.
    std::vector<RunRecord> few = {run(1, 5, 5)};
    CHECK_FALSE(compare_with_last_attempt(few, run(1, 5, 0)).new_best);
    // A run with no graded notes has no comparison.
    CHECK_FALSE(compare_with_last_attempt(runs, run(1, 0, 0)).has_previous);
    // No identity, no comparison.
    std::vector<RunRecord> anon = {run(0, 40, 4)};
    CHECK_FALSE(compare_with_last_attempt(anon, run(0, 40, 2)).has_previous);
}
