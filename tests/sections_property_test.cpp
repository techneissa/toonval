// SPDX-License-Identifier: GPL-3.0-or-later
// Random edit sequences on random songs: after every edit, successful or refused, the sections obey
// their invariants, a refused edit changed nothing, and ids are never reused. The invariants are
// checked here by hand, not through valid(), and valid() has to agree. A fixed seed makes a failure
// repeat; the iteration and step are printed.
#include "toonval/sections.hpp"

#include "section_fixtures.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace toonval;
using namespace toonval::testing;

namespace {

std::string random_text(std::mt19937_64& rng) {
    std::string s;
    std::size_t n = rng() % 90;
    for (std::size_t i = 0; i < n; ++i) {
        s.push_back(static_cast<char>(rng() % 256));
    }
    return s;
}

// The invariants from sections.hpp, written out independently of SongSections::valid().
void check_invariants(const SongSections& s, const Song& song) {
    CHECK(s.sections.size() <= kMaxSectionsPerSong);
    std::set<SectionId> ids;
    for (std::size_t i = 0; i < s.sections.size(); ++i) {
        const Section& x = s.sections[i];
        CHECK(x.id != kNoSection);
        CHECK(ids.insert(x.id).second);
        CHECK(x.id < s.next_id);
        CHECK(x.end_s - x.start_s >= kMinLoopSeconds);
        CHECK(x.start_s >= 0.0);
        CHECK(x.end_s <= song.duration_s);
        CHECK(!x.name.empty());
        CHECK(x.name.size() <= kMaxSectionNameBytes);
        CHECK(sanitize_section_name(x.name) == x.name);
        if (i == 0) {
            CHECK(x.start_s == 0.0);
        } else {
            CHECK(x.start_s == s.sections[i - 1].end_s);
        }
        if (i + 1 == s.sections.size()) {
            CHECK(x.end_s == song.duration_s);
        }
        if (x.snapped_to_bars) {
            CHECK(x.first_measure >= 0);
            CHECK(x.last_measure >= x.first_measure);
        }
    }
    CHECK(s.plan.size() <= kMaxPlanItems);
    for (const PlanItem& p : s.plan) {
        CHECK(ids.count(p.section) == 1);
        CHECK(p.repeats >= 0);
        CHECK(p.repeats <= kMaxPlanRepeats);
    }
    CHECK(s.valid(song));
}

} // namespace

TEST_CASE("sections property: any edit sequence keeps the sections valid") {
    std::mt19937_64 rng(0x5EC7105);
    for (int iteration = 0; iteration < 120; ++iteration) {
        Song song = random_song(rng);
        const double length = song.duration_s;
        SongSections s;
        s.song_hash = 1;
        SectionId highest_issued = 0;
        for (int step = 0; step < 80; ++step) {
            CAPTURE(iteration);
            CAPTURE(step);
            const SongSections before = s;
            auto random_time = [&] {
                // Mostly inside the song, sometimes on the edges or outside.
                switch (rng() % 12) {
                case 0:
                    return 0.0;
                case 1:
                    return length;
                case 2:
                    return -5.0;
                case 3:
                    return length + 10.0;
                default:
                    return length * static_cast<double>(rng() % 100001) / 100000.0;
                }
            };
            auto random_id = [&]() -> SectionId {
                if (s.sections.empty() || rng() % 10 == 0) {
                    return static_cast<SectionId>(rng() % 50);
                }
                return s.sections[rng() % s.sections.size()].id;
            };
            const bool snap = rng() % 3 != 0;
            EditResult r;
            switch (rng() % 13) {
            case 0:
            case 1:
            case 2:
                r = s.add_marker(song, random_time(), snap);
                break;
            case 3:
                r = s.split(song, random_id(), random_time(), snap);
                break;
            case 4:
                r = s.add_range(song, random_time(), random_time(), snap, random_text(rng));
                break;
            case 5:
                r = s.remove_marker(song, random_time());
                break;
            case 6: {
                std::vector<SectionId> pick;
                std::size_t n = rng() % 5;
                for (std::size_t i = 0; i < n; ++i) {
                    pick.push_back(random_id());
                }
                r = s.merge(song, pick);
                break;
            }
            case 7:
                r = s.erase(song, random_id());
                break;
            case 8:
                r = s.rename(random_id(), random_text(rng));
                break;
            case 9: {
                PlanItem p;
                p.section = random_id();
                p.repeats = static_cast<int>(rng() % 200) - 50;
                p.speed_up = AutoSpeedUp{rng() % 2 == 0, static_cast<int>(rng() % 300) - 50,
                                         static_cast<int>(rng() % 60) - 10,
                                         static_cast<int>(rng() % 300) - 50};
                r = s.plan_append(p);
                break;
            }
            case 10:
                r = s.plan_move(rng() % 8, rng() % 8);
                break;
            case 11:
                r = s.plan_remove(rng() % 8);
                break;
            default: {
                PlanItem p;
                p.section = random_id();
                r = s.plan_set(rng() % 8, p);
                break;
            }
            }
            if (!r.ok) {
                CHECK(r.error != EditError::None);
                REQUIRE(s == before);
            } else {
                CHECK(r.error == EditError::None);
            }
            check_invariants(s, song);
            // Ids only grow: nothing issued before comes back as another section.
            for (const Section& x : s.sections) {
                bool existed = before.find(x.id) != nullptr;
                if (!existed) {
                    CHECK(x.id >= std::max(before.next_id, highest_issued + 1));
                }
                highest_issued = std::max(highest_issued, x.id);
            }
            CHECK(s.next_id >= before.next_id);
        }
    }
}

TEST_CASE("sections property: markers survive and remove_marker undoes add_marker") {
    std::mt19937_64 rng(42);
    for (int iteration = 0; iteration < 60; ++iteration) {
        Song song = random_song(rng);
        SongSections s;
        std::vector<double> placed;
        for (int i = 0; i < 20; ++i) {
            double t = song.duration_s * static_cast<double>(rng() % 10001) / 10000.0;
            if (s.add_marker(song, t).ok) {
                placed.push_back(s.markers().empty() ? t : t);
            }
        }
        // Markers are strictly increasing and every section is long enough.
        std::vector<double> m = s.markers();
        CHECK(std::is_sorted(m.begin(), m.end()));
        CHECK(std::adjacent_find(m.begin(), m.end()) == m.end());
        CHECK(m.size() + 1 == s.sections.size());
        // Removing every marker one by one leaves one whole-song section.
        while (!s.markers().empty()) {
            REQUIRE(s.remove_marker(song, song.duration_s * 0.5).ok);
            CHECK(s.valid(song));
        }
        REQUIRE(s.sections.size() == 1);
        CHECK(s.sections[0].start_s == 0.0);
        CHECK(s.sections[0].end_s == song.duration_s);
    }
}

TEST_CASE("sections property: fit_to_song makes any sections valid for the song") {
    std::mt19937_64 rng(7);
    for (int iteration = 0; iteration < 300; ++iteration) {
        Song song = random_song(rng);
        SongSections s;
        s.next_id = static_cast<SectionId>(rng() % 100);
        std::size_t n = rng() % 12;
        for (std::size_t i = 0; i < n; ++i) {
            Section x;
            x.id = static_cast<SectionId>(rng() % 20);
            x.start_s = (static_cast<double>(rng() % 20001) - 2000.0) / 100.0;
            x.end_s = x.start_s + (static_cast<double>(rng() % 3001) - 100.0) / 100.0;
            x.name = random_text(rng);
            x.custom_name = rng() % 2 == 0;
            s.sections.push_back(x);
        }
        for (std::size_t i = 0; i < rng() % 6; ++i) {
            PlanItem p;
            p.section = static_cast<SectionId>(rng() % 20);
            p.repeats = static_cast<int>(rng() % 300) - 100;
            s.plan.push_back(p);
        }
        s.fit_to_song(song);
        CAPTURE(iteration);
        check_invariants(s, song);
        SongSections again = s;
        again.fit_to_song(song);
        CHECK(again == s); // idempotent
    }
}
