// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/sections.hpp"

#include "section_fixtures.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <limits>
#include <string>

using namespace toonval;
using namespace toonval::testing;

namespace {

// A 16-bar song is 32 s long at 120 BPM in 4/4: bar n spans [2(n-1), 2n).
Song song16() { return bars_song(16); }

std::vector<std::string> names(const SongSections& s) {
    std::vector<std::string> out;
    for (const Section& x : s.sections) {
        out.push_back(x.name);
    }
    return out;
}

SongSections with_markers(const Song& song, std::initializer_list<double> times) {
    SongSections s;
    s.song_hash = 7;
    for (double t : times) {
        REQUIRE(s.add_marker(song, t).ok);
    }
    REQUIRE(s.valid(song));
    return s;
}

} // namespace

// ---- Names

TEST_CASE("sections: a name keeps its words, loses control characters and extra blanks") {
    CHECK(sanitize_section_name("Intro") == "Intro");
    CHECK(sanitize_section_name("  Hard   run \t\n bars 12-13  ") == "Hard run bars 12-13");
    CHECK(sanitize_section_name("a\x01"
                                "b") == "a b");
    CHECK(sanitize_section_name("a\xC2\xA0"
                                "b") == "a b"); // no-break space
    CHECK(sanitize_section_name("a\xE2\x80\xA8"
                                "b") == "a b"); // line separator
    CHECK(sanitize_section_name("") == "");
    CHECK(sanitize_section_name(" \t ") == "");
}

TEST_CASE("sections: a name keeps accents and other scripts") {
    CHECK(sanitize_section_name("Sch\xC3\xB6n") == "Sch\xC3\xB6n");
    CHECK(sanitize_section_name("\xE3\x82\xA4\xE3\x83\xB3\xE3\x83\x88\xE3\x83\xAD") ==
          "\xE3\x82\xA4\xE3\x83\xB3\xE3\x83\x88\xE3\x83\xAD");
}

TEST_CASE("sections: invalid UTF-8 bytes in a name are dropped") {
    CHECK(sanitize_section_name("ab\xFFzz") == "abzz");
    CHECK(sanitize_section_name("ab\xC3") == "ab");       // cut off
    CHECK(sanitize_section_name("\xED\xA0\x80x") == "x"); // a surrogate
    CHECK(sanitize_section_name("\xC0\xAF"
                                "x") == "x");                 // overlong slash
    CHECK(sanitize_section_name("\xF4\x90\x80\x80y") == "y"); // above U+10FFFF
}

TEST_CASE("sections: bidi overrides and zero-width characters are removed from a name") {
    CHECK(sanitize_section_name("a\xE2\x80\xAE"
                                "b") == "ab"); // right-to-left override
    CHECK(sanitize_section_name("a\xE2\x80\x8B"
                                "b") == "ab"); // zero-width space
    CHECK(sanitize_section_name("\xEF\xBB\xBF"
                                "ab") == "ab"); // byte order mark
}

TEST_CASE("sections: a name is cut to the limit at a character boundary") {
    std::string long_ascii(200, 'x');
    CHECK(sanitize_section_name(long_ascii).size() == kMaxSectionNameBytes);

    std::string accents;
    for (int i = 0; i < 100; ++i) {
        accents += "\xC3\xA9"; // e acute, 2 bytes
    }
    std::string cut = sanitize_section_name(accents);
    CHECK(cut.size() == kMaxSectionNameBytes); // 32 whole characters
    CHECK(cut.size() % 2 == 0);

    std::string three(kMaxSectionNameBytes - 1, 'a');
    three += "\xE2\x82\xAC"; // euro sign would reach 66 bytes
    CHECK(sanitize_section_name(three).size() == kMaxSectionNameBytes - 1);
}

TEST_CASE("sections: sanitizing a name twice changes nothing") {
    for (std::string_view s : {"  a  b ", "x\xFF y", "\t",
                               "\xE2\x80\xAE"
                               "x"}) {
        std::string once = sanitize_section_name(s);
        CHECK(sanitize_section_name(once) == once);
    }
}

// ---- Markers

TEST_CASE("sections: a song starts with no sections") {
    SongSections s;
    CHECK(s.empty());
    CHECK(s.markers().empty());
    CHECK(s.valid(song16()));
}

TEST_CASE("sections: the first marker splits the song into two sections") {
    Song song = song16();
    SongSections s;
    s.song_hash = 7;
    EditResult r = s.add_marker(song, 16.0);
    REQUIRE(r.ok);
    REQUIRE(s.sections.size() == 2);
    CHECK(s.sections[0].start_s == 0.0);
    CHECK(s.sections[0].end_s == 16.0);
    CHECK(s.sections[1].start_s == 16.0);
    CHECK(s.sections[1].end_s == 32.0);
    CHECK(r.id == s.sections[1].id);
    CHECK(s.sections[0].id != s.sections[1].id);
    CHECK(s.sections[0].id != kNoSection);
    CHECK(s.valid(song));
}

TEST_CASE("sections: a marker snaps to the nearest bar by default") {
    Song song = song16();
    SongSections s;
    REQUIRE(s.add_marker(song, 16.9).ok); // bar 9 starts at 16, bar 10 at 18
    CHECK(s.sections[0].end_s == 16.0);
    SongSections t;
    REQUIRE(t.add_marker(song, 17.1).ok);
    CHECK(t.sections[0].end_s == 18.0);
    CHECK(t.sections[0].snapped_to_bars);
    CHECK(t.sections[1].snapped_to_bars);
}

TEST_CASE("sections: a marker placed without snapping stays where it was put") {
    Song song = song16();
    SongSections s;
    REQUIRE(s.add_marker(song, 16.9, false).ok);
    CHECK(s.sections[0].end_s == 16.9);
    CHECK_FALSE(s.sections[0].snapped_to_bars);
    CHECK_FALSE(s.sections[1].snapped_to_bars);
    CHECK(s.sections[0].first_measure == 1);
    CHECK(s.sections[0].last_measure == 9);
    CHECK(s.sections[1].first_measure == 9);
    CHECK(s.sections[1].last_measure == 16);
    CHECK(s.valid(song));
}

TEST_CASE("sections: the first and last measure of snapped sections are exact") {
    Song song = song16();
    SongSections s = with_markers(song, {16.0, 24.0});
    REQUIRE(s.sections.size() == 3);
    CHECK(s.sections[0].first_measure == 1);
    CHECK(s.sections[0].last_measure == 8);
    CHECK(s.sections[1].first_measure == 9);
    CHECK(s.sections[1].last_measure == 12);
    CHECK(s.sections[2].first_measure == 13);
    CHECK(s.sections[2].last_measure == 16);
}

TEST_CASE("sections: markers come back as the boundaries between sections") {
    Song song = song16();
    SongSections s = with_markers(song, {24.0, 8.0, 16.0});
    CHECK(s.markers() == std::vector<double>{8.0, 16.0, 24.0});
    REQUIRE(s.sections.size() == 4);
    CHECK(s.sections[1].start_s == 8.0);
}

TEST_CASE("sections: a later marker splits the section it lands in and keeps the left id") {
    Song song = song16();
    SongSections s = with_markers(song, {16.0});
    SectionId left = s.sections[0].id;
    SectionId right = s.sections[1].id;
    EditResult r = s.add_marker(song, 20.0);
    REQUIRE(r.ok);
    REQUIRE(s.sections.size() == 3);
    CHECK(s.sections[0].id == left);
    CHECK(s.sections[1].id == right);
    CHECK(s.sections[1].end_s == 20.0);
    CHECK(s.sections[2].start_s == 20.0);
    CHECK(s.sections[2].id == r.id);
    CHECK(r.id != left);
    CHECK(r.id != right);
}

TEST_CASE("sections: a marker on an existing marker, the start or the end is refused") {
    Song song = song16();
    SongSections s = with_markers(song, {16.0});
    SongSections before = s;
    EditResult r = s.add_marker(song, 16.4); // snaps to 16
    CHECK_FALSE(r.ok);
    CHECK(r.error == EditError::AtMarker);
    CHECK(s.add_marker(song, 0.5).error == EditError::AtMarker);  // snaps to 0
    CHECK(s.add_marker(song, 31.5).error == EditError::AtMarker); // snaps to the end
    CHECK(s.add_marker(song, 99.0).error == EditError::AtMarker); // clamps to the end
    CHECK(s == before);
}

TEST_CASE("sections: a marker that would leave a piece shorter than a minimum loop is refused") {
    Song song = song16();
    SongSections s = with_markers(song, {16.0});
    SongSections before = s;
    EditResult r = s.add_marker(song, 16.05, false);
    CHECK_FALSE(r.ok);
    CHECK(r.error == EditError::TooShort);
    CHECK(s.add_marker(song, 31.95, false).error == EditError::TooShort);
    CHECK(s == before);
    CHECK(s.add_marker(song, 16.0 + kMinSectionSeconds, false).ok);
}

TEST_CASE("sections: NaN and infinite times are handled") {
    Song song = song16();
    SongSections s;
    CHECK(s.add_marker(song, std::nan("")).error == EditError::OutOfRange);
    CHECK(s.empty());
    // Infinity clamps to the song's end, which is a boundary.
    CHECK(s.add_marker(song, std::numeric_limits<double>::infinity()).error == EditError::AtMarker);
    CHECK(s.empty());
}

TEST_CASE("sections: a song too short for two sections takes no marker") {
    Song tiny;
    tiny.duration_s = 0.15;
    SongSections s;
    CHECK(s.add_marker(tiny, 0.075, false).error == EditError::TooShort);
    Song none;
    CHECK(s.add_marker(none, 0.0, false).error == EditError::AtMarker);
    CHECK(s.empty());
}

TEST_CASE("sections: a song with no bars takes unsnapped markers") {
    Song song;
    song.duration_s = 40.0;
    SongSections s;
    REQUIRE(s.add_marker(song, 12.3).ok);
    CHECK(s.sections[0].end_s == 12.3);
    CHECK_FALSE(s.sections[0].snapped_to_bars);
    CHECK(s.sections[0].name == "Section 1");
    CHECK(s.sections[1].name == "Section 2");
    CHECK(s.valid(song));
}

TEST_CASE("sections: no more than the limit of sections") {
    Song song = bars_song(100);
    SongSections s;
    for (std::size_t i = 1; i < kMaxSectionsPerSong; ++i) {
        REQUIRE(s.add_marker(song, 2.0 * static_cast<double>(i)).ok);
    }
    CHECK(s.sections.size() == kMaxSectionsPerSong);
    SongSections before = s;
    EditResult r = s.add_marker(song, 150.0);
    CHECK(r.error == EditError::TooMany);
    CHECK(s == before);
    CHECK(s.valid(song));
}

// ---- Default names

TEST_CASE("sections: snapped sections are named by their bars") {
    Song song = song16();
    SongSections s = with_markers(song, {16.0, 24.0});
    CHECK(names(s) == std::vector<std::string>{"Bars 1-8", "Bars 9-12", "Bars 13-16"});
}

TEST_CASE("sections: a one-bar section is named Bar N") {
    Song song = song16();
    SongSections s = with_markers(song, {16.0, 18.0});
    CHECK(s.sections[1].name == "Bar 9");
}

TEST_CASE("sections: unsnapped sections are named Section N by place") {
    Song song = song16();
    SongSections s;
    REQUIRE(s.add_marker(song, 10.5, false).ok);
    REQUIRE(s.add_marker(song, 20.5, false).ok);
    CHECK(names(s) == std::vector<std::string>{"Section 1", "Section 2", "Section 3"});
}

TEST_CASE("sections: default names follow the section when it changes") {
    Song song = song16();
    SongSections s = with_markers(song, {16.0});
    CHECK(names(s) == std::vector<std::string>{"Bars 1-8", "Bars 9-16"});
    REQUIRE(s.add_marker(song, 20.0).ok);
    CHECK(names(s) == std::vector<std::string>{"Bars 1-8", "Bars 9-10", "Bars 11-16"});
}

// ---- Rename

TEST_CASE("sections: renaming sets a custom name that survives later edits") {
    Song song = song16();
    SongSections s = with_markers(song, {16.0});
    SectionId id = s.sections[1].id;
    EditResult r = s.rename(id, "  Chorus ");
    REQUIRE(r.ok);
    CHECK(r.id == id);
    CHECK(s.find(id)->name == "Chorus");
    CHECK(s.find(id)->custom_name);
    REQUIRE(s.add_marker(song, 24.0).ok);
    CHECK(s.find(id)->name == "Chorus");
    CHECK(s.sections[2].name == "Bars 13-16");
}

TEST_CASE("sections: renaming cleans the text") {
    Song song = song16();
    SongSections s = with_markers(song, {16.0});
    SectionId id = s.sections[0].id;
    REQUIRE(s.rename(id, "a\xFF\tb").ok);
    CHECK(s.find(id)->name == "a b");
    REQUIRE(s.rename(id, std::string(300, 'z')).ok);
    CHECK(s.find(id)->name.size() == kMaxSectionNameBytes);
}

TEST_CASE("sections: an empty name returns to the default name") {
    Song song = song16();
    SongSections s = with_markers(song, {16.0});
    SectionId id = s.sections[1].id;
    REQUIRE(s.rename(id, "Chorus").ok);
    REQUIRE(s.rename(id, " \t").ok);
    CHECK(s.find(id)->name == "Bars 9-16");
    CHECK_FALSE(s.find(id)->custom_name);
}

TEST_CASE("sections: renaming an unknown section fails") {
    SongSections s;
    EditResult r = s.rename(5, "x");
    CHECK_FALSE(r.ok);
    CHECK(r.error == EditError::NotFound);
}

TEST_CASE("sections: a section may be named like another") {
    Song song = song16();
    SongSections s = with_markers(song, {16.0});
    REQUIRE(s.rename(s.sections[0].id, "Verse").ok);
    REQUIRE(s.rename(s.sections[1].id, "Verse").ok);
    CHECK(names(s) == std::vector<std::string>{"Verse", "Verse"});
}

// ---- Lookup

TEST_CASE("sections: at() finds the section holding a time") {
    Song song = song16();
    SongSections s = with_markers(song, {16.0});
    CHECK(s.at(0.0)->id == s.sections[0].id);
    CHECK(s.at(15.99)->id == s.sections[0].id);
    CHECK(s.at(16.0)->id == s.sections[1].id);
    CHECK(s.at(32.0)->id == s.sections[1].id);
    CHECK(s.at(33.0) == nullptr);
    CHECK(s.at(-1.0) == nullptr);
    CHECK(s.at(std::nan("")) == nullptr);
}

TEST_CASE("sections: span_of gives the loop to set") {
    Song song = song16();
    SongSections s = with_markers(song, {16.0});
    LoopSpan span = s.span_of(s.sections[1].id);
    CHECK(span.ok);
    CHECK(span.start_s == 16.0);
    CHECK(span.end_s == 32.0);
    CHECK_FALSE(s.span_of(99).ok);
}

// ---- Split

TEST_CASE("sections: split cuts one section and keeps the left id and name") {
    Song song = song16();
    SongSections s = with_markers(song, {16.0});
    SectionId left = s.sections[0].id;
    REQUIRE(s.rename(left, "Intro").ok);
    EditResult r = s.split(song, left, 8.0);
    REQUIRE(r.ok);
    REQUIRE(s.sections.size() == 3);
    CHECK(s.sections[0].id == left);
    CHECK(s.sections[0].name == "Intro");
    CHECK(s.sections[0].end_s == 8.0);
    CHECK(s.sections[1].id == r.id);
    CHECK(s.sections[1].start_s == 8.0);
    CHECK(s.sections[1].end_s == 16.0);
}

TEST_CASE("sections: split refuses a time outside the section") {
    Song song = song16();
    SongSections s = with_markers(song, {16.0});
    SongSections before = s;
    CHECK(s.split(song, s.sections[0].id, 20.0).error == EditError::OutOfRange);
    CHECK(s.split(song, s.sections[0].id, 16.0).error == EditError::AtMarker);
    CHECK(s.split(song, 99, 4.0).error == EditError::NotFound);
    CHECK(s == before);
}

// ---- Remove a marker, merge, delete

TEST_CASE("sections: removing a marker merges its neighbours into the left one") {
    Song song = song16();
    SongSections s = with_markers(song, {8.0, 16.0});
    SectionId first = s.sections[0].id;
    SectionId middle = s.sections[1].id;
    REQUIRE(s.rename(first, "Intro").ok);
    EditResult r = s.remove_marker(song, 8.5);
    REQUIRE(r.ok);
    REQUIRE(s.sections.size() == 2);
    CHECK(r.id == first);
    CHECK(s.sections[0].id == first);
    CHECK(s.sections[0].name == "Intro");
    CHECK(s.sections[0].end_s == 16.0);
    CHECK(s.find(middle) == nullptr);
    CHECK(s.valid(song));
}

TEST_CASE("sections: the marker nearest the given time is the one removed") {
    Song song = song16();
    SongSections s = with_markers(song, {8.0, 16.0, 24.0});
    REQUIRE(s.remove_marker(song, 15.0).ok);
    CHECK(s.markers() == std::vector<double>{8.0, 24.0});
}

TEST_CASE("sections: with no marker there is nothing to remove") {
    Song song = song16();
    SongSections s;
    CHECK(s.remove_marker(song, 5.0).error == EditError::NotFound);
    REQUIRE(s.add_marker(song, 16.0).ok);
    REQUIRE(s.remove_marker(song, 16.0).ok);
    CHECK(s.sections.size() == 1);
    CHECK(s.sections[0].start_s == 0.0);
    CHECK(s.sections[0].end_s == 32.0);
    CHECK(s.remove_marker(song, 5.0).error == EditError::NotFound);
}

TEST_CASE("sections: merge joins neighbours into the earliest and keeps its name") {
    Song song = song16();
    SongSections s = with_markers(song, {8.0, 16.0, 24.0});
    SectionId a = s.sections[1].id;
    SectionId b = s.sections[2].id;
    REQUIRE(s.rename(a, "Verse").ok);
    std::array<SectionId, 2> pick{b, a}; // the order of the selection does not matter
    EditResult r = s.merge(song, pick);
    REQUIRE(r.ok);
    CHECK(r.id == a);
    REQUIRE(s.sections.size() == 3);
    CHECK(s.sections[1].id == a);
    CHECK(s.sections[1].name == "Verse");
    CHECK(s.sections[1].start_s == 8.0);
    CHECK(s.sections[1].end_s == 24.0);
    CHECK(s.find(b) == nullptr);
    CHECK(s.valid(song));
}

TEST_CASE("sections: merge of three updates the default name") {
    Song song = song16();
    SongSections s = with_markers(song, {8.0, 16.0, 24.0});
    std::array<SectionId, 3> pick{s.sections[0].id, s.sections[1].id, s.sections[2].id};
    REQUIRE(s.merge(song, pick).ok);
    CHECK(names(s) == std::vector<std::string>{"Bars 1-12", "Bars 13-16"});
}

TEST_CASE("sections: merge refuses sections that are not neighbours") {
    Song song = song16();
    SongSections s = with_markers(song, {8.0, 16.0, 24.0});
    SongSections before = s;
    std::array<SectionId, 2> gap{s.sections[0].id, s.sections[2].id};
    CHECK(s.merge(song, gap).error == EditError::NotAdjacent);
    std::array<SectionId, 1> one{s.sections[0].id};
    CHECK(s.merge(song, one).error == EditError::NotEnough);
    std::array<SectionId, 2> same{s.sections[0].id, s.sections[0].id};
    CHECK(s.merge(song, same).error == EditError::NotEnough);
    std::array<SectionId, 2> unknown{s.sections[0].id, 99};
    CHECK(s.merge(song, unknown).error == EditError::NotFound);
    CHECK(s == before);
}

TEST_CASE("sections: deleting a section gives its time to the one before it") {
    Song song = song16();
    SongSections s = with_markers(song, {8.0, 16.0});
    SectionId first = s.sections[0].id;
    SectionId middle = s.sections[1].id;
    EditResult r = s.erase(song, middle);
    REQUIRE(r.ok);
    CHECK(r.id == first);
    REQUIRE(s.sections.size() == 2);
    CHECK(s.sections[0].end_s == 16.0);
    CHECK(s.find(middle) == nullptr);
    CHECK(s.valid(song));
}

TEST_CASE("sections: deleting the first section gives its time to the next one") {
    Song song = song16();
    SongSections s = with_markers(song, {8.0});
    SectionId second = s.sections[1].id;
    EditResult r = s.erase(song, s.sections[0].id);
    REQUIRE(r.ok);
    CHECK(r.id == second);
    REQUIRE(s.sections.size() == 1);
    CHECK(s.sections[0].start_s == 0.0);
    CHECK(s.sections[0].end_s == 32.0);
}

TEST_CASE("sections: deleting the only section leaves none") {
    Song song = song16();
    SongSections s = with_markers(song, {8.0});
    REQUIRE(s.erase(song, s.sections[0].id).ok);
    EditResult r = s.erase(song, s.sections[0].id);
    REQUIRE(r.ok);
    CHECK(r.id == kNoSection);
    CHECK(s.empty());
    CHECK(s.valid(song));
    CHECK(s.erase(song, 1).error == EditError::NotFound);
}

TEST_CASE("sections: ids are not reused after a delete") {
    Song song = song16();
    SongSections s = with_markers(song, {8.0});
    SectionId gone = s.sections[1].id;
    REQUIRE(s.erase(song, gone).ok);
    EditResult r = s.add_marker(song, 8.0);
    REQUIRE(r.ok);
    CHECK(r.id != gone);
    CHECK(r.id > gone);
}

// ---- Save an A-B loop as a section

TEST_CASE("sections: add_range makes a named section of the range") {
    Song song = song16();
    SongSections s;
    EditResult r = s.add_range(song, 22.0, 26.0, true, "Hard run");
    REQUIRE(r.ok);
    REQUIRE(s.sections.size() == 3);
    CHECK(s.sections[1].id == r.id);
    CHECK(s.sections[1].name == "Hard run");
    CHECK(s.sections[1].start_s == 22.0);
    CHECK(s.sections[1].end_s == 26.0);
    CHECK(s.sections[1].first_measure == 12);
    CHECK(s.sections[1].last_measure == 13);
    CHECK(s.valid(song));
}

TEST_CASE("sections: add_range snaps both ends to bars") {
    Song song = song16();
    SongSections s;
    EditResult r = s.add_range(song, 22.4, 25.7, true);
    REQUIRE(r.ok);
    CHECK(s.find(r.id)->start_s == 22.0);
    CHECK(s.find(r.id)->end_s == 26.0);
    CHECK(s.find(r.id)->name == "Bars 12-13");
}

TEST_CASE("sections: add_range accepts the ends in either order") {
    Song song = song16();
    SongSections s;
    EditResult r = s.add_range(song, 26.0, 22.0);
    REQUIRE(r.ok);
    CHECK(s.find(r.id)->start_s == 22.0);
}

TEST_CASE("sections: add_range at the start of the song needs one marker") {
    Song song = song16();
    SongSections s;
    EditResult r = s.add_range(song, 0.0, 8.0, true, "Intro");
    REQUIRE(r.ok);
    CHECK(s.sections.size() == 2);
    CHECK(s.sections[0].name == "Intro");
}

TEST_CASE("sections: add_range inside one existing section splits it twice") {
    Song song = song16();
    SongSections s = with_markers(song, {16.0});
    REQUIRE(s.add_range(song, 20.0, 24.0).ok);
    CHECK(s.markers() == std::vector<double>{16.0, 20.0, 24.0});
}

TEST_CASE("sections: add_range equal to a section just names it") {
    Song song = song16();
    SongSections s = with_markers(song, {16.0});
    EditResult r = s.add_range(song, 16.0, 32.0, true, "Chorus");
    REQUIRE(r.ok);
    CHECK(r.id == s.sections[1].id);
    CHECK(s.sections.size() == 2);
    CHECK(s.sections[1].name == "Chorus");
}

TEST_CASE("sections: add_range across a marker is refused") {
    Song song = song16();
    SongSections s = with_markers(song, {16.0});
    SongSections before = s;
    EditResult r = s.add_range(song, 12.0, 20.0);
    CHECK_FALSE(r.ok);
    CHECK(r.error == EditError::Overlaps);
    CHECK(s == before);
}

TEST_CASE("sections: add_range that is too short or past the limit is refused") {
    Song song = song16();
    SongSections s;
    CHECK(s.add_range(song, 10.0, 10.05, false).error == EditError::TooShort);
    CHECK(s.add_range(song, 10.0, 10.0).error == EditError::TooShort);
    CHECK(s.add_range(song, std::nan(""), 10.0).error == EditError::OutOfRange);
    CHECK(s.empty());
}

TEST_CASE("sections: add_range does not make a section when it would exceed the limit") {
    Song song = bars_song(100);
    SongSections s;
    for (std::size_t i = 1; i < kMaxSectionsPerSong - 1; ++i) {
        REQUIRE(s.add_marker(song, 2.0 * static_cast<double>(i)).ok);
    }
    REQUIRE(s.sections.size() == kMaxSectionsPerSong - 1);
    SongSections before = s;
    // Inside the last section: two markers, three pieces, one too many.
    CHECK(s.add_range(song, 150.0, 160.0).error == EditError::TooMany);
    CHECK(s == before);
}

// ---- Binding to a song

TEST_CASE("sections: fit_to_song closes gaps, trims the end and drops what lies past the song") {
    Song song = song16();
    SongSections s;
    s.song_hash = 7;
    Section a;
    a.id = 1;
    a.start_s = 2.0;
    a.end_s = 8.0;
    Section b;
    b.id = 2;
    b.start_s = 10.0;
    b.end_s = 20.0;
    Section c;
    c.id = 3;
    c.start_s = 40.0;
    c.end_s = 50.0;
    s.sections = {c, b, a};
    s.next_id = 4;
    s.fit_to_song(song);
    REQUIRE(s.sections.size() == 2);
    CHECK(s.sections[0].id == 1);
    CHECK(s.sections[0].start_s == 0.0);
    CHECK(s.sections[0].end_s == 10.0);
    CHECK(s.sections[1].id == 2);
    CHECK(s.sections[1].end_s == 32.0);
    CHECK(s.valid(song));
}

TEST_CASE("sections: fit_to_song drops overlapping and too-short sections") {
    Song song = song16();
    SongSections s;
    Section a;
    a.id = 1;
    a.start_s = 0.0;
    a.end_s = 10.0;
    Section overlap;
    overlap.id = 2;
    overlap.start_s = 5.0;
    overlap.end_s = 20.0;
    Section tiny;
    tiny.id = 3;
    tiny.start_s = 10.0;
    tiny.end_s = 10.01;
    Section d;
    d.id = 4;
    d.start_s = 10.0;
    d.end_s = 30.0;
    s.sections = {a, overlap, tiny, d};
    s.next_id = 5;
    s.fit_to_song(song);
    REQUIRE(s.sections.size() == 2);
    CHECK(s.sections[0].id == 1);
    CHECK(s.sections[1].id == 4);
    CHECK(s.sections[1].end_s == 32.0);
    CHECK(s.valid(song));
}

TEST_CASE("sections: fit_to_song on a song shorter than a section drops them all") {
    Song tiny;
    tiny.duration_s = 0.05;
    SongSections s;
    Section a;
    a.id = 1;
    a.start_s = 0.0;
    a.end_s = 0.1;
    s.sections = {a};
    s.next_id = 2;
    s.fit_to_song(tiny);
    CHECK(s.empty());
}

TEST_CASE("sections: fit_to_song keeps a custom name and cleans a bad one") {
    Song song = song16();
    SongSections s = with_markers(song, {16.0});
    s.sections[0].name = "Intro";
    s.sections[0].custom_name = true;
    s.sections[1].name = std::string("x\xFF") + std::string(100, 'y');
    s.sections[1].custom_name = true;
    s.fit_to_song(song);
    CHECK(s.sections[0].name == "Intro");
    CHECK(s.sections[1].name.size() == kMaxSectionNameBytes);
    CHECK(s.valid(song));
}

TEST_CASE("sections: valid() notices each broken invariant") {
    Song song = song16();
    const SongSections good = with_markers(song, {8.0, 16.0});
    CHECK(good.valid(song));

    SongSections gap = good;
    gap.sections[1].start_s = 9.0;
    CHECK_FALSE(gap.valid(song));

    SongSections late = good;
    late.sections.back().end_s = 31.0;
    CHECK_FALSE(late.valid(song));

    SongSections dup = good;
    dup.sections[1].id = dup.sections[0].id;
    CHECK_FALSE(dup.valid(song));

    SongSections zero_id = good;
    zero_id.sections[0].id = kNoSection;
    CHECK_FALSE(zero_id.valid(song));

    SongSections stale_next = good;
    stale_next.next_id = 1;
    CHECK_FALSE(stale_next.valid(song));

    SongSections bad_name = good;
    bad_name.sections[0].name = "x\xFF";
    CHECK_FALSE(bad_name.valid(song));

    SongSections wrong_measure = good;
    wrong_measure.sections[0].last_measure = 5;
    CHECK_FALSE(wrong_measure.valid(song));

    SongSections dangling = good;
    PlanItem item;
    item.section = 99;
    dangling.plan.push_back(item);
    CHECK_FALSE(dangling.valid(song));
}

// ---- The saved playlist

namespace {

PlanItem item(SectionId id, int repeats = 1) {
    PlanItem p;
    p.section = id;
    p.repeats = repeats;
    return p;
}

std::vector<SectionId> plan_ids(const SongSections& s) {
    std::vector<SectionId> out;
    for (const PlanItem& p : s.plan) {
        out.push_back(p.section);
    }
    return out;
}

} // namespace

TEST_CASE("sections: playlist items are played in the order they were added") {
    Song song = song16();
    SongSections s = with_markers(song, {8.0, 16.0});
    SectionId a = s.sections[0].id;
    SectionId b = s.sections[1].id;
    SectionId c = s.sections[2].id;
    REQUIRE(s.plan_append(item(c, 3)).ok);
    REQUIRE(s.plan_append(item(a)).ok);
    REQUIRE(s.plan_append(item(b, 0)).ok);
    CHECK(plan_ids(s) == std::vector<SectionId>{c, a, b});
    CHECK(s.plan[0].repeats == 3);
    CHECK(s.plan[2].repeats == 0);
    CHECK(s.valid(song));
}

TEST_CASE("sections: reordering the playlist moves one item") {
    Song song = song16();
    SongSections s = with_markers(song, {8.0, 16.0});
    SectionId a = s.sections[0].id;
    SectionId b = s.sections[1].id;
    SectionId c = s.sections[2].id;
    for (SectionId id : {a, b, c}) {
        REQUIRE(s.plan_append(item(id)).ok);
    }
    REQUIRE(s.plan_move(0, 2).ok);
    CHECK(plan_ids(s) == std::vector<SectionId>{b, c, a});
    REQUIRE(s.plan_move(2, 0).ok);
    CHECK(plan_ids(s) == std::vector<SectionId>{a, b, c});
    REQUIRE(s.plan_move(1, 1).ok);
    CHECK(plan_ids(s) == std::vector<SectionId>{a, b, c});
    CHECK(s.plan_move(0, 3).error == EditError::NotFound);
    CHECK(s.plan_move(5, 0).error == EditError::NotFound);
    CHECK(plan_ids(s) == std::vector<SectionId>{a, b, c});
}

TEST_CASE("sections: the playlist survives renames, since it names ids") {
    Song song = song16();
    SongSections s = with_markers(song, {16.0});
    SectionId b = s.sections[1].id;
    REQUIRE(s.plan_append(item(b)).ok);
    REQUIRE(s.rename(b, "Chorus").ok);
    REQUIRE(s.add_marker(song, 8.0).ok);
    REQUIRE(s.add_marker(song, 24.0).ok);
    CHECK(plan_ids(s) == std::vector<SectionId>{b});
    CHECK(s.valid(song));
}

TEST_CASE("sections: a playlist item for an unknown section is refused") {
    Song song = song16();
    SongSections s = with_markers(song, {16.0});
    CHECK(s.plan_append(item(99)).error == EditError::NotFound);
    CHECK(s.plan.empty());
    REQUIRE(s.plan_append(item(s.sections[0].id)).ok);
    CHECK(s.plan_set(0, item(99)).error == EditError::NotFound);
    CHECK(s.plan_set(4, item(s.sections[0].id)).error == EditError::NotFound);
}

TEST_CASE("sections: playlist items are clamped to their ranges") {
    Song song = song16();
    SongSections s = with_markers(song, {16.0});
    PlanItem p = item(s.sections[0].id, 500);
    p.speed_up = AutoSpeedUp{true, 62, 3, 90};
    REQUIRE(s.plan_append(p).ok);
    CHECK(s.plan[0].repeats == kMaxPlanRepeats);
    CHECK(s.plan[0].speed_up.start_percent == 60);
    CHECK(s.plan[0].speed_up.step_percent == 5);
    CHECK(s.plan[0].speed_up.max_percent == 90);

    PlanItem q = item(s.sections[0].id, -4);
    q.speed_up = AutoSpeedUp{true, 100, 80, 40};
    REQUIRE(s.plan_set(0, q).ok);
    CHECK(s.plan[0].repeats == 0);
    CHECK(s.plan[0].speed_up.step_percent == 25);
    CHECK(s.plan[0].speed_up.max_percent == 100); // never below the start
}

TEST_CASE("sections: the playlist has a size limit") {
    Song song = song16();
    SongSections s = with_markers(song, {16.0});
    for (std::size_t i = 0; i < kMaxPlanItems; ++i) {
        REQUIRE(s.plan_append(item(s.sections[0].id)).ok);
    }
    CHECK(s.plan_append(item(s.sections[0].id)).error == EditError::TooMany);
    CHECK(s.plan.size() == kMaxPlanItems);
}

TEST_CASE("sections: deleting a section takes it out of the playlist") {
    Song song = song16();
    SongSections s = with_markers(song, {8.0, 16.0});
    SectionId a = s.sections[0].id;
    SectionId b = s.sections[1].id;
    SectionId c = s.sections[2].id;
    for (SectionId id : {a, b, c, b}) {
        REQUIRE(s.plan_append(item(id)).ok);
    }
    REQUIRE(s.erase(song, b).ok);
    CHECK(plan_ids(s) == std::vector<SectionId>{a, c});
    CHECK(s.valid(song));
}

TEST_CASE("sections: merging moves playlist items to the surviving section") {
    Song song = song16();
    SongSections s = with_markers(song, {8.0, 16.0, 24.0});
    SectionId a = s.sections[0].id;
    SectionId b = s.sections[1].id;
    SectionId c = s.sections[2].id;
    SectionId d = s.sections[3].id;
    for (SectionId id : {a, b, c, d}) {
        REQUIRE(s.plan_append(item(id, 2)).ok);
    }
    std::array<SectionId, 2> pick{b, c};
    REQUIRE(s.merge(song, pick).ok);
    CHECK(plan_ids(s) == std::vector<SectionId>{a, b, d}); // b and c became one step
    CHECK(s.valid(song));
}

TEST_CASE("sections: removing a marker keeps the playlist valid") {
    Song song = song16();
    SongSections s = with_markers(song, {16.0});
    SectionId a = s.sections[0].id;
    SectionId b = s.sections[1].id;
    REQUIRE(s.plan_append(item(b)).ok);
    REQUIRE(s.plan_append(item(a)).ok);
    REQUIRE(s.remove_marker(song, 16.0).ok);
    CHECK(plan_ids(s) == std::vector<SectionId>{a});
    CHECK(s.valid(song));
}

// ---- The library

TEST_CASE("sections library: edit creates an entry, finds it again and ignores hash 0") {
    SectionLibrary lib;
    CHECK(lib.edit(0) == nullptr);
    CHECK(lib.find(5) == nullptr);
    SongSections* s = lib.edit(5);
    REQUIRE(s != nullptr);
    CHECK(s->song_hash == 5);
    CHECK(s->empty());
    Song song = song16();
    REQUIRE(s->add_marker(song, 16.0).ok);
    REQUIRE(lib.find(5) != nullptr);
    CHECK(lib.find(5)->sections.size() == 2);
    CHECK(lib.edit(5) == lib.edit(5));
    CHECK(lib.songs().size() == 1);
}

TEST_CASE("sections library: past the limit the song used longest ago is forgotten") {
    SectionLibrary lib;
    Song song = song16();
    for (std::uint64_t h = 1; h <= kMaxSongsWithSections; ++h) {
        REQUIRE(lib.edit(h)->add_marker(song, 16.0).ok);
    }
    REQUIRE(lib.songs().size() == kMaxSongsWithSections);
    lib.edit(1); // used again: hash 2 is now the oldest
    lib.edit(1000);
    CHECK(lib.songs().size() == kMaxSongsWithSections);
    CHECK(lib.find(1) != nullptr);
    CHECK(lib.find(2) == nullptr);
    CHECK(lib.find(1000) != nullptr);
    REQUIRE(lib.stale_names().size() == 1);
    CHECK(lib.stale_names()[0] == "sections.0000000000000002");
}

TEST_CASE("sections library: put replaces, and an entry with nothing in it is removed") {
    SectionLibrary lib;
    Song song = song16();
    SongSections s;
    s.song_hash = 9;
    REQUIRE(s.add_marker(song, 16.0).ok);
    lib.put(s);
    REQUIRE(lib.find(9) != nullptr);
    REQUIRE(s.add_marker(song, 8.0).ok);
    lib.put(s);
    CHECK(lib.songs().size() == 1);
    CHECK(lib.find(9)->sections.size() == 3);

    SongSections none;
    none.song_hash = 9;
    lib.put(none);
    CHECK(lib.find(9) == nullptr);
    CHECK(lib.stale_names() == std::vector<std::string>{"sections.0000000000000009"});

    SongSections anon;
    anon.sections = s.sections;
    lib.put(anon); // hash 0
    CHECK(lib.songs().empty());
}

TEST_CASE("sections library: erase forgets a song and marks its document section stale") {
    SectionLibrary lib;
    Song song = song16();
    REQUIRE(lib.edit(0xABCDEF)->add_marker(song, 16.0).ok);
    lib.erase(0xABCDEF);
    CHECK(lib.songs().empty());
    CHECK(lib.stale_names() == std::vector<std::string>{"sections.0000000000abcdef"});
    lib.erase(0xABCDEF); // not there: nothing more
    CHECK(lib.stale_names().size() == 1);
}

TEST_CASE("sections library: the use counter never wraps") {
    SectionLibrary lib;
    SongSections a;
    a.song_hash = 1;
    a.last_used = 4'294'967'295u;
    Song song = song16();
    REQUIRE(a.add_marker(song, 16.0).ok);
    SongSections b = a;
    b.song_hash = 2;
    b.last_used = 4'294'967'290u;
    lib.put(a);
    lib.put(b);
    SongSections* c = lib.edit(3);
    REQUIRE(c != nullptr);
    CHECK(c->last_used > lib.find(1)->last_used);
    CHECK(lib.find(1)->last_used > lib.find(2)->last_used);
}

// ---- Audit follow-ups (issue #32)

TEST_CASE("sections: a speed-up step near INT_MAX is clamped before it is rounded") {
    PlanItem p;
    p.speed_up = AutoSpeedUp{true, 60, std::numeric_limits<int>::max(), 100};
    CHECK(sanitized(p).speed_up.step_percent == 25);
    p.speed_up.step_percent = std::numeric_limits<int>::max() - 1;
    CHECK(sanitized(p).speed_up.step_percent == 25);
    p.speed_up.step_percent = std::numeric_limits<int>::min();
    CHECK(sanitized(p).speed_up.step_percent == 5);
    p.speed_up =
        AutoSpeedUp{true, std::numeric_limits<int>::max(), 5, std::numeric_limits<int>::max()};
    PlanItem q = sanitized(p);
    CHECK(q.speed_up.start_percent == 150);
    CHECK(q.speed_up.max_percent == 150);
    p.repeats = std::numeric_limits<int>::max();
    CHECK(sanitized(p).repeats == kMaxPlanRepeats);
}

TEST_CASE("sections: merging unequal playlist items keeps the larger repeat count") {
    Song song = song16();
    SongSections s = with_markers(song, {8.0, 16.0});
    SectionId a = s.sections[0].id;
    SectionId b = s.sections[1].id;
    SectionId c = s.sections[2].id;
    PlanItem first = item(a, 3);
    first.speed_up = AutoSpeedUp{true, 50, 5, 90};
    PlanItem second = item(b, 1);
    second.clean_passes_only = true;
    second.speed_up = AutoSpeedUp{true, 70, 10, 120};
    REQUIRE(s.plan_append(first).ok);
    REQUIRE(s.plan_append(second).ok);
    REQUIRE(s.plan_append(item(c, 2)).ok);

    std::array<SectionId, 2> pick{a, b};
    std::vector<SectionId> absorbed;
    REQUIRE(s.merge(song, pick, &absorbed).ok);
    CHECK(absorbed == std::vector<SectionId>{b});
    REQUIRE(s.plan.size() == 2);
    CHECK(s.plan[0].section == a);
    CHECK(s.plan[0].repeats == 3);      // the larger of 3 and 1
    CHECK(s.plan[0].clean_passes_only); // the second one was
    PlanItem want = first; // everything else, the speed-up included, is the first item's
    want.repeats = 3;
    want.clean_passes_only = true;
    CHECK(s.plan[0] == want);
    CHECK(s.plan[1].section == c);
    CHECK(s.valid(song));
}

TEST_CASE("sections: merging takes the second item's repeats when they are more, 0 is the most") {
    Song song = song16();
    SongSections s = with_markers(song, {8.0});
    SectionId a = s.sections[0].id;
    SectionId b = s.sections[1].id;
    REQUIRE(s.plan_append(item(a, 2)).ok);
    REQUIRE(s.plan_append(item(b, 5)).ok);
    std::array<SectionId, 2> pick{a, b};
    REQUIRE(s.merge(song, pick).ok);
    REQUIRE(s.plan.size() == 1);
    CHECK(s.plan[0].repeats == 5);
    CHECK_FALSE(s.plan[0].clean_passes_only);

    SongSections t = with_markers(song, {8.0});
    REQUIRE(t.plan_append(item(t.sections[0].id, 4)).ok);
    REQUIRE(t.plan_append(item(t.sections[1].id, 0)).ok);
    REQUIRE(t.remove_marker(song, 8.0).ok);
    REQUIRE(t.plan.size() == 1);
    CHECK(t.plan[0].repeats == 0); // until skipped
}

TEST_CASE("sections: merging keeps playlist items that are not next to each other apart") {
    Song song = song16();
    SongSections s = with_markers(song, {8.0, 16.0});
    SectionId a = s.sections[0].id;
    SectionId b = s.sections[1].id;
    SectionId c = s.sections[2].id;
    REQUIRE(s.plan_append(item(a, 2)).ok);
    REQUIRE(s.plan_append(item(c, 1)).ok);
    REQUIRE(s.plan_append(item(b, 3)).ok);
    std::array<SectionId, 2> pick{a, b};
    REQUIRE(s.merge(song, pick).ok);
    CHECK(plan_ids(s) == std::vector<SectionId>{a, c, a});
    CHECK(s.plan[0].repeats == 2);
    CHECK(s.plan[2].repeats == 3);
}

TEST_CASE("sections: normalize drops non-finite times before it sorts") {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    SongSections s;
    s.next_id = 1;
    auto section = [](SectionId id, double a, double b) {
        Section x;
        x.id = id;
        x.start_s = a;
        x.end_s = b;
        return x;
    };
    s.sections = {section(1, 20.0, 30.0), section(2, nan, 5.0),   section(3, 0.0, 10.0),
                  section(4, 10.0, nan),  section(5, 10.0, 20.0), section(6, 3.0, 4.0)};
    s.plan = {item(2), item(5), item(3)};
    s.normalize();
    REQUIRE(s.sections.size() == 3);
    CHECK(s.sections[0].id == 3);
    CHECK(s.sections[1].id == 5);
    CHECK(s.sections[2].id == 1);
    CHECK(s.next_id == 6);                              // id 6 was too short to keep
    CHECK(plan_ids(s) == std::vector<SectionId>{5, 3}); // the item on a dropped section goes
    CHECK(s.well_formed());
}

TEST_CASE("sections: normalize drops repeated ids, overlaps and short sections, and renames") {
    SongSections s;
    auto section = [](SectionId id, double a, double b, std::string name = {}) {
        Section x;
        x.id = id;
        x.start_s = a;
        x.end_s = b;
        x.custom_name = !name.empty();
        x.name = std::move(name);
        return x;
    };
    s.sections = {section(2, 5.0, 9.0, "  Verse\t1 "),
                  section(2, 9.0, 12.0),
                  section(0, 12.0, 20.0),
                  section(3, 8.0, 15.0),
                  section(4, 15.0, 15.0),
                  section(5, 15.0, 20.0)};
    s.next_id = 1;
    s.normalize();
    REQUIRE(s.sections.size() == 2);
    CHECK(s.sections[0].id == 2);
    CHECK(s.sections[0].name == "Verse 1");
    CHECK(s.sections[1].id == 5);
    CHECK(s.sections[1].name == "Section 2");
    CHECK(s.next_id == 6);
}

TEST_CASE("sections: plan_clear empties the playlist and leaves the sections") {
    Song song = song16();
    SongSections s = with_markers(song, {8.0});
    REQUIRE(s.plan_append(item(s.sections[0].id)).ok);
    REQUIRE(s.plan_append(item(s.sections[1].id)).ok);
    s.plan_clear();
    CHECK(s.plan.empty());
    CHECK(s.sections.size() == 2);
    CHECK(s.valid(song));
    s.plan_clear(); // already empty
    CHECK(s.plan.empty());
}
