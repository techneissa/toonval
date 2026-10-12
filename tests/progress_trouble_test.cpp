// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/progress.hpp"

#include "toonval/transport.hpp"

#include "progress_fixtures.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>
#include <vector>

using namespace toonval;
using toonval::testing::make;
using toonval::testing::make_bars;
using toonval::testing::RunSpec;

namespace {

const BarHeat* bar(const TroublePlan& plan, int measure) {
    for (const BarHeat& b : plan.bars) {
        if (b.measure == measure) {
            return &b;
        }
    }
    return nullptr;
}

// A run whose bars are given as (measure, missed of 4) pairs; every bar has 4 graded notes.
RunRecord bars_run(std::uint64_t song, std::initializer_list<std::pair<int, int>> list,
                   int wrong = 0) {
    RunSpec s;
    s.song = song;
    for (auto [m, missed] : list) {
        s.bars.push_back({m, 4, missed, missed > 0 ? wrong : 0});
    }
    return make(s);
}

} // namespace

TEST_CASE("trouble: no runs, no song identity, nothing to practise") {
    std::vector<RunRecord> none;
    TroublePlan p = trouble_plan(none, 1);
    CHECK(p.spots.empty());
    CHECK(p.bars.empty());
    CHECK(p.runs_read == 0);

    std::vector<RunRecord> runs = {make_bars(0, 20, {12, 13})};
    CHECK(trouble_plan(runs, 0).spots.empty());
    CHECK(trouble_plan(runs, 99).spots.empty()); // a song with no runs

    // A flawless song has no trouble spot, however many runs.
    runs.clear();
    for (int i = 0; i < 8; ++i) {
        runs.push_back(make_bars(1, 20, {}));
    }
    p = trouble_plan(runs, 1);
    CHECK(p.spots.empty());
    CHECK(p.bars.size() == 20);
    CHECK(p.runs_read == 8);
}

TEST_CASE("trouble: one run is enough to say where it went wrong") {
    std::vector<RunRecord> runs = {make_bars(1, 20, {12, 13})};
    TroublePlan p = trouble_plan(runs, 1);
    REQUIRE(p.spots.size() == 1);
    const TroubleSpot& s = p.spots[0];
    CHECK(s.first_measure == 12);
    CHECK(s.last_measure == 13);
    CHECK(s.heat == doctest::Approx(0.75));
    CHECK(s.attempts == 8);
    CHECK(s.runs_seen == 1);
    CHECK(s.runs_with_errors == 1);
    CHECK(s.reason == TroubleReason::MissedNotes);
    REQUIRE(p.bars.size() == 20);
    CHECK(bar(p, 12)->heat == doctest::Approx(0.75));
    CHECK(bar(p, 12)->reliable);
    CHECK(bar(p, 12)->attempts == 4);
    CHECK(bar(p, 5)->heat == 0.0);
    CHECK(bar(p, 5)->reliable);
}

TEST_CASE("trouble: a song whose runs always miss bars 12 and 13 returns that loop first") {
    std::mt19937 rng(12);
    std::vector<RunRecord> runs;
    for (int i = 0; i < 12; ++i) {
        // Bars 12 and 13 always fail; one other random bar slips by a single note each run.
        RunSpec s;
        s.song = 7;
        int slip = 1 + static_cast<int>(rng() % 24);
        for (int m = 1; m <= 24; ++m) {
            bool bad = m == 12 || m == 13;
            s.bars.push_back(
                {m, 4, bad ? 3 + static_cast<int>(rng() % 2) : (m == slip ? 1 : 0), 0});
        }
        runs.push_back(make(s));
        runs.push_back(make_bars(8, 24, {2, 3})); // another song in between
    }
    TroublePlan p = trouble_plan(runs, 7);
    REQUIRE(!p.spots.empty());
    CHECK(p.spots[0].first_measure == 12);
    CHECK(p.spots[0].last_measure == 13);
    CHECK(p.spots[0].runs_seen == 12);
    CHECK(p.spots[0].runs_with_errors == 12);
    CHECK(p.spots.size() == 1); // the random slips are not trouble
    CHECK(p.runs_read == 12);
    // The same song seen from the other song's runs points at its own bars.
    TroublePlan q = trouble_plan(runs, 8);
    REQUIRE(!q.spots.empty());
    CHECK(q.spots[0].first_measure == 2);
    CHECK(q.spots[0].last_measure == 3);
}

TEST_CASE("trouble: bars with too few attempts have no heat") {
    // Bar 2 has two notes, both missed, in the only run.
    RunSpec s;
    s.bars = {{1, 4, 0, 0}, {2, 2, 2, 0}, {3, 4, 0, 0}};
    std::vector<RunRecord> runs = {make(s)};
    TroublePlan p = trouble_plan(runs, 1);
    CHECK(p.spots.empty());
    REQUIRE(bar(p, 2) != nullptr);
    CHECK_FALSE(bar(p, 2)->reliable);
    CHECK(bar(p, 2)->heat == 0.0);
    CHECK(bar(p, 2)->attempts == 2);
    // Two runs bring it to four attempts.
    runs.push_back(make(s));
    p = trouble_plan(runs, 1);
    REQUIRE(p.spots.size() == 1);
    CHECK(p.spots[0].first_measure == 2);
    CHECK(p.spots[0].last_measure == 2);
    CHECK(bar(p, 2)->reliable);
    CHECK(bar(p, 2)->heat == doctest::Approx(1.0));
    // The limit is the caller's to choose.
    TroubleOptions strict;
    strict.min_attempts = 5;
    CHECK(trouble_plan(runs, 1, strict).spots.empty());
}

TEST_CASE("trouble: adjacent trouble bars merge, separated ones are separate spots") {
    std::vector<RunRecord> runs = {
        bars_run(1, {{4, 0}, {5, 3}, {6, 3}, {7, 3}, {8, 0}, {9, 3}, {10, 0}, {11, 2}, {12, 2}})};
    TroublePlan p = trouble_plan(runs, 1);
    REQUIRE(p.spots.size() == 3);
    CHECK(p.spots[0].first_measure == 5);
    CHECK(p.spots[0].last_measure == 7);
    CHECK(p.spots[0].attempts == 12);
    CHECK(p.spots[1].first_measure == 9);
    CHECK(p.spots[1].last_measure == 9);
    CHECK(p.spots[2].first_measure == 11);
    CHECK(p.spots[2].last_measure == 12);
    CHECK(p.spots[2].heat == doctest::Approx(0.5));
    // Hottest first.
    CHECK(p.spots[0].heat >= p.spots[1].heat);
    CHECK(p.spots[1].heat >= p.spots[2].heat);
}

TEST_CASE("trouble: a spot is at most max_loop_bars long and grows toward the hotter side") {
    RunSpec s;
    for (int m = 3; m <= 10; ++m) {
        s.bars.push_back({m, 4, 3, 0});
    }
    std::vector<RunRecord> runs = {make(s)};
    TroublePlan p = trouble_plan(runs, 1);
    REQUIRE(p.spots.size() == 2);
    CHECK(p.spots[0].first_measure == 3);
    CHECK(p.spots[0].last_measure == 6);
    CHECK(p.spots[1].first_measure == 7);
    CHECK(p.spots[1].last_measure == 10);

    // Bar 5 is the worst; 4 is hotter than 6.
    runs = {bars_run(1, {{3, 0}, {4, 3}, {5, 4}, {6, 2}, {7, 0}})};
    TroubleOptions two;
    two.max_loop_bars = 2;
    p = trouble_plan(runs, 1, two);
    REQUIRE(!p.spots.empty());
    CHECK(p.spots[0].first_measure == 4);
    CHECK(p.spots[0].last_measure == 5);
    CHECK(p.spots[1].first_measure == 6);
    CHECK(p.spots[1].last_measure == 6);
    TroubleOptions three;
    three.max_loop_bars = 3;
    p = trouble_plan(runs, 1, three);
    REQUIRE(p.spots.size() == 1);
    CHECK(p.spots[0].first_measure == 4);
    CHECK(p.spots[0].last_measure == 6);
    // Equally hot neighbours: the later bar.
    runs = {bars_run(1, {{4, 3}, {5, 4}, {6, 3}})};
    p = trouble_plan(runs, 1, two);
    REQUIRE(p.spots.size() == 2);
    CHECK(p.spots[0].first_measure == 5);
    CHECK(p.spots[0].last_measure == 6);
    CHECK(p.spots[1].first_measure == 4);
    runs = {bars_run(1, {{4, 3}, {5, 4}, {6, 2}, {7, 0}})};
    TroubleOptions one;
    one.max_loop_bars = 1;
    p = trouble_plan(runs, 1, one);
    CHECK(p.spots.size() == 3);
}

TEST_CASE("trouble: a pickup bar 0 is adjacent to bar 1") {
    std::vector<RunRecord> runs = {bars_run(1, {{0, 3}, {1, 3}, {2, 0}})};
    TroublePlan p = trouble_plan(runs, 1);
    REQUIRE(p.spots.size() == 1);
    CHECK(p.spots[0].first_measure == 0);
    CHECK(p.spots[0].last_measure == 1);
}

TEST_CASE("trouble: max_spots caps the list") {
    std::vector<RunRecord> runs = {bars_run(1, {{1, 3}, {3, 3}, {5, 3}, {7, 3}, {9, 3}})};
    CHECK(trouble_plan(runs, 1).spots.size() == 3);
    TroubleOptions o;
    o.max_spots = 1;
    CHECK(trouble_plan(runs, 1, o).spots.size() == 1);
    o.max_spots = 0;
    CHECK(trouble_plan(runs, 1, o).spots.empty());
    o.max_spots = 50;
    CHECK(trouble_plan(runs, 1, o).spots.size() == 5);
}

TEST_CASE("trouble: recent runs weigh more than old ones") {
    // Six old runs missed bars 3 and 4; the six since then miss bars 9 and 10 instead.
    std::vector<RunRecord> runs;
    for (int i = 0; i < 6; ++i) {
        runs.push_back(make_bars(1, 12, {3, 4}));
    }
    for (int i = 0; i < 6; ++i) {
        runs.push_back(make_bars(1, 12, {9, 10}));
    }
    TroublePlan p = trouble_plan(runs, 1);
    REQUIRE(p.spots.size() >= 1);
    CHECK(p.spots[0].first_measure == 9);
    CHECK(p.spots[0].last_measure == 10);
    CHECK(bar(p, 3)->heat < bar(p, 9)->heat);
    CHECK(bar(p, 3)->heat > 0.0); // still remembered, only cooler
    // All twelve runs were read; flip the order and the old bars lead.
    std::reverse(runs.begin(), runs.end());
    p = trouble_plan(runs, 1);
    CHECK(p.spots[0].first_measure == 3);
}

TEST_CASE("trouble: looping the bar cools it down until it is no longer today's job") {
    std::vector<RunRecord> runs;
    for (int i = 0; i < 8; ++i) {
        runs.push_back(make_bars(1, 20, {12, 13}));
    }
    double previous = 2.0;
    for (int loop_runs = 0; loop_runs <= 10; ++loop_runs) {
        TroublePlan p = trouble_plan(runs, 1);
        double heat = bar(p, 12)->heat;
        CAPTURE(loop_runs);
        CHECK(heat < previous); // strictly cooler after each clean loop
        previous = heat;
        if (loop_runs == 0) {
            CHECK(p.spots[0].first_measure == 12);
        }
        if (loop_runs == 10) {
            CHECK(p.spots.empty());
        }
        // The next run is a loop over bars 12 and 13 only, played clean.
        runs.push_back(bars_run(1, {{12, 0}, {13, 0}}));
    }
}

TEST_CASE("trouble: only the latest max_runs runs are read") {
    std::vector<RunRecord> runs;
    for (int i = 0; i < 40; ++i) {
        runs.push_back(make_bars(1, 12, {2}));
    }
    TroubleOptions o;
    o.max_runs = 5;
    TroublePlan p = trouble_plan(runs, 1, o);
    CHECK(p.runs_read == 5);
    CHECK(trouble_plan(runs, 1).runs_read == 30);
    o.max_runs = 0; // at least one
    CHECK(trouble_plan(runs, 1, o).runs_read == 1);
}

TEST_CASE("trouble: wrong presses count, half as much as a miss, and set the reason") {
    // Wait Mode: nothing is ever missed, but bar 6 has two wrong presses on four notes.
    RunSpec s;
    s.mode = RunMode::Wait;
    s.bars = {{5, 4, 0, 0}, {6, 4, 0, 2}, {7, 4, 0, 1}};
    std::vector<RunRecord> runs = {make(s)};
    TroublePlan p = trouble_plan(runs, 1);
    REQUIRE(p.spots.size() == 1);
    CHECK(p.spots[0].first_measure == 6);
    CHECK(p.spots[0].reason == TroubleReason::WrongNotes);
    CHECK(p.spots[0].heat == doctest::Approx(0.25));  // 2 wrong * 0.5 / 4 notes
    CHECK(bar(p, 7)->heat == doctest::Approx(0.125)); // one wrong press: not trouble
    // Misses and wrong presses together: the larger share names the reason.
    runs = {bars_run(1, {{6, 3}}, 2)};
    p = trouble_plan(runs, 1);
    REQUIRE(p.spots.size() == 1);
    CHECK(p.spots[0].reason == TroubleReason::MissedNotes);
    CHECK(bar(p, 6)->heat == doctest::Approx(1.0)); // never above 1
    CHECK(std::string(trouble_reason_text(TroubleReason::MissedNotes)).size() > 0);
    CHECK(std::string(trouble_reason_text(TroubleReason::WrongNotes)).size() > 0);
    CHECK(trouble_reason_text(TroubleReason::MissedNotes) !=
          trouble_reason_text(TroubleReason::WrongNotes));
}

TEST_CASE("trouble: runs where a bar was not played do not cool it") {
    // Eight full runs miss bar 4; a short run that only covers bars 1 and 2 follows.
    std::vector<RunRecord> runs;
    for (int i = 0; i < 8; ++i) {
        runs.push_back(make_bars(1, 10, {4}));
    }
    TroublePlan before = trouble_plan(runs, 1);
    runs.push_back(bars_run(1, {{1, 0}, {2, 0}}));
    TroublePlan after = trouble_plan(runs, 1);
    CHECK(bar(after, 4)->heat == doctest::Approx(bar(before, 4)->heat));
    CHECK(after.runs_read == 9);
    // A run with no bars at all says nothing and is not a run read.
    RunSpec empty;
    runs.push_back(make(empty));
    CHECK(trouble_plan(runs, 1).runs_read == 9);
}

TEST_CASE("trouble: a run saved twice is read once") {
    RunRecord r = make_bars(1, 12, {4, 5});
    std::vector<RunRecord> runs = {make_bars(1, 12, {}), r, r};
    CHECK(trouble_plan(runs, 1).runs_read == 2);
}

TEST_CASE("trouble: spots fit the options for any history") {
    std::mt19937 rng(99);
    for (int iter = 0; iter < 200; ++iter) {
        std::vector<RunRecord> runs;
        int n = 1 + static_cast<int>(rng() % 40);
        int bars = 4 + static_cast<int>(rng() % 30);
        for (int i = 0; i < n; ++i) {
            RunSpec s;
            s.song = 1 + rng() % 2;
            for (int m = 1; m <= bars; ++m) {
                if (rng() % 7 == 0) {
                    continue; // a loop run that skipped the bar
                }
                int graded = 1 + static_cast<int>(rng() % 6);
                s.bars.push_back({m, graded,
                                  static_cast<int>(rng() % static_cast<unsigned>(graded + 1)),
                                  static_cast<int>(rng() % 4)});
            }
            runs.push_back(make(s));
        }
        TroubleOptions o;
        o.max_loop_bars = 1 + static_cast<int>(rng() % 5);
        o.max_spots = static_cast<int>(rng() % 6);
        o.min_attempts = 1 + static_cast<int>(rng() % 8);
        o.half_life_runs = 0.5 + static_cast<double>(rng() % 20);
        CAPTURE(iter);
        TroublePlan p = trouble_plan(runs, 1, o);
        REQUIRE(p.spots.size() <= static_cast<std::size_t>(o.max_spots));
        std::vector<int> used;
        double last_heat = 2.0;
        for (const TroubleSpot& s : p.spots) {
            CHECK(s.last_measure >= s.first_measure);
            CHECK(s.last_measure - s.first_measure + 1 <= o.max_loop_bars);
            CHECK(s.heat >= o.min_heat);
            CHECK(s.heat <= 1.0);
            CHECK(s.runs_seen >= s.runs_with_errors);
            CHECK(s.runs_seen >= 1);
            CHECK(s.heat <= last_heat);
            last_heat = s.heat;
            for (int m = s.first_measure; m <= s.last_measure; ++m) {
                REQUIRE(std::find(used.begin(), used.end(), m) == used.end());
                used.push_back(m);
                const BarHeat* b = bar(p, m);
                REQUIRE(b != nullptr);
                CHECK(b->reliable);
                CHECK(b->heat >= o.min_heat);
            }
        }
        for (const BarHeat& b : p.bars) {
            CHECK(b.heat >= 0.0);
            CHECK(b.heat <= 1.0);
            CHECK((b.reliable || b.heat == 0.0));
            CHECK(b.reliable == (b.attempts >= o.min_attempts));
        }
        CHECK(std::is_sorted(p.bars.begin(), p.bars.end(), [](const BarHeat& a, const BarHeat& b) {
            return a.measure < b.measure;
        }));
    }
}

TEST_CASE("trouble: a spot gives the seconds to loop through measures_span") {
    Song song;
    for (int m = 1; m <= 20; ++m) {
        Measure bar_;
        bar_.number = m;
        bar_.start_s = (m - 1) * 2.0;
        bar_.start_tick = (m - 1) * 1920;
        song.measures.push_back(bar_);
    }
    song.duration_s = 40.0;
    std::vector<RunRecord> runs = {make_bars(1, 20, {12, 13})};
    TroublePlan p = trouble_plan(runs, 1);
    REQUIRE(!p.spots.empty());
    LoopSpan span = measures_span(song, p.spots[0].first_measure, p.spots[0].last_measure);
    REQUIRE(span.ok);
    CHECK(span.start_s == doctest::Approx(22.0));
    CHECK(span.end_s == doctest::Approx(26.0));
}

TEST_CASE("trouble: 100,000 runs of other songs and of one song") {
    // Song 7 has 100 runs among 100,000 of other songs.
    std::vector<RunRecord> runs;
    runs.reserve(100'000);
    for (int i = 0; i < 100'000; ++i) {
        RunSpec s;
        s.song = (i % 1000 == 500) ? 7 : 100 + static_cast<std::uint64_t>(i % 50);
        s.start_utc_s = i;
        for (int m = 1; m <= 16; ++m) {
            bool bad = s.song == 7 && (m == 12 || m == 13);
            s.bars.push_back({m, 4, bad ? 3 : 0, 0});
        }
        runs.push_back(make(s));
    }
    auto t0 = std::chrono::steady_clock::now();
    TroublePlan rare = trouble_plan(runs, 7);
    TroublePlan common = trouble_plan(runs, 100);
    TroublePlan none = trouble_plan(runs, 12345);
    auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count();
    CHECK(ms < 10'000);
    REQUIRE(rare.spots.size() == 1);
    CHECK(rare.spots[0].first_measure == 12);
    CHECK(rare.runs_read == 30);
    CHECK(common.spots.empty());
    CHECK(common.runs_read == 30); // only the latest are read
    CHECK(none.runs_read == 0);
}
