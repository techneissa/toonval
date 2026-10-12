// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/ear_training.hpp"

#include "toonval/pitch.hpp"

#include <doctest/doctest.h>

#include <array>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace toonval;

namespace {

EarSettings notes_in(KeyRange range) {
    EarSettings s;
    s.range = range;
    s.kind = EarQuestionKind::Note;
    return s;
}

EarSettings intervals_in(KeyRange range, int lo, int hi) {
    EarSettings s;
    s.range = range;
    s.kind = EarQuestionKind::Interval;
    s.min_interval = lo;
    s.max_interval = hi;
    return s;
}

std::vector<EarQuestion> draw(std::uint64_t seed, const EarSettings& settings, int count) {
    EarTrainer trainer(seed, settings);
    std::vector<EarQuestion> out;
    for (int i = 0; i < count; ++i) {
        const EarQuestionResult r = trainer.next();
        REQUIRE(r.ok);
        out.push_back(r.question);
    }
    return out;
}

} // namespace

TEST_CASE("ear: the random number generator matches the published SplitMix64 sequence") {
    // Reference outputs of splitmix64 seeded with 0.
    EarRng rng(0);
    CHECK(rng.next() == 0xE220A8397B1DCDAFULL);
    CHECK(rng.next() == 0x6E789E6AA1B965F4ULL);
    CHECK(rng.next() == 0x06C45D188009454FULL);
}

TEST_CASE("ear: below(n) stays under n, is even enough, and below(1) is 0") {
    EarRng rng(123);
    std::array<int, 6> counts{};
    for (int i = 0; i < 60000; ++i) {
        const std::uint64_t v = rng.below(6);
        REQUIRE(v < 6);
        ++counts[v];
    }
    for (int c : counts) {
        CHECK(c > 9500);
        CHECK(c < 10500);
    }
    CHECK(rng.below(1) == 0);
    CHECK(rng.below(0) == 0);
}

TEST_CASE("ear: the same seed gives the same questions, another seed another set") {
    const EarSettings s = intervals_in(kRange61, 1, 12);
    const auto a = draw(42, s, 200);
    const auto b = draw(42, s, 200);
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i].first == b[i].first);
        CHECK(a[i].second == b[i].second);
    }
    const auto c = draw(43, s, 200);
    int differing = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        differing += a[i].first != c[i].first || a[i].second != c[i].second ? 1 : 0;
    }
    CHECK(differing > 100);
}

TEST_CASE("ear: a fixed seed gives fixed questions, so a change to the draw shows up") {
    // A snapshot of the draw (SplitMix64 above, then the order of the rules in EarTrainer::next).
    // Change these numbers only on purpose: saved practice sessions replay by seed.
    const auto notes = draw(1, notes_in(kRange61), 6);
    const std::vector<int> expected_notes = {62, 81, 48, 39, 54, 41};
    REQUIRE(notes.size() == expected_notes.size());
    for (std::size_t i = 0; i < notes.size(); ++i) {
        CHECK(notes[i].first == expected_notes[i]);
    }
    const auto ivs = draw(1, intervals_in(kRange61, 1, 12), 4);
    const std::vector<std::array<int, 2>> expected_ivs = {{42, 36}, {92, 80}, {82, 72}, {67, 56}};
    REQUIRE(ivs.size() == expected_ivs.size());
    for (std::size_t i = 0; i < ivs.size(); ++i) {
        CHECK(ivs[i].first == expected_ivs[i][0]);
        CHECK(ivs[i].second == expected_ivs[i][1]);
    }
}

TEST_CASE("ear: single-note questions stay in the range, never repeat in a row, and cover it") {
    const KeyRange range{60, 72};
    const auto qs = draw(7, notes_in(range), 600);
    std::set<int> seen;
    for (std::size_t i = 0; i < qs.size(); ++i) {
        CHECK(qs[i].kind == EarQuestionKind::Note);
        CHECK(range.contains(qs[i].first));
        CHECK(qs[i].second == qs[i].first);
        if (i > 0) {
            CHECK(qs[i].first != qs[i - 1].first);
        }
        seen.insert(qs[i].first);
    }
    CHECK(seen.size() == 13);
}

TEST_CASE("ear: white keys only asks only white keys") {
    EarSettings s = notes_in(kRange61);
    s.white_keys_only = true;
    for (const EarQuestion& q : draw(9, s, 300)) {
        CHECK_FALSE(is_black_key(q.first));
    }
    EarSettings iv = intervals_in(kRange61, 2, 9);
    iv.white_keys_only = true;
    for (const EarQuestion& q : draw(9, iv, 300)) {
        CHECK_FALSE(is_black_key(q.first));
        CHECK_FALSE(is_black_key(q.second));
    }
}

TEST_CASE("ear: interval questions respect the size, the direction and the keyboard") {
    const EarSettings s = intervals_in(kRange49, 3, 8);
    std::set<int> sizes;
    bool up = false;
    bool down = false;
    for (const EarQuestion& q : draw(5, s, 2000)) {
        const int semis = q.second - q.first;
        CHECK(kRange49.contains(q.first));
        CHECK(kRange49.contains(q.second));
        CHECK(std::abs(semis) >= 3);
        CHECK(std::abs(semis) <= 8);
        CHECK(q.kind == EarQuestionKind::Interval);
        CHECK(q.semitones() == semis);
        sizes.insert(std::abs(semis));
        up = up || semis > 0;
        down = down || semis < 0;
    }
    CHECK(sizes == std::set<int>{3, 4, 5, 6, 7, 8});
    CHECK(up);
    CHECK(down);
}

TEST_CASE("ear: every interval size is asked about equally often") {
    const EarSettings s = intervals_in(kRange61, 1, 12);
    std::map<int, int> counts;
    for (const EarQuestion& q : draw(11, s, 12000)) {
        ++counts[std::abs(q.semitones())];
    }
    REQUIRE(counts.size() == 12);
    for (const auto& [size, n] : counts) {
        CAPTURE(size);
        CHECK(n > 800);
        CHECK(n < 1200);
    }
}

TEST_CASE("ear: ascending only never goes down") {
    EarSettings s = intervals_in(kRange61, 1, 12);
    s.allow_descending = false;
    for (const EarQuestion& q : draw(3, s, 500)) {
        CHECK(q.semitones() > 0);
    }
}

TEST_CASE("ear: the harmonic flag is carried to the question") {
    EarSettings s = intervals_in(kRange61, 1, 12);
    s.harmonic = true;
    for (const EarQuestion& q : draw(3, s, 20)) {
        CHECK(q.harmonic);
    }
    for (const EarQuestion& q : draw(3, intervals_in(kRange61, 1, 12), 20)) {
        CHECK_FALSE(q.harmonic);
    }
}

TEST_CASE("ear: a range with room for only one interval still works") {
    // Keys 60..64 and an interval of 4: the only pairs are C-E in each direction.
    const EarSettings s = intervals_in(KeyRange{60, 64}, 4, 4);
    for (const EarQuestion& q : draw(1, s, 50)) {
        CHECK(std::abs(q.semitones()) == 4);
        CHECK(((q.first == 60 && q.second == 64) || (q.first == 64 && q.second == 60)));
    }
}

TEST_CASE("ear: settings that cannot make a question give an error") {
    EarTrainer backwards(1, notes_in(KeyRange{72, 60}));
    CHECK_FALSE(backwards.next().ok);
    EarTrainer outside(1, notes_in(KeyRange{-5, 200}));
    CHECK_FALSE(outside.next().ok);
    EarTrainer reversed(1, intervals_in(kRange61, 8, 3));
    const EarQuestionResult r = reversed.next();
    CHECK_FALSE(r.ok);
    CHECK_FALSE(r.message.empty());
    EarTrainer zero(1, intervals_in(kRange61, 0, 5));
    CHECK_FALSE(zero.next().ok);
    EarTrainer too_wide(1, intervals_in(KeyRange{60, 64}, 5, 12));
    CHECK_FALSE(too_wide.next().ok);
    EarTrainer huge(1, intervals_in(kRange88, 1, 40));
    CHECK_FALSE(huge.next().ok);
    // White keys only on a range with no white keys: nothing to ask. 61 is C#4.
    EarSettings none = notes_in(KeyRange{61, 61});
    none.white_keys_only = true;
    EarTrainer black(1, none);
    CHECK_FALSE(black.next().ok);
}

TEST_CASE("ear: a note answer is the pitch asked, or its pitch class") {
    EarQuestion q;
    q.kind = EarQuestionKind::Note;
    q.first = q.second = 64;
    CHECK(check_pitch_answer(q, 64));
    CHECK_FALSE(check_pitch_answer(q, 65));
    CHECK_FALSE(check_pitch_answer(q, 52));
    CHECK(check_pitch_answer(q, 52, true));
    CHECK(check_pitch_answer(q, 76, true));
    CHECK_FALSE(check_pitch_answer(q, 53, true));
    CHECK_FALSE(check_pitch_answer(q, -1));
    CHECK_FALSE(check_pitch_answer(q, 200));
    CHECK_FALSE(check_pitch_answer(q, -1, true));
}

TEST_CASE("ear: an interval question's pitch answer is the second note") {
    EarQuestion q;
    q.kind = EarQuestionKind::Interval;
    q.first = 60;
    q.second = 67;
    CHECK(check_pitch_answer(q, 67));
    CHECK_FALSE(check_pitch_answer(q, 60));
    CHECK_FALSE(check_pitch_answer(q, 55));
    CHECK(check_pitch_answer(q, 55, true));
    CHECK(expected_pitch(q) == 67);
    q.kind = EarQuestionKind::Note;
    q.second = q.first;
    CHECK(expected_pitch(q) == 60);
}

TEST_CASE("ear: an interval answer is a size in semitones, with or without the direction") {
    EarQuestion up;
    up.kind = EarQuestionKind::Interval;
    up.first = 60;
    up.second = 67;
    CHECK(check_interval_answer(up, 7));
    CHECK_FALSE(check_interval_answer(up, -7));
    CHECK(check_interval_answer(up, -7, false));
    CHECK(check_interval_answer(up, 7, false));
    CHECK_FALSE(check_interval_answer(up, 6));
    CHECK_FALSE(check_interval_answer(up, 5, false));
    EarQuestion down = up;
    down.first = 67;
    down.second = 60;
    CHECK(check_interval_answer(down, -7));
    CHECK_FALSE(check_interval_answer(down, 7));
    CHECK(check_interval_answer(down, 7, false));
    EarQuestion note;
    note.kind = EarQuestionKind::Note;
    note.first = note.second = 60;
    CHECK_FALSE(check_interval_answer(note, 0));
}

TEST_CASE("ear: interval names") {
    const char* names[] = {"unison",      "minor 2nd", "major 2nd",   "minor 3rd", "major 3rd",
                           "perfect 4th", "tritone",   "perfect 5th", "minor 6th", "major 6th",
                           "minor 7th",   "major 7th", "octave"};
    for (int i = 0; i <= 12; ++i) {
        CHECK(interval_name(i) == names[i]);
        CHECK(interval_name(-i) == names[i]);
    }
    CHECK(interval_name(14) == "14 semitones");
    CHECK(interval_name(-19) == "19 semitones");
}

TEST_CASE("ear: a question becomes a song the piano plays") {
    ExerciseSettings play;
    play.bpm = 60.0;
    play.note_beats = 1.0;
    EarQuestion q;
    q.kind = EarQuestionKind::Interval;
    q.first = 60;
    q.second = 67;
    Song melodic = ear_question_song(q, play);
    REQUIRE(melodic.notes.size() == 2);
    CHECK(melodic.notes[0].pitch == 60);
    CHECK(melodic.notes[1].pitch == 67);
    CHECK(melodic.notes[1].start_s == doctest::Approx(1.0));
    CHECK(melodic.notes[0].duration_s == doctest::Approx(1.0));
    CHECK(melodic.notes[0].hand == Hand::Unknown);
    CHECK_FALSE(melodic.measures.empty());

    q.harmonic = true;
    Song harmonic = ear_question_song(q, play);
    REQUIRE(harmonic.notes.size() == 2);
    CHECK(harmonic.notes[0].start_tick == harmonic.notes[1].start_tick);
    CHECK(harmonic.notes[0].duration_s == doctest::Approx(2.0));

    EarQuestion single;
    single.kind = EarQuestionKind::Note;
    single.first = single.second = 62;
    Song one = ear_question_song(single, play);
    REQUIRE(one.notes.size() == 1);
    CHECK(one.notes[0].pitch == 62);
}
