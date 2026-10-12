// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/song.hpp"

#include <algorithm>
#include <initializer_list>
#include <string>

namespace toonval {

double TempoMap::seconds_at(std::int64_t tick) const {
    auto it = std::upper_bound(changes.begin(), changes.end(), tick,
                               [](std::int64_t t, const TempoChange& c) { return t < c.tick; });
    const TempoChange& c = it == changes.begin() ? changes.front() : *(it - 1);
    return c.seconds +
           static_cast<double>(tick - c.tick) * c.us_per_quarter / 1e6 / ticks_per_quarter;
}

double TempoMap::tick_at(double seconds) const {
    auto it = std::upper_bound(changes.begin(), changes.end(), seconds,
                               [](double s, const TempoChange& c) { return s < c.seconds; });
    const TempoChange& c = it == changes.begin() ? changes.front() : *(it - 1);
    return static_cast<double>(c.tick) +
           (seconds - c.seconds) * 1e6 * ticks_per_quarter / c.us_per_quarter;
}

int Song::measure_at(std::int64_t tick) const {
    if (measures.empty()) {
        return 1;
    }
    auto it = std::upper_bound(measures.begin(), measures.end(), tick,
                               [](std::int64_t t, const Measure& m) { return t < m.start_tick; });
    return it == measures.begin() ? measures.front().number : (it - 1)->number;
}

namespace {

struct NameMatch {
    Hand hand = Hand::Unknown;
    bool only_bass = false; // the name says Left only through the word "bass"
};

bool is_one_of(std::string_view word, std::initializer_list<std::string_view> words) {
    return std::find(words.begin(), words.end(), word) != words.end();
}

NameMatch match_name(std::string_view name) {
    // Lower-case words, split on anything that isn't a letter or digit.
    std::vector<std::string> words(1);
    for (char c : name) {
        bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (alnum) {
            words.back() += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        } else if (!words.back().empty()) {
            words.emplace_back();
        }
    }
    if (words.back().empty()) {
        words.pop_back();
    }
    bool context = false; // "Piano R": a lone R or L only counts next to a word like this
    for (const std::string& w : words) {
        context = context || is_one_of(w, {"piano", "pno", "pf", "klavier", "hand"});
    }
    bool right = false;
    bool left = false;
    bool bass = false;
    for (std::size_t i = 0; i < words.size(); ++i) {
        const std::string& w = words[i];
        bool next_h = i + 1 < words.size() && words[i + 1] == "h"; // "R.H."
        if (is_one_of(w, {"right", "rh", "righthand", "treble", "upper"}) ||
            (w == "r" && (next_h || context))) {
            right = true;
        }
        if (is_one_of(w, {"left", "lh", "lefthand", "lower"}) ||
            (w == "l" && (next_h || context))) {
            left = true;
        }
        bass = bass || w == "bass";
    }
    if (right && (left || bass)) {
        return {};
    }
    if (right) {
        return {Hand::Right, false};
    }
    if (left || bass) {
        return {Hand::Left, !left};
    }
    return {};
}

bool is_gm_bass(std::uint8_t program) { return program >= 32 && program <= 39; }

} // namespace

Hand hand_from_name(std::string_view name) { return match_name(name).hand; }

void assign_hands_from_names(Song& song) {
    for (Part& part : song.parts) {
        NameMatch m = match_name(part.name);
        if (m.hand == Hand::Unknown) {
            m = match_name(part.instrument_name);
        }
        if (part.drum || (m.only_bass && part.has_program && is_gm_bass(part.program))) {
            m = {};
        }
        part.hand = m.hand;
    }
    for (Note& note : song.notes) {
        note.hand = Hand::Unknown;
        if (note.drum) {
            continue;
        }
        for (const Part& part : song.parts) {
            if (part.track == note.track && part.channel == note.channel) {
                note.hand = part.hand;
                break;
            }
        }
    }
}

} // namespace toonval
