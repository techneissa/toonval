// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

namespace toonval {

// MIDI note numbers run 0..127. Middle C is 60 (C4), A4 is 69.
inline constexpr int kMidiNoteMin = 0;
inline constexpr int kMidiNoteMax = 127;
inline constexpr int kMiddleC = 60;
inline constexpr int kA4 = 69;

constexpr bool is_valid_note(int note) { return note >= kMidiNoteMin && note <= kMidiNoteMax; }

// Pitch class 0..11 with C = 0.
constexpr int pitch_class(int note) { return ((note % 12) + 12) % 12; }

constexpr bool is_black_key(int note) {
    switch (pitch_class(note)) {
    case 1:
    case 3:
    case 6:
    case 8:
    case 10:
        return true;
    default:
        return false;
    }
}

// Scientific pitch notation octave: note 60 is octave 4.
constexpr int octave(int note) { return (note - pitch_class(note)) / 12 - 1; }

// Equal-tempered frequency. `a4_hz` lets the tuning reference move for acoustic pianos.
double frequency_hz(int note, double a4_hz = 440.0);

// Nearest note to a frequency, as a fractional MIDI number (69.0 for 440 Hz).
// Any negative result means "no valid note": it is returned for <= 0, NaN and infinite `hz` or
// `a4_hz`, and sub-audio frequencies below MIDI 0 also come out negative.
double note_from_frequency(double hz, double a4_hz = 440.0);

// "C4", "F#3", or "Gb3" when `prefer_flats` is set. Empty for invalid notes. `octave_offset` is
// added to the octave number: 0 for scientific naming (middle C is C4), -1 for Yamaha (C3).
std::string note_name(int note, bool prefer_flats = false, int octave_offset = 0);

} // namespace toonval
