// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/scales.hpp"

#include "toonval/pitch.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace toonval;

namespace {

std::vector<int> v(std::initializer_list<int> xs) { return xs; }

std::vector<int> intervals(ScaleType t) {
    const auto s = scale_intervals(t);
    return {s.begin(), s.end()};
}

std::vector<int> sorted_pcs(int root, ScaleType t) {
    std::vector<int> pcs = scale_pitch_classes(root, t);
    std::sort(pcs.begin(), pcs.end());
    return pcs;
}

std::string join(const std::vector<std::string>& names) {
    std::string out;
    for (const std::string& n : names) {
        out += out.empty() ? "" : " ";
        out += n;
    }
    return out;
}

constexpr int C = 0, Db = 1, D = 2, Eb = 3, E = 4, F = 5, Gb = 6, G = 7, Ab = 8, A = 9, Bb = 10,
              B = 11;

} // namespace

TEST_CASE("scales: semitone steps of every scale type, from a worked example") {
    CHECK(intervals(ScaleType::Major) == v({0, 2, 4, 5, 7, 9, 11}));
    CHECK(intervals(ScaleType::NaturalMinor) == v({0, 2, 3, 5, 7, 8, 10}));
    CHECK(intervals(ScaleType::HarmonicMinor) == v({0, 2, 3, 5, 7, 8, 11}));
    CHECK(intervals(ScaleType::MelodicMinor) == v({0, 2, 3, 5, 7, 9, 11}));
    CHECK(intervals(ScaleType::Dorian) == v({0, 2, 3, 5, 7, 9, 10}));
    CHECK(intervals(ScaleType::Phrygian) == v({0, 1, 3, 5, 7, 8, 10}));
    CHECK(intervals(ScaleType::Lydian) == v({0, 2, 4, 6, 7, 9, 11}));
    CHECK(intervals(ScaleType::Mixolydian) == v({0, 2, 4, 5, 7, 9, 10}));
    CHECK(intervals(ScaleType::Locrian) == v({0, 1, 3, 5, 6, 8, 10}));
    CHECK(intervals(ScaleType::MajorPentatonic) == v({0, 2, 4, 7, 9}));
    CHECK(intervals(ScaleType::MinorPentatonic) == v({0, 3, 5, 7, 10}));
    CHECK(intervals(ScaleType::Blues) == v({0, 3, 5, 6, 7, 10}));
    CHECK(intervals(ScaleType::WholeTone) == v({0, 2, 4, 6, 8, 10}));
    CHECK(intervals(ScaleType::Chromatic) == v({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}));
}

TEST_CASE("scales: Ionian and Aeolian are the major and natural minor scales") {
    CHECK(ScaleType::Ionian == ScaleType::Major);
    CHECK(ScaleType::Aeolian == ScaleType::NaturalMinor);
}

TEST_CASE("scales: the melodic minor comes down as the natural minor") {
    const auto down = scale_intervals_descending(ScaleType::MelodicMinor);
    CHECK(std::vector<int>(down.begin(), down.end()) == intervals(ScaleType::NaturalMinor));
    for (ScaleType t : kAllScaleTypes) {
        if (t != ScaleType::MelodicMinor) {
            const auto d = scale_intervals_descending(t);
            CHECK(std::vector<int>(d.begin(), d.end()) == intervals(t));
        }
    }
}

TEST_CASE("scales: names and the full list") {
    CHECK(std::string(scale_type_name(ScaleType::Major)) == "Major");
    CHECK(std::string(scale_type_name(ScaleType::HarmonicMinor)) == "Harmonic minor");
    CHECK(std::string(scale_type_name(ScaleType::WholeTone)) == "Whole tone");
    CHECK(kAllScaleTypes.size() == 14);
    for (ScaleType t : kAllScaleTypes) {
        CHECK(std::string(scale_type_name(t)) != "");
    }
}

TEST_CASE("scales: pitch classes in scale order, for every type and all 12 roots") {
    for (ScaleType t : kAllScaleTypes) {
        for (int root = 0; root < 12; ++root) {
            CAPTURE(scale_type_name(t));
            CAPTURE(root);
            const std::vector<int> pcs = scale_pitch_classes(root, t);
            REQUIRE(pcs.size() == intervals(t).size());
            CHECK(pcs.front() == root);
            for (std::size_t i = 0; i < pcs.size(); ++i) {
                CHECK(pcs[i] == (root + intervals(t)[i]) % 12);
            }
        }
    }
}

TEST_CASE("scales: C major and A natural minor are the white keys") {
    CHECK(scale_pitch_classes(C, ScaleType::Major) == v({0, 2, 4, 5, 7, 9, 11}));
    CHECK(scale_pitch_classes(A, ScaleType::NaturalMinor) == v({9, 11, 0, 2, 4, 5, 7}));
    CHECK(scale_pitch_classes(Bb, ScaleType::Major) == v({10, 0, 2, 3, 5, 7, 9}));
}

TEST_CASE("scales: a root outside 0..11 wraps to its pitch class") {
    CHECK(scale_pitch_classes(14, ScaleType::Major) == scale_pitch_classes(D, ScaleType::Major));
    CHECK(scale_pitch_classes(-1, ScaleType::Major) == scale_pitch_classes(B, ScaleType::Major));
}

TEST_CASE("scales: every mode is a major scale started on another degree") {
    // D dorian has the notes of C major, E phrygian those of C major, and so on.
    struct Mode {
        ScaleType type;
        int degree_semitones; // distance up from the parent major root to the mode's root
    };
    const Mode modes[] = {{ScaleType::Dorian, 2},       {ScaleType::Phrygian, 4},
                          {ScaleType::Lydian, 5},       {ScaleType::Mixolydian, 7},
                          {ScaleType::NaturalMinor, 9}, {ScaleType::Locrian, 11}};
    for (const Mode& m : modes) {
        for (int parent = 0; parent < 12; ++parent) {
            CAPTURE(scale_type_name(m.type));
            CAPTURE(parent);
            const int root = (parent + m.degree_semitones) % 12;
            CHECK(sorted_pcs(root, m.type) == sorted_pcs(parent, ScaleType::Major));
        }
    }
}

TEST_CASE("scales: pentatonic and blues scales sit inside the scales they come from") {
    for (int root = 0; root < 12; ++root) {
        CAPTURE(root);
        // The major pentatonic is the minor pentatonic of the relative minor, three semitones down.
        CHECK(sorted_pcs(root, ScaleType::MajorPentatonic) ==
              sorted_pcs((root + 9) % 12, ScaleType::MinorPentatonic));
        // The major pentatonic leaves out the 4th and 7th of the major scale.
        for (int pc : scale_pitch_classes(root, ScaleType::MajorPentatonic)) {
            CHECK(is_in_scale(60 + pc, root, ScaleType::Major));
        }
        for (int pc : scale_pitch_classes(root, ScaleType::MinorPentatonic)) {
            CHECK(is_in_scale(60 + pc, root, ScaleType::NaturalMinor));
        }
        // The blues scale is the minor pentatonic plus the flat fifth.
        std::vector<int> blues = scale_pitch_classes(root, ScaleType::Blues);
        std::vector<int> pent = scale_pitch_classes(root, ScaleType::MinorPentatonic);
        pent.push_back((root + 6) % 12);
        std::sort(blues.begin(), blues.end());
        std::sort(pent.begin(), pent.end());
        CHECK(blues == pent);
        // The whole-tone scale has no semitone steps: 6 notes, each 2 apart.
        CHECK(sorted_pcs(root, ScaleType::WholeTone).size() == 6);
    }
}

TEST_CASE("scales: is_in_scale and is_root tint the right keys across the keyboard") {
    // A major: A B C# D E F# G#.
    const std::vector<int> a_major = {9, 11, 1, 2, 4, 6, 8};
    for (int note = 0; note <= 127; ++note) {
        const bool expected = std::find(a_major.begin(), a_major.end(), note % 12) != a_major.end();
        CAPTURE(note);
        CHECK(is_in_scale(note, A, ScaleType::Major) == expected);
        CHECK(is_root(note, A) == (note % 12 == 9));
    }
    CHECK(is_in_scale(61, A, ScaleType::Major));       // C#4
    CHECK_FALSE(is_in_scale(60, A, ScaleType::Major)); // C4
    CHECK(is_root(57, A));                             // A3
    CHECK(is_root(81, A));                             // A5
    CHECK_FALSE(is_root(58, A));
}

TEST_CASE("scales: notes outside MIDI are never in a scale or a root") {
    CHECK_FALSE(is_in_scale(-1, C, ScaleType::Chromatic));
    CHECK_FALSE(is_in_scale(128, C, ScaleType::Chromatic));
    CHECK_FALSE(is_root(-12, C));
    CHECK_FALSE(is_root(132, C));
}

TEST_CASE("scales: the pitch class mask has one bit per scale note") {
    CHECK(scale_pitch_class_mask(C, ScaleType::Major) == 0b101010110101u);
    CHECK(scale_pitch_class_mask(C, ScaleType::Chromatic) == 0xFFFu);
    for (ScaleType t : kAllScaleTypes) {
        for (int root = 0; root < 12; ++root) {
            unsigned want = 0;
            for (int pc : scale_pitch_classes(root, t)) {
                want |= 1u << pc;
            }
            CHECK(scale_pitch_class_mask(root, t) == want);
        }
    }
}

// ----- spelling -----

TEST_CASE("scales: a tonic is a letter and an accidental") {
    CHECK(Tonic{'C', 0}.pitch_class() == 0);
    CHECK(Tonic{'F', 1}.pitch_class() == 6);
    CHECK(Tonic{'G', -1}.pitch_class() == 6);
    CHECK(Tonic{'B', 1}.pitch_class() == 0);
    CHECK(Tonic{'C', -1}.pitch_class() == 11);
    CHECK(Tonic{'E', 1}.pitch_class() == 5);
}

TEST_CASE("scales: every major key is spelled with each letter once") {
    struct Key {
        Tonic tonic;
        const char* names;
    };
    const Key keys[] = {
        {{'C', 0}, "C D E F G A B"},         {{'G', 0}, "G A B C D E F#"},
        {{'D', 0}, "D E F# G A B C#"},       {{'A', 0}, "A B C# D E F# G#"},
        {{'E', 0}, "E F# G# A B C# D#"},     {{'B', 0}, "B C# D# E F# G# A#"},
        {{'F', 1}, "F# G# A# B C# D# E#"},   {{'C', 1}, "C# D# E# F# G# A# B#"},
        {{'F', 0}, "F G A Bb C D E"},        {{'B', -1}, "Bb C D Eb F G A"},
        {{'E', -1}, "Eb F G Ab Bb C D"},     {{'A', -1}, "Ab Bb C Db Eb F G"},
        {{'D', -1}, "Db Eb F Gb Ab Bb C"},   {{'G', -1}, "Gb Ab Bb Cb Db Eb F"},
        {{'C', -1}, "Cb Db Eb Fb Gb Ab Bb"},
    };
    for (const Key& k : keys) {
        CAPTURE(k.names);
        CHECK(join(scale_note_names(k.tonic, ScaleType::Major)) == k.names);
    }
}

TEST_CASE("scales: F major has Bb, never A#, and F# major has E#, never F") {
    const auto f = scale_note_names(Tonic{'F', 0}, ScaleType::Major);
    CHECK(std::find(f.begin(), f.end(), "Bb") != f.end());
    CHECK(std::find(f.begin(), f.end(), "A#") == f.end());
    const auto fs = scale_note_names(Tonic{'F', 1}, ScaleType::Major);
    CHECK(std::find(fs.begin(), fs.end(), "E#") != fs.end());
    CHECK(std::find(fs.begin(), fs.end(), "F") == fs.end());
}

TEST_CASE("scales: harmonic minor keys raise the seventh by a sharp or natural") {
    struct Key {
        Tonic tonic;
        const char* names;
    };
    const Key keys[] = {
        {{'A', 0}, "A B C D E F G#"},       {{'E', 0}, "E F# G A B C D#"},
        {{'B', 0}, "B C# D E F# G A#"},     {{'F', 1}, "F# G# A B C# D E#"},
        {{'C', 1}, "C# D# E F# G# A B#"},   {{'G', 1}, "G# A# B C# D# E F##"},
        {{'D', 1}, "D# E# F# G# A# B C##"}, {{'D', 0}, "D E F G A Bb C#"},
        {{'G', 0}, "G A Bb C D Eb F#"},     {{'C', 0}, "C D Eb F G Ab B"},
        {{'F', 0}, "F G Ab Bb C Db E"},     {{'B', -1}, "Bb C Db Eb F Gb A"},
        {{'E', -1}, "Eb F Gb Ab Bb Cb D"},  {{'A', -1}, "Ab Bb Cb Db Eb Fb G"},
    };
    for (const Key& k : keys) {
        CAPTURE(k.names);
        CHECK(join(scale_note_names(k.tonic, ScaleType::HarmonicMinor)) == k.names);
    }
}

TEST_CASE("scales: natural and melodic minor and the modes follow the same lettering") {
    CHECK(join(scale_note_names(Tonic{'C', 0}, ScaleType::NaturalMinor)) == "C D Eb F G Ab Bb");
    CHECK(join(scale_note_names(Tonic{'C', 0}, ScaleType::MelodicMinor)) == "C D Eb F G A B");
    CHECK(join(scale_note_names(Tonic{'D', 0}, ScaleType::Dorian)) == "D E F G A B C");
    CHECK(join(scale_note_names(Tonic{'E', 0}, ScaleType::Phrygian)) == "E F G A B C D");
    CHECK(join(scale_note_names(Tonic{'F', 0}, ScaleType::Lydian)) == "F G A B C D E");
    CHECK(join(scale_note_names(Tonic{'G', 0}, ScaleType::Mixolydian)) == "G A B C D E F");
    CHECK(join(scale_note_names(Tonic{'B', 0}, ScaleType::Locrian)) == "B C D E F G A");
    CHECK(join(scale_note_names(Tonic{'F', 0}, ScaleType::Mixolydian)) == "F G A Bb C D Eb");
    CHECK(join(scale_note_names(Tonic{'F', 1}, ScaleType::Lydian)) == "F# G# A# B# C# D# E#");
}

TEST_CASE("scales: pentatonic, blues, whole tone and chromatic names") {
    CHECK(join(scale_note_names(Tonic{'C', 0}, ScaleType::MajorPentatonic)) == "C D E G A");
    CHECK(join(scale_note_names(Tonic{'C', 0}, ScaleType::MinorPentatonic)) == "C Eb F G Bb");
    CHECK(join(scale_note_names(Tonic{'A', 0}, ScaleType::MinorPentatonic)) == "A C D E G");
    CHECK(join(scale_note_names(Tonic{'F', 1}, ScaleType::MajorPentatonic)) == "F# G# A# C# D#");
    CHECK(join(scale_note_names(Tonic{'C', 0}, ScaleType::Blues)) == "C Eb F Gb G Bb");
    CHECK(join(scale_note_names(Tonic{'E', 0}, ScaleType::Blues)) == "E G A Bb B D");
    CHECK(join(scale_note_names(Tonic{'C', 0}, ScaleType::WholeTone)) == "C D E F# G# Bb");
    CHECK(join(scale_note_names(Tonic{'C', 0}, ScaleType::Chromatic)) ==
          "C C# D D# E F F# G G# A A# B");
    CHECK(join(scale_note_names(Tonic{'G', -1}, ScaleType::Chromatic)) ==
          "Gb G Ab A Bb B C Db D Eb E F");
}

TEST_CASE("scales: spelling matches the pitch classes for every type and tonic") {
    const Tonic tonics[] = {{'C', 0},  {'D', 0}, {'E', 0},  {'F', 0}, {'G', 0},  {'A', 0},
                            {'B', 0},  {'C', 1}, {'D', -1}, {'D', 1}, {'E', -1}, {'F', 1},
                            {'G', -1}, {'G', 1}, {'A', -1}, {'A', 1}, {'B', -1}};
    for (ScaleType t : kAllScaleTypes) {
        for (const Tonic& tonic : tonics) {
            CAPTURE(scale_type_name(t));
            CAPTURE(tonic.letter);
            CAPTURE(tonic.accidental);
            const std::vector<SpelledNote> spelled = spell_scale(tonic, t);
            if (spelled.empty()) {
                continue; // needs triple accidentals: allowed to refuse
            }
            const std::vector<int> pcs = scale_pitch_classes(tonic.pitch_class(), t);
            REQUIRE(spelled.size() == pcs.size());
            for (std::size_t i = 0; i < pcs.size(); ++i) {
                CHECK(spelled[i].pitch_class == pcs[i]);
                // Letter plus accidental is that pitch class.
                const int base = Tonic{spelled[i].letter, spelled[i].accidental}.pitch_class();
                CHECK(base == pcs[i]);
                CHECK(std::abs(spelled[i].accidental) <= 2);
            }
        }
    }
}

TEST_CASE("scales: seven-note scales use every letter once") {
    for (ScaleType t : {ScaleType::Major, ScaleType::NaturalMinor, ScaleType::HarmonicMinor,
                        ScaleType::MelodicMinor, ScaleType::Dorian, ScaleType::Phrygian,
                        ScaleType::Lydian, ScaleType::Mixolydian, ScaleType::Locrian}) {
        for (int root = 0; root < 12; ++root) {
            const Tonic tonic = default_tonic(root, t);
            const auto spelled = spell_scale(tonic, t);
            REQUIRE(spelled.size() == 7);
            std::string letters;
            for (const SpelledNote& n : spelled) {
                letters += n.letter;
            }
            std::sort(letters.begin(), letters.end());
            CHECK(letters == "ABCDEFG");
        }
    }
}

TEST_CASE("scales: the default tonic is the spelling with the fewest accidentals") {
    // Major keys.
    CHECK(default_tonic(C, ScaleType::Major).letter == 'C');
    const Tonic db = default_tonic(Db, ScaleType::Major);
    CHECK((db.letter == 'D' && db.accidental == -1));
    const Tonic eb = default_tonic(Eb, ScaleType::Major);
    CHECK((eb.letter == 'E' && eb.accidental == -1));
    const Tonic ab = default_tonic(Ab, ScaleType::Major);
    CHECK((ab.letter == 'A' && ab.accidental == -1));
    const Tonic bb = default_tonic(Bb, ScaleType::Major);
    CHECK((bb.letter == 'B' && bb.accidental == -1));
    // Minor keys.
    const Tonic cs = default_tonic(Db, ScaleType::NaturalMinor);
    CHECK((cs.letter == 'C' && cs.accidental == 1));
    const Tonic fs = default_tonic(Gb, ScaleType::NaturalMinor);
    CHECK((fs.letter == 'F' && fs.accidental == 1));
    const Tonic gs = default_tonic(Ab, ScaleType::HarmonicMinor);
    CHECK((gs.letter == 'G' && gs.accidental == 1));
    const Tonic bbm = default_tonic(Bb, ScaleType::NaturalMinor);
    CHECK((bbm.letter == 'B' && bbm.accidental == -1));
    // A tie (F# major and Gb major each need six signs) goes to sharps, or to flats on request.
    const Tonic six_sharps = default_tonic(Gb, ScaleType::Major);
    CHECK((six_sharps.letter == 'F' && six_sharps.accidental == 1));
    const Tonic six_flats = default_tonic(Gb, ScaleType::Major, true);
    CHECK((six_flats.letter == 'G' && six_flats.accidental == -1));
    // A white key is always spelled as itself.
    for (int white : {C, D, E, F, G, A, B}) {
        for (ScaleType t : kAllScaleTypes) {
            const Tonic tonic = default_tonic(white, t);
            CHECK(tonic.accidental == 0);
            CHECK(tonic.pitch_class() == white);
        }
    }
}

TEST_CASE("scales: all 12 roots of every scale type spell with no more than double accidentals") {
    for (ScaleType t : kAllScaleTypes) {
        for (int root = 0; root < 12; ++root) {
            CAPTURE(scale_type_name(t));
            CAPTURE(root);
            const Tonic tonic = default_tonic(root, t);
            CHECK(tonic.pitch_class() == root);
            const auto spelled = spell_scale(tonic, t);
            REQUIRE_FALSE(spelled.empty());
            CHECK(spelled.front().pitch_class == root);
            for (const SpelledNote& n : spelled) {
                CHECK(std::abs(n.accidental) <= 2);
            }
        }
    }
}

TEST_CASE("scales: a note name shows the octave that goes with its letter") {
    // Middle C is C4 by default and C3 in Yamaha numbering.
    CHECK(spelled_name(60, SpelledNote{'C', 0, 0}) == "C4");
    CHECK(spelled_name(60, SpelledNote{'C', 0, 0}, -1) == "C3");
    CHECK(spelled_name(70, SpelledNote{'B', -1, 10}) == "Bb4");
    CHECK(spelled_name(70, SpelledNote{'B', -1, 10}, -1) == "Bb3");
    CHECK(spelled_name(66, SpelledNote{'F', 1, 6}) == "F#4");
    CHECK(spelled_name(66, SpelledNote{'G', -1, 6}) == "Gb4");
    // B# is the pitch class of C, but the octave of its letter B: B#3 is the key of middle C.
    CHECK(spelled_name(60, SpelledNote{'B', 1, 0}) == "B#3");
    // Cb4 is the key of B3.
    CHECK(spelled_name(59, SpelledNote{'C', -1, 11}) == "Cb4");
    CHECK(spelled_name(64, SpelledNote{'F', -1, 4}) == "Fb4");
    CHECK(spelled_name(65, SpelledNote{'E', 1, 5}) == "E#4");
    CHECK(spelled_name(67, SpelledNote{'F', 2, 7}) == "F##4");
    CHECK(spelled_name(0, SpelledNote{'C', 0, 0}) == "C-1");
    CHECK(spelled_name(60, SpelledNote{'D', -2, 0}) == "Dbb4");
    CHECK(spelled_name(200, SpelledNote{'C', 0, 0}) == "");
}

TEST_CASE("scales: one octave of names with octave numbers, in either numbering") {
    const auto f = scale_note_names_with_octave(Tonic{'F', 0}, ScaleType::Major, 65);
    CHECK(join(f) == "F4 G4 A4 Bb4 C5 D5 E5");
    const auto fy = scale_note_names_with_octave(Tonic{'F', 0}, ScaleType::Major, 65, -1);
    CHECK(join(fy) == "F3 G3 A3 Bb3 C4 D4 E4");
    const auto fs = scale_note_names_with_octave(Tonic{'F', 1}, ScaleType::Major, 66);
    CHECK(join(fs) == "F#4 G#4 A#4 B4 C#5 D#5 E#5");
    // Cb major from B3: Cb is the letter C, so it is octave 4, and the notes above are in octave 4.
    const auto cb = scale_note_names_with_octave(Tonic{'C', -1}, ScaleType::Major, 59);
    CHECK(join(cb) == "Cb4 Db4 Eb4 Fb4 Gb4 Ab4 Bb4");
    // A root that is not the tonic's pitch class, or not a note, gives nothing.
    CHECK(scale_note_names_with_octave(Tonic{'F', 0}, ScaleType::Major, 66).empty());
    CHECK(scale_note_names_with_octave(Tonic{'F', 0}, ScaleType::Major, 200).empty());
    // Past note 127 the list stops.
    CHECK(scale_note_names_with_octave(Tonic{'G', 0}, ScaleType::Major, 127).size() == 1);
}

TEST_CASE("scales: bad tonics and root names give nothing") {
    CHECK(spell_scale(Tonic{'H', 0}, ScaleType::Major).empty());
    CHECK(spell_scale(Tonic{'C', 5}, ScaleType::Major).empty());
    CHECK(spell_scale(Tonic{'C', -3}, ScaleType::Major).empty());
    // D## major would need triple accidentals (E### is not available): refused.
    CHECK(spell_scale(Tonic{'D', 2}, ScaleType::Major).empty());
    CHECK(scale_note_names(Tonic{'H', 0}, ScaleType::Major).empty());
}
