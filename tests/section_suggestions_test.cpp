// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/section_suggestions.hpp"

#include "progress_fixtures.hpp"
#include "section_fixtures.hpp"

#include <doctest/doctest.h>

#include <random>
#include <vector>

using namespace toonval;
using namespace toonval::testing;

namespace {

std::vector<std::pair<int, int>> ranges(const SectionSuggestions& s) {
    std::vector<std::pair<int, int>> out;
    for (const SuggestedSection& x : s.sections) {
        out.emplace_back(x.first_measure, x.last_measure);
    }
    return out;
}

using Ranges = std::vector<std::pair<int, int>>;

// The suggestion covers the song once, in order, with no gap or overlap.
void check_tiles(const SectionSuggestions& s, const Song& song) {
    REQUIRE_FALSE(s.sections.empty());
    CHECK(s.sections.front().start_s == 0.0);
    CHECK(s.sections.back().end_s == song.duration_s);
    for (std::size_t i = 0; i < s.sections.size(); ++i) {
        CHECK(s.sections[i].end_s - s.sections[i].start_s >= kMinSectionSeconds);
        if (i > 0) {
            CHECK(s.sections[i].start_s == s.sections[i - 1].end_s);
            CHECK(s.sections[i].first_measure == s.sections[i - 1].last_measure + 1);
        }
    }
    CHECK(s.sections.size() <= kMaxSectionsPerSong);
}

} // namespace

TEST_CASE("suggest: a song with no history is cut into four-bar phrases") {
    Song song = bars_song(32);
    SectionSuggestions s = suggest_sections(song, {}, 1);
    CHECK_FALSE(s.from_history);
    CHECK(s.phrase_bars == 4);
    REQUIRE(s.sections.size() == 8);
    CHECK(ranges(s) ==
          Ranges{{1, 4}, {5, 8}, {9, 12}, {13, 16}, {17, 20}, {21, 24}, {25, 28}, {29, 32}});
    for (const SuggestedSection& x : s.sections) {
        CHECK(x.kind == SuggestionKind::Phrase);
    }
    CHECK(s.sections[0].start_s == 0.0);
    CHECK(s.sections[0].end_s == 8.0);
    CHECK(s.sections[1].start_s == 8.0);
    check_tiles(s, song);
}

TEST_CASE("suggest: a longer song is cut into eight-bar phrases") {
    Song song = bars_song(40);
    SectionSuggestions s = suggest_sections(song, {}, 1);
    CHECK(s.phrase_bars == 8);
    CHECK(ranges(s) == Ranges{{1, 8}, {9, 16}, {17, 24}, {25, 32}, {33, 40}});
}

TEST_CASE("suggest: the phrase length can be chosen") {
    Song song = bars_song(12);
    SuggestOptions o;
    o.phrase_bars = 3;
    CHECK(ranges(suggest_sections(song, {}, 1, o)) == Ranges{{1, 3}, {4, 6}, {7, 9}, {10, 12}});
    o.phrase_bars = 0;
    CHECK(suggest_sections(song, {}, 1, o).phrase_bars == 4);
    o.phrase_bars = 100;
    CHECK(suggest_sections(bars_song(40), {}, 1, o).phrase_bars == kMaxPhraseBars);
    o.phrase_bars = -3;
    CHECK(suggest_sections(song, {}, 1, o).phrase_bars == 4); // not a length: automatic
}

TEST_CASE("suggest: a short last phrase joins the one before it") {
    Song song = bars_song(17);
    SectionSuggestions s = suggest_sections(song, {}, 1);
    CHECK(ranges(s) == Ranges{{1, 4}, {5, 8}, {9, 12}, {13, 17}}); // bar 17 alone is under half
    check_tiles(s, song);

    Song longer = bars_song(18);
    CHECK(ranges(suggest_sections(longer, {}, 1)) ==
          Ranges{{1, 4}, {5, 8}, {9, 12}, {13, 16}, {17, 18}}); // two bars are enough to stand
}

TEST_CASE("suggest: a song of one bar, or none, gets no suggestion") {
    CHECK(suggest_sections(bars_song(1), {}, 1).sections.empty());
    Song empty;
    CHECK(suggest_sections(empty, {}, 1).sections.empty());
}

TEST_CASE("suggest: a song too short for two sections gets none") {
    Song tiny = bars_song(3, 4000); // a bar lasts under 0.1 s
    CHECK(suggest_sections(tiny, {}, 1).sections.empty());
}

TEST_CASE("suggest: a song that comes out as one section gets no suggestion") {
    Song song = bars_song(5); // 4 + 1: the single bar joins the phrase
    CHECK(suggest_sections(song, {}, 1).sections.empty());
    CHECK(suggest_sections(bars_song(4), {}, 1).sections.empty());
}

TEST_CASE("suggest: a pickup bar belongs to the first phrase") {
    Song song = pickup_song(8);
    SectionSuggestions s = suggest_sections(song, {}, 1);
    CHECK(ranges(s) == Ranges{{0, 4}, {5, 8}});
    check_tiles(s, song);
}

TEST_CASE("suggest: phrases restart at a change of time signature") {
    Song song = meter_change_song(6, 6); // 4/4 for bars 1-6, 3/4 for bars 7-12
    SectionSuggestions s = suggest_sections(song, {}, 1);
    CHECK(ranges(s) == Ranges{{1, 4}, {5, 6}, {7, 10}, {11, 12}});
    check_tiles(s, song);
}

TEST_CASE("suggest: bars of 3/4 and 6/8 count as one bar each") {
    Song waltz = bars_song(16, 120, 3, 4);
    CHECK(ranges(suggest_sections(waltz, {}, 1)) == Ranges{{1, 4}, {5, 8}, {9, 12}, {13, 16}});
    Song jig = bars_song(16, 90, 6, 8);
    CHECK(ranges(suggest_sections(jig, {}, 1)) == Ranges{{1, 4}, {5, 8}, {9, 12}, {13, 16}});
}

// ---- Trouble spots

TEST_CASE("suggest: bars 12-13 with trouble become their own section") {
    Song song = bars_song(20);
    std::vector<RunRecord> runs = {make_bars(1, 20, {12, 13})};
    SectionSuggestions s = suggest_sections(song, runs, 1);
    CHECK(s.from_history);
    check_tiles(s, song);
    const SuggestedSection* trouble = nullptr;
    for (const SuggestedSection& x : s.sections) {
        if (x.kind == SuggestionKind::Trouble) {
            REQUIRE(trouble == nullptr);
            trouble = &x;
        }
    }
    REQUIRE(trouble != nullptr);
    CHECK(trouble->first_measure == 12);
    CHECK(trouble->last_measure == 13);
    CHECK(trouble->start_s == 22.0); // bar 12 starts at 2 * 11
    CHECK(trouble->end_s == 26.0);
    CHECK(trouble->heat == doctest::Approx(0.75));
    CHECK(trouble->reason == TroubleReason::MissedNotes);
    // Phrases fill the rest on the regular grid: 1-4 5-8 9-11 | 12-13 | 14-16 17-20.
    CHECK(ranges(s) == Ranges{{1, 4}, {5, 8}, {9, 11}, {12, 13}, {14, 16}, {17, 20}});
}

TEST_CASE("suggest: wrong presses give a trouble section the other reason") {
    Song song = bars_song(20);
    std::vector<RunRecord> runs = {make_bars(1, 20, {5}, 1, 100, 4)};
    SectionSuggestions s = suggest_sections(song, runs, 1);
    for (const SuggestedSection& x : s.sections) {
        if (x.kind == SuggestionKind::Trouble) {
            CHECK(x.reason == TroubleReason::WrongNotes);
        }
    }
    CHECK(s.from_history);
}

TEST_CASE("suggest: several spots, each its own section, in time order") {
    Song song = bars_song(40);
    std::vector<RunRecord> runs = {make_bars(1, 40, {6, 7, 30})};
    SectionSuggestions s = suggest_sections(song, runs, 1);
    check_tiles(s, song);
    std::vector<std::pair<int, int>> trouble;
    for (const SuggestedSection& x : s.sections) {
        if (x.kind == SuggestionKind::Trouble) {
            trouble.emplace_back(x.first_measure, x.last_measure);
        }
    }
    CHECK(trouble == Ranges{{6, 7}, {30, 30}});
}

TEST_CASE("suggest: a spot at the very start or end of the song leaves no empty phrase") {
    Song song = bars_song(20);
    std::vector<RunRecord> runs = {make_bars(1, 20, {1, 2, 19, 20})};
    SectionSuggestions s = suggest_sections(song, runs, 1);
    check_tiles(s, song);
    CHECK(s.sections.front().kind == SuggestionKind::Trouble);
    CHECK(s.sections.front().first_measure == 1);
    CHECK(s.sections.back().kind == SuggestionKind::Trouble);
    CHECK(s.sections.back().last_measure == 20);
}

TEST_CASE("suggest: history without trouble falls back to phrases") {
    Song song = bars_song(16);
    std::vector<RunRecord> runs;
    for (int i = 0; i < 5; ++i) {
        runs.push_back(make_bars(1, 16, {}));
    }
    SectionSuggestions s = suggest_sections(song, runs, 1);
    CHECK_FALSE(s.from_history);
    CHECK(ranges(s) == Ranges{{1, 4}, {5, 8}, {9, 12}, {13, 16}});
}

TEST_CASE("suggest: runs of other songs are not history") {
    Song song = bars_song(16);
    std::vector<RunRecord> runs = {make_bars(2, 16, {6, 7})};
    SectionSuggestions s = suggest_sections(song, runs, 1);
    CHECK_FALSE(s.from_history);
    CHECK(suggest_sections(song, runs, 0).from_history == false);
}

TEST_CASE("suggest: a spot in bars the song does not have is ignored") {
    Song song = bars_song(10);
    std::vector<RunRecord> runs = {make_bars(1, 20, {15, 16})}; // a longer song, same hash
    SectionSuggestions s = suggest_sections(song, runs, 1);
    CHECK_FALSE(s.from_history);
    check_tiles(s, song);
}

TEST_CASE("suggest: too many pieces make the phrases longer") {
    Song song = bars_song(300);
    SuggestOptions o;
    o.phrase_bars = 2;
    SectionSuggestions s = suggest_sections(song, {}, 1, o);
    check_tiles(s, song);
    CHECK(s.phrase_bars > 2);
    CHECK(s.sections.size() <= kMaxSectionsPerSong);
}

// ---- Applying

TEST_CASE("suggest: applying the proposal makes valid sections with the bar names") {
    Song song = bars_song(20);
    std::vector<RunRecord> runs = {make_bars(1, 20, {12, 13})};
    SectionSuggestions s = suggest_sections(song, runs, 1);
    SongSections sections;
    sections.song_hash = 1;
    int placed = apply_suggestions(sections, song, s);
    CHECK(placed == static_cast<int>(s.sections.size()) - 1);
    REQUIRE(sections.valid(song));
    REQUIRE(sections.sections.size() == s.sections.size());
    for (std::size_t i = 0; i < s.sections.size(); ++i) {
        CHECK(sections.sections[i].start_s == s.sections[i].start_s);
        CHECK(sections.sections[i].end_s == s.sections[i].end_s);
        CHECK(sections.sections[i].first_measure == s.sections[i].first_measure);
        CHECK(sections.sections[i].snapped_to_bars);
    }
    CHECK(sections.sections[3].name == "Bars 12-13");
}

TEST_CASE("suggest: applying only the trouble spots leaves the rest in big sections") {
    Song song = bars_song(20);
    std::vector<RunRecord> runs = {make_bars(1, 20, {12, 13})};
    SectionSuggestions s = suggest_sections(song, runs, 1);
    SongSections sections;
    sections.song_hash = 1;
    CHECK(apply_suggestions(sections, song, s, true) == 2);
    REQUIRE(sections.valid(song));
    REQUIRE(sections.sections.size() == 3);
    CHECK(sections.sections[0].name == "Bars 1-11");
    CHECK(sections.sections[1].name == "Bars 12-13");
    CHECK(sections.sections[2].name == "Bars 14-20");
}

TEST_CASE("suggest: applying twice places nothing new") {
    Song song = bars_song(16);
    SectionSuggestions s = suggest_sections(song, {}, 1);
    SongSections sections;
    apply_suggestions(sections, song, s);
    SongSections once = sections;
    CHECK(apply_suggestions(sections, song, s) == 0);
    CHECK(sections == once);
}

TEST_CASE("suggest: applying nothing changes nothing") {
    Song song = bars_song(16);
    SongSections sections;
    CHECK(apply_suggestions(sections, song, SectionSuggestions{}) == 0);
    CHECK(sections.empty());
}

TEST_CASE("suggest property: any song and history gives a tiling that applies cleanly") {
    std::mt19937_64 rng(5);
    for (int iteration = 0; iteration < 150; ++iteration) {
        int bars = 2 + static_cast<int>(rng() % 120);
        Song song =
            bars_song(bars, 60 + static_cast<int>(rng() % 120), 2 + static_cast<int>(rng() % 4), 4);
        std::vector<int> bad;
        for (int i = 0; i < 6; ++i) {
            bad.push_back(1 + static_cast<int>(rng() % static_cast<unsigned>(bars)));
        }
        std::vector<RunRecord> runs;
        for (std::size_t i = 0; i < rng() % 4; ++i) {
            runs.push_back(make_bars(1, bars, bad, 1 + static_cast<int>(rng() % 4)));
        }
        SuggestOptions o;
        o.phrase_bars = static_cast<int>(rng() % 20);
        SectionSuggestions s = suggest_sections(song, runs, 1, o);
        CAPTURE(iteration);
        CAPTURE(bars);
        if (s.sections.empty()) {
            continue;
        }
        check_tiles(s, song);
        SongSections sections;
        apply_suggestions(sections, song, s);
        CHECK(sections.valid(song));
        CHECK(sections.sections.size() == s.sections.size());
    }
}
