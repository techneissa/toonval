// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/hand_split.hpp"

#include "toonval/midi_file.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace toonval;

namespace {

struct N {
    double beat;
    int pitch;
    double beats = 1.0;
};

// A song of notes at 120 BPM (0.5 s per beat), 480 ticks per quarter, all with Unknown hands.
Song make_song(const std::vector<N>& notes) {
    Song song;
    song.tempo.ticks_per_quarter = 480;
    for (const N& n : notes) {
        Note note;
        note.start_tick = static_cast<std::int64_t>(n.beat * 480.0);
        note.end_tick = note.start_tick + static_cast<std::int64_t>(n.beats * 480.0);
        note.start_s = n.beat * 0.5;
        note.duration_s = n.beats * 0.5;
        note.pitch = static_cast<std::uint8_t>(n.pitch);
        note.hand = Hand::Unknown;
        note.measure = 1 + static_cast<int>(n.beat / 4.0);
        song.notes.push_back(note);
    }
    return song;
}

HandSplitOptions fixed(int split = kMiddleC) {
    HandSplitOptions o;
    o.method = SplitMethod::Fixed;
    o.split_point = split;
    return o;
}

HandSplitOptions smart(int split = kMiddleC) {
    HandSplitOptions o;
    o.method = SplitMethod::Smart;
    o.split_point = split;
    return o;
}

} // namespace

TEST_CASE("hand split, fixed: notes from the split point up are the right hand") {
    Song song = make_song({{0, 48}, {1, 59}, {2, 60}, {3, 72}});
    std::vector<Hand> h = split_hands(song, fixed());
    REQUIRE(h.size() == 4);
    CHECK(h[0] == Hand::Left);
    CHECK(h[1] == Hand::Left);
    CHECK(h[2] == Hand::Right);
    CHECK(h[3] == Hand::Right);
}

TEST_CASE("hand split, fixed: the split point can be moved") {
    Song song = make_song({{0, 55}, {1, 57}, {2, 62}});
    std::vector<Hand> h = split_hands(song, fixed(56));
    CHECK(h[0] == Hand::Left);
    CHECK(h[1] == Hand::Right);
    CHECK(h[2] == Hand::Right);
    h = split_hands(song, fixed(63));
    CHECK(h[0] == Hand::Left);
    CHECK(h[1] == Hand::Left);
    CHECK(h[2] == Hand::Left);
}

TEST_CASE("hand split: the original song is not touched, the result has one hand per note") {
    Song song = make_song({{0, 48}, {1, 72}});
    Song copy = song;
    std::vector<Hand> h = split_hands(song, smart());
    CHECK(h.size() == song.notes.size());
    for (std::size_t i = 0; i < song.notes.size(); ++i) {
        CHECK(song.notes[i].hand == copy.notes[i].hand);
        CHECK(song.notes[i].hand == Hand::Unknown);
    }
    Song split = with_split_hands(song, smart());
    CHECK(split.notes[0].hand == Hand::Left);
    CHECK(split.notes[1].hand == Hand::Right);
    CHECK(song.notes[0].hand == Hand::Unknown);
}

TEST_CASE("hand split: notes that already have a hand keep it, drums stay unknown") {
    Song song = make_song({{0, 80}, {1, 40}, {2, 40}});
    song.notes[0].hand = Hand::Left; // named track says so, whatever the pitch
    song.notes[2].drum = true;
    for (SplitMethod m : {SplitMethod::Fixed, SplitMethod::Smart}) {
        HandSplitOptions o;
        o.method = m;
        std::vector<Hand> h = split_hands(song, o);
        CHECK(h[0] == Hand::Left);
        CHECK(h[1] == Hand::Left);
        CHECK(h[2] == Hand::Unknown);
    }
}

TEST_CASE("hand split: needs_hand_split is true while a playable note has no hand") {
    Song song = make_song({{0, 48}, {1, 72}});
    CHECK(needs_hand_split(song));
    song.notes[0].hand = Hand::Left;
    CHECK(needs_hand_split(song));
    song.notes[1].hand = Hand::Right;
    CHECK_FALSE(needs_hand_split(song));
    Song drums = make_song({{0, 36}});
    drums.notes[0].drum = true;
    CHECK_FALSE(needs_hand_split(drums));
    CHECK_FALSE(needs_hand_split(Song{}));
}

TEST_CASE("hand split: a chord stays in one hand even when the split point is inside it") {
    // C major triad 58 62 65 straddles middle C (60): one hand, chosen by where the chord sits.
    Song song = make_song({{0, 58, 2}, {0, 62, 2}, {0, 65, 2}});
    for (SplitMethod m : {SplitMethod::Fixed, SplitMethod::Smart}) {
        HandSplitOptions o;
        o.method = m;
        std::vector<Hand> h = split_hands(song, o);
        CHECK(h[0] == h[1]);
        CHECK(h[1] == h[2]);
        CHECK(h[0] == Hand::Right); // the middle note, 62, is above middle C
    }
}

TEST_CASE("hand split: a chord one hand cannot reach is split at the gap nearest the split point") {
    // 36 43 52 | 64 71 76: two chords played together, 40 semitones wide.
    Song song = make_song({{0, 36}, {0, 43}, {0, 52}, {0, 64}, {0, 71}, {0, 76}});
    for (SplitMethod m : {SplitMethod::Fixed, SplitMethod::Smart}) {
        HandSplitOptions o;
        o.method = m;
        std::vector<Hand> h = split_hands(song, o);
        CHECK(h[0] == Hand::Left);
        CHECK(h[1] == Hand::Left);
        CHECK(h[2] == Hand::Left);
        CHECK(h[3] == Hand::Right);
        CHECK(h[4] == Hand::Right);
        CHECK(h[5] == Hand::Right);
    }
}

TEST_CASE("hand split: a tenth is within reach, an eleventh is not") {
    // 48..64 is 16 semitones (a major tenth): one hand. 48..65 is 17: two hands.
    Song within = make_song({{0, 48}, {0, 64}});
    std::vector<Hand> h = split_hands(within, fixed());
    CHECK(h[0] == h[1]);
    Song beyond = make_song({{0, 48}, {0, 65}});
    h = split_hands(beyond, fixed());
    CHECK(h[0] == Hand::Left);
    CHECK(h[1] == Hand::Right);
}

TEST_CASE("hand split: notes that start within a few milliseconds are one chord") {
    // A rolled chord from a played-in file: 20 ms apart.
    Song song = make_song({{0, 58}, {0, 62}, {0, 65}});
    song.notes[1].start_s += 0.02;
    song.notes[2].start_s += 0.04;
    std::vector<Hand> h = split_hands(song, fixed());
    CHECK(h[0] == h[1]);
    CHECK(h[1] == h[2]);
}

TEST_CASE("hand split, smart: a melody that dips under middle C stays in the right hand") {
    // The right hand plays 64 62 60 58 57 58 60 62 over a left hand bass at 40 and 47.
    std::vector<N> notes;
    const int melody[] = {64, 62, 60, 58, 57, 58, 60, 62, 64, 65, 64, 62, 60, 58, 57, 58};
    for (int i = 0; i < 16; ++i) {
        notes.push_back({i * 1.0, melody[i]});
    }
    for (int i = 0; i < 8; ++i) {
        notes.push_back({i * 2.0 + 0.5, i % 2 == 0 ? 40 : 47});
    }
    Song song = make_song(notes);
    // Notes are sorted by tick in a real Song; make_song above is not, so sort like the loader.
    std::stable_sort(song.notes.begin(), song.notes.end(), [](const Note& a, const Note& b) {
        return a.start_tick != b.start_tick ? a.start_tick < b.start_tick : a.pitch < b.pitch;
    });
    std::vector<Hand> fixed_h = split_hands(song, fixed());
    std::vector<Hand> smart_h = split_hands(song, smart());
    int fixed_wrong = 0;
    int smart_wrong = 0;
    for (std::size_t i = 0; i < song.notes.size(); ++i) {
        bool is_bass = song.notes[i].pitch <= 47;
        Hand want = is_bass ? Hand::Left : Hand::Right;
        fixed_wrong += fixed_h[i] != want ? 1 : 0;
        smart_wrong += smart_h[i] != want ? 1 : 0;
    }
    CHECK(fixed_wrong > 0); // the plain split sends the 57s and 58s to the left hand
    CHECK(smart_wrong == 0);
}

TEST_CASE("hand split, smart: a lone melody keeps its hand through a short dip under the split") {
    // No other register in the window, so the split point stays at middle C. 57 is three
    // semitones under it: the line continues in the right hand. The plain split drops it.
    const int melody[] = {64, 62, 60, 58, 57, 58, 60, 62, 64};
    std::vector<N> notes;
    for (int i = 0; i < 9; ++i) {
        notes.push_back({i * 1.0, melody[i]});
    }
    Song song = make_song(notes);
    std::vector<Hand> smart_h = split_hands(song, smart());
    for (Hand h : smart_h) {
        CHECK(h == Hand::Right);
    }
    std::vector<Hand> fixed_h = split_hands(song, fixed());
    CHECK(fixed_h[4] == Hand::Left);
}

TEST_CASE("hand split, smart: a line that goes further under the split changes hands") {
    // Down by steps to 52: the dip is more than three semitones under the split.
    const int line[] = {62, 60, 58, 57, 55, 53, 52};
    std::vector<N> notes;
    for (int i = 0; i < 7; ++i) {
        notes.push_back({i * 1.0, line[i]});
    }
    std::vector<Hand> h = split_hands(make_song(notes), smart());
    CHECK(h[0] == Hand::Right);
    CHECK(h[3] == Hand::Right); // 57, three under, still the same line
    CHECK(h[4] == Hand::Left);  // 55, five under
    CHECK(h[6] == Hand::Left);
}

TEST_CASE("hand split, smart: a gap in time ends the line") {
    // Right hand ends on 60; thirty beats later a bass note 57 starts fresh: left hand.
    Song song = make_song({{0, 62}, {1, 60}, {30, 57}});
    std::vector<Hand> h = split_hands(song, smart());
    CHECK(h[1] == Hand::Right);
    CHECK(h[2] == Hand::Left);
}

TEST_CASE("hand split, smart: a right hand scale up and down an octave is not cut in two") {
    // C major, 60 to 72 and back at two notes a beat, alone: one register, so the split point
    // decides, and every note is at or above it.
    const int scale[] = {60, 62, 64, 65, 67, 69, 71, 72, 71, 69, 67, 65, 64, 62};
    std::vector<N> notes;
    for (int round = 0; round < 4; ++round) {
        for (int i = 0; i < 14; ++i) {
            notes.push_back({(round * 14 + i) * 0.5, scale[i]});
        }
    }
    std::vector<Hand> h = split_hands(make_song(notes), smart());
    for (Hand hand : h) {
        CHECK(hand == Hand::Right);
    }
}

TEST_CASE("hand split, smart: both hands in the same register follow where each is playing") {
    // The whole piece sits above middle C, in two registers: a bass at 61..64 and a melody
    // at 76..81. The local split moves up between them, so the bass is the left hand.
    std::vector<N> notes;
    for (int i = 0; i < 16; ++i) {
        notes.push_back({i * 1.0, 76 + (i % 4) * 2});
        notes.push_back({i * 1.0 + 0.5, 61 + (i % 3)});
    }
    Song song = make_song(notes);
    std::stable_sort(song.notes.begin(), song.notes.end(), [](const Note& a, const Note& b) {
        return a.start_tick != b.start_tick ? a.start_tick < b.start_tick : a.pitch < b.pitch;
    });
    std::vector<Hand> smart_h = split_hands(song, smart());
    std::vector<Hand> fixed_h = split_hands(song, fixed());
    for (std::size_t i = 0; i < song.notes.size(); ++i) {
        bool bass = song.notes[i].pitch < 70;
        CHECK(smart_h[i] == (bass ? Hand::Left : Hand::Right));
    }
    // The plain split calls all of it the right hand.
    for (Hand h : fixed_h) {
        CHECK(h == Hand::Right);
    }
}

TEST_CASE("hand split: compare_hands counts only notes whose true hand is known") {
    Song truth = make_song({{0, 48}, {1, 72}, {2, 50}, {3, 40}});
    truth.notes[0].hand = Hand::Left;
    truth.notes[1].hand = Hand::Right;
    truth.notes[2].hand = Hand::Left;
    // note 3 stays Unknown: not compared
    const std::vector<Hand> assigned{Hand::Left, Hand::Left, Hand::Left, Hand::Right};
    HandAgreement a = compare_hands(truth, assigned);
    CHECK(a.compared == 3);
    CHECK(a.matching == 2);
    CHECK(a.fraction() == doctest::Approx(2.0 / 3.0));
    CHECK(compare_hands(Song{}, {}).fraction() == 1.0);
}

namespace {

std::vector<std::uint8_t> read_test_file(const char* name) {
    std::ifstream in(std::string(TOONVAL_TEST_DATA_DIR) + "/" + name, std::ios::binary);
    REQUIRE(in.good());
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in),
                                     std::istreambuf_iterator<char>());
}

struct RealFile {
    Song named;    // hands from the track names (ground truth)
    Song stripped; // the same notes with every hand Unknown
};

RealFile load_real(const char* name) {
    std::vector<std::uint8_t> bytes = read_test_file(name);
    RealFile out;
    MidiLoadResult named = load_midi(bytes);
    REQUIRE(named.ok);
    MidiLoadOptions strip;
    strip.assign_hands = false;
    MidiLoadResult plain = load_midi(bytes, strip);
    REQUIRE(plain.ok);
    REQUIRE(plain.song.notes.size() == named.song.notes.size());
    for (const Note& n : plain.song.notes) {
        REQUIRE(n.hand == Hand::Unknown);
    }
    out.named = std::move(named.song);
    out.stripped = std::move(plain.song);
    return out;
}

} // namespace

TEST_CASE("hand split, real files: agreement with the named tracks") {
    struct Case {
        const char* file;
        double fixed_floor;
        double smart_floor;
    };
    // Measured: Bach 77.0% fixed, 92.2% smart; Satie 89.7% fixed, 88.7% smart. Satie's left hand
    // plays chords in the middle of the keyboard, which no pitch rule can tell from the right
    // hand's, so the plain split does as well there as the smart one.
    for (const Case& c : {Case{"bach-bwv846-prelude.mid", 0.75, 0.90},
                          Case{"satie-gymnopedie-1.mid", 0.85, 0.87}}) {
        CAPTURE(c.file);
        RealFile f = load_real(c.file);
        REQUIRE(needs_hand_split(f.stripped));
        HandAgreement fixed_a = compare_hands(f.named, split_hands(f.stripped, fixed()));
        HandAgreement smart_a = compare_hands(f.named, split_hands(f.stripped, smart()));
        MESSAGE(c.file << ": fixed split at middle C agrees on " << fixed_a.fraction() * 100.0
                       << "% of " << fixed_a.compared << " notes, smart on "
                       << smart_a.fraction() * 100.0 << "%");
        CHECK(fixed_a.fraction() >= c.fixed_floor);
        CHECK(smart_a.fraction() >= c.smart_floor);
        if (std::string(c.file).starts_with("bach")) {
            CHECK(smart_a.fraction() >= fixed_a.fraction() + 0.10);
        }
        // Splitting must not change anything but the hands.
        Song split = with_split_hands(f.stripped, smart());
        CHECK_FALSE(needs_hand_split(split));
        REQUIRE(split.notes.size() == f.stripped.notes.size());
        for (std::size_t i = 0; i < split.notes.size(); ++i) {
            CHECK(split.notes[i].pitch == f.stripped.notes[i].pitch);
            CHECK(split.notes[i].start_tick == f.stripped.notes[i].start_tick);
        }
    }
}
