// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/hand_path.hpp"

#include "progress_fixtures.hpp"
#include "section_fixtures.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <limits>
#include <random>
#include <vector>

using namespace toonval;
using namespace toonval::testing;

namespace {

constexpr std::uint64_t kSong = 1;

// A whole-song run: `graded` notes of which `missed` were missed, practising `hands` at `speed`.
RunRecord run(HandFilter hands, int speed, int missed = 0, int graded = 40,
              std::uint64_t song = kSong, int wrong = 0) {
    RunSpec spec;
    spec.song = song;
    spec.hands = hands;
    spec.speed = speed;
    spec.graded = graded;
    spec.missed = missed;
    RunRecord r = make(spec);
    r.wrong = wrong;
    return r;
}

// A run of 16 bars with 4 graded notes in each; bars in `bad` miss `miss` of theirs.
RunRecord bar_run(HandFilter hands, int speed, const std::vector<int>& bad, int miss = 4,
                  std::uint64_t song = kSong) {
    RunSpec spec;
    spec.song = song;
    spec.hands = hands;
    spec.speed = speed;
    for (int m = 1; m <= 16; ++m) {
        bool is_bad = std::find(bad.begin(), bad.end(), m) != bad.end();
        spec.bars.push_back({m, 4, is_bad ? miss : 0, 0});
    }
    return make(spec);
}

constexpr auto R = HandFilter::Right;
constexpr auto L = HandFilter::Left;
constexpr auto B = HandFilter::Both;

} // namespace

TEST_CASE("hand path: with no runs the path starts with the right hand at the start speed") {
    HandPathStatus s = hand_path({}, kSong);
    CHECK(s.stage == PathStage::RightHand);
    PathSuggestion p = next_practice(s);
    CHECK(p.advice == PathAdvice::Start);
    CHECK(p.hands == R);
    CHECK(p.speed_percent == 60);
    CHECK_FALSE(p.moved_on);
    CHECK(path_hint(s, p) == "Start with the right hand at 60%.");
}

TEST_CASE("hand path: a clean run raises the speed by a step") {
    std::vector<RunRecord> runs = {run(R, 60)};
    HandPathStatus s = hand_path(runs, kSong);
    CHECK(s.stage == PathStage::RightHand);
    CHECK(s.right.runs == 1);
    CHECK(s.right.best_accuracy == doctest::Approx(1.0));
    CHECK(s.right.best_speed_percent == 60);
    CHECK_FALSE(s.right.mastered);
    PathSuggestion p = next_practice(s);
    CHECK(p.advice == PathAdvice::RaiseSpeed);
    CHECK(p.hands == R);
    CHECK(p.speed_percent == 70);
    CHECK(path_hint(s, p) == "Right hand: clean run. Try 70%.");
}

TEST_CASE("hand path: the speed never goes past the goal of the hand") {
    std::vector<RunRecord> runs = {run(R, 75)};
    PathSuggestion p = next_practice(hand_path(runs, kSong));
    CHECK(p.speed_percent == 80);
}

TEST_CASE("hand path: a close run asks for another at the same speed") {
    std::vector<RunRecord> runs = {run(R, 70, 6)}; // 34 of 40: 85%
    HandPathStatus s = hand_path(runs, kSong);
    PathSuggestion p = next_practice(s);
    CHECK(p.advice == PathAdvice::KeepPracticing);
    CHECK(p.speed_percent == 70);
    CHECK(path_hint(s, p) == "Right hand: close. Once more at 70%.");
}

TEST_CASE("hand path: a shaky run asks for a slower one") {
    std::vector<RunRecord> runs = {run(R, 70, 20)}; // 50%
    HandPathStatus s = hand_path(runs, kSong);
    PathSuggestion p = next_practice(s);
    CHECK(p.advice == PathAdvice::SlowDown);
    CHECK(p.speed_percent == 60);
    CHECK(path_hint(s, p) == "Right hand: a little slower, 60%.");
    std::vector<RunRecord> slow = {run(R, 30, 20)};
    CHECK(next_practice(hand_path(slow, kSong)).speed_percent == 25); // never under 25
}

TEST_CASE("hand path: the right hand at the goal speed moves the path to the left hand") {
    std::vector<RunRecord> runs = {run(R, 60), run(R, 80)};
    HandPathStatus s = hand_path(runs, kSong);
    CHECK(s.right.mastered);
    CHECK(s.stage == PathStage::LeftHand);
    PathSuggestion p = next_practice(s);
    CHECK(p.advice == PathAdvice::Start);
    CHECK(p.hands == L);
    CHECK(p.speed_percent == 60);
    CHECK(p.moved_on);
    CHECK(path_hint(s, p) == "Right hand at 80%: done. Try the left hand at 60%.");
}

TEST_CASE("hand path: a fast but sloppy run does not master a hand") {
    std::vector<RunRecord> runs = {run(R, 100, 12)}; // 70%
    HandPathStatus s = hand_path(runs, kSong);
    CHECK_FALSE(s.right.mastered);
    CHECK(s.right.best_speed_percent == 0);
    CHECK(s.stage == PathStage::RightHand);
}

TEST_CASE("hand path: an accurate run below the goal speed does not master a hand either") {
    std::vector<RunRecord> runs = {run(R, 75)};
    CHECK_FALSE(hand_path(runs, kSong).right.mastered);
}

TEST_CASE("hand path: both hands start where the weaker hand got to") {
    std::vector<RunRecord> runs = {run(R, 100), run(L, 80)};
    HandPathStatus s = hand_path(runs, kSong);
    CHECK(s.stage == PathStage::BothHands);
    PathSuggestion p = next_practice(s);
    CHECK(p.hands == B);
    CHECK(p.advice == PathAdvice::Start);
    CHECK(p.speed_percent == 80);
    CHECK(p.moved_on);
    CHECK(path_hint(s, p) == "Left hand at 80%: done. Try both hands at 80%.");
}

TEST_CASE("hand path: both hands at their goal speed finish the path") {
    std::vector<RunRecord> runs = {run(R, 80), run(L, 80), run(B, 90), run(B, 100)};
    HandPathStatus s = hand_path(runs, kSong);
    CHECK(s.stage == PathStage::Done);
    PathSuggestion p = next_practice(s);
    CHECK(p.advice == PathAdvice::AllDone);
    CHECK(p.speed_percent == 0);
    CHECK(path_hint(s, p) == "Everything on this path is done.");
}

TEST_CASE("hand path: a player who already masters both hands is not sent back to one") {
    std::vector<RunRecord> runs = {run(B, 100)};
    CHECK(hand_path(runs, kSong).stage == PathStage::Done);
}

TEST_CASE("hand path: both hands keep raising the speed up to their goal") {
    std::vector<RunRecord> runs = {run(R, 80), run(L, 80), run(B, 80)};
    HandPathStatus s = hand_path(runs, kSong);
    CHECK(s.stage == PathStage::BothHands);
    PathSuggestion p = next_practice(s);
    CHECK(p.advice == PathAdvice::RaiseSpeed);
    CHECK(p.speed_percent == 90);
}

TEST_CASE("hand path: the latest run decides the advice, the best run decides mastery") {
    std::vector<RunRecord> runs = {run(R, 80), run(R, 100, 25)};
    HandPathStatus s = hand_path(runs, kSong);
    CHECK(s.right.mastered);
    CHECK(s.right.last_speed_percent == 100);
    CHECK(s.stage == PathStage::LeftHand);
}

TEST_CASE("hand path: runs of other songs and runs with too few notes do not count") {
    std::vector<RunRecord> runs = {run(R, 80, 0, 40, 2), run(R, 80, 0, 3)};
    HandPathStatus s = hand_path(runs, kSong);
    CHECK(s.right.runs == 0);
    CHECK(s.stage == PathStage::RightHand);
    CHECK(hand_path(runs, 0).right.runs == 0);
}

TEST_CASE("hand path: only the latest runs of a hand are read") {
    HandPathOptions o;
    o.recent_runs = 3;
    std::vector<RunRecord> runs = {run(R, 80)};
    for (int i = 0; i < 3; ++i) {
        runs.push_back(run(R, 60, 25)); // three bad runs since
    }
    HandPathStatus s = hand_path(runs, kSong, {}, o);
    CHECK(s.right.runs == 3);
    CHECK_FALSE(s.right.mastered); // the old good run fell out of the window
    s = hand_path(runs, kSong);
    CHECK(s.right.mastered);
}

TEST_CASE("hand path: wrong presses count against the accuracy like misses do") {
    std::vector<RunRecord> clean = {run(R, 80, 0, 40, kSong, 0)};
    std::vector<RunRecord> fumbled = {run(R, 80, 0, 40, kSong, 12)}; // 1 - 6/40 = 85%
    CHECK(hand_path(clean, kSong).right.mastered);
    HandPathStatus s = hand_path(fumbled, kSong);
    CHECK_FALSE(s.right.mastered);
    CHECK(s.right.last_accuracy == doctest::Approx(0.85));
}

// ---- A section

TEST_CASE("hand path: a section reads only its own bars") {
    HandPathScope scope;
    scope.whole_song = false;
    scope.first_measure = 5;
    scope.last_measure = 8;
    // The run is perfect in bars 5-8 and bad elsewhere.
    std::vector<RunRecord> runs = {bar_run(R, 80, {1, 2, 3, 4, 9, 10, 11, 12, 13, 14, 15, 16})};
    HandPathStatus in_section = hand_path(runs, kSong, scope);
    CHECK(in_section.right.mastered);
    CHECK(in_section.right.runs == 1);
    HandPathStatus whole = hand_path(runs, kSong);
    CHECK_FALSE(whole.right.mastered);
}

TEST_CASE("hand path: a run that never reached the section is not a run of it") {
    HandPathScope scope;
    scope.whole_song = false;
    scope.first_measure = 12;
    scope.last_measure = 13;
    RunSpec spec;
    spec.song = kSong;
    spec.hands = R;
    spec.speed = 80;
    for (int m = 1; m <= 6; ++m) { // stopped after bar 6
        spec.bars.push_back({m, 4, 0, 0});
    }
    std::vector<RunRecord> runs = {make(spec)};
    CHECK(hand_path(runs, kSong, scope).right.runs == 0);
}

TEST_CASE("hand path: the bars 12-13 trouble spot, right hand first") {
    HandPathScope scope;
    scope.whole_song = false;
    scope.first_measure = 12;
    scope.last_measure = 13;
    std::vector<RunRecord> runs = {bar_run(R, 60, {12, 13}, 3), bar_run(R, 60, {12}, 1),
                                   bar_run(R, 70, {})};
    HandPathStatus s = hand_path(runs, kSong, scope);
    CHECK(s.stage == PathStage::RightHand);
    CHECK(s.right.runs == 3);
    PathSuggestion p = next_practice(s);
    CHECK(p.advice == PathAdvice::RaiseSpeed);
    CHECK(p.speed_percent == 80);
    runs.push_back(bar_run(R, 80, {}));
    s = hand_path(runs, kSong, scope);
    CHECK(s.stage == PathStage::LeftHand);
    CHECK(path_hint(s, next_practice(s)) == "Right hand at 80%: done. Try the left hand at 60%.");
}

// ---- Which hands play

TEST_CASE("hand path: a section with only right-hand notes skips the left and both stages") {
    HandPathScope scope;
    scope.has_left = false;
    std::vector<RunRecord> runs;
    HandPathStatus s = hand_path(runs, kSong, scope);
    CHECK(s.left.skipped);
    CHECK(s.both.skipped);
    CHECK(s.stage == PathStage::RightHand);
    runs.push_back(run(R, 80));
    s = hand_path(runs, kSong, scope);
    CHECK(s.stage == PathStage::Done);
}

TEST_CASE("hand path: a left-hand-only section starts with the left hand") {
    HandPathScope scope;
    scope.has_right = false;
    HandPathStatus s = hand_path({}, kSong, scope);
    CHECK(s.right.skipped);
    CHECK(s.stage == PathStage::LeftHand);
    PathSuggestion p = next_practice(s);
    CHECK(p.hands == L);
    CHECK_FALSE(p.moved_on);
}

TEST_CASE("hand path: a song with no hand information is practised with both hands") {
    HandPathScope scope;
    scope.has_left = false;
    scope.has_right = false;
    HandPathStatus s = hand_path({}, kSong, scope);
    CHECK(s.stage == PathStage::BothHands);
    PathSuggestion p = next_practice(s);
    CHECK(p.hands == B);
    CHECK(p.speed_percent == 60);
    CHECK_FALSE(p.moved_on);
    std::vector<RunRecord> runs = {run(B, 100)};
    CHECK(hand_path(runs, kSong, scope).stage == PathStage::Done);
}

TEST_CASE("hand path: the scope of a section comes from its notes") {
    Song song = bars_song(8); // 16 s
    song.notes.clear();
    auto add = [&](double at, Hand hand, bool drum = false) {
        Note n;
        n.start_s = at;
        n.duration_s = 0.5;
        n.hand = hand;
        n.drum = drum;
        song.notes.push_back(n);
    };
    add(1.0, Hand::Right);
    add(9.0, Hand::Left);
    add(10.0, Hand::Right);
    add(13.0, Hand::Unknown);
    add(14.0, Hand::Left, true); // a drum part is not a hand

    SongSections sections;
    REQUIRE(sections.add_marker(song, 8.0).ok);
    HandPathScope first = scope_of_section(song, &sections.sections[0]);
    CHECK_FALSE(first.whole_song);
    CHECK(first.first_measure == 1);
    CHECK(first.last_measure == 4);
    CHECK(first.has_right);
    CHECK_FALSE(first.has_left);
    HandPathScope second = scope_of_section(song, &sections.sections[1]);
    CHECK(second.first_measure == 5);
    CHECK(second.has_right);
    CHECK(second.has_left);

    HandPathScope whole = scope_of_section(song, nullptr);
    CHECK(whole.whole_song);
    CHECK(whole.has_right);
    CHECK(whole.has_left);
}

TEST_CASE("hand path property: any history gives a suggestion that fits the stage") {
    std::mt19937_64 rng(3);
    for (int iteration = 0; iteration < 400; ++iteration) {
        std::vector<RunRecord> runs;
        for (std::size_t i = 0; i < rng() % 15; ++i) {
            HandFilter h = static_cast<HandFilter>(rng() % 3);
            runs.push_back(run(h, 25 + 5 * static_cast<int>(rng() % 26),
                               static_cast<int>(rng() % 41), 40, 1 + rng() % 2,
                               static_cast<int>(rng() % 10)));
        }
        HandPathScope scope;
        scope.has_right = rng() % 4 != 0;
        scope.has_left = rng() % 4 != 0;
        HandPathOptions o;
        o.recent_runs = 1 + static_cast<int>(rng() % 12);
        HandPathStatus s = hand_path(runs, 1, scope, o);
        PathSuggestion p = next_practice(s, o);
        CAPTURE(iteration);
        if (s.stage == PathStage::Done) {
            CHECK(p.advice == PathAdvice::AllDone);
            continue;
        }
        CHECK(p.speed_percent >= 25);
        CHECK(p.speed_percent <= 150);
        CHECK(p.speed_percent % 5 == 0);
        CHECK(p.stage == s.stage);
        CHECK(p.hands == (s.stage == PathStage::RightHand  ? R
                          : s.stage == PathStage::LeftHand ? L
                                                           : B));
        CHECK_FALSE(path_hint(s, p).empty());
    }
}

// ---- Audit follow-ups (issue #32)

namespace {

Note note_at(double start_s, Hand hand) {
    Note n;
    n.start_s = start_s;
    n.duration_s = 0.5;
    n.hand = hand;
    return n;
}

} // namespace

TEST_CASE("hand path: a non-finite song length puts no note in the whole-song scope") {
    Song song;
    song.notes = {note_at(1.0, Hand::Right), note_at(2.0, Hand::Left)};
    song.duration_s = std::numeric_limits<double>::quiet_NaN();
    HandPathScope nan_scope = scope_of_section(song, nullptr);
    CHECK_FALSE(nan_scope.has_right);
    CHECK_FALSE(nan_scope.has_left);
    song.duration_s = std::numeric_limits<double>::infinity();
    HandPathScope inf_scope = scope_of_section(song, nullptr);
    CHECK_FALSE(inf_scope.has_right);
    CHECK_FALSE(inf_scope.has_left);
    song.duration_s = 10.0;
    HandPathScope fine = scope_of_section(song, nullptr);
    CHECK(fine.has_right);
    CHECK(fine.has_left);
}

TEST_CASE("hand path: a huge speed step cannot overflow") {
    std::vector<RunRecord> runs{run(R, 150)};
    HandPathOptions options;
    options.speed_step_percent = std::numeric_limits<int>::max();
    options.hand_goal_speed_percent = 150;
    HandPathStatus status = hand_path(runs, kSong, {}, options);
    // Mastered at the goal, so the right hand is done; ask for the left, which is untried.
    runs.push_back(run(L, 100));
    status = hand_path(runs, kSong, {}, options);
    PathSuggestion s = next_practice(status, options);
    CHECK(s.speed_percent >= 25);
    CHECK(s.speed_percent <= 150);

    // A raise on a clean run lands on the goal, not on a wrapped number.
    options.hand_goal_speed_percent = 150;
    std::vector<RunRecord> clean{run(R, 100)};
    PathSuggestion faster = next_practice(hand_path(clean, kSong, {}, options), options);
    CHECK(faster.advice == PathAdvice::RaiseSpeed);
    CHECK(faster.speed_percent == 150);
    options.speed_step_percent = std::numeric_limits<int>::min();
    PathSuggestion small = next_practice(hand_path(clean, kSong, {}, options), options);
    CHECK(small.speed_percent >= 25);
    CHECK(small.speed_percent <= 150);
}

TEST_CASE("hand path: a song with no bars gives its sections the bar its runs record") {
    Song song;
    song.duration_s = 20.0;
    song.notes = {note_at(1.0, Hand::Right), note_at(12.0, Hand::Right)};
    REQUIRE(song.measures.empty());
    SongSections sections;
    REQUIRE(sections.add_marker(song, 10.0).ok);
    REQUIRE(sections.sections.size() == 2);
    HandPathScope scope = scope_of_section(song, &sections.sections[0]);
    CHECK_FALSE(scope.whole_song);
    CHECK(scope.first_measure == song.measure_at(0));
    CHECK(scope.last_measure == song.measure_at(0));

    // A run of this song records bar 1: the section sees it.
    RunSpec spec;
    spec.song = kSong;
    spec.hands = R;
    spec.speed = 80;
    spec.bars.push_back({1, 8, 0, 0});
    std::vector<RunRecord> runs{make(spec)};
    HandPathStatus status = hand_path(runs, kSong, scope);
    CHECK(status.right.runs == 1);
}
