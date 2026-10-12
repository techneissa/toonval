// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/exercises.hpp"

#include "song_compare.hpp"
#include "toonval/chord.hpp"
#include "toonval/keyboard.hpp"
#include "toonval/midi_file.hpp"
#include "toonval/midi_writer.hpp"
#include "toonval/pitch.hpp"
#include "toonval/scales.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace toonval;

namespace {

std::vector<int> v(std::initializer_list<int> xs) { return xs; }

std::vector<int> pitches(const Song& s, std::uint16_t track) {
    std::vector<int> out;
    for (const Note& n : s.notes) {
        if (n.track == track) {
            out.push_back(n.pitch);
        }
    }
    return out;
}

std::vector<int> fingers(const Song& s, std::uint16_t track) {
    std::vector<int> out;
    for (const Note& n : s.notes) {
        if (n.track == track) {
            out.push_back(n.finger);
        }
    }
    return out;
}

ScaleSpec scale(int root, ScaleType type, int octaves = 1, Direction dir = Direction::Up,
                HandsChoice hands = HandsChoice::Right) {
    ScaleSpec s;
    s.root_pc = root;
    s.type = type;
    s.octaves = octaves;
    s.direction = dir;
    s.hands = hands;
    return s;
}

ArpeggioSpec arpeggio(int root, ChordQuality q, int inversion = 0, int octaves = 1,
                      Direction dir = Direction::Up, HandsChoice hands = HandsChoice::Right) {
    ArpeggioSpec a;
    a.root_pc = root;
    a.quality = q;
    a.inversion = inversion;
    a.octaves = octaves;
    a.direction = dir;
    a.hands = hands;
    return a;
}

constexpr int C = 0, Db = 1, D = 2, Eb = 3, E = 4, F = 5, Gb = 6, G = 7, Ab = 8, A = 9, Bb = 10,
              B = 11;

const KeyRange kRanges[] = {kRange49, kRange61, kRange76, kRange88};

bool all_in_range(const Song& s, KeyRange r) {
    return std::all_of(s.notes.begin(), s.notes.end(),
                       [&](const Note& n) { return r.contains(n.pitch); });
}

} // namespace

// ----- fingering -----

TEST_CASE("fingering: C major right hand, one octave and two") {
    CHECK(scale_fingers(C, ScaleType::Major, Hand::Right, 1) == v({1, 2, 3, 1, 2, 3, 4, 5}));
    CHECK(scale_fingers(C, ScaleType::Major, Hand::Right, 2) ==
          v({1, 2, 3, 1, 2, 3, 4, 1, 2, 3, 1, 2, 3, 4, 5}));
}

TEST_CASE("fingering: C major left hand, one octave and two") {
    CHECK(scale_fingers(C, ScaleType::Major, Hand::Left, 1) == v({5, 4, 3, 2, 1, 3, 2, 1}));
    CHECK(scale_fingers(C, ScaleType::Major, Hand::Left, 2) ==
          v({5, 4, 3, 2, 1, 3, 2, 1, 4, 3, 2, 1, 3, 2, 1}));
}

TEST_CASE("fingering: well-known major scales") {
    CHECK(scale_fingers(F, ScaleType::Major, Hand::Right, 1) == v({1, 2, 3, 4, 1, 2, 3, 4}));
    CHECK(scale_fingers(F, ScaleType::Major, Hand::Left, 1) == v({5, 4, 3, 2, 1, 3, 2, 1}));
    CHECK(scale_fingers(G, ScaleType::Major, Hand::Right, 1) == v({1, 2, 3, 1, 2, 3, 4, 5}));
    CHECK(scale_fingers(D, ScaleType::Major, Hand::Right, 1) == v({1, 2, 3, 1, 2, 3, 4, 5}));
    CHECK(scale_fingers(Bb, ScaleType::Major, Hand::Right, 1) == v({4, 1, 2, 3, 1, 2, 3, 4}));
    CHECK(scale_fingers(Bb, ScaleType::Major, Hand::Left, 1) == v({3, 2, 1, 4, 3, 2, 1, 3}));
    CHECK(scale_fingers(Eb, ScaleType::Major, Hand::Right, 1) == v({3, 1, 2, 3, 4, 1, 2, 3}));
    CHECK(scale_fingers(Eb, ScaleType::Major, Hand::Left, 1) == v({3, 2, 1, 4, 3, 2, 1, 3}));
    CHECK(scale_fingers(B, ScaleType::Major, Hand::Right, 1) == v({1, 2, 3, 1, 2, 3, 4, 5}));
    CHECK(scale_fingers(B, ScaleType::Major, Hand::Left, 1) == v({4, 3, 2, 1, 4, 3, 2, 1}));
    CHECK(scale_fingers(Gb, ScaleType::Major, Hand::Right, 1) == v({2, 3, 4, 1, 2, 3, 1, 2}));
    CHECK(scale_fingers(Gb, ScaleType::Major, Hand::Left, 1) == v({4, 3, 2, 1, 3, 2, 1, 4}));
    CHECK(scale_fingers(Db, ScaleType::Major, Hand::Right, 1) == v({2, 3, 1, 2, 3, 4, 1, 2}));
    CHECK(scale_fingers(Ab, ScaleType::Major, Hand::Right, 1) == v({3, 4, 1, 2, 3, 1, 2, 3}));
}

TEST_CASE("fingering: well-known minor scales, natural and harmonic alike") {
    for (ScaleType t : {ScaleType::NaturalMinor, ScaleType::HarmonicMinor}) {
        CHECK(scale_fingers(A, t, Hand::Right, 1) == v({1, 2, 3, 1, 2, 3, 4, 5}));
        CHECK(scale_fingers(A, t, Hand::Left, 1) == v({5, 4, 3, 2, 1, 3, 2, 1}));
        CHECK(scale_fingers(E, t, Hand::Right, 1) == v({1, 2, 3, 1, 2, 3, 4, 5}));
        CHECK(scale_fingers(D, t, Hand::Right, 1) == v({1, 2, 3, 1, 2, 3, 4, 5}));
        CHECK(scale_fingers(C, t, Hand::Right, 1) == v({1, 2, 3, 1, 2, 3, 4, 5}));
        CHECK(scale_fingers(C, t, Hand::Left, 1) == v({5, 4, 3, 2, 1, 3, 2, 1}));
        CHECK(scale_fingers(F, t, Hand::Right, 1) == v({1, 2, 3, 4, 1, 2, 3, 4}));
        CHECK(scale_fingers(Gb, t, Hand::Right, 1) == v({3, 4, 1, 2, 3, 1, 2, 3}));
    }
}

TEST_CASE("fingering: the fourth finger sits where the teaching charts put it (major keys)") {
    // Place of the 4th finger in each hand, as scale degrees 1..7, from Prof. Escadon's chart.
    struct Place {
        int root;
        int lh;
        int rh;
    };
    const Place places[] = {{C, 2, 7}, {D, 2, 7},  {E, 2, 7},  {G, 2, 7},  {A, 2, 7},  {F, 2, 4},
                            {B, 5, 7}, {Db, 4, 6}, {Eb, 4, 5}, {Gb, 1, 3}, {Ab, 4, 2}, {Bb, 4, 1}};
    for (const Place& p : places) {
        CAPTURE(p.root);
        // The second octave of a two-octave scale: degrees 1..7 there use the repeating pattern
        // (a scale may start on a different finger than it repeats on, as B major's left hand).
        const auto lh = scale_fingers(p.root, ScaleType::Major, Hand::Left, 2);
        const auto rh = scale_fingers(p.root, ScaleType::Major, Hand::Right, 2);
        REQUIRE(lh.size() == 15);
        REQUIRE(rh.size() == 15);
        for (int d = 1; d <= 7; ++d) {
            CAPTURE(d);
            CHECK((lh[static_cast<std::size_t>(6 + d)] == 4) == (d == p.lh));
            CHECK((rh[static_cast<std::size_t>(6 + d)] == 4) == (d == p.rh));
        }
    }
}

TEST_CASE("fingering: defined for major, natural and harmonic minor in all 12 keys, both hands") {
    for (ScaleType t : {ScaleType::Major, ScaleType::NaturalMinor, ScaleType::HarmonicMinor}) {
        for (int root = 0; root < 12; ++root) {
            for (Hand h : {Hand::Right, Hand::Left}) {
                for (int octaves : {1, 2}) {
                    CAPTURE(scale_type_name(t));
                    CAPTURE(root);
                    const auto f = scale_fingers(root, t, h, octaves);
                    REQUIRE(f.size() == static_cast<std::size_t>(7 * octaves + 1));
                    for (int x : f) {
                        CHECK(x >= 1);
                        CHECK(x <= 5);
                    }
                }
            }
        }
    }
}

TEST_CASE("fingering: undefined for the other scales, an unknown hand and a bad octave count") {
    for (ScaleType t : {ScaleType::MelodicMinor, ScaleType::Dorian, ScaleType::MajorPentatonic,
                        ScaleType::Blues, ScaleType::Chromatic, ScaleType::WholeTone}) {
        CHECK(scale_fingers(C, t, Hand::Right, 1).empty());
    }
    CHECK(scale_fingers(C, ScaleType::Major, Hand::Unknown, 1).empty());
    CHECK(scale_fingers(C, ScaleType::Major, Hand::Right, 0).empty());
    CHECK(scale_fingers(C, ScaleType::Major, Hand::Right, 3).empty());
}

TEST_CASE("fingering: the thumb is on a white key and crossings are legal, in every table") {
    for (ScaleType t : {ScaleType::Major, ScaleType::NaturalMinor, ScaleType::HarmonicMinor}) {
        for (int root = 0; root < 12; ++root) {
            for (Hand h : {Hand::Right, Hand::Left}) {
                CAPTURE(scale_type_name(t));
                CAPTURE(root);
                CAPTURE(h == Hand::Right);
                const auto f = scale_fingers(root, t, h, 2);
                const std::vector<int> steps = [&] {
                    const auto s = scale_intervals(t);
                    std::vector<int> out;
                    for (int o = 0; o < 2; ++o) {
                        for (int iv : s) {
                            out.push_back(root + 12 * o + iv);
                        }
                    }
                    out.push_back(root + 24);
                    return out;
                }();
                REQUIRE(f.size() == steps.size());
                for (std::size_t i = 0; i < f.size(); ++i) {
                    // The thumb falls on a white key. The one exception is the natural G# minor in
                    // the left hand, whose seventh (F#) is black where the harmonic's (F##) is
                    // white.
                    const bool exception =
                        t == ScaleType::NaturalMinor && root == Ab && h == Hand::Left;
                    if (f[i] == 1 && !exception) {
                        CHECK_FALSE(is_black_key(steps[i]));
                    }
                    // The fifth finger only ends a right-hand scale or starts a left-hand one.
                    if (f[i] == 5) {
                        CHECK(i == (h == Hand::Right ? f.size() - 1 : 0));
                    }
                    if (i == 0) {
                        continue;
                    }
                    const int prev = f[i - 1];
                    const int next = f[i];
                    if (h == Hand::Right) {
                        // Up a finger, or the thumb passes under the 3rd or 4th.
                        CHECK((next == prev + 1 || (next == 1 && (prev == 3 || prev == 4))));
                    } else {
                        // Down a finger, or the 3rd or 4th crosses over the thumb.
                        CHECK((next == prev - 1 || (prev == 1 && (next == 3 || next == 4))));
                    }
                }
            }
        }
    }
}

// ----- scale exercises -----

TEST_CASE("exercise: C major, one octave up, right hand, from middle C") {
    const ExerciseResult r = make_scale_exercise(scale(C, ScaleType::Major));
    REQUIRE(r.ok);
    CHECK(r.title == "C major scale, right hand, 1 octave");
    const Song& s = r.song;
    CHECK(pitches(s, 1) == v({60, 62, 64, 65, 67, 69, 71, 72}));
    CHECK(fingers(s, 1) == v({1, 2, 3, 1, 2, 3, 4, 5}));
    for (const Note& n : s.notes) {
        CHECK(n.hand == Hand::Right);
        CHECK(n.track == 1);
        CHECK(n.channel == 0);
        CHECK(n.velocity == 80);
    }
    // 80 BPM, eighth notes: 0.375 s each, one after the other.
    CHECK(s.tempo.changes[0].us_per_quarter == 750'000);
    CHECK(s.notes[0].start_s == doctest::Approx(0.0));
    CHECK(s.notes[1].start_s == doctest::Approx(0.375));
    CHECK(s.notes[0].duration_s == doctest::Approx(0.375));
    CHECK(s.notes[0].start_tick == 0);
    CHECK(s.notes[0].end_tick == 240);
    CHECK(s.notes[7].end_tick == 8 * 240);
    CHECK(s.measures.size() == 1);
    REQUIRE(s.parts.size() == 1);
    CHECK(s.parts[0].name == "Right Hand");
    CHECK(s.parts[0].hand == Hand::Right);
}

TEST_CASE("exercise: down, and up and down without repeating the top note") {
    const ExerciseResult down = make_scale_exercise(scale(C, ScaleType::Major, 1, Direction::Down));
    REQUIRE(down.ok);
    CHECK(pitches(down.song, 1) == v({72, 71, 69, 67, 65, 64, 62, 60}));
    CHECK(fingers(down.song, 1) == v({5, 4, 3, 2, 1, 3, 2, 1}));
    // The notes of a song are sorted by start, so they play in this order.
    const ExerciseResult both =
        make_scale_exercise(scale(C, ScaleType::Major, 1, Direction::UpDown));
    REQUIRE(both.ok);
    CHECK(pitches(both.song, 1) == v({60, 62, 64, 65, 67, 69, 71, 72, 71, 69, 67, 65, 64, 62, 60}));
    CHECK(fingers(both.song, 1) == v({1, 2, 3, 1, 2, 3, 4, 5, 4, 3, 2, 1, 3, 2, 1}));
}

TEST_CASE("exercise: two octaves") {
    const ExerciseResult r = make_scale_exercise(scale(G, ScaleType::Major, 2));
    REQUIRE(r.ok);
    CHECK(r.song.notes.size() == 15);
    CHECK(pitches(r.song, 1).front() == 67);
    CHECK(pitches(r.song, 1).back() == 91);
    CHECK(r.title == "G major scale, right hand, 2 octaves");
}

TEST_CASE("exercise: left hand alone plays from the octave below middle C") {
    const ExerciseResult r =
        make_scale_exercise(scale(C, ScaleType::Major, 1, Direction::Up, HandsChoice::Left));
    REQUIRE(r.ok);
    CHECK(pitches(r.song, 1) == v({48, 50, 52, 53, 55, 57, 59, 60}));
    CHECK(fingers(r.song, 1) == v({5, 4, 3, 2, 1, 3, 2, 1}));
    for (const Note& n : r.song.notes) {
        CHECK(n.hand == Hand::Left);
    }
    REQUIRE(r.song.parts.size() == 1);
    CHECK(r.song.parts[0].name == "Left Hand");
}

TEST_CASE("exercise: hands together play the same notes an octave apart, in step") {
    const ExerciseResult r = make_scale_exercise(
        scale(F, ScaleType::Major, 2, Direction::UpDown, HandsChoice::Together));
    REQUIRE(r.ok);
    const auto rh = pitches(r.song, 1);
    const auto lh = pitches(r.song, 2);
    REQUIRE(rh.size() == 29);
    REQUIRE(lh.size() == 29);
    for (std::size_t i = 0; i < rh.size(); ++i) {
        CHECK(rh[i] - 12 == lh[i]);
    }
    CHECK(fingers(r.song, 1)[0] == 1);
    CHECK(fingers(r.song, 2)[0] == 5);
    for (const Note& n : r.song.notes) {
        CHECK(n.hand == (n.track == 1 ? Hand::Right : Hand::Left));
    }
    // Two notes start on every tick that has a note.
    int simultaneous = 0;
    for (std::size_t i = 1; i < r.song.notes.size(); ++i) {
        if (r.song.notes[i].start_tick == r.song.notes[i - 1].start_tick) {
            ++simultaneous;
        }
    }
    CHECK(simultaneous == 29);
    REQUIRE(r.song.parts.size() == 2);
    CHECK(r.title == "F major scale, hands together, 2 octaves");
}

TEST_CASE("exercise: harmonic minor raises the seventh, and the melodic comes down natural") {
    const ExerciseResult harmonic = make_scale_exercise(scale(A, ScaleType::HarmonicMinor));
    REQUIRE(harmonic.ok);
    CHECK(pitches(harmonic.song, 1) == v({69, 71, 72, 74, 76, 77, 80, 81}));
    CHECK(fingers(harmonic.song, 1) == v({1, 2, 3, 1, 2, 3, 4, 5}));
    const ExerciseResult melodic =
        make_scale_exercise(scale(A, ScaleType::MelodicMinor, 1, Direction::UpDown));
    REQUIRE(melodic.ok);
    CHECK(pitches(melodic.song, 1) ==
          v({69, 71, 72, 74, 76, 78, 80, 81, 79, 77, 76, 74, 72, 71, 69}));
    // No fingering is defined for the melodic minor.
    CHECK(fingers(melodic.song, 1) == std::vector<int>(15, 0));
}

TEST_CASE("exercise: scales with no fingering table have finger 0") {
    const ExerciseResult r = make_scale_exercise(scale(D, ScaleType::Dorian));
    REQUIRE(r.ok);
    for (const Note& n : r.song.notes) {
        CHECK(n.finger == 0);
    }
}

TEST_CASE("exercise: every scale type, key, length, direction and hand stays on the keyboard") {
    for (KeyRange range : kRanges) {
        ExerciseSettings settings;
        settings.range = range;
        for (ScaleType t : kAllScaleTypes) {
            for (int root = 0; root < 12; ++root) {
                for (int octaves : {1, 2}) {
                    for (Direction dir : {Direction::Up, Direction::Down, Direction::UpDown}) {
                        for (HandsChoice hands :
                             {HandsChoice::Right, HandsChoice::Left, HandsChoice::Together}) {
                            CAPTURE(range.lowest);
                            CAPTURE(scale_type_name(t));
                            CAPTURE(root);
                            CAPTURE(octaves);
                            const ExerciseResult r =
                                make_scale_exercise(scale(root, t, octaves, dir, hands), settings);
                            REQUIRE(r.ok);
                            REQUIRE_FALSE(r.song.notes.empty());
                            CHECK(all_in_range(r.song, range));
                            for (const Note& n : r.song.notes) {
                                CHECK(n.hand != Hand::Unknown);
                                // The melodic minor comes down as the natural minor.
                                CHECK((is_in_scale(n.pitch, root, t) ||
                                       (t == ScaleType::MelodicMinor &&
                                        is_in_scale(n.pitch, root, ScaleType::NaturalMinor))));
                            }
                        }
                    }
                }
            }
        }
    }
}

TEST_CASE("exercise: hands are right on the right track, whatever the key") {
    for (int root = 0; root < 12; ++root) {
        const ExerciseResult r = make_scale_exercise(
            scale(root, ScaleType::Major, 2, Direction::UpDown, HandsChoice::Together));
        REQUIRE(r.ok);
        for (const Note& n : r.song.notes) {
            CHECK(n.hand == (n.track == 1 ? Hand::Right : Hand::Left));
            CHECK(n.finger >= 1);
        }
    }
}

TEST_CASE("exercise: a scale too high for a small keyboard is shifted down whole octaves") {
    ExerciseSettings settings;
    settings.range = kRange49; // 36..84
    const ExerciseResult r = make_scale_exercise(scale(B, ScaleType::Major, 2), settings);
    REQUIRE(r.ok);
    // The default start is B4 (71) and the top would be B6 (95): one octave down fits.
    CHECK(pitches(r.song, 1).front() == 59);
    CHECK(pitches(r.song, 1).back() == 83);
}

TEST_CASE("exercise: a keyboard too small for the scale gives an error and no song") {
    ExerciseSettings settings;
    settings.range = KeyRange{36, 48}; // 13 keys
    const ExerciseResult r = make_scale_exercise(scale(C, ScaleType::Major, 2), settings);
    CHECK_FALSE(r.ok);
    CHECK_FALSE(r.message.empty());
    CHECK(r.song.notes.empty());
    // One octave still fits, from the lowest key.
    const ExerciseResult one = make_scale_exercise(scale(C, ScaleType::Major, 1), settings);
    REQUIRE(one.ok);
    CHECK(all_in_range(one.song, settings.range));
    // An empty or backwards range fits nothing.
    settings.range = KeyRange{60, 40};
    CHECK_FALSE(make_scale_exercise(scale(C, ScaleType::Major), settings).ok);
}

TEST_CASE("exercise: bad specs are errors") {
    CHECK_FALSE(make_scale_exercise(scale(C, ScaleType::Major, 0)).ok);
    CHECK_FALSE(make_scale_exercise(scale(C, ScaleType::Major, 3)).ok);
    ExerciseSettings bad;
    bad.bpm = 0.0;
    CHECK_FALSE(make_scale_exercise(scale(C, ScaleType::Major), bad).ok);
    bad = {};
    bad.note_beats = 0.0;
    CHECK_FALSE(make_scale_exercise(scale(C, ScaleType::Major), bad).ok);
    bad = {};
    bad.ticks_per_quarter = 0;
    CHECK_FALSE(make_scale_exercise(scale(C, ScaleType::Major), bad).ok);
}

TEST_CASE("exercise: tempo and note length follow the settings") {
    ExerciseSettings settings;
    settings.bpm = 120.0;
    settings.note_beats = 1.0;
    settings.velocity = 100;
    const ExerciseResult r = make_scale_exercise(scale(C, ScaleType::Major), settings);
    REQUIRE(r.ok);
    CHECK(r.song.notes[1].start_s == doctest::Approx(0.5));
    CHECK(r.song.notes[0].duration_s == doctest::Approx(0.5));
    CHECK(r.song.notes[0].velocity == 100);
    CHECK(r.song.measures.size() == 2); // 8 beats in 4/4
}

TEST_CASE("exercise: a generated scale is a finished song that writes to .mid and reads back") {
    ExerciseSettings settings;
    settings.bpm = 100.0;
    const ExerciseResult r = make_scale_exercise(
        scale(Bb, ScaleType::HarmonicMinor, 2, Direction::UpDown, HandsChoice::Together), settings);
    REQUIRE(r.ok);
    const MidiWriteResult w = write_midi(r.song);
    REQUIRE(w.ok);
    CHECK(w.notes_shortened == 0);
    CHECK(w.notes_skipped == 0);
    const MidiLoadResult loaded = load_midi(w.bytes);
    REQUIRE(loaded.ok);
    // Hands survive through the part names; fingers are not in a MIDI file.
    Song expected = r.song;
    for (Note& n : expected.notes) {
        n.finger = 0;
    }
    test::expect_same_song(expected, loaded.song);
}

// ----- arpeggios -----

TEST_CASE("arpeggio: C major root position, one octave and two") {
    const ExerciseResult one = make_arpeggio_exercise(arpeggio(C, ChordQuality::Major));
    REQUIRE(one.ok);
    CHECK(pitches(one.song, 1) == v({60, 64, 67, 72}));
    CHECK(one.title == "C major arpeggio, right hand, 1 octave");
    const ExerciseResult two = make_arpeggio_exercise(arpeggio(C, ChordQuality::Major, 0, 2));
    REQUIRE(two.ok);
    CHECK(pitches(two.song, 1) == v({60, 64, 67, 72, 76, 79, 84}));
}

TEST_CASE("arpeggio: minor, and inversions start on the third and the fifth") {
    CHECK(pitches(make_arpeggio_exercise(arpeggio(C, ChordQuality::Minor)).song, 1) ==
          v({60, 63, 67, 72}));
    const ExerciseResult first = make_arpeggio_exercise(arpeggio(C, ChordQuality::Major, 1));
    REQUIRE(first.ok);
    CHECK(pitches(first.song, 1) == v({64, 67, 72, 76}));
    CHECK(first.title == "C major arpeggio, first inversion, right hand, 1 octave");
    const ExerciseResult second = make_arpeggio_exercise(arpeggio(C, ChordQuality::Major, 2));
    REQUIRE(second.ok);
    CHECK(pitches(second.song, 1) == v({67, 72, 76, 79}));
    CHECK(pitches(make_arpeggio_exercise(arpeggio(A, ChordQuality::Minor, 1)).song, 1) ==
          v({60, 64, 69, 72}));
}

TEST_CASE("arpeggio: seventh chords, directions and the left hand") {
    CHECK(pitches(make_arpeggio_exercise(arpeggio(G, ChordQuality::Dominant7)).song, 1) ==
          v({67, 71, 74, 77, 79}));
    CHECK(pitches(
              make_arpeggio_exercise(arpeggio(C, ChordQuality::Major, 0, 1, Direction::Down)).song,
              1) == v({72, 67, 64, 60}));
    CHECK(
        pitches(
            make_arpeggio_exercise(arpeggio(C, ChordQuality::Major, 0, 1, Direction::UpDown)).song,
            1) == v({60, 64, 67, 72, 67, 64, 60}));
    const ExerciseResult lh = make_arpeggio_exercise(
        arpeggio(C, ChordQuality::Major, 0, 1, Direction::Up, HandsChoice::Left));
    REQUIRE(lh.ok);
    CHECK(pitches(lh.song, 1) == v({48, 52, 55, 60}));
    for (const Note& n : lh.song.notes) {
        CHECK(n.hand == Hand::Left);
        CHECK(n.finger == 0); // no fingering table for arpeggios
    }
    const ExerciseResult both = make_arpeggio_exercise(
        arpeggio(C, ChordQuality::Major, 0, 1, Direction::Up, HandsChoice::Together));
    REQUIRE(both.ok);
    CHECK(pitches(both.song, 1) == v({60, 64, 67, 72}));
    CHECK(pitches(both.song, 2) == v({48, 52, 55, 60}));
}

TEST_CASE("arpeggio: every chord type, root and inversion stays on the keyboard and in the chord") {
    const ChordQuality qualities[] = {
        ChordQuality::Major,           ChordQuality::Minor,       ChordQuality::Diminished,
        ChordQuality::Augmented,       ChordQuality::Sus2,        ChordQuality::Sus4,
        ChordQuality::Dominant7,       ChordQuality::Major7,      ChordQuality::Minor7,
        ChordQuality::HalfDiminished7, ChordQuality::Diminished7, ChordQuality::MinorMajor7,
        ChordQuality::Major6,          ChordQuality::Minor6,      ChordQuality::Power};
    for (KeyRange range : kRanges) {
        ExerciseSettings settings;
        settings.range = range;
        for (ChordQuality q : qualities) {
            const std::size_t tones = chord_intervals(q).size();
            for (int root = 0; root < 12; ++root) {
                for (int inv = 0; inv < static_cast<int>(tones); ++inv) {
                    for (int octaves : {1, 2}) {
                        for (HandsChoice hands :
                             {HandsChoice::Right, HandsChoice::Left, HandsChoice::Together}) {
                            CAPTURE(static_cast<int>(q));
                            CAPTURE(root);
                            CAPTURE(inv);
                            const ExerciseResult r = make_arpeggio_exercise(
                                arpeggio(root, q, inv, octaves, Direction::UpDown, hands),
                                settings);
                            REQUIRE(r.ok);
                            CHECK(all_in_range(r.song, range));
                            const auto in_chord = [&](int pitch) {
                                for (int iv : chord_intervals(q)) {
                                    if (pitch_class(pitch) == (root + iv) % 12) {
                                        return true;
                                    }
                                }
                                return false;
                            };
                            for (const Note& n : r.song.notes) {
                                CHECK(in_chord(n.pitch));
                            }
                            // One octave of N chord tones is N*octaves + 1 notes up, the same
                            // coming down without the top repeated.
                            const std::size_t up = tones * static_cast<std::size_t>(octaves) + 1;
                            const std::size_t hand_notes =
                                r.song.notes.size() / (hands == HandsChoice::Together ? 2 : 1);
                            CHECK(hand_notes == 2 * up - 1);
                        }
                    }
                }
            }
        }
    }
}

TEST_CASE("arpeggio: an inversion the chord does not have, or a chord too wide, is an error") {
    CHECK_FALSE(make_arpeggio_exercise(arpeggio(C, ChordQuality::Major, 3)).ok);
    CHECK_FALSE(make_arpeggio_exercise(arpeggio(C, ChordQuality::Major, -1)).ok);
    CHECK_FALSE(make_arpeggio_exercise(arpeggio(C, ChordQuality::Dominant9)).ok);
    CHECK_FALSE(make_arpeggio_exercise(arpeggio(C, ChordQuality::Add9)).ok);
    CHECK_FALSE(make_arpeggio_exercise(arpeggio(C, ChordQuality::None)).ok);
    CHECK_FALSE(make_arpeggio_exercise(arpeggio(C, ChordQuality::Major, 0, 3)).ok);
    CHECK(make_arpeggio_exercise(arpeggio(C, ChordQuality::Dominant7, 3)).ok);
}

// ----- chord drills -----

namespace {

ChordDrillSpec drill(std::vector<ChordDrillItem> items, HandsChoice hands = HandsChoice::Right) {
    ChordDrillSpec d;
    d.items = std::move(items);
    d.hands = hands;
    return d;
}

} // namespace

TEST_CASE("chord drill: C major in root position and both inversions, with names that match") {
    const ExerciseResult r = make_chord_drill(drill(
        {{C, ChordQuality::Major, 0}, {C, ChordQuality::Major, 1}, {C, ChordQuality::Major, 2}}));
    REQUIRE(r.ok);
    REQUIRE(r.steps.size() == 3);
    CHECK(r.steps[0].label == "C");
    CHECK(r.steps[0].notes == v({60, 64, 67}));
    CHECK(r.steps[1].label == "C/E");
    CHECK(r.steps[1].notes == v({64, 67, 72}));
    CHECK(r.steps[2].label == "C/G");
    CHECK(r.steps[2].notes == v({67, 72, 76}));
    // Each chord is a block: its notes start on one tick, four beats after the one before.
    CHECK(r.steps[1].start_tick == 4 * 480);
    CHECK(r.steps[1].start_s == doctest::Approx(4 * 0.75));
    CHECK(pitches(r.song, 1) == v({60, 64, 67, 64, 67, 72, 67, 72, 76}));
    for (const Note& n : r.song.notes) {
        CHECK(n.hand == Hand::Right);
        CHECK(n.finger == 0);
        CHECK(n.end_tick - n.start_tick == 3 * 480);
    }
}

TEST_CASE("chord drill: the label is what detect_chord shows for the notes, every chord type") {
    const ChordQuality qualities[] = {ChordQuality::Major,       ChordQuality::Minor,
                                      ChordQuality::Diminished,  ChordQuality::Augmented,
                                      ChordQuality::Sus2,        ChordQuality::Sus4,
                                      ChordQuality::Dominant7,   ChordQuality::Major7,
                                      ChordQuality::Minor7,      ChordQuality::HalfDiminished7,
                                      ChordQuality::Diminished7, ChordQuality::MinorMajor7,
                                      ChordQuality::Major6,      ChordQuality::Minor6,
                                      ChordQuality::Add9,        ChordQuality::Dominant9,
                                      ChordQuality::Major9,      ChordQuality::Minor9,
                                      ChordQuality::Power};
    int built = 0;
    for (KeyRange range : kRanges) {
        ExerciseSettings settings;
        settings.range = range;
        for (ChordQuality q : qualities) {
            for (int root = 0; root < 12; ++root) {
                for (int inv = 0; inv < static_cast<int>(chord_intervals(q).size()); ++inv) {
                    for (bool flats : {false, true}) {
                        CAPTURE(static_cast<int>(q));
                        CAPTURE(root);
                        CAPTURE(inv);
                        ChordDrillSpec d = drill({{root, q, inv}});
                        d.prefer_flats = flats;
                        const ExerciseResult r = make_chord_drill(d, settings);
                        if (!r.ok) {
                            // Only a chord that detect_chord would name differently is refused.
                            CHECK(inv > 0);
                            CHECK_FALSE(r.message.empty());
                            continue;
                        }
                        ++built;
                        REQUIRE(r.steps.size() == 1);
                        std::vector<int> notes(r.steps[0].notes.begin(), r.steps[0].notes.end());
                        const Chord c = detect_chord(notes);
                        REQUIRE(c.found);
                        CHECK(c.root_pc == root);
                        CHECK(c.quality == q);
                        CHECK(chord_name(c, flats) == r.steps[0].label);
                        CHECK(all_in_range(r.song, range));
                        // The first note is the bass of the inversion.
                        CHECK(pitch_class(notes.front()) == c.bass_pc);
                    }
                }
            }
        }
    }
    CHECK(built > 1000);
}

TEST_CASE("chord drill: every root-position chord of every type is accepted") {
    for (ChordQuality q :
         {ChordQuality::Major, ChordQuality::Minor, ChordQuality::Diminished,
          ChordQuality::Augmented, ChordQuality::Sus2, ChordQuality::Sus4, ChordQuality::Dominant7,
          ChordQuality::Major7, ChordQuality::Minor7, ChordQuality::HalfDiminished7,
          ChordQuality::Diminished7, ChordQuality::MinorMajor7, ChordQuality::Major6,
          ChordQuality::Minor6, ChordQuality::Add9, ChordQuality::Dominant9, ChordQuality::Major9,
          ChordQuality::Minor9, ChordQuality::Power}) {
        for (int root = 0; root < 12; ++root) {
            CAPTURE(static_cast<int>(q));
            CAPTURE(root);
            CHECK(make_chord_drill(drill({{root, q, 0}})).ok);
        }
    }
}

TEST_CASE("chord drill: an inversion that reads as another chord is refused, with the reason") {
    // C E G A over E is Am7/E to detect_chord, so C6 in first inversion cannot be asked for.
    const ExerciseResult r = make_chord_drill(drill({{C, ChordQuality::Major6, 1}}));
    CHECK_FALSE(r.ok);
    CHECK(r.message.find("Am7/E") != std::string::npos);
    CHECK(r.song.notes.empty());
    // A power chord has no inversions that read as a power chord.
    CHECK_FALSE(make_chord_drill(drill({{C, ChordQuality::Power, 1}})).ok);
}

TEST_CASE("chord drill: inversion_drill lists the inversions that read back right") {
    const std::vector<ChordDrillItem> triads = inversion_drill(C, ChordQuality::Major);
    REQUIRE(triads.size() == 3);
    for (int i = 0; i < 3; ++i) {
        CHECK(triads[static_cast<std::size_t>(i)].inversion == i);
        CHECK(triads[static_cast<std::size_t>(i)].root_pc == C);
        CHECK(triads[static_cast<std::size_t>(i)].quality == ChordQuality::Major);
    }
    const std::vector<ChordDrillItem> six = inversion_drill(C, ChordQuality::Major6);
    CHECK(six.size() < 4);
    CHECK(make_chord_drill(drill(six)).ok);
    for (ChordQuality q : {ChordQuality::Minor, ChordQuality::Dominant7, ChordQuality::Major7}) {
        for (int root = 0; root < 12; ++root) {
            const auto items = inversion_drill(root, q);
            CHECK_FALSE(items.empty());
            CHECK(make_chord_drill(drill(items)).ok);
        }
    }
}

TEST_CASE("chord drill: left hand plays an octave lower, together adds the bass below") {
    const ChordDrillSpec left = drill({{C, ChordQuality::Major, 0}}, HandsChoice::Left);
    const ExerciseResult lr = make_chord_drill(left);
    REQUIRE(lr.ok);
    CHECK(lr.steps[0].notes == v({48, 52, 55}));
    for (const Note& n : lr.song.notes) {
        CHECK(n.hand == Hand::Left);
    }
    const ExerciseResult both =
        make_chord_drill(drill({{C, ChordQuality::Major, 1}}, HandsChoice::Together));
    REQUIRE(both.ok);
    // Right hand holds E G C; the left hand doubles the bass E an octave below.
    CHECK(both.steps[0].notes == v({52, 64, 67, 72}));
    CHECK(both.steps[0].label == "C/E");
    CHECK(pitches(both.song, 1) == v({64, 67, 72}));
    CHECK(pitches(both.song, 2) == v({52}));
}

TEST_CASE("chord drill: steps follow step_beats and notes last hold_beats, at the given bpm") {
    ChordDrillSpec d = drill({{G, ChordQuality::Dominant7, 0}, {C, ChordQuality::Major, 0}});
    d.step_beats = 2.0;
    d.hold_beats = 1.0;
    ExerciseSettings s;
    s.bpm = 60.0;
    const ExerciseResult r = make_chord_drill(d, s);
    REQUIRE(r.ok);
    CHECK(r.steps[1].start_s == doctest::Approx(2.0));
    CHECK(r.song.notes.back().duration_s == doctest::Approx(1.0));
    // The song lasts to the end of the last chord, and each chord starts a bar-fraction apart.
    CHECK(r.song.duration_s == doctest::Approx(3.0));
}

TEST_CASE("chord drill: flat names when asked") {
    ChordDrillSpec d = drill({{Bb, ChordQuality::Major, 0}, {Eb, ChordQuality::Minor7, 0}});
    CHECK(make_chord_drill(d).steps[0].label == "A#");
    d.prefer_flats = true;
    const ExerciseResult r = make_chord_drill(d);
    CHECK(r.steps[0].label == "Bb");
    CHECK(r.steps[1].label == "Ebm7");
}

TEST_CASE("chord drill: bad specs are errors") {
    CHECK_FALSE(make_chord_drill(drill({})).ok);
    CHECK_FALSE(make_chord_drill(drill({{C, ChordQuality::None, 0}})).ok);
    CHECK_FALSE(make_chord_drill(drill({{C, ChordQuality::Major, 3}})).ok);
    CHECK_FALSE(make_chord_drill(drill({{C, ChordQuality::Major, -1}})).ok);
    ChordDrillSpec d = drill({{C, ChordQuality::Major, 0}});
    d.step_beats = 0.0;
    CHECK_FALSE(make_chord_drill(d).ok);
    d = drill({{C, ChordQuality::Major, 0}});
    d.hold_beats = 5.0; // longer than the step
    CHECK_FALSE(make_chord_drill(d).ok);
    ExerciseSettings tiny;
    tiny.range = KeyRange{60, 62};
    CHECK_FALSE(make_chord_drill(drill({{C, ChordQuality::Major, 0}}), tiny).ok);
}

TEST_CASE("chord drill: the song is finished and survives a .mid file") {
    const ExerciseResult r =
        make_chord_drill(drill(inversion_drill(F, ChordQuality::Major), HandsChoice::Together));
    REQUIRE(r.ok);
    const MidiWriteResult w = write_midi(r.song);
    REQUIRE(w.ok);
    MidiLoadOptions opt;
    opt.keep_ghost_notes = true;
    const MidiLoadResult loaded = load_midi(w.bytes, opt);
    REQUIRE(loaded.ok);
    test::expect_same_song(r.song, loaded.song);
}
