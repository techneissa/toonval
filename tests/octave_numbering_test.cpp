// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/look.hpp"
#include "toonval/pitch.hpp"

#include <string>

#include <doctest/doctest.h>

using namespace toonval;

namespace {

LookSettings with(OctaveNumbering numbering, NameMode mode = NameMode::Letters) {
    LookSettings s;
    s.octave_numbering = numbering;
    s.name_mode = mode;
    s.names_show_octave = true;
    return s;
}

} // namespace

TEST_CASE("the offset follows the numbering") {
    CHECK(with(OctaveNumbering::Scientific).octave_offset() == 0);
    CHECK(with(OctaveNumbering::Yamaha).octave_offset() == -1);
}

TEST_CASE("middle C is C4 in scientific numbering and C3 in Yamaha numbering") {
    CHECK(note_label(60, with(OctaveNumbering::Scientific)) == "C4");
    CHECK(note_label(60, with(OctaveNumbering::Yamaha)) == "C3");
    CHECK(c_marker_label(60, with(OctaveNumbering::Scientific)) == "C4");
    CHECK(c_marker_label(60, with(OctaveNumbering::Yamaha)) == "C3");
}

TEST_CASE("the E383's lowest and highest keys: 36 is C2 or C1, 96 is C7 or C6") {
    CHECK(c_marker_label(36, with(OctaveNumbering::Scientific)) == "C2");
    CHECK(c_marker_label(36, with(OctaveNumbering::Yamaha)) == "C1");
    CHECK(c_marker_label(96, with(OctaveNumbering::Scientific)) == "C7");
    CHECK(c_marker_label(96, with(OctaveNumbering::Yamaha)) == "C6");
    CHECK(note_label(96, with(OctaveNumbering::Yamaha)) == "C6");
}

TEST_CASE("notes 0 to 11 are octave -1 in scientific numbering and -2 in Yamaha numbering") {
    for (int note = 0; note < 12; ++note) {
        CHECK(octave_number(note, with(OctaveNumbering::Scientific)) == -1);
        CHECK(octave_number(note, with(OctaveNumbering::Yamaha)) == -2);
    }
    CHECK(note_label(1, with(OctaveNumbering::Yamaha)) == "C#-2");
    CHECK(c_marker_label(0, with(OctaveNumbering::Scientific)) == "C-1");
    CHECK(c_marker_label(0, with(OctaveNumbering::Yamaha)) == "C-2");
    CHECK(note_label(11, with(OctaveNumbering::Yamaha)) == "B-2");
}

TEST_CASE("octave_number is the scientific octave plus the offset for every note") {
    for (OctaveNumbering n : {OctaveNumbering::Scientific, OctaveNumbering::Yamaha}) {
        const LookSettings s = with(n);
        for (int note = 0; note <= 127; ++note) {
            CHECK(octave_number(note, s) == octave(note) + s.octave_offset());
        }
    }
}

TEST_CASE("note_name, used in status text, takes the same offset") {
    CHECK(note_name(60) == "C4");
    CHECK(note_name(60, false, -1) == "C3");
    CHECK(note_name(70, true, -1) == "Bb3");
    CHECK(note_name(128, false, -1).empty());
}

TEST_CASE("C markers: only on Cs, named by the mode, independent of the name switches") {
    LookSettings s;
    s.name_mode = NameMode::Off; // markers do not depend on the name mode being on
    s.names_show_octave = false; // nor on the octave switch: a marker always has its number
    s.names_on_keys = false;
    for (int note = 0; note <= 127; ++note) {
        if (note % 12 == 0) {
            CHECK(c_marker_label(note, s) == "C" + std::to_string(octave(note)));
        } else {
            CHECK(c_marker_label(note, s).empty());
        }
    }
    s.name_mode = NameMode::Solfege;
    CHECK(c_marker_label(60, s) == "Do4");
    CHECK(c_marker_label(48, with(OctaveNumbering::Yamaha, NameMode::Solfege)) == "Do2");
    CHECK(c_marker_label(61, s).empty());
}

TEST_CASE("C markers can be switched off, and notes outside 0..127 have none") {
    LookSettings s;
    CHECK(s.c_markers); // on by default
    s.c_markers = false;
    CHECK(c_marker_label(60, s).empty());
    s.c_markers = true;
    CHECK(c_marker_label(-12, s).empty());
    CHECK(c_marker_label(132, s).empty());
}

TEST_CASE("middle C is key 60 whatever the numbering") {
    CHECK(is_middle_c(60));
    CHECK_FALSE(is_middle_c(72));
    CHECK_FALSE(is_middle_c(48));
}

TEST_CASE("every preset keeps C markers on and scientific numbering") {
    for (PresetId id : kAllPresets) {
        CHECK(preset(id).c_markers);
        CHECK(preset(id).octave_numbering == OctaveNumbering::Scientific);
    }
}
