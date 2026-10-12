// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/note_trail.hpp"

#include <algorithm>

namespace toonval {

namespace {
double clamp01(double value) { return std::clamp(value, 0.0, 1.0); }
} // namespace

NoteTrail::NoteTrail(std::size_t max_blocks) : max_blocks_(std::max(max_blocks, kMinMaxBlocks)) {}

void NoteTrail::close_open(std::uint8_t note, std::int64_t time_ns) {
    for (auto it = blocks_.rbegin(); it != blocks_.rend(); ++it) {
        if (it->open && it->note == note) {
            it->open = false;
            it->end_ns = std::max(time_ns, it->start_ns);
            return;
        }
    }
}

void NoteTrail::enforce_cap() {
    while (blocks_.size() > max_blocks_) {
        auto oldest = blocks_.end();
        for (auto it = blocks_.begin(); it != blocks_.end(); ++it) {
            if (!it->open && (oldest == blocks_.end() || it->end_ns < oldest->end_ns)) {
                oldest = it;
            }
        }
        if (oldest == blocks_.end()) {
            return; // Only open blocks left, at most one per note.
        }
        blocks_.erase(oldest);
    }
}

void NoteTrail::apply(const KeyTransition& transition, std::int64_t time_ns) {
    switch (transition.type) {
    case KeyTransitionType::Pressed:
        if (!is_valid_note(transition.note)) {
            return;
        }
        close_open(transition.note, time_ns);
        blocks_.push_back({transition.note, transition.velocity, time_ns, 0, true});
        enforce_cap();
        break;
    case KeyTransitionType::Released:
        if (is_valid_note(transition.note)) {
            close_open(transition.note, time_ns);
        }
        break;
    case KeyTransitionType::AllReleased:
        for (TrailBlock& block : blocks_) {
            if (block.open) {
                block.open = false;
                block.end_ns = std::max(time_ns, block.start_ns);
            }
        }
        break;
    case KeyTransitionType::None:
        break;
    }
}

void NoteTrail::prune(std::int64_t now_ns, std::int64_t horizon_ns) {
    std::erase_if(blocks_, [&](const TrailBlock& block) {
        return !block.open && now_ns - block.end_ns > horizon_ns;
    });
}

std::vector<TrailBar> NoteTrail::visible(std::int64_t now_ns, std::int64_t span_ns) const {
    std::vector<TrailBar> bars;
    visible(now_ns, span_ns, bars);
    return bars;
}

void NoteTrail::visible(std::int64_t now_ns, std::int64_t span_ns,
                        std::vector<TrailBar>& bars) const {
    bars.clear();
    if (span_ns <= 0) {
        return;
    }
    const double span = static_cast<double>(span_ns);
    for (const TrailBlock& block : blocks_) {
        const double top = clamp01(static_cast<double>(now_ns - block.start_ns) / span);
        const double bottom =
            block.open ? 0.0 : clamp01(static_cast<double>(now_ns - block.end_ns) / span);
        if (!block.open && (now_ns - block.end_ns >= span_ns || top <= bottom)) {
            continue;
        }
        bars.push_back({block.note, block.velocity, block.open, bottom, top, block.start_ns});
    }
}

} // namespace toonval
