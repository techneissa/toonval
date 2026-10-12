// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/keyboard_view.hpp"

#include "toonval/pitch.hpp"

#include <algorithm>
#include <cmath>

namespace toonval {
namespace {

constexpr int kStepOfPitchClass[12] = {0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6};
constexpr int kPitchClassOfStep[7] = {0, 2, 4, 5, 7, 9, 11};

constexpr float kFallbackWidthPx = 1280.0f;
constexpr int kMaxMarginWhites = 3;
constexpr double kRankEpsilon = 1e-6;    // frame(): a position this close to a key edge is on it
constexpr double kSettleDistance = 1e-3; // white keys
constexpr double kSettleSpeed = 1e-2;    // white keys per second

// A run of white keys, counted from the keyboard's lowest white key (0).
struct Extent {
    bool any = false;
    int lo = 0;
    int hi = 0;

    int width() const { return hi - lo + 1; }
    void add(int a, int b) {
        if (!any) {
            any = true;
            lo = a;
            hi = b;
        } else {
            lo = std::min(lo, a);
            hi = std::max(hi, b);
        }
    }
    void add(const Extent& e) {
        if (e.any) {
            add(e.lo, e.hi);
        }
    }
};

// The white keys a note needs to be fully shown: its own, and for a black key the one above too.
bool add_note(Extent& e, int note, const KeyRange& kb, int base) {
    if (!kb.contains(note)) {
        return false;
    }
    const int r = white_rank(note) - base;
    e.add(r, r + (is_black_key(note) ? 1 : 0));
    return true;
}

Extent extent_of(std::span<const int> notes, const KeyRange& kb, int base) {
    Extent e;
    for (const int n : notes) {
        add_note(e, n, kb, base);
    }
    return e;
}

int clamp_int(int v, int lo, int hi) { return std::max(lo, std::min(v, hi)); }

KeyRange range_of(int base, int left, int whites) {
    return {note_of_rank(base + left), note_of_rank(base + left + whites - 1)};
}

// Where the `n`-white-key window starts so `focus` is inside it with a margin from each edge,
// moving as little as possible from `current_left`/`current_w` (or centred on the focus when there
// is no current window). Result is inside 0..total-n.
int place(int n, int total, const Extent& focus, bool has_current, int current_left, int current_w,
          int fallback_center) {
    // The window the caller had, resized around its own middle.
    int desired = has_current
                      ? current_left + (current_w - n) / 2
                      : (focus.any ? focus.lo + (focus.width() - n) / 2 : fallback_center - n / 2);
    if (focus.any) {
        const int extra = n - focus.width();
        const int margin = std::min(clamp_int(n / 8, 1, kMaxMarginWhites), extra / 2);
        const int lowest_left = focus.hi + margin - n + 1;
        const int highest_left = focus.lo - margin;
        desired = clamp_int(desired, lowest_left, highest_left);
    }
    return clamp_int(desired, 0, std::max(0, total - n));
}

} // namespace

float key_size_white_mm(KeySize size) {
    switch (size) {
    case KeySize::Small:
        return kSmallKeyMm;
    case KeySize::Medium:
        return kMediumKeyMm;
    case KeySize::Large:
        return kLargeKeyMm;
    case KeySize::ExtraLarge:
        return kExtraLargeKeyMm;
    case KeySize::FitKeyboard:
    case KeySize::FitSong:
        break;
    }
    return 0.0f;
}

int white_rank(int note) {
    const int n = clamp_int(note, kMidiNoteMin, kMidiNoteMax);
    return 7 * (n / 12) + kStepOfPitchClass[n % 12];
}

int note_of_rank(int rank) {
    if (rank < 0) {
        return -1;
    }
    const int note = 12 * (rank / 7) + kPitchClassOfStep[rank % 7];
    return note > kMidiNoteMax ? -1 : note;
}

KeyViewTarget compute_key_view(const KeyViewParams& p) {
    const KeyboardLayout layout(p.keyboard);
    const KeyRange kb = layout.range();
    const int base = white_rank(kb.lowest);
    const int total = layout.white_key_count();
    KeyViewTarget out;
    out.range = kb;

    const bool fit_song = p.size == KeySize::FitSong;
    const float mm = key_size_white_mm(p.size);
    const bool fixed = mm > 0.0f;
    if (!fit_song && !fixed) {
        return out; // FitKeyboard, and any stray value
    }

    const Extent upcoming = extent_of(p.upcoming, kb, base);
    const Extent held = extent_of(p.held, kb, base);

    int n = total;
    Extent focus;
    if (fit_song) {
        if (!p.song || p.song->highest < kb.lowest || p.song->lowest > kb.highest ||
            p.song->lowest > p.song->highest) {
            return out;
        }
        Extent song;
        add_note(song, std::max(p.song->lowest, kb.lowest), kb, base);
        add_note(song, std::min(p.song->highest, kb.highest), kb, base);
        focus.add(song);
        focus.add(upcoming); // held keys are left out: a wrong key must not zoom the view
        n = clamp_int(focus.width() + 2 * kSongMarginWhites, std::min(kMinSongWhites, total),
                      total);
    } else {
        const float width =
            std::isfinite(p.width_px) && p.width_px > 0.0f ? p.width_px : kFallbackWidthPx;
        const float ppmm =
            std::isfinite(p.px_per_mm) && p.px_per_mm > 0.0f ? p.px_per_mm : kDefaultPxPerMm;
        const float fit = std::floor(width / (mm * ppmm));
        n = clamp_int(static_cast<int>(std::min(fit, 1e6f)), std::min(kMinFixedWhites, total),
                      total);
        // The notes that must be seen: upcoming ones, else the held keys.
        const Extent& primary = upcoming.any ? upcoming : held;
        if (primary.any && primary.width() > n) {
            n = std::min(primary.width(), total);
            out.zoomed_out = true;
        }
        if (upcoming.any && held.any) {
            Extent both = upcoming;
            both.add(held);
            focus = both.width() <= n ? both : upcoming;
        } else {
            focus = primary;
        }
    }
    if (n >= total) {
        return out;
    }

    bool has_current = false;
    int current_left = 0;
    int current_w = n;
    if (p.current && p.current->lowest <= p.current->highest) {
        has_current = true;
        current_left = white_rank(p.current->lowest) - base;
        current_w = std::max(1, white_rank(p.current->highest) - white_rank(p.current->lowest) + 1);
    }
    // With nothing to show and no window yet, start on middle C (or the keyboard's middle).
    const int anchor = kb.contains(kMiddleC) ? white_rank(kMiddleC) - base : total / 2;
    const int left = place(n, total, focus, has_current, current_left, current_w, anchor);
    out.range = range_of(base, left, n);
    return out;
}

KeyRect KeyViewFrame::to_window(KeyRect layout_unit, int layout_white_count) const {
    if (!(layout_unit.width > 0.0f) || layout_white_count <= 0 || !(span_whites > 0.0f)) {
        return {};
    }
    const float whites = static_cast<float>(layout_white_count);
    return {(layout_unit.x * whites - pan) / span_whites, layout_unit.width * whites / span_whites,
            layout_unit.black};
}

int KeyViewFrame::side_of(int note) const {
    // `layout_range` holds every key at least partly in the window, so a key is off screen exactly
    // when it is not in the range (a black key whose upper neighbour is cut is not drawn either).
    if (note < layout_range.lowest) {
        return -1;
    }
    return note > layout_range.highest ? 1 : 0;
}

KeyViewFollower::KeyViewFollower(double omega) : omega_(omega > 0.0 ? omega : kDefaultOmega) {}

namespace {

// Critically damped spring, exact for any `dt`: x'' = -2w x' - w^2 (x - target).
void spring(double& x, double& v, double target, double omega, double dt) {
    const double e = x - target;
    const double c = v + omega * e;
    const double decay = std::exp(-omega * dt);
    x = target + (e + c * dt) * decay;
    v = (v - omega * c * dt) * decay;
}

} // namespace

void KeyViewFollower::snap(const KeyRange& keyboard, const KeyRange& target) {
    const KeyboardLayout layout(keyboard);
    base_rank_ = white_rank(layout.range().lowest);
    limit_rank_ = white_rank(layout.range().highest) + 1;
    left_ = std::clamp(white_rank(target.lowest), base_rank_, limit_rank_ - 1);
    right_ = std::clamp(white_rank(target.highest) + 1, base_rank_ + 1, limit_rank_);
    right_ = std::max(right_, left_ + 1.0);
    left_vel_ = 0.0;
    right_vel_ = 0.0;
    started_ = true;
    settled_ = true;
}

void KeyViewFollower::advance(const KeyRange& keyboard, const KeyRange& target, double dt_s) {
    if (!started_) {
        snap(keyboard, target);
        return;
    }
    if (!(dt_s > 0.0) || !std::isfinite(dt_s)) {
        return;
    }
    const KeyboardLayout layout(keyboard);
    base_rank_ = white_rank(layout.range().lowest);
    limit_rank_ = white_rank(layout.range().highest) + 1;
    const double lo = base_rank_;
    const double hi = limit_rank_;
    const double target_left =
        std::clamp(static_cast<double>(white_rank(target.lowest)), lo, hi - 1);
    const double target_right =
        std::clamp(static_cast<double>(white_rank(target.highest) + 1), target_left + 1.0, hi);

    spring(left_, left_vel_, target_left, omega_, dt_s);
    spring(right_, right_vel_, target_right, omega_, dt_s);

    // Stay inside the keyboard and keep at least one key in view. A spring that overshoots
    // because of leftover speed stops at the wall.
    if (left_ < lo) {
        left_ = lo;
        left_vel_ = std::max(left_vel_, 0.0);
    }
    if (right_ > hi) {
        right_ = hi;
        right_vel_ = std::min(right_vel_, 0.0);
    }
    if (right_ < left_ + 1.0) {
        right_ = std::min(left_ + 1.0, hi);
        left_ = std::min(left_, right_ - 1.0);
    }

    settled_ = std::abs(left_ - target_left) < kSettleDistance &&
               std::abs(right_ - target_right) < kSettleDistance &&
               std::abs(left_vel_) < kSettleSpeed && std::abs(right_vel_) < kSettleSpeed;
    if (settled_) {
        left_ = target_left;
        right_ = target_right;
        left_vel_ = 0.0;
        right_vel_ = 0.0;
    }
}

KeyViewFrame KeyViewFollower::frame() const {
    KeyViewFrame f;
    const double left =
        std::abs(left_ - std::round(left_)) < kRankEpsilon ? std::round(left_) : left_;
    const double right =
        std::abs(right_ - std::round(right_)) < kRankEpsilon ? std::round(right_) : right_;
    const int first = clamp_int(static_cast<int>(std::floor(left)), base_rank_, limit_rank_ - 1);
    const int last = clamp_int(static_cast<int>(std::ceil(right)) - 1, first, limit_rank_ - 1);
    f.layout_range = {note_of_rank(first), note_of_rank(last)};
    f.pan = static_cast<float>(std::max(0.0, left - static_cast<double>(first)));
    f.span_whites = static_cast<float>(right - left);
    f.settled = settled_;
    return f;
}

} // namespace toonval
