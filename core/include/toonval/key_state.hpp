// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "toonval/midi_event.hpp"
#include "toonval/pitch.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace toonval {

enum class KeyTransitionType : std::uint8_t {
    None,        // Nothing changed.
    Pressed,     // `note` went from up to down.
    Released,    // `note` went from down to up.
    AllReleased, // CC120 / CC123 or clear() released `released_count` keys at once.
};

struct KeyTransition {
    KeyTransitionType type = KeyTransitionType::None;
    std::uint8_t note = 0;           // Pressed, Released
    std::uint8_t velocity = 0;       // Pressed: note-on velocity. Released: release velocity.
    std::uint8_t released_count = 0; // AllReleased
};

// Which keys are physically held, from live MIDI. Notes are tracked by number across all channels
// and without any KeyRange check: a key outside the keyboard's range is still tracked, and range
// checks happen elsewhere. The sustain pedal is tracked separately and never holds a key.
class KeyState {
  public:
    // Applies one event and reports what changed. Non-note events other than sustain, All Sound Off
    // and All Notes Off, and notes outside 0..127, change nothing.
    KeyTransition apply(const MidiEvent& ev);

    // Releases every key and lifts the pedal, e.g. when the input device is unplugged. Returns
    // AllReleased, or None when no key was held.
    KeyTransition clear();

    bool is_down(int note) const;
    int held_count() const { return held_count_; }
    // Held notes in ascending order.
    std::vector<std::uint8_t> held_notes() const;
    // Same, written into `out` (cleared first) so a frame loop can reuse one buffer.
    void held_notes(std::vector<std::uint8_t>& out) const;
    // Arrival time of the note-on of a held note, or nullopt when the key is up.
    std::optional<std::int64_t> press_time_ns(int note) const;
    // Velocity of the note-on of a held note, or nullopt when the key is up.
    std::optional<std::uint8_t> press_velocity(int note) const;
    bool sustain_down() const { return sustain_down_; }

  private:
    struct Key {
        bool down = false;
        std::uint8_t velocity = 0;
        std::int64_t press_time_ns = 0;
    };

    KeyTransition release_all();

    std::array<Key, 128> keys_{};
    int held_count_ = 0;
    bool sustain_down_ = false;
};

} // namespace toonval
