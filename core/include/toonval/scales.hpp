// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace toonval {

enum class ScaleType : std::uint8_t {
    Major,
    NaturalMinor,
    HarmonicMinor,
    MelodicMinor, // as written going up; coming down it is the natural minor
    Dorian,
    Phrygian,
    Lydian,
    Mixolydian,
    Locrian,
    MajorPentatonic,
    MinorPentatonic,
    Blues, // minor pentatonic plus the flat fifth
    Chromatic,
    WholeTone,
    // The two modes that are the major and natural minor scales under their modal names.
    Ionian = Major,
    Aeolian = NaturalMinor,
};

inline constexpr std::array<ScaleType, 14> kAllScaleTypes{
    ScaleType::Major,           ScaleType::NaturalMinor,    ScaleType::HarmonicMinor,
    ScaleType::MelodicMinor,    ScaleType::Dorian,          ScaleType::Phrygian,
    ScaleType::Lydian,          ScaleType::Mixolydian,      ScaleType::Locrian,
    ScaleType::MajorPentatonic, ScaleType::MinorPentatonic, ScaleType::Blues,
    ScaleType::Chromatic,       ScaleType::WholeTone};

// "Major", "Harmonic minor", "Whole tone", ...
const char* scale_type_name(ScaleType type);

// Semitones above the root of each note of the scale going up, from 0, without the octave.
std::span<const int> scale_intervals(ScaleType type);
// The same going down, from the root upward: differs from scale_intervals() only for the melodic
// minor, which comes down as the natural minor.
std::span<const int> scale_intervals_descending(ScaleType type);

// ----- Which keys belong to a scale (the free-play overlay) -----

// Pitch classes (0..11, C = 0) of the scale in scale order, starting on `root_pc`, which may be any
// integer and wraps to 0..11.
std::vector<int> scale_pitch_classes(int root_pc, ScaleType type);

// The same as a bit mask, bit n set when pitch class n is in the scale.
unsigned scale_pitch_class_mask(int root_pc, ScaleType type);

// True when MIDI `note` (0..127) has a pitch class of the scale; false for notes outside MIDI.
bool is_in_scale(int note, int root_pc, ScaleType type);
// True when MIDI `note` (0..127) has the pitch class of the root; false for notes outside MIDI.
bool is_root(int note, int root_pc);

// ----- Note names -----

// A key's root as it is written: a letter and an accidental (-2 bb, -1 b, 0 natural, 1 #, 2 x).
// F# major and Gb major share a pitch class and are different keys with different note names.
struct Tonic {
    char letter = 'C'; // 'A'..'G'
    int accidental = 0;

    // 0..11, or -1 when the letter is not A..G.
    int pitch_class() const;
};

// One note of a spelled scale.
struct SpelledNote {
    char letter = 'C';
    int accidental = 0; // -2..2
    int pitch_class = 0;
};

// How a root pitch class is usually written for this scale type: the spelling whose scale needs
// the fewest accidentals (Db major, not C# major; C# minor, not Db minor). A white key is written
// as itself. A tie (F# major and Gb major need six signs each) goes to sharps, or to flats with
// `prefer_flats`.
Tonic default_tonic(int root_pc, ScaleType type, bool prefer_flats = false);

// The scale's notes from the tonic upward, one octave, each with its letter and accidental. A
// seven-note scale uses every letter once (F major has Bb, not A#; F# major has E#, not F). The
// pentatonic, blues and whole-tone scales keep the letters of the degrees they are built from
// (C blues: C Eb F Gb G Bb). The chromatic scale uses sharps, or flats when the tonic is flat.
// Empty for a tonic that is not a letter A..G with an accidental of -2..2, or one that needs more
// than a double sharp or double flat.
std::vector<SpelledNote> spell_scale(Tonic tonic, ScaleType type);

// "C", "Bb", "E#", "F##", "Bbb": the name without an octave.
std::string spelled_name(const SpelledNote& note);

// Name of MIDI `midi_note` written as `note`, with its octave number: "Bb3". The octave goes with
// the letter, so B#3 is the key of middle C and Cb4 the key of B3. `octave_offset` shifts the
// numbering: 0 is scientific (middle C = C4), -1 is Yamaha (middle C = C3). Empty when the
// note is outside 0..127.
std::string spelled_name(int midi_note, const SpelledNote& note, int octave_offset = 0);

// The names of spell_scale() without octaves. Empty when spell_scale() is.
std::vector<std::string> scale_note_names(Tonic tonic, ScaleType type);

// One octave of the scale from MIDI `root_note`, named with octave numbers ("F4 G4 A4 Bb4 ..."),
// stopping at note 127. Empty when `root_note` is not the tonic's pitch class, is outside 0..127,
// or the scale cannot be spelled.
std::vector<std::string> scale_note_names_with_octave(Tonic tonic, ScaleType type, int root_note,
                                                      int octave_offset = 0);

} // namespace toonval
