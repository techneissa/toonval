// SPDX-License-Identifier: GPL-3.0-or-later
// Turns SDL mouse, touch and keyboard events into the same MidiEvents a MIDI keyboard makes, so
// playing on the screen or the computer keyboard goes through the same key, trail and chord path.
#pragma once

#include "toonval/computer_keyboard.hpp"
#include "toonval/keyboard.hpp"
#include "toonval/pointer_keys.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace toonval::app {

// Time stamps. An SDL event's timestamp is the moment the OS delivered it, in SDL's tick clock,
// which is not midi_clock_ns()'s. The two clocks are bridged by the event's age: the stamp is
// `midi_clock_ns()` now minus how long ago (SDL_GetTicksNS() - timestamp) the event happened, so a
// key pressed 4 ms before the frame drained its events is stamped 4 ms before. An age that is
// negative or over a second (a missing or stale timestamp) counts as now. The key-down to present
// time that --latency-log prints starts at this stamp.
std::int64_t event_time_ns(std::uint64_t sdl_timestamp_ns);

class VirtualInput {
  public:
    VirtualInput();

    // The player's keyboard range, for the typing keys' octave limits.
    void set_keyboard(KeyRange keyboard) { typing_.set_keyboard(keyboard); }
    // Where the keys are on screen after the last frame, in output pixels, and the left edge of the
    // settings panel (negative when closed): presses under the panel are the panel's.
    void set_geometry(const KeyboardLayout& layout, const KeyViewFrame& view, const RectF& key_area,
                      float panel_x);

    // Handles one SDL event. `ui_wants_keys` is true while a text field in the UI has the keyboard.
    // Appends the events it makes to `out` and returns true when the event was a playing input
    // (typing key, a press on a key), so the caller leaves it alone. Presses the UI is using
    // (ImGui's WantCaptureMouse / WantCaptureKeyboard) and key presses with Ctrl, Alt or Cmd held
    // are not playing. Releases always get through.
    bool handle(const SDL_Event& event, SDL_Window* window, bool ui_wants_keys,
                std::vector<MidiEvent>& out);

    // Lets go of every note and the pedal: lost focus, quit.
    void release_all(std::vector<MidiEvent>& out);

    const ComputerKeyboard& typing() const { return typing_; }
    int held_notes() const { return pointers_.held_notes() + typing_.held_count(); }

  private:
    // Window points to output pixels.
    static float pixel_scale(SDL_Window* window);

    ComputerKeyboard typing_;
    PointerKeys pointers_;
    std::optional<PointerGeometry> geometry_;
    float panel_x_ = -1.0f;
};

} // namespace toonval::app
