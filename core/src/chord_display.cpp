// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/chord_display.hpp"

#include "toonval/chord.hpp"
#include "toonval/pitch.hpp"

#include <algorithm>
#include <array>

namespace toonval {

ChordDisplay::ChordDisplay(ChordDisplayConfig config) : config_(config) {
    config_.settle_ns = std::max<std::int64_t>(config_.settle_ns, 0);
    config_.hold_ns = std::max<std::int64_t>(config_.hold_ns, 0);
    config_.fade_ns = std::clamp<std::int64_t>(config_.fade_ns, 0, config_.hold_ns);
}

void ChordDisplay::reset() {
    candidate_.clear();
    candidate_since_ns_ = 0;
    started_ = false;
    evaluated_ = false;
    text_.clear();
    live_ = false;
    ended_ns_ = 0;
}

ChordLabel ChordDisplay::update(std::span<const std::uint8_t> held_notes, std::int64_t now_ns) {
    if (!started_ || !std::ranges::equal(held_notes, candidate_)) {
        candidate_.assign(held_notes.begin(), held_notes.end());
        candidate_since_ns_ = now_ns;
        started_ = true;
        evaluated_ = false;
    }

    if (!evaluated_ && now_ns - candidate_since_ns_ >= config_.settle_ns) {
        evaluated_ = true;
        std::array<int, kMidiNoteMax + 1> notes{};
        const std::size_t count = std::min(candidate_.size(), notes.size());
        std::copy_n(candidate_.begin(), count, notes.begin());
        const Chord chord = detect_chord(notes.data(), count);
        if (chord.found) {
            text_ = chord_name(chord, config_.prefer_flats);
            live_ = true;
        } else if (live_) {
            live_ = false;
            ended_ns_ = candidate_since_ns_;
        }
    }

    if (text_.empty()) {
        return {};
    }
    if (live_) {
        return {text_, 1.0};
    }
    const std::int64_t age = std::max<std::int64_t>(now_ns - ended_ns_, 0);
    if (age >= config_.hold_ns) {
        text_.clear();
        return {};
    }
    const std::int64_t fade_start = config_.hold_ns - config_.fade_ns;
    if (age < fade_start) {
        return {text_, 1.0};
    }
    const double remaining = static_cast<double>(config_.hold_ns - age);
    return {text_, remaining / static_cast<double>(config_.fade_ns)};
}

} // namespace toonval
