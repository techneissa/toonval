// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "toonval/midi_event.hpp"

#include <span>
#include <string>
#include <string_view>

namespace toonval {

// Where the notes the player plays without a piano go to be heard. A MIDI output to the piano
// implements it today; the built-in piano sound will implement it later. All calls come from the
// frame thread. Implementations never throw.
class SoundSink {
  public:
    virtual ~SoundSink() = default;

    // `channel` 0..15, `note` and `velocity` 0..127 (velocity 1..127 for note_on).
    virtual void note_on(int channel, int note, int velocity) = 0;
    virtual void note_off(int channel, int note) = 0;
    virtual void sustain(int channel, bool down) = 0;
    // Silences everything now: every note this sink started, and the pedal. Safe to call when
    // nothing sounds. Called on lost focus and on quit.
    virtual void all_off() = 0;
};

// The default: plays nothing.
class SilentSoundSink final : public SoundSink {
  public:
    void note_on(int, int, int) override {}
    void note_off(int, int) override {}
    void sustain(int, bool) override {}
    void all_off() override {}
};

// Passes one event to `sink` when it is the player's own virtual press: a NoteOn, a NoteOff, or the
// sustain pedal. Events from a MIDI keyboard (the piano already sounds those), synthetic resync
// events, and other controllers are not forwarded. Returns true when something was sent.
bool forward_to_sink(SoundSink& sink, const MidiEvent& ev);

// Whether a MIDI output port is the piano the player plays on, so virtual notes can be sent to it:
// its name names a built-in keyboard profile (`find_profile`), or equals, ignoring case, the name
// of one of the open MIDI inputs (the other half of the same device, which is how a piano shows
// up on CoreMIDI and ALSA). Software ports (names starting with "toonval", "Midi Through", the
// app's own virtual ports) never match, and an empty name does not.
bool is_piano_output(std::string_view output_name, std::span<const std::string> open_input_names);

} // namespace toonval
