// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/octave_shift.hpp"

#include <algorithm>
#include <cstddef>

namespace toonval {

void OctaveShiftDetector::set_range(KeyRange range) {
    range_ = range;
    reset();
}

void OctaveShiftDetector::reset() {
    head_ = 0;
    count_ = 0;
}

void OctaveShiftDetector::observe(int pitch) { observe(pitch, {}); }

void OctaveShiftDetector::observe(int pitch, std::span<const int> expected) {
    if (pitch < 0 || pitch > 127) {
        return;
    }
    Entry& e = ring_[static_cast<std::size_t>(head_)];
    e = Entry{};
    e.pitch = static_cast<std::uint8_t>(pitch);
    for (int p : expected) {
        if (e.expected_count >= kOctaveShiftMaxExpected) {
            break;
        }
        if (p >= 0 && p <= 127) {
            e.expected[e.expected_count++] = static_cast<std::uint8_t>(p);
        }
    }
    head_ = (head_ + 1) % kOctaveShiftWindow;
    count_ = std::min(count_ + 1, kOctaveShiftWindow);
}

const OctaveShiftDetector::Entry& OctaveShiftDetector::at(int i) const {
    int slot = (head_ - count_ + i + 2 * kOctaveShiftWindow) % kOctaveShiftWindow;
    return ring_[static_cast<std::size_t>(slot)];
}

ShiftResult OctaveShiftDetector::result() const {
    ShiftResult song = song_result();
    if (song.status != ShiftStatus::NotSure) {
        return song;
    }
    ShiftResult range = range_result();
    if (range.status != ShiftStatus::NotSure) {
        return range;
    }
    ShiftResult none;
    none.notes_considered = count_;
    return none;
}

ShiftResult OctaveShiftDetector::song_result() const {
    ShiftResult out;
    out.notes_considered = count_;

    // support[k + max] = played notes for which pitch + k is an expected pitch.
    constexpr int kOffsets = 2 * kOctaveShiftMaxOffset + 1;
    std::array<int, kOffsets> support{};
    int with_expectation = 0;
    for (int i = 0; i < count_; ++i) {
        const Entry& e = at(i);
        if (e.expected_count == 0) {
            continue;
        }
        ++with_expectation;
        for (int k = 0; k < e.expected_count; ++k) {
            int offset = static_cast<int>(e.expected[static_cast<std::size_t>(k)]) - e.pitch;
            if (offset >= -kOctaveShiftMaxOffset && offset <= kOctaveShiftMaxOffset) {
                // An expected pitch listed twice must count once for this note.
                bool seen = false;
                for (int j = 0; j < k; ++j) {
                    seen = seen || e.expected[static_cast<std::size_t>(j)] ==
                                       e.expected[static_cast<std::size_t>(k)];
                }
                if (!seen) {
                    ++support[static_cast<std::size_t>(offset + kOctaveShiftMaxOffset)];
                }
            }
        }
    }
    if (with_expectation < kOctaveShiftMinSongNotes) {
        return out;
    }

    int best = 0;
    for (int i = 1; i < kOffsets; ++i) {
        if (support[static_cast<std::size_t>(i)] > support[static_cast<std::size_t>(best)]) {
            best = i;
        }
    }
    int runner_up = 0;
    for (int i = 0; i < kOffsets; ++i) {
        if (i != best) {
            runner_up = std::max(runner_up, support[static_cast<std::size_t>(i)]);
        }
    }
    double n = with_expectation;
    double best_share = support[static_cast<std::size_t>(best)] / n;
    double margin = (support[static_cast<std::size_t>(best)] - runner_up) / n;
    if (best_share < kOctaveShiftSongShare || margin < kOctaveShiftSongMargin) {
        return out;
    }
    int offset = best - kOctaveShiftMaxOffset;
    out.status = offset == 0 ? ShiftStatus::NoShift : ShiftStatus::Shift;
    out.offset_semitones = offset;
    out.confidence = best_share;
    out.evidence = ShiftEvidence::Song;
    return out;
}

ShiftResult OctaveShiftDetector::range_result() const {
    ShiftResult out;
    out.notes_considered = count_;
    if (count_ < kOctaveShiftMinRangeNotes) {
        return out;
    }
    int outside = 0;
    for (int i = 0; i < count_; ++i) {
        outside += range_.contains(at(i).pitch) ? 0 : 1;
    }
    double n = count_;
    if (outside / n < kOctaveShiftOutsideShare) {
        return out;
    }
    // The smallest whole-octave shift that brings nearly every note inside.
    for (int octaves = 1; octaves <= kOctaveShiftMaxOffset / 12; ++octaves) {
        for (int sign : {-1, 1}) {
            int offset = sign * 12 * octaves;
            int inside = 0;
            for (int i = 0; i < count_; ++i) {
                inside += range_.contains(at(i).pitch + offset) ? 1 : 0;
            }
            if (inside / n >= kOctaveShiftFitShare) {
                out.status = ShiftStatus::Shift;
                out.offset_semitones = offset;
                out.confidence = inside / n;
                out.evidence = ShiftEvidence::Range;
                return out;
            }
        }
    }
    return out;
}

} // namespace toonval
