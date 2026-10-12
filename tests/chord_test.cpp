// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/chord.hpp"

#include <doctest/doctest.h>

#include <initializer_list>
#include <string>
#include <vector>

using namespace toonval;

namespace {

Chord detect(std::initializer_list<int> notes) {
    const std::vector<int> v(notes);
    return detect_chord(v.data(), v.size());
}

std::string name_of(std::initializer_list<int> notes, bool prefer_flats = false) {
    return chord_name(detect(notes), prefer_flats);
}

} // namespace

TEST_CASE("triads and suspended chords in root position") {
    CHECK(name_of({60, 64, 67}) == "C");
    CHECK(name_of({60, 63, 67}) == "Cm");
    CHECK(name_of({60, 63, 66}) == "Cdim");
    CHECK(name_of({60, 64, 68}) == "Caug");
    CHECK(name_of({60, 62, 67}) == "Csus2");
    CHECK(name_of({60, 65, 67}) == "Csus4");
}

TEST_CASE("detect_chord reports root, quality and bass") {
    const Chord c = detect({62, 66, 69}); // D F# A
    CHECK(c.found);
    CHECK(c.root_pc == 2);
    CHECK(c.quality == ChordQuality::Major);
    CHECK(c.bass_pc == 2);
}

TEST_CASE("seventh chords in root position") {
    CHECK(name_of({60, 64, 67, 70}) == "C7");
    CHECK(name_of({60, 64, 67, 71}) == "Cmaj7");
    CHECK(name_of({60, 63, 67, 70}) == "Cm7");
    CHECK(name_of({60, 63, 66, 70}) == "Cm7b5");
    CHECK(name_of({60, 63, 66, 69}) == "Cdim7");
    CHECK(name_of({60, 63, 67, 71}) == "CmMaj7");
}

TEST_CASE("sixth chords and add9 in root position") {
    CHECK(name_of({60, 64, 67, 69}) == "C6");
    CHECK(name_of({60, 63, 67, 69}) == "Cm6");
    CHECK(name_of({60, 64, 67, 74}) == "Cadd9");
}

TEST_CASE("ninth chords in root position") {
    CHECK(name_of({60, 64, 67, 70, 74}) == "C9");
    CHECK(name_of({60, 64, 67, 71, 74}) == "Cmaj9");
    CHECK(name_of({60, 63, 67, 70, 74}) == "Cm9");
}

TEST_CASE("a power chord is a root and its fifth") {
    CHECK(name_of({60, 67}) == "C5");
    CHECK(name_of({48, 55, 60}) == "C5");
    CHECK(detect({60, 67}).quality == ChordQuality::Power);
}

TEST_CASE("common omissions stay unambiguous") {
    CHECK(name_of({60, 64, 70}) == "C7");     // no fifth
    CHECK(name_of({60, 64, 70, 74}) == "C9"); // 9th chords without the fifth
    CHECK(name_of({60, 64, 71, 74}) == "Cmaj9");
    CHECK(name_of({60, 63, 70, 74}) == "Cm9");
}

TEST_CASE("triad inversions get a slash bass") {
    CHECK(name_of({64, 67, 72}) == "C/E"); // first inversion
    CHECK(name_of({55, 60, 64}) == "C/G"); // second inversion
    CHECK(name_of({59, 62, 67}) == "G/B");
    CHECK(name_of({63, 67, 72}) == "Cm/D#");
    CHECK(name_of({63, 67, 72}, true) == "Cm/Eb");
    CHECK(name_of({67, 72, 75}, true) == "Cm/G");
}

TEST_CASE("seventh chord inversions get a slash bass") {
    CHECK(name_of({64, 67, 70, 72}) == "C7/E");
    CHECK(name_of({67, 70, 72, 76}) == "C7/G");
    CHECK(name_of({58, 64, 67, 72}, true) == "C7/Bb");
    CHECK(name_of({64, 67, 71, 72}) == "Cmaj7/E");
    CHECK(name_of({67, 70, 72, 75}, true) == "Cm7/G");
    CHECK(name_of({70, 72, 75, 79}, true) == "Cm7/Bb");
}

TEST_CASE("the bass root wins over other readings of the same notes") {
    CHECK(name_of({60, 64, 67, 69}) == "C6");        // C E G A over C
    CHECK(name_of({57, 60, 64, 67}) == "Am7");       // same pitch classes over A
    CHECK(name_of({63, 67, 70, 72}, true) == "Eb6"); // Eb G Bb C over Eb
    CHECK(name_of({60, 63, 67, 70}) == "Cm7");       // same pitch classes over C
}

TEST_CASE("an inversion bass that is not a root resolves to the same name every time") {
    CHECK(name_of({64, 67, 69, 72}) == "Am7/E");
    CHECK(name_of({67, 69, 72, 76}) == "Am7/G");
    CHECK(name_of({62, 67, 72}) == "Csus2/D");
    CHECK(name_of({67, 72, 74}) == "Gsus4");
}

TEST_CASE("symmetric chords take the bass as root") {
    CHECK(name_of({60, 64, 68}) == "Caug");
    CHECK(name_of({64, 68, 72}) == "Eaug");
    CHECK(name_of({68, 72, 76}) == "G#aug");
    CHECK(name_of({68, 72, 76}, true) == "Abaug");
    CHECK(name_of({60, 63, 66, 69}) == "Cdim7");
    CHECK(name_of({63, 66, 69, 72}, true) == "Ebdim7");
    CHECK(name_of({66, 69, 72, 75}) == "F#dim7");
}

TEST_CASE("octave doublings, duplicates and any order do not change the name") {
    CHECK(name_of({60, 64, 67, 72, 76, 79}) == "C");
    CHECK(name_of({67, 72, 64, 79, 67}) == "C/E");
    CHECK(name_of({76, 60, 67, 64, 48}) == "C");
    CHECK(name_of({72, 64, 55, 67}) == "C/G");
    CHECK(name_of({70, 60, 64, 67, 60}) == "C7");
}

TEST_CASE("black-key roots are spelled with sharps or flats on request") {
    CHECK(name_of({70, 74, 77}) == "A#");
    CHECK(name_of({70, 74, 77}, true) == "Bb");
    CHECK(name_of({70, 74, 77, 80}) == "A#7");
    CHECK(name_of({70, 74, 77, 80}, true) == "Bb7");
    CHECK(name_of({61, 64, 68}) == "C#m");
    CHECK(name_of({61, 64, 68}, true) == "Dbm");
    CHECK(name_of({66, 70, 73}, true) == "Gb");
    CHECK(name_of({66, 70, 73}) == "F#");
}

TEST_CASE("an unclear set of notes is not named") {
    CHECK_FALSE(detect({}).found);
    CHECK_FALSE(detect({60}).found);
    CHECK_FALSE(detect({60, 72}).found); // octave of one note
    CHECK_FALSE(detect({60, 60, 60}).found);
    CHECK_FALSE(detect({60, 64}).found); // two notes, not a power chord
    CHECK_FALSE(detect({60, 63}).found);
    CHECK_FALSE(detect({60, 61, 62}).found); // cluster
    CHECK_FALSE(detect({60, 61, 62, 63, 64}).found);
    CHECK_FALSE(detect({60, 62, 64}).found);         // C D E: no template
    CHECK_FALSE(detect({60, 64, 67, 70, 73}).found); // C7b9: unknown extension
    CHECK_FALSE(detect({60, 64, 67, 62, 65}).found);
    CHECK(name_of({60}).empty());
    CHECK(name_of({60, 61, 62}).empty());
}

TEST_CASE("a power chord needs its root in the bass") {
    CHECK(name_of({55, 60}).empty()); // G under C is a fourth, not C5
    CHECK(name_of({67, 72}).empty());
    CHECK(name_of({60, 67, 72}) == "C5");
    CHECK(name_of({61, 68}) == "C#5");
    CHECK(name_of({61, 68}, true) == "Db5");
}

TEST_CASE("invalid input is never named") {
    CHECK_FALSE(detect_chord(nullptr, 3).found);
    CHECK_FALSE(detect_chord(nullptr, 0).found);
    CHECK_FALSE(detect({60, 64, 67, 128}).found);
    CHECK_FALSE(detect({-1, 64, 67}).found);
    CHECK(chord_name(Chord{}).empty());
}

TEST_CASE("the span overload matches the pointer overload") {
    const std::vector<int> notes{60, 64, 67};
    CHECK(detect_chord(std::span<const int>(notes)).quality == ChordQuality::Major);
}
