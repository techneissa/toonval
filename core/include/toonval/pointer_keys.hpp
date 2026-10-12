// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "toonval/keyboard.hpp"
#include "toonval/keyboard_view.hpp"
#include "toonval/midi_event.hpp"
#include "toonval/screen_layout.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace toonval {

// Playing the on-screen keys by mouse or touch.

// Black keys' length as a share of the white keys' length, measured from the back (top) of the key
// area. The renderer draws them this long, and a press in that top part of a black key's width
// plays the black key, below it the white key beside it.
inline constexpr float kBlackKeyLengthShare = 0.62f;

// Velocity from where along the key the press lands: the back (top) edge of the key is
// `kPointerMinVelocity` and the front (bottom) edge is `kPointerMaxVelocity`, linear in between,
// rounded. A black key counts along its own length. Nearer the front is louder, like a real key
// pressed far from its pivot.
inline constexpr int kPointerMinVelocity = 24;
inline constexpr int kPointerMaxVelocity = 127;

// Where the keys are on screen, as drawn this frame.
struct PointerGeometry {
    KeyboardLayout layout; // built from `view.layout_range`
    KeyViewFrame view;     // where the window sits on the layout
    RectF key_area;        // window pixels; keys are drawn, and hit, only here
};

struct KeyHit {
    int note = 0;
    std::uint8_t velocity = 0;
};

// The key under the point (x, y) in window pixels, black keys first in their upper part, or nullopt
// when the point is outside `key_area` or on no key. The key area's right and bottom edges are
// outside it.
std::optional<KeyHit> hit_test_key(const PointerGeometry& geometry, float x, float y);

// Mouse and fingers playing keys. Each pointer (a finger id, or the mouse) holds at most one note.
//  * down on a key presses it; down elsewhere is ignored, and so is everything that pointer does
//    until it is lifted;
//  * moving onto another key releases the old key and presses the new one (a glissando), with the
//    velocity of the place it entered;
//  * moving off the keys releases the note; coming back onto a key while still down presses again;
//  * up releases it. `release_all` lets go of everything (focus lost, quitting).
// Two pointers on the same key share one note: it sounds until both let go.
// Events are appended to `out` with source `InputSource::Pointer`.
class PointerKeys {
  public:
    using PointerId = std::uint64_t;
    static constexpr std::size_t kMaxPointers = 16;

    void down(PointerId id, float x, float y, const PointerGeometry& geometry, std::int64_t time_ns,
              std::vector<MidiEvent>& out);
    void move(PointerId id, float x, float y, const PointerGeometry& geometry, std::int64_t time_ns,
              std::vector<MidiEvent>& out);
    // A pointer that is not down does nothing.
    void up(PointerId id, std::int64_t time_ns, std::vector<MidiEvent>& out);
    void release_all(std::int64_t time_ns, std::vector<MidiEvent>& out);

    // The pointer's current note, or nullopt.
    std::optional<int> note_of(PointerId id) const;
    // Pointers that went down on a key and are still down, whether or not a key is under them.
    int active_pointers() const;
    // Notes sounding.
    int held_notes() const;

  private:
    struct Pointer {
        bool used = false;
        PointerId id = 0;
        int note = -1; // -1: down on a key earlier, now over none
    };

    Pointer* find(PointerId id);
    const Pointer* find(PointerId id) const;
    void press(int note, int velocity, std::int64_t time_ns, std::vector<MidiEvent>& out);
    void release(int note, std::int64_t time_ns, std::vector<MidiEvent>& out);

    std::array<Pointer, kMaxPointers> pointers_{};
    std::array<std::uint8_t, 128> shared_{};
};

} // namespace toonval
