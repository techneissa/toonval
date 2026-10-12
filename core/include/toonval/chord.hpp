// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace toonval {

// Chord families recognised from the held notes. Pure pitch-class logic.
enum class ChordQuality : std::uint8_t {
    None,
    Major,
    Minor,
    Diminished,
    Augmented,
    Sus2,
    Sus4,
    Dominant7,
    Major7,
    Minor7,
    HalfDiminished7, // written "m7b5"
    Diminished7,
    MinorMajor7,
    Major6,
    Minor6,
    Add9,
    Dominant9,
    Major9,
    Minor9,
    Power, // root and fifth only, written "5"
};

// The chord a player is holding. `found` is false when the notes do not match a known chord
// cleanly; the other fields are then meaningless and the UI shows nothing.
struct Chord {
    bool found = false;
    int root_pc = 0; // pitch class 0..11, C = 0
    ChordQuality quality = ChordQuality::None;
    int bass_pc = 0; // pitch class of the lowest held note; differs from root_pc for inversions
};

// Names the chord formed by `count` held MIDI notes. Order does not matter, and duplicates and
// octave doublings are fine. The lowest note is the bass.
//
// Never guesses: any invalid note, a null pointer with a nonzero count, fewer than two distinct
// pitch classes, a cluster or an unknown extension all give found = false.
//
// When several readings fit (C E G A is C6 or Am7), a reading rooted on the bass wins. Otherwise
// triads and suspended chords beat sevenths, which beat ninths, which beat the sixth chords, so
// C E G A over E is Am7/E. Inversions therefore resolve the same way every time. Symmetric chords
// (Augmented, Diminished7) take the bass as root. A power chord must have its root in the bass.
Chord detect_chord(const int* notes, std::size_t count);

// Same as above for a span.
inline Chord detect_chord(std::span<const int> notes) {
    return detect_chord(notes.data(), notes.size());
}

// "C", "Am7", "G/B", "Cm7b5", "C5". Empty when `chord.found` is false. `prefer_flats` spells
// black-key roots and basses as flats ("Bb7") instead of sharps ("A#7").
std::string chord_name(const Chord& chord, bool prefer_flats = false);

} // namespace toonval
