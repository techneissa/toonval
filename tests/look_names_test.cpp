// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/look.hpp"

#include <doctest/doctest.h>

using namespace toonval;

namespace {

LookSettings names(NameMode mode, bool flats = false, bool octave = false) {
    LookSettings s;
    s.name_mode = mode;
    s.names_prefer_flats = flats;
    s.names_show_octave = octave;
    return s;
}

} // namespace

TEST_CASE("letters with sharps") {
    const char* expected[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    const LookSettings s = names(NameMode::Letters);
    for (int pc = 0; pc < 12; ++pc) {
        CHECK(note_label(60 + pc, s) == expected[pc]);
        CHECK(note_label(24 + pc, s) == expected[pc]);
    }
}

TEST_CASE("letters with flats") {
    const char* expected[12] = {"C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"};
    const LookSettings s = names(NameMode::Letters, true);
    for (int pc = 0; pc < 12; ++pc) {
        CHECK(note_label(60 + pc, s) == expected[pc]);
    }
}

TEST_CASE("fixed-do solfege with sharps") {
    const char* expected[12] = {"Do",  "Do#", "Re",   "Re#", "Mi",  "Fa",
                                "Fa#", "Sol", "Sol#", "La",  "La#", "Si"};
    const LookSettings s = names(NameMode::Solfege);
    for (int pc = 0; pc < 12; ++pc) {
        CHECK(note_label(60 + pc, s) == expected[pc]);
    }
}

TEST_CASE("fixed-do solfege with flats") {
    const char* expected[12] = {"Do",   "Reb", "Re",  "Mib", "Mi",  "Fa",
                                "Solb", "Sol", "Lab", "La",  "Sib", "Si"};
    const LookSettings s = names(NameMode::Solfege, true);
    for (int pc = 0; pc < 12; ++pc) {
        CHECK(note_label(60 + pc, s) == expected[pc]);
    }
}

TEST_CASE("the octave is appended in scientific pitch notation") {
    CHECK(note_label(60, names(NameMode::Letters, false, true)) == "C4");
    CHECK(note_label(61, names(NameMode::Letters, false, true)) == "C#4");
    CHECK(note_label(61, names(NameMode::Letters, true, true)) == "Db4");
    CHECK(note_label(69, names(NameMode::Letters, false, true)) == "A4");
    CHECK(note_label(59, names(NameMode::Letters, false, true)) == "B3");
    CHECK(note_label(21, names(NameMode::Letters, false, true)) == "A0");
    CHECK(note_label(0, names(NameMode::Letters, false, true)) == "C-1");
    CHECK(note_label(127, names(NameMode::Letters, false, true)) == "G9");
    CHECK(note_label(60, names(NameMode::Solfege, false, true)) == "Do4");
    CHECK(note_label(70, names(NameMode::Solfege, true, true)) == "Sib4");
}

TEST_CASE("every pitch class with the octave, in both modes") {
    const char* letters[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    const char* solfege[12] = {"Do",  "Do#", "Re",   "Re#", "Mi",  "Fa",
                               "Fa#", "Sol", "Sol#", "La",  "La#", "Si"};
    for (int pc = 0; pc < 12; ++pc) {
        CHECK(note_label(48 + pc, names(NameMode::Letters, false, true)) ==
              std::string(letters[pc]) + "3");
        CHECK(note_label(48 + pc, names(NameMode::Solfege, false, true)) ==
              std::string(solfege[pc]) + "3");
    }
}

TEST_CASE("names off or an invalid note give an empty label") {
    CHECK(note_label(60, names(NameMode::Off)).empty());
    CHECK(note_label(60, names(NameMode::Off, true, true)).empty());
    CHECK(note_label(-1, names(NameMode::Letters)).empty());
    CHECK(note_label(128, names(NameMode::Solfege)).empty());
}

TEST_CASE("the label does not depend on where names are drawn") {
    LookSettings s = names(NameMode::Letters);
    s.names_on_blocks = false;
    s.names_on_keys = false;
    CHECK(note_label(60, s) == "C"); // the renderer reads the where-flags, the text is the same
}

TEST_CASE("names are switched separately for blocks and keys") {
    LookSettings s = names(NameMode::Letters);
    const bool combos[4][2] = {{false, false}, {true, false}, {false, true}, {true, true}};
    for (const auto& c : combos) {
        s.names_on_blocks = c[0];
        s.names_on_keys = c[1];
        CHECK(s.shows_block_names() == c[0]);
        CHECK(s.shows_key_names() == c[1]);
    }
}

TEST_CASE("names off hides them everywhere, whatever the where-switches say") {
    LookSettings s = names(NameMode::Off);
    s.names_on_blocks = true;
    s.names_on_keys = true;
    CHECK_FALSE(s.shows_block_names());
    CHECK_FALSE(s.shows_key_names());
}

TEST_CASE("the short label is the letter or syllable alone") {
    for (bool flats : {false, true}) {
        for (bool octave : {false, true}) {
            const LookSettings letters = names(NameMode::Letters, flats, octave);
            const LookSettings solfege = names(NameMode::Solfege, flats, octave);
            const char* expected_letters[12] = {"C", "C", "D", "D", "E", "F",
                                                "F", "G", "G", "A", "A", "B"};
            const char* expected_solfege[12] = {"Do", "Do",  "Re",  "Re", "Mi", "Fa",
                                                "Fa", "Sol", "Sol", "La", "La", "Si"};
            const char* flat_solfege[12] = {"Do",  "Re",  "Re", "Mi", "Mi", "Fa",
                                            "Sol", "Sol", "La", "La", "Si", "Si"};
            const char* flat_letters[12] = {"C", "D", "D", "E", "E", "F",
                                            "G", "G", "A", "A", "B", "B"};
            for (int pc = 0; pc < 12; ++pc) {
                CHECK(note_short_label(60 + pc, letters) ==
                      (flats ? flat_letters[pc] : expected_letters[pc]));
                CHECK(note_short_label(24 + pc, solfege) ==
                      (flats ? flat_solfege[pc] : expected_solfege[pc]));
            }
        }
    }
    CHECK(note_short_label(61, names(NameMode::Letters, false, true)) ==
          "C"); // no accidental, no octave
    CHECK(note_short_label(0, names(NameMode::Letters, false, true)) == "C"); // C-1
}

TEST_CASE("the short label is empty with names off or outside 0..127") {
    CHECK(note_short_label(60, names(NameMode::Off, false, false)).empty());
    CHECK(note_short_label(-1, names(NameMode::Letters, false, false)).empty());
    CHECK(note_short_label(128, names(NameMode::Letters, false, false)).empty());
}
