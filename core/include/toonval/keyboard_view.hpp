// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "toonval/keyboard.hpp"
#include "toonval/look.hpp"
#include "toonval/screen_layout.hpp"

#include <cstdint>
#include <optional>
#include <span>

namespace toonval {

// Which part of the keyboard the screen shows, as pure math. The
// app asks `compute_key_view` for the window it should rest on, and a `KeyViewFollower` slides the
// drawn window there. Everything is counted in white keys: a *rank* is the index of a white key
// among all white keys from MIDI note 0 (C-1 is rank 0, D-1 rank 1, ..., middle C rank 35). A black
// key has the rank of the white key just below it.

// Width of one white key in millimetres for the fixed sizes, 0 for the two fit modes. A real
// piano's white key is about 23.5 mm, so Extra large is a real-piano key and Small is about half.
inline constexpr float kSmallKeyMm = 12.0f;
inline constexpr float kMediumKeyMm = 16.0f;
inline constexpr float kLargeKeyMm = 20.0f;
inline constexpr float kExtraLargeKeyMm = 24.0f;
float key_size_white_mm(KeySize size);

// Fewest white keys the view ever shows for a fit mode or a fixed size: an octave for Fit song,
// three for a fixed size on a window too narrow for even that many at the chosen width.
inline constexpr int kMinSongWhites = 7;
inline constexpr int kMinFixedWhites = 3;
// White keys of room Fit song leaves on each side of the song's lowest and highest note.
inline constexpr int kSongMarginWhites = 2;

// Rank of the white key at or below `note` (0..74 for notes 0..127); the inverse for a rank of a
// white key. `note_of_rank` returns -1 for a negative rank and 127 or less only up to rank 74.
int white_rank(int note);
int note_of_rank(int rank);

struct KeyViewParams {
    KeyRange keyboard = kRange88; // the player's keyboard; normalised like KeyboardLayout does
    float width_px = 1280.0f;     // width the keys fill
    float px_per_mm = kDefaultPxPerMm;
    KeySize size = KeySize::FitKeyboard;
    // The song's lowest and highest note, when there is a song. Fit song zooms to it; without one
    // it shows the whole keyboard.
    std::optional<KeyRange> song;
    // Notes the player must be able to see: upcoming = about to be played (never cut off), held =
    // keys down now (kept in view when that does not cost an upcoming note). Notes off the
    // keyboard are ignored.
    std::span<const int> upcoming;
    std::span<const int> held;
    // The range this function returned last time. A window that still holds the notes with their
    // margin is kept as it is, so the view moves only when it has to, not on every new note.
    std::optional<KeyRange> current;
};

// The window the view should rest on, in whole keys.
struct KeyViewTarget {
    // Both ends are white keys, inside the keyboard, and `range.key_count()` keys fit the width
    // (stretched up to one key wider) unless `zoomed_out`.
    KeyRange range;
    // A fixed size was widened (smaller keys than asked) because the upcoming notes span more
    // than the chosen size shows.
    bool zoomed_out = false;
};

// Rules, in order:
//  * FitKeyboard: the whole keyboard.
//  * FitSong: the song's range plus `kSongMarginWhites` on each side, at least `kMinSongWhites`,
//    at most the keyboard; centred on the song; upcoming notes outside the song range widen it.
//  * Small..ExtraLarge: floor(width_px / (mm * px_per_mm)) white keys (at least
//    `kMinFixedWhites`, at most the keyboard), so keys are at least the stated width and
//    at most one key wider. The window holds the upcoming notes and, if they all fit with them,
//    the held keys, with a margin of N/8 white keys (1 to 3) from the edge, moving the least it
//    can from `current`. With nothing to show it stays where it is, or starts on middle C.
//  * If the upcoming notes span more white keys than the size shows, the window is exactly as
//    wide as they need (widest keys that still hold them): `zoomed_out`.
// Never throws; bad widths or densities fall back to the defaults.
KeyViewTarget compute_key_view(const KeyViewParams& params);

// One drawn frame of the view: where the window is right now, possibly between two resting places.
struct KeyViewFrame {
    // The keys to lay out: every key at least partly in the window. Both ends are white keys.
    // Build a `KeyboardLayout` from it.
    KeyRange layout_range;
    // How much of the first white key's width is off the left edge of the window, 0 up to (not
    // including) 1. Shifts the layout left by this many white keys.
    float pan = 0.0f;
    // Width of the window in white keys, a whole number at rest.
    float span_whites = 1.0f;
    // True once it has reached its target.
    bool settled = true;

    // A key's rect from `KeyboardLayout(layout_range).key()`, which spans 0..1 over the layout,
    // converted to window units where the visible window spans 0..1. Parts outside 0..1 are cut
    // off by the window edge.
    KeyRect to_window(KeyRect layout_unit, int layout_white_count) const;

    // Where a note's key is relative to the window: -1 when it is below `layout_range` (off the
    // left edge), 1 when it is above (off the right edge), 0 when the layout draws it. Works for
    // notes outside the keyboard too (0..127 and beyond). For the edge indicators on notes that
    // are playing off screen.
    int side_of(int note) const;
};

// Critically damped follower, exact for any frame time (a closed-form solution, not a stepped
// approximation), so 30, 60 and 144 FPS arrive at the same place at the same moment. Both edges of
// the window move, so a zoom animates as well as a pan.
class KeyViewFollower {
  public:
    // How fast it settles: omega in 1/s. About 98% of a jump is done in 6/omega seconds (half a
    // second at the default).
    static constexpr double kDefaultOmega = 12.0;

    explicit KeyViewFollower(double omega = kDefaultOmega);

    // Jumps to `target` with no animation: first frame, a window resize, a keyboard or size change.
    void snap(const KeyRange& keyboard, const KeyRange& target);
    // Moves toward `target` by `dt_s` seconds (non-positive or NaN: no movement). The edges are
    // always kept inside `keyboard`.
    void advance(const KeyRange& keyboard, const KeyRange& target, double dt_s);

    bool has_position() const { return started_; }
    KeyViewFrame frame() const;

  private:
    double omega_;
    bool started_ = false;
    bool settled_ = true;
    int base_rank_ = 0;  // keyboard's lowest white rank
    int limit_rank_ = 0; // keyboard's highest white rank + 1
    double left_ = 0.0;  // window edges in ranks, right exclusive
    double right_ = 0.0;
    double left_vel_ = 0.0; // ranks per second
    double right_vel_ = 0.0;
};

} // namespace toonval
