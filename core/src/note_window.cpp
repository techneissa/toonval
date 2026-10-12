// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/note_window.hpp"

#include <algorithm>
#include <limits>
#include <numeric>

namespace toonval {

void NoteWindow::build(const Song& song) {
    std::vector<std::uint32_t> order(song.notes.size());
    std::iota(order.begin(), order.end(), std::uint32_t{0});
    std::stable_sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) {
        return song.notes[a].start_s < song.notes[b].start_s;
    });
    entries_.clear();
    entries_.reserve(order.size());
    for (std::uint32_t i : order) {
        const Note& n = song.notes[i];
        entries_.push_back({n.start_s, n.start_s + n.duration_s, i, n.pitch, n.hand, n.drum});
    }
    max_end_.assign(entries_.size(), 0.0);
    build_range(0, entries_.size());
}

// The tree is implicit: the node of a range [first, last) is its middle entry.
double NoteWindow::build_range(std::size_t first, std::size_t last) {
    if (first >= last) {
        return -std::numeric_limits<double>::infinity();
    }
    std::size_t mid = first + (last - first) / 2;
    double best =
        std::max({entries_[mid].end_s, build_range(first, mid), build_range(mid + 1, last)});
    max_end_[mid] = best;
    return best;
}

void NoteWindow::visit(std::size_t first, std::size_t last, const Query& q,
                       std::vector<VisibleNote>& out, std::size_t& examined) const {
    if (first >= last) {
        return;
    }
    std::size_t mid = first + (last - first) / 2;
    ++examined;
    if (max_end_[mid] <= q.lo) {
        return; // nothing under this node ends after the window starts
    }
    visit(first, mid, q, out, examined);
    const Entry& e = entries_[mid];
    if (e.start_s >= q.hi) {
        return; // this note and every later one start above the window
    }
    if (e.end_s > q.lo && (q.options.include_drums || !e.drum)) {
        bool hand_ok = q.options.hand == HandFilter::Both ||
                       e.hand == (q.options.hand == HandFilter::Left ? Hand::Left : Hand::Right);
        if (hand_ok) {
            const TransposedPitch key = q.options.transposition.map_note(e.pitch, e.drum);
            out.push_back({e.index, (e.start_s - q.position) / q.options.ahead_s,
                           (e.end_s - q.position) / q.options.ahead_s, key.pitch, key.folded,
                           key.fold_octaves, key.playable});
        }
    }
    visit(mid + 1, last, q, out, examined);
}

std::size_t NoteWindow::query(double position_s, const NoteWindowOptions& options,
                              std::vector<VisibleNote>& out) const {
    out.clear();
    if (!(options.ahead_s > 0.0) || entries_.empty()) {
        return 0;
    }
    const Query q{position_s - std::max(0.0, options.behind_s), position_s + options.ahead_s,
                  position_s, options};
    std::size_t examined = 0;
    visit(0, entries_.size(), q, out, examined);
    return examined;
}

} // namespace toonval
