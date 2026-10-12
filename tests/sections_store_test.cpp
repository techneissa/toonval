// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/sections_store.hpp"

#include "toonval/content_hash.hpp"
#include "toonval/session_state.hpp"

#include "section_fixtures.hpp"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace toonval;
using namespace toonval::testing;

namespace {

constexpr std::uint64_t kHash = 0x9f3a0c1b22d4e5f6ULL;
constexpr const char* kName = "sections.9f3a0c1b22d4e5f6";

Song song16() { return bars_song(16); }

// The entry of the golden examples: Intro, an unnamed second section, one playlist item.
SongSections example(const Song& song) {
    SongSections s;
    s.song_hash = kHash;
    REQUIRE(s.add_marker(song, 16.0).ok);
    REQUIRE(s.rename(s.sections[0].id, "Intro").ok);
    PlanItem p;
    p.section = s.sections[1].id;
    p.repeats = 4;
    p.clean_passes_only = true;
    p.speed_up = AutoSpeedUp{true, 60, 5, 100};
    REQUIRE(s.plan_append(p).ok);
    s.last_used = 5;
    return s;
}

SettingsDocument written(const SectionLibrary& lib, std::string_view before = "") {
    SettingsDocument doc = SettingsDocument::parse(before);
    write_sections(doc, lib);
    return doc;
}

std::size_t count_issues(const std::vector<SettingsIssue>& issues, SettingsIssueKind kind) {
    std::size_t n = 0;
    for (const SettingsIssue& i : issues) {
        n += i.kind == kind ? 1 : 0;
    }
    return n;
}

} // namespace

TEST_CASE("sections store: an entry is written in this exact format") {
    Song song = song16();
    SectionLibrary lib;
    lib.put(example(song));
    SettingsDocument doc = written(lib);
    CHECK(doc.section_text(kName) == "version = 1\n"
                                     "next_id = 3\n"
                                     "used = 5\n"
                                     "section.1 = 0 16 Intro\n"
                                     "section.2 = 16 32\n"
                                     "plan.1 = 2 4 1 1 60 5 100\n");
    CHECK(doc.serialize().find(std::string("[") + kName + "]\n") != std::string::npos);
}

TEST_CASE("sections store: an entry reads back as it was written") {
    Song song = song16();
    SongSections original = example(song);
    SectionLibrary lib;
    lib.put(original);
    SectionLibrary back = read_sections(SettingsDocument::parse(written(lib).serialize()));
    REQUIRE(back.songs().size() == 1);
    SongSections s = back.songs()[0];
    CHECK(s.well_formed());
    s.fit_to_song(song); // derived fields come with the song
    CHECK(s.valid(song));
    CHECK(s == original);
}

TEST_CASE("sections store: names with blanks, quotes, hashes and other scripts round trip") {
    Song song = song16();
    SongSections s;
    s.song_hash = 77;
    REQUIRE(s.add_marker(song, 8.0).ok);
    REQUIRE(s.add_marker(song, 16.0).ok);
    REQUIRE(s.rename(s.sections[0].id, "\"Intro\" # 1 = a b").ok);
    REQUIRE(s.rename(s.sections[1].id, "Sch\xC3\xB6n \xE3\x82\xA4").ok);
    SectionLibrary lib;
    lib.put(s);
    SectionLibrary back = read_sections(SettingsDocument::parse(written(lib).serialize()));
    REQUIRE(back.songs().size() == 1);
    SongSections r = back.songs()[0];
    r.fit_to_song(song);
    CHECK(r == s);
}

TEST_CASE("sections store: times that are not round numbers come back exactly") {
    Song song;
    song.duration_s = 100.0;
    SongSections s;
    s.song_hash = 5;
    REQUIRE(s.add_marker(song, 33.333333333333336, false).ok);
    REQUIRE(s.add_marker(song, 1.0 / 3.0 + 40.0, false).ok);
    SectionLibrary lib;
    lib.put(s);
    SectionLibrary back = read_sections(SettingsDocument::parse(written(lib).serialize()));
    REQUIRE(back.songs().size() == 1);
    SongSections r = back.songs()[0];
    r.fit_to_song(song);
    CHECK(r == s);
}

TEST_CASE("sections store: several songs, each in its own document section") {
    Song song = song16();
    SectionLibrary lib;
    for (std::uint64_t h : {1ULL, 2ULL, 0xFFFFFFFFFFFFFFFFULL}) {
        REQUIRE(lib.edit(h)->add_marker(song, 16.0).ok);
    }
    SettingsDocument doc = written(lib);
    CHECK(doc.has_section("sections.0000000000000001"));
    CHECK(doc.has_section("sections.0000000000000002"));
    CHECK(doc.has_section("sections.ffffffffffffffff"));
    SectionLibrary back = read_sections(doc);
    CHECK(back.songs().size() == 3);
    CHECK(back.find(0xFFFFFFFFFFFFFFFFULL) != nullptr);
}

TEST_CASE(
    "sections store: writing changes nothing but the entries, and twice changes nothing more") {
    Song song = song16();
    SectionLibrary lib;
    lib.put(example(song));
    const std::string before =
        "# my settings\nformat = 1.0\n\n[app]\nhit_window_ms = 90\n\n[session]\nsong = x.mid\n";
    SettingsDocument doc = written(lib, before);
    const std::string once = doc.serialize();
    CHECK(once.compare(0, before.size(), before) == 0); // the file so far is as it was
    write_sections(doc, lib);
    CHECK(doc.serialize() == once);
    CHECK(doc.section_text("app") == "hit_window_ms = 90\n");
}

TEST_CASE("sections store: keys and comments this build does not know are kept") {
    Song song = song16();
    const std::string text = std::string("[") + kName +
                             "]\n"
                             "# written by a newer build\n"
                             "version = 1\n"
                             "next_id = 3\n"
                             "future_key = 42\n"
                             "section.1 = 0 16 Intro\n"
                             "section.2 = 16 32\n"
                             "not a key value line\n"
                             "plan.1 = 2 4 1 1 60 5 100\n";
    std::vector<SettingsIssue> issues;
    SettingsDocument doc = SettingsDocument::parse(text);
    SectionLibrary lib = read_sections(doc, &issues);
    REQUIRE(lib.songs().size() == 1);
    CHECK(count_issues(issues, SettingsIssueKind::UnknownKey) == 1);

    SongSections* s = lib.edit(kHash);
    REQUIRE(s != nullptr);
    s->fit_to_song(song);
    REQUIRE(s->rename(s->sections[1].id, "Verse").ok);
    write_sections(doc, lib);
    std::string out = doc.section_text(kName);
    CHECK(out.find("# written by a newer build\n") != std::string::npos);
    CHECK(out.find("future_key = 42\n") != std::string::npos);
    CHECK(out.find("not a key value line\n") != std::string::npos);
    CHECK(out.find("section.2 = 16 32 Verse\n") != std::string::npos);
    CHECK(out.find("section.2 = 16 32\n") == std::string::npos);
    // And again: nothing is added or lost.
    write_sections(doc, lib);
    CHECK(doc.section_text(kName) == out);
}

TEST_CASE("sections store: a line that does not parse is dropped, the rest of the entry stays") {
    const std::string text = std::string("[") + kName +
                             "]\n"
                             "version = 1\n"
                             "section.1 = 0 16 Intro\n"
                             "section.2 = sixteen 32 Broken\n"
                             "section.3 = 16\n"
                             "section.4 = 16 nan Nope\n"
                             "section.five = 16 20\n"
                             "section.6 = 16 32 Verse\n"
                             "plan.1 = 1 x 0 0 60 5 100\n"
                             "plan.2 = 6 2 0 0 60 5 100\n"
                             "used = lots\n"
                             "next_id = 7\n";
    std::vector<SettingsIssue> issues;
    SectionLibrary lib = read_sections(SettingsDocument::parse(text), &issues);
    REQUIRE(lib.songs().size() == 1);
    const SongSections& s = lib.songs()[0];
    REQUIRE(s.sections.size() == 2);
    CHECK(s.sections[0].id == 1);
    CHECK(s.sections[0].name == "Intro");
    CHECK(s.sections[1].id == 6);
    CHECK(s.sections[1].name == "Verse");
    REQUIRE(s.plan.size() == 1);
    CHECK(s.plan[0].section == 6);
    CHECK(s.plan[0].repeats == 2);
    CHECK(s.next_id == 7);
    CHECK(s.last_used == 0);
    CHECK(count_issues(issues, SettingsIssueKind::BadValue) == 5);
    CHECK(count_issues(issues, SettingsIssueKind::UnknownKey) == 1); // section.five
    CHECK(s.well_formed());
}

TEST_CASE("sections store: a damaged entry does not cost the others") {
    const std::string good =
        "[sections.0000000000000001]\nsection.1 = 0 16 A\nsection.2 = 16 32 B\n";
    const std::string text =
        good + "\n"
               "[sections.garbage]\nsection.1 = 0 16\n\n"
               "[sections.0000000000000002]\nsection.1 = 5 3\n\n"
               "[sections.0000000000000003]\nsection.1 = 0 16 C\nsection.2 = 16 32 D\n\n"
               "[sections.ABCDEF0000000004]\nsection.1 = 0 16 E\nsection.2 = 16 32 F\n\n"
               "[sections.5]\nsection.1 = 0 16 G\nsection.2 = 16 32 H\n\n"
               "[app]\nhit_window_ms = 90\n";
    std::vector<SettingsIssue> issues;
    SettingsDocument doc = SettingsDocument::parse(text);
    SectionLibrary lib = read_sections(doc, &issues);
    CHECK(lib.songs().size() == 2);
    CHECK(lib.find(1) != nullptr);
    CHECK(lib.find(3) != nullptr);
    CHECK(lib.find(2) == nullptr); // no usable section
    // The three that could not be read are on the list to delete.
    CHECK(lib.stale_names().size() == 4);
    write_sections(doc, lib);
    CHECK_FALSE(doc.has_section("sections.garbage"));
    CHECK_FALSE(doc.has_section("sections.0000000000000002"));
    CHECK_FALSE(doc.has_section("sections.ABCDEF0000000004"));
    CHECK_FALSE(doc.has_section("sections.5"));
    CHECK(doc.has_section("sections.0000000000000001"));
    CHECK(doc.has_section("sections.0000000000000003"));
    CHECK(doc.section_text("app") == "hit_window_ms = 90\n");
    CHECK(count_issues(issues, SettingsIssueKind::UnknownSection) == 3);
    CHECK(count_issues(issues, SettingsIssueKind::OutOfRange) == 1); // sections.0...02
}

TEST_CASE("sections store: an entry of a newer version is left alone") {
    const std::string text =
        "[sections.0000000000000001]\nversion = 2\nsection.1 = 0 16 A\nnew = 1\n";
    std::vector<SettingsIssue> issues;
    SettingsDocument doc = SettingsDocument::parse(text);
    SectionLibrary lib = read_sections(doc, &issues);
    CHECK(lib.songs().empty());
    CHECK(lib.stale_names().empty());
    CHECK(count_issues(issues, SettingsIssueKind::NewerVersion) == 1);
    // Another song is written next to it; the newer entry stays byte for byte.
    Song song = song16();
    REQUIRE(lib.edit(9)->add_marker(song, 16.0).ok);
    write_sections(doc, lib);
    CHECK(doc.section_text("sections.0000000000000001") ==
          "version = 2\nsection.1 = 0 16 A\nnew = 1\n");
    CHECK(doc.has_section("sections.0000000000000009"));
}

TEST_CASE("sections store: a bad version number reads as version 1") {
    const std::string text =
        "[sections.0000000000000001]\nversion = x\nsection.1 = 0 16 A\nsection.2 = 16 32\n";
    std::vector<SettingsIssue> issues;
    SectionLibrary lib = read_sections(SettingsDocument::parse(text), &issues);
    CHECK(lib.songs().size() == 1);
    CHECK(count_issues(issues, SettingsIssueKind::BadValue) == 1);
}

TEST_CASE("sections store: sections that overlap, repeat an id or run too short are dropped") {
    const std::string text = std::string("[") + kName +
                             "]\n"
                             "section.1 = 0 10 A\n"
                             "section.2 = 5 20 Overlaps\n"
                             "section.3 = 10 10.05 Tiny\n"
                             "section.1x = 12 20 Odd\n"
                             "section.4 = 10 20 Fine\n"
                             "section.0 = 20 30 NoId\n"
                             "section.5 = 30 5 Backwards\n"
                             "section.9999999999 = 20 30 BigId\n";
    SectionLibrary lib = read_sections(SettingsDocument::parse(text));
    REQUIRE(lib.songs().size() == 1);
    const SongSections& s = lib.songs()[0];
    std::vector<SectionId> ids;
    for (const Section& x : s.sections) {
        ids.push_back(x.id);
    }
    CHECK(ids == std::vector<SectionId>{1, 4});
    CHECK(s.well_formed());
    CHECK(s.next_id == 5);
}

TEST_CASE("sections store: a repeated key counts once, the last wins") {
    const std::string text = std::string("[") + kName +
                             "]\n"
                             "section.1 = 0 10 First\n"
                             "section.1 = 0 12 Second\n";
    std::vector<SettingsIssue> issues;
    SectionLibrary lib = read_sections(SettingsDocument::parse(text), &issues);
    REQUIRE(lib.songs().size() == 1);
    REQUIRE(lib.songs()[0].sections.size() == 1);
    CHECK(lib.songs()[0].sections[0].end_s == 12.0);
    CHECK(count_issues(issues, SettingsIssueKind::DuplicateKey) == 1);
}

TEST_CASE("sections store: names are cleaned and cut on reading") {
    const std::string text = std::string("[") + kName +
                             "]\n"
                             "section.1 = 0 16 \xE2\x80\xAE"
                             "evil\xFF  name\n"
                             "section.2 = 16 32 " +
                             std::string(200, 'x') + "\n";
    SectionLibrary lib = read_sections(SettingsDocument::parse(text));
    REQUIRE(lib.songs().size() == 1);
    CHECK(lib.songs()[0].sections[0].name == "evil name");
    CHECK(lib.songs()[0].sections[1].name.size() == kMaxSectionNameBytes);
}

TEST_CASE("sections store: counts are capped") {
    std::string text = std::string("[") + kName + "]\n";
    for (int i = 0; i < 200; ++i) {
        text += "section." + std::to_string(i + 1) + " = " + std::to_string(i) + " " +
                std::to_string(i + 1) + "\n";
    }
    for (int i = 0; i < 200; ++i) {
        text += "plan." + std::to_string(i + 1) + " = 1 1 0 0 60 5 100\n";
    }
    SectionLibrary lib = read_sections(SettingsDocument::parse(text));
    REQUIRE(lib.songs().size() == 1);
    CHECK(lib.songs()[0].sections.size() == kMaxSectionsPerSong);
    CHECK(lib.songs()[0].plan.size() == kMaxPlanItems);
    CHECK(lib.songs()[0].well_formed());
}

TEST_CASE("sections store: past the song limit the most recently used entries load") {
    std::string text;
    const std::size_t extra = 10;
    for (std::size_t i = 0; i < kMaxSongsWithSections + extra; ++i) {
        text += "[sections." + content_hash_hex(i + 1) + "]\nused = " + std::to_string(i + 1) +
                "\nsection.1 = 0 16 A\n\n";
    }
    SettingsDocument doc = SettingsDocument::parse(text);
    SectionLibrary lib = read_sections(doc);
    CHECK(lib.songs().size() == kMaxSongsWithSections);
    CHECK(lib.find(1) == nullptr); // the oldest are the ones forgotten
    CHECK(lib.find(extra) == nullptr);
    CHECK(lib.find(extra + 1) != nullptr);
    CHECK(lib.find(kMaxSongsWithSections + extra) != nullptr);
    CHECK(lib.stale_names().size() == extra);
    write_sections(doc, lib);
    CHECK(sections_section_names(doc).size() == kMaxSongsWithSections);
}

TEST_CASE("sections store: erasing a song deletes its entry on the next write") {
    Song song = song16();
    SectionLibrary lib;
    REQUIRE(lib.edit(1)->add_marker(song, 16.0).ok);
    REQUIRE(lib.edit(2)->add_marker(song, 16.0).ok);
    SettingsDocument doc = written(lib);
    lib.erase(1);
    write_sections(doc, lib);
    CHECK_FALSE(doc.has_section("sections.0000000000000001"));
    CHECK(doc.has_section("sections.0000000000000002"));
}

TEST_CASE("sections store: deleting every section of a song deletes its entry") {
    Song song = song16();
    SectionLibrary lib;
    SongSections* s = lib.edit(1);
    REQUIRE(s->add_marker(song, 16.0).ok);
    SettingsDocument doc = written(lib);
    REQUIRE(doc.has_section("sections.0000000000000001"));
    while (!s->sections.empty()) {
        REQUIRE(s->erase(song, s->sections[0].id).ok);
    }
    lib.put(*s);
    write_sections(doc, lib);
    CHECK_FALSE(doc.has_section("sections.0000000000000001"));
}

TEST_CASE("sections store: a song put back after being forgotten is written again") {
    Song song = song16();
    SectionLibrary lib;
    REQUIRE(lib.edit(1)->add_marker(song, 16.0).ok);
    SettingsDocument doc = written(lib);
    lib.erase(1);
    REQUIRE(lib.edit(1)->add_marker(song, 8.0).ok);
    write_sections(doc, lib);
    SectionLibrary back = read_sections(doc);
    REQUIRE(back.find(1) != nullptr);
    CHECK(back.find(1)->sections.size() == 2);
    CHECK(back.find(1)->sections[0].end_s == 8.0);
}

TEST_CASE("sections store: the layer's own section names, for load_settings") {
    Song song = song16();
    SectionLibrary lib;
    REQUIRE(lib.edit(1)->add_marker(song, 16.0).ok);
    REQUIRE(lib.edit(2)->add_marker(song, 16.0).ok);
    SettingsDocument doc = written(lib, "[app]\nhit_window_ms = 90\n");
    std::vector<std::string> names = sections_section_names(doc);
    CHECK(names ==
          std::vector<std::string>{"sections.0000000000000001", "sections.0000000000000002"});
    std::vector<std::string_view> known(names.begin(), names.end());
    LoadedSettings loaded = load_settings(doc.serialize(), known);
    for (const SettingsIssue& i : loaded.issues) {
        CHECK(i.kind != SettingsIssueKind::UnknownSection);
    }
}

TEST_CASE("sections store: an empty or unrelated document gives an empty library") {
    CHECK(read_sections(SettingsDocument::parse("")).songs().empty());
    CHECK(read_sections(SettingsDocument::parse("[app]\nx = 1\n[sectionsX]\ny = 2\n"))
              .songs()
              .empty());
    SectionLibrary none;
    SettingsDocument doc = written(none, "[app]\nx = 1\n");
    CHECK(doc.serialize() == "[app]\nx = 1\n");
}

// ---- Audit follow-ups (issue #32)

TEST_CASE("sections store: a speed-up step near INT_MAX reads clamped") {
    SettingsDocument doc = SettingsDocument::parse(std::string("[") + kName +
                                                   "]\nsection.1 = 0 32\n"
                                                   "plan.1 = 1 1 0 1 60 2147483647 100\n"
                                                   "plan.2 = 1 2147483647 0 1 2147483647 5 "
                                                   "2147483647\n");
    SectionLibrary lib = read_sections(doc);
    REQUIRE(lib.songs().size() == 1);
    const SongSections& s = lib.songs()[0];
    REQUIRE(s.plan.size() == 2);
    CHECK(s.plan[0].speed_up.step_percent == 25);
    CHECK(s.plan[1].repeats == kMaxPlanRepeats);
    CHECK(s.plan[1].speed_up.start_percent == 150);
    CHECK(s.well_formed());
}

TEST_CASE("sections store: an entry with only unknown keys is kept untouched") {
    const std::string text = std::string("[") + kName + "]\nversion = 1\ncolor = red\n";
    SettingsDocument doc = SettingsDocument::parse(text);
    std::vector<SettingsIssue> issues;
    SectionLibrary lib = read_sections(doc, &issues);
    CHECK(lib.songs().empty());
    CHECK(lib.stale_names().empty());
    CHECK(count_issues(issues, SettingsIssueKind::UnknownKey) == 1);
    write_sections(doc, lib);
    CHECK(doc.serialize() == text);
}

TEST_CASE("sections store: an entry with nothing at all is still dropped") {
    SettingsDocument doc =
        SettingsDocument::parse(std::string("[") + kName + "]\nversion = 1\nused = 3\n");
    SectionLibrary lib = read_sections(doc);
    CHECK(lib.songs().empty());
    CHECK(lib.stale_names().size() == 1);
    write_sections(doc, lib);
    CHECK_FALSE(doc.has_section(kName));
}

TEST_CASE("sections store: deleting the last section keeps the unknown keys of the entry") {
    Song song = song16();
    SectionLibrary lib;
    lib.put(example(song));
    SettingsDocument doc = written(lib);
    doc.set(kName, "color", "red");
    SectionLibrary read = read_sections(doc);
    REQUIRE(read.songs().size() == 1);
    SongSections* s = read.edit(kHash);
    s->sections.clear();
    s->plan.clear();
    write_sections(doc, read);
    CHECK(doc.has_section(kName));
    CHECK(doc.section_text(kName) == "color = red\n");
    // Without unknown keys it goes.
    SectionLibrary lib2;
    lib2.put(example(song));
    SettingsDocument plain = written(lib2);
    SectionLibrary read2 = read_sections(plain);
    SongSections* s2 = read2.edit(kHash);
    s2->sections.clear();
    s2->plan.clear();
    write_sections(plain, read2);
    CHECK_FALSE(plain.has_section(kName));
}

TEST_CASE("sections store: values near INT_MAX and UINT_MAX read and write stably") {
    for (const char* big : {"2147483647", "2147483648", "4294967295", "4294967296", "-1",
                            "-2147483648", "99999999999999999999"}) {
        CAPTURE(big);
        std::string text = std::string("[") + kName + "]\nversion = 1\nnext_id = " + big +
                           "\nused = " + big + "\nsection.1 = 0 16\nsection." + big +
                           " = 16 32\nplan.1 = 1 " + big + " 1 1 " + big + " " + big + " " + big +
                           "\nplan." + big + " = 1 1 0 0 60 5 100\nplan.2 = " + big +
                           " 1 0 0 60 5 100\n";
        SettingsDocument doc = SettingsDocument::parse(text);
        SectionLibrary lib = read_sections(doc);
        for (const SongSections& s : lib.songs()) {
            CHECK(s.well_formed());
        }
        SettingsDocument out;
        write_sections(out, lib);
        SectionLibrary again = read_sections(out);
        REQUIRE(again.songs().size() == lib.songs().size());
        for (std::size_t i = 0; i < lib.songs().size(); ++i) {
            CHECK(again.songs()[i] == lib.songs()[i]);
        }
    }
}
