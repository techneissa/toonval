// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "toonval/chord.hpp"
#include "toonval/hand.hpp"
#include "toonval/keyboard.hpp"
#include "toonval/scales.hpp"
#include "toonval/song.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace toonval {

// Exercises are Songs: NoteWindow, Playback and Transport work on them as on a loaded file, and
// write_midi() saves them. Each generator returns an ExerciseResult; `ok` is false with a message
// (and an empty song) when the request cannot be met, never an exception.

enum class Direction : std::uint8_t { Up, Down, UpDown };
enum class HandsChoice : std::uint8_t { Right, Left, Together };

struct ExerciseSettings {
    KeyRange range = kRange61; // every generated note lies inside it
    double bpm = 80.0;
    int ticks_per_quarter = 480;
    double note_beats = 0.5; // length of each note of a scale or arpeggio (0.5 = an eighth note)
    int velocity = 80;
};

// One thing the player plays at once: the chord of a drill. Empty for scales and arpeggios.
struct ExerciseStep {
    std::string label;      // what detect_chord() and chord_name() call these notes, "C/E"
    std::vector<int> notes; // MIDI notes, ascending, both hands
    std::int64_t start_tick = 0;
    double start_s = 0.0;
};

struct ExerciseResult {
    bool ok = false;
    std::string message; // English detail when ok is false
    std::string title;   // "F major scale, right hand, 2 octaves"
    Song song;           // empty unless ok
    std::vector<ExerciseStep> steps;
};

// ----- Scale fingering -----

// Standard fingering numbers (1 thumb .. 5 little finger) for the notes of a scale going up from
// its tonic, `octaves` (1 or 2) octaves, both hands, for the major, natural minor and harmonic
// minor scales in all 12 keys (F# and Gb share a table). The same note keeps its finger coming
// down. Empty for any other scale type, hand or octave count: no table is defined there.
//
// The tables are the conventional ones: a hand plays groups of three or four fingers and the
// thumb passes under (right hand ascending) or the 3rd or 4th crosses over it (left hand
// ascending), with the thumb on white keys wherever the scale allows. The place of the 4th finger
// in each major scale follows the chart of Prof. Escadon (University of Miami, c. 1999) printed at
// https://www.cs.miami.edu/~burt/music/finger-charts.html. The minor keys apply the same rule
// (the natural and harmonic minor share a table, as in the ABRSM scale books) and every table is
// checked in tests/exercises_test.cpp against the rule itself: thumb on a white key, only legal
// crossings, the 5th finger only on the first left-hand or last right-hand note. The data was not
// copied from a printed edition; check it against one before it goes to students.
std::vector<int> scale_fingers(int root_pc, ScaleType type, Hand hand, int octaves);

// ----- Scales -----

struct ScaleSpec {
    int root_pc = 0; // 0..11, C = 0
    ScaleType type = ScaleType::Major;
    int octaves = 1; // 1 or 2
    Direction direction = Direction::Up;
    HandsChoice hands = HandsChoice::Right;
};

// A scale played note after note. The right hand starts at the tonic in the octave of middle
// C (C4 to B4), the left hand alone in the octave below, and together the left hand plays the
// same notes an octave below the right, in step. The whole exercise moves by whole octaves to fit
// `settings.range`, nearest the default place first; a keyboard too small is an error. Up and
// down ends at the tonic without repeating the top note, and the melodic minor comes down as the
// natural minor. Notes carry hand, track (1 right hand or the only hand, 2 left hand in a pair),
// and finger where scale_fingers() has one (0 elsewhere).
ExerciseResult make_scale_exercise(const ScaleSpec& spec, const ExerciseSettings& settings = {});

// ----- Arpeggios -----

// Semitones above the root of each tone of a chord, ascending: {0, 4, 7} for major. The ninth
// chords reach past an octave ({0, 4, 7, 10, 14}). Empty for ChordQuality::None.
std::span<const int> chord_intervals(ChordQuality quality);

struct ArpeggioSpec {
    int root_pc = 0;
    ChordQuality quality = ChordQuality::Major;
    int inversion = 0; // 0 root position; 1 starts on the 3rd (second tone); and so on
    int octaves = 1;   // 1 or 2
    Direction direction = Direction::Up;
    HandsChoice hands = HandsChoice::Right;
};

// The chord tones one after another, from the inversion's bass up `octaves` octaves to its octave
// (C E G C for C major, one octave, root position), placed and fitted like a scale. Any chord that
// fits in an octave works (not the ninth chords). No fingering table exists: finger is 0.
ExerciseResult make_arpeggio_exercise(const ArpeggioSpec& spec,
                                      const ExerciseSettings& settings = {});

// ----- Chord drills -----

struct ChordDrillItem {
    int root_pc = 0;
    ChordQuality quality = ChordQuality::Major;
    int inversion = 0; // 0 root position; k puts the k-th tone (counting from 0) in the bass
};

struct ChordDrillSpec {
    std::vector<ChordDrillItem> items;
    HandsChoice hands = HandsChoice::Right;
    double step_beats = 4.0; // time from one chord to the next
    double hold_beats = 3.0; // how long each is held; at most step_beats
    bool prefer_flats = false;
};

// Chords one after another. The right hand plays the chord with its bass in C4..B4, the left hand
// alone in C3..B3, and together the left hand adds the bass an octave below (the name is
// unchanged). Each chord moves by whole octaves to fit `settings.range`.
//
// Every chord is checked with detect_chord(): the notes must read as the root and quality asked
// for, and the step's label is chord_name() of that reading, so the name on screen is what the
// chord display shows when the player holds those keys. A request that reads as another chord (C6
// in first inversion is Am7/E) or as no chord is an error naming it; inversion_drill() lists the
// ones that work.
ExerciseResult make_chord_drill(const ChordDrillSpec& spec, const ExerciseSettings& settings = {});

// Root position and every inversion of a chord that detect_chord() reads back as the same chord.
std::vector<ChordDrillItem> inversion_drill(int root_pc, ChordQuality quality);

} // namespace toonval
