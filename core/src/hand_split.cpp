// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/hand_split.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace toonval {

namespace {

constexpr double kMinClusterShare = 0.15;
constexpr double kMinClusterSeparation = 8.0;
constexpr double kSideBias = 1.0;
constexpr double kInitialSpread = 6.0;
constexpr int kLloydIterations = 6;

// Notes that start together: indices into song.notes, sorted by pitch.
struct Chord {
    std::vector<std::size_t> notes;
    std::int64_t start_tick = 0;
};

// Pitch histogram of the unknown notes inside a window of the song.
struct Histogram {
    std::array<int, 128> count{};
    int total = 0;
    void add(int pitch) {
        ++count[static_cast<std::size_t>(pitch)];
        ++total;
    }
    void remove(int pitch) {
        --count[static_cast<std::size_t>(pitch)];
        --total;
    }
};

// The two registers the hands play in: mean pitch of the low and of the high group.
struct Registers {
    double low = 0.0;
    double high = 0.0;
};

// True when `pitch` belongs to the high register: it is nearer to the high centre, with a small
// pull toward the side of the split point it lies on, so the split point still counts for a note
// that sits between the centres. A tie goes by the split point.
bool in_high_register(double pitch, const Registers& r, int split) {
    double to_low = std::abs(pitch - r.low) + (pitch >= split ? kSideBias : 0.0);
    double to_high = std::abs(pitch - r.high) + (pitch < split ? kSideBias : 0.0);
    return to_high < to_low || (to_high == to_low && pitch >= split);
}

// Fits two registers to the window by k-means (Lloyd's iterations), warm-started from the
// registers of the previous chord, so a passage keeps the groups it already had instead of jumping
// to a different grouping of the same pitches. Returns false when the window does not look like
// two registers (see hand_split.hpp); `fit` is then untouched.
bool fit_registers(const Histogram& h, const Registers& start, int split, Registers& fit) {
    if (h.total < 4) {
        return false;
    }
    Registers r = start;
    int low_n = 0;
    int high_n = 0;
    for (int iteration = 0; iteration < kLloydIterations; ++iteration) {
        double low_sum = 0.0;
        double high_sum = 0.0;
        low_n = 0;
        high_n = 0;
        for (int p = 0; p < 128; ++p) {
            int c = h.count[static_cast<std::size_t>(p)];
            if (c == 0) {
                continue;
            }
            if (in_high_register(p, r, split)) {
                high_sum += static_cast<double>(c) * p;
                high_n += c;
            } else {
                low_sum += static_cast<double>(c) * p;
                low_n += c;
            }
        }
        Registers next{low_n > 0 ? low_sum / low_n : r.low,
                       high_n > 0 ? high_sum / high_n : r.high};
        bool same = next.low == r.low && next.high == r.high;
        r = next;
        if (same) {
            break;
        }
    }
    int min_side = std::max(2, static_cast<int>(std::ceil(kMinClusterShare * h.total)));
    if (low_n < min_side || high_n < min_side || r.high - r.low < kMinClusterSeparation) {
        return false;
    }
    fit = r;
    return true;
}

struct Line {
    bool valid = false;
    int pitch = 0;
    std::int64_t tick = 0;
};

// Where to cut a chord wider than a hand: the first note of the right hand part.
std::size_t cut_chord(const Song& song, const std::vector<std::size_t>& sorted_notes,
                      double boundary) {
    auto pitch = [&](std::size_t k) { return static_cast<int>(song.notes[sorted_notes[k]].pitch); };
    std::size_t n = sorted_notes.size();
    std::size_t best = 1;
    double best_cost = 0.0;
    bool best_reachable = false;
    bool have = false;
    for (std::size_t j = 1; j < n; ++j) { // lower part: [0, j), upper part: [j, n)
        bool reachable = pitch(j - 1) - pitch(0) <= kHandReachSemitones &&
                         pitch(n - 1) - pitch(j) <= kHandReachSemitones;
        double midpoint = (pitch(j - 1) + pitch(j)) / 2.0;
        double cost = std::abs(midpoint - boundary);
        bool better = !have || (reachable && !best_reachable) ||
                      (reachable == best_reachable && cost < best_cost);
        if (better) {
            have = true;
            best = j;
            best_cost = cost;
            best_reachable = reachable;
        }
    }
    return best;
}

double median_pitch(const Song& song, const std::vector<std::size_t>& sorted_notes) {
    std::size_t n = sorted_notes.size();
    auto p = [&](std::size_t k) { return static_cast<double>(song.notes[sorted_notes[k]].pitch); };
    return n % 2 == 1 ? p(n / 2) : (p(n / 2 - 1) + p(n / 2)) / 2.0;
}

} // namespace

bool needs_hand_split(const Song& song) {
    return std::any_of(song.notes.begin(), song.notes.end(),
                       [](const Note& n) { return !n.drum && n.hand == Hand::Unknown; });
}

std::vector<Hand> split_hands(const Song& song, const HandSplitOptions& options) {
    std::vector<Hand> hands(song.notes.size(), Hand::Unknown);
    std::vector<std::size_t> candidates;
    for (std::size_t i = 0; i < song.notes.size(); ++i) {
        const Note& n = song.notes[i];
        if (n.drum) {
            continue;
        }
        hands[i] = n.hand;
        if (n.hand == Hand::Unknown) {
            candidates.push_back(i);
        }
    }
    if (candidates.empty()) {
        return hands;
    }
    const bool smart = options.method == SplitMethod::Smart;
    const int split = std::clamp(options.split_point, 1, 127);
    const std::int64_t ticks_per_beat = std::max(1, song.tempo.ticks_per_quarter);

    // Chords: unknown notes starting within the chord window of the chord's first note.
    std::vector<Chord> chords;
    for (std::size_t i : candidates) {
        const Note& n = song.notes[i];
        if (chords.empty() ||
            n.start_s - song.notes[chords.back().notes.front()].start_s > kChordWindowSeconds) {
            chords.push_back(Chord{{}, n.start_tick});
        }
        chords.back().notes.push_back(i);
    }
    for (Chord& c : chords) {
        std::stable_sort(c.notes.begin(), c.notes.end(), [&](std::size_t a, std::size_t b) {
            return song.notes[a].pitch < song.notes[b].pitch;
        });
    }

    // Window over the candidate notes, advanced chord by chord.
    Histogram window;
    std::size_t win_lo = 0; // first candidate in the window
    std::size_t win_hi = 0; // one past the last candidate in the window
    const double beat = static_cast<double>(ticks_per_beat);
    const std::int64_t before_ticks = static_cast<std::int64_t>(kSplitWindowBeatsBefore * beat);
    const std::int64_t after_ticks = static_cast<std::int64_t>(kSplitWindowBeatsAfter * beat);
    Registers registers{split - kInitialSpread, split + kInitialSpread};
    Line last_left;
    Line last_right;

    for (const Chord& chord : chords) {
        bool two_registers = false;
        if (smart) {
            while (win_hi < candidates.size() &&
                   song.notes[candidates[win_hi]].start_tick <= chord.start_tick + after_ticks) {
                window.add(song.notes[candidates[win_hi]].pitch);
                ++win_hi;
            }
            while (win_lo < win_hi &&
                   song.notes[candidates[win_lo]].start_tick < chord.start_tick - before_ticks) {
                window.remove(song.notes[candidates[win_lo]].pitch);
                ++win_lo;
            }
            Registers fit;
            if (fit_registers(window, registers, split, fit)) {
                registers = fit;
                two_registers = true;
            }
        }
        // The pitch that divides the hands here, and which side a pitch is on.
        const double boundary = two_registers ? (registers.low + registers.high) / 2.0 : split;
        auto is_right = [&](double pitch) {
            return two_registers ? in_high_register(pitch, registers, split) : pitch >= split;
        };

        const std::vector<std::size_t>& notes = chord.notes;
        int lowest = song.notes[notes.front()].pitch;
        int highest = song.notes[notes.back()].pitch;
        std::size_t cut = 0; // notes before `cut` are the left hand
        if (highest - lowest > kHandReachSemitones) {
            cut = cut_chord(song, notes, boundary);
        } else {
            double middle = median_pitch(song, notes);
            Hand whole = is_right(middle) ? Hand::Right : Hand::Left;
            if (smart && !two_registers && std::abs(middle - boundary) <= kHysteresisSemitones) {
                // Near the boundary: stay with the hand whose line this chord continues.
                auto distance = [&](const Line& line) {
                    if (!line.valid ||
                        static_cast<double>(chord.start_tick - line.tick) > kFollowBeats * beat) {
                        return 1000;
                    }
                    int d = std::min(std::abs(lowest - line.pitch), std::abs(highest - line.pitch));
                    return d <= kFollowSemitones ? d : 1000;
                };
                int dl = distance(last_left);
                int dr = distance(last_right);
                if (dl < 1000 || dr < 1000) {
                    whole = dr < dl ? Hand::Right : (dl < dr ? Hand::Left : whole);
                }
            }
            cut = whole == Hand::Left ? notes.size() : 0;
        }

        double left_sum = 0.0;
        double right_sum = 0.0;
        int left_n = 0;
        int right_n = 0;
        for (std::size_t k = 0; k < notes.size(); ++k) {
            Hand h = k < cut ? Hand::Left : Hand::Right;
            hands[notes[k]] = h;
            double p = song.notes[notes[k]].pitch;
            (h == Hand::Left ? left_sum : right_sum) += p;
            ++(h == Hand::Left ? left_n : right_n);
        }
        if (left_n > 0) {
            last_left =
                Line{true, static_cast<int>(std::lround(left_sum / left_n)), chord.start_tick};
        }
        if (right_n > 0) {
            last_right =
                Line{true, static_cast<int>(std::lround(right_sum / right_n)), chord.start_tick};
        }
    }
    return hands;
}

Song with_split_hands(const Song& song, const HandSplitOptions& options) {
    Song out = song;
    std::vector<Hand> hands = split_hands(song, options);
    for (std::size_t i = 0; i < out.notes.size(); ++i) {
        out.notes[i].hand = hands[i];
    }
    for (Part& part : out.parts) {
        if (part.drum) {
            continue;
        }
        Hand common = Hand::Unknown;
        bool mixed = false;
        for (const Note& n : out.notes) {
            if (n.drum || n.track != part.track || n.channel != part.channel) {
                continue;
            }
            if (common == Hand::Unknown) {
                common = n.hand;
            } else if (n.hand != common) {
                mixed = true;
            }
        }
        part.hand = mixed ? Hand::Unknown : common;
    }
    return out;
}

HandAgreement compare_hands(const Song& truth, std::span<const Hand> assigned) {
    HandAgreement out;
    std::size_t n = std::min(truth.notes.size(), assigned.size());
    for (std::size_t i = 0; i < n; ++i) {
        Hand want = truth.notes[i].hand;
        if (truth.notes[i].drum || want == Hand::Unknown) {
            continue;
        }
        ++out.compared;
        out.matching += assigned[i] == want ? 1 : 0;
    }
    return out;
}

} // namespace toonval
