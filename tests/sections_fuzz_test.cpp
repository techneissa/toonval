// SPDX-License-Identifier: GPL-3.0-or-later
// Random libraries written and read back, and random damage to the saved text: reading never
// crashes and always gives well-formed sections within the caps, a damaged entry costs only
// itself, and what is read can be written and read again without change. Fixed seeds; the
// iteration is printed on a failure.
#include "toonval/sections_store.hpp"

#include "section_fixtures.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <random>
#include <string>
#include <vector>

using namespace toonval;
using namespace toonval::testing;

namespace {

std::string random_name(std::mt19937_64& rng) {
    static const char* kWords[] = {"Intro",      "Verse",        "Chorus", "Bridge",
                                   "Hard run",   "\xC3\xA9tude", "a=b",    "# not a comment",
                                   "\"quoted\"", "x  y"};
    std::string s = kWords[rng() % 10];
    if (rng() % 4 == 0) {
        s.push_back(static_cast<char>(rng() % 256));
    }
    return s;
}

// Edits a library with a few songs at random.
SectionLibrary random_library(std::mt19937_64& rng, std::vector<Song>& songs) {
    SectionLibrary lib;
    std::size_t count = 1 + rng() % 5;
    for (std::size_t k = 0; k < count; ++k) {
        songs.push_back(random_song(rng));
        const Song& song = songs.back();
        SongSections* s = lib.edit(0x1000 + k * 7919);
        for (int i = 0; i < 12; ++i) {
            double t = song.duration_s * static_cast<double>(rng() % 10001) / 10000.0;
            s->add_marker(song, t, rng() % 2 == 0);
        }
        for (const Section& x : std::vector<Section>(s->sections)) {
            if (rng() % 3 == 0) {
                s->rename(x.id, random_name(rng));
            }
        }
        for (int i = 0; i < 4 && !s->sections.empty(); ++i) {
            PlanItem p;
            p.section = s->sections[rng() % s->sections.size()].id;
            p.repeats = static_cast<int>(rng() % 6);
            p.clean_passes_only = rng() % 2 == 0;
            p.speed_up = AutoSpeedUp{rng() % 2 == 0, 40 + 5 * static_cast<int>(rng() % 10), 5, 100};
            s->plan_append(p);
        }
    }
    return lib;
}

std::string mutate(std::mt19937_64& rng, std::string text) {
    std::size_t n = 1 + rng() % 6;
    for (std::size_t i = 0; i < n && !text.empty(); ++i) {
        std::size_t at = rng() % text.size();
        switch (rng() % 7) {
        case 0:
            text[at] = static_cast<char>(rng() % 256);
            break;
        case 1:
            text.erase(at, 1 + rng() % 20);
            break;
        case 2:
            text.insert(at, std::string(1 + rng() % 5, static_cast<char>(rng() % 256)));
            break;
        case 3: { // cut a line out
            std::size_t a = text.rfind('\n', at);
            std::size_t b = text.find('\n', at);
            a = a == std::string::npos ? 0 : a;
            b = b == std::string::npos ? text.size() : b;
            text.erase(a, b - a);
            break;
        }
        case 4: { // repeat a line
            std::size_t a = text.rfind('\n', at);
            std::size_t b = text.find('\n', at);
            a = a == std::string::npos ? 0 : a;
            b = b == std::string::npos ? text.size() : b;
            text.insert(b, text.substr(a, b - a));
            break;
        }
        case 5:
            text.resize(at);
            break;
        default: {
            static const char* kJunk[] = {
                "\nsection.7 = 1e999 2 x\n",       "\nplan.3 = 1 2\n",
                "\n[sections.0000000000000001]\n", "\nused = -4\n",
                "\nnext_id = 4294967296\n",        "\nsection.1 = 0 1e308\n",
                "\nsection.2 = -5 5 neg\n",        "\nversion = 99\n"};
            text.insert(at, kJunk[rng() % 8]);
            break;
        }
        }
    }
    return text;
}

void check_library(const SectionLibrary& lib) {
    CHECK(lib.songs().size() <= kMaxSongsWithSections);
    std::vector<std::uint64_t> hashes;
    for (const SongSections& s : lib.songs()) {
        CHECK(s.song_hash != 0);
        CHECK(s.well_formed());
        CHECK(s.sections.size() <= kMaxSectionsPerSong);
        CHECK(s.plan.size() <= kMaxPlanItems);
        CHECK((!s.sections.empty() || !s.plan.empty()));
        hashes.push_back(s.song_hash);
    }
    std::sort(hashes.begin(), hashes.end());
    CHECK(std::adjacent_find(hashes.begin(), hashes.end()) == hashes.end());
}

} // namespace

TEST_CASE("sections fuzz: a library survives a write and a read") {
    std::mt19937_64 rng(2024);
    for (int iteration = 0; iteration < 200; ++iteration) {
        CAPTURE(iteration);
        std::vector<Song> songs;
        SectionLibrary lib = random_library(rng, songs);
        SettingsDocument doc;
        write_sections(doc, lib);
        SectionLibrary back = read_sections(SettingsDocument::parse(doc.serialize()));
        REQUIRE(back.songs().size() == lib.songs().size());
        for (std::size_t k = 0; k < songs.size(); ++k) {
            const SongSections* original = lib.find(0x1000 + k * 7919);
            const SongSections* read = back.find(0x1000 + k * 7919);
            REQUIRE(original != nullptr);
            REQUIRE(read != nullptr);
            if (original->sections.empty() && original->plan.empty()) {
                continue;
            }
            SongSections fitted = *read;
            fitted.fit_to_song(songs[k]);
            CHECK(fitted.valid(songs[k]));
            CHECK(fitted == *original);
        }
    }
}

TEST_CASE("sections fuzz: damaged text reads into well-formed sections and writes back stably") {
    std::mt19937_64 rng(77);
    for (int iteration = 0; iteration < 600; ++iteration) {
        CAPTURE(iteration);
        std::vector<Song> songs;
        SectionLibrary lib = random_library(rng, songs);
        SettingsDocument doc;
        write_sections(doc, lib);
        const std::string untouched = "# kept\n[app]\nhit_window_ms = 90\n\n";
        std::string text = mutate(rng, doc.serialize());
        // The damage is to the sections; the other section comes after it, whole.
        if (!text.empty() && text.back() != '\n') {
            text.push_back('\n');
        }
        text += "\n" + untouched;

        std::vector<SettingsIssue> issues;
        SettingsDocument parsed = SettingsDocument::parse(text);
        SectionLibrary read = read_sections(parsed, &issues);
        check_library(read);

        write_sections(parsed, read);
        const std::string once = parsed.serialize();
        SectionLibrary again = read_sections(SettingsDocument::parse(once));
        check_library(again);
        REQUIRE(again.songs().size() == read.songs().size());
        for (const SongSections& s : read.songs()) {
            const SongSections* b = again.find(s.song_hash);
            REQUIRE(b != nullptr);
            CHECK(*b == s);
        }
        // Writing again changes nothing.
        write_sections(parsed, again);
        CHECK(parsed.serialize() == once);
        CHECK(parsed.section_text("app") == "hit_window_ms = 90\n");
    }
}

TEST_CASE("sections fuzz: random bytes are not a crash") {
    std::mt19937_64 rng(31337);
    for (int iteration = 0; iteration < 400; ++iteration) {
        std::string text;
        std::size_t n = rng() % 600;
        for (std::size_t i = 0; i < n; ++i) {
            // Bias toward the characters the format is made of.
            static const char kAlphabet[] = "[]=. \n\nsections0123456789abcdef-+eE#\"\\xplanuvi";
            text.push_back(rng() % 5 == 0 ? static_cast<char>(rng() % 256)
                                          : kAlphabet[rng() % (sizeof kAlphabet - 1)]);
        }
        SectionLibrary lib = read_sections(SettingsDocument::parse(text));
        check_library(lib);
        SettingsDocument doc = SettingsDocument::parse(text);
        write_sections(doc, lib);
        SectionLibrary again = read_sections(SettingsDocument::parse(doc.serialize()));
        CHECK(again.songs().size() == lib.songs().size());
    }
}

TEST_CASE("sections fuzz: unknown keys in an entry are still there after edits and writes") {
    std::mt19937_64 rng(4);
    for (int iteration = 0; iteration < 100; ++iteration) {
        CAPTURE(iteration);
        std::vector<Song> songs;
        SectionLibrary lib = random_library(rng, songs);
        SettingsDocument doc;
        write_sections(doc, lib);
        // A newer build adds keys and comments to every entry.
        for (const std::string& name : sections_section_names(doc)) {
            doc.set_section_text(name,
                                 doc.section_text(name) + "# note " + std::to_string(iteration) +
                                     "\nfuture_" + std::to_string(iteration) + " = " +
                                     std::to_string(rng() % 1000) + "\nsection.1.color = red\n");
        }
        std::vector<SettingsIssue> issues;
        SectionLibrary read = read_sections(doc, &issues);
        // Edit every song, then write.
        for (std::size_t k = 0; k < songs.size(); ++k) {
            SongSections* s = read.edit(0x1000 + k * 7919);
            if (s == nullptr || s->sections.empty()) {
                continue;
            }
            s->fit_to_song(songs[k]);
            s->add_marker(songs[k], songs[k].duration_s * 0.37, true);
        }
        write_sections(doc, read);
        for (const std::string& name : sections_section_names(doc)) {
            std::string text = doc.section_text(name);
            CHECK(text.find("# note " + std::to_string(iteration) + "\n") != std::string::npos);
            CHECK(text.find("future_" + std::to_string(iteration) + " = ") != std::string::npos);
            CHECK(text.find("section.1.color = red\n") != std::string::npos);
        }
    }
}
