// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "toonval/exercises.hpp"
#include "toonval/keyboard.hpp"
#include "toonval/song.hpp"

#include <cstdint>
#include <string>

namespace toonval {

// SplitMix64. Own generator, not <random>, so a seed gives the same questions on every platform
// and standard library (and on the iPad).
class EarRng {
  public:
    explicit EarRng(std::uint64_t seed) : state_(seed) {}
    std::uint64_t next();
    // Uniform in 0..n-1 with no modulo bias; 0 when n is 0 or 1.
    std::uint64_t below(std::uint64_t n);

  private:
    std::uint64_t state_;
};

enum class EarQuestionKind : std::uint8_t {
    Note,     // the piano plays one note; find it
    Interval, // the piano plays a note and a second one; name the interval or find the second note
};

struct EarSettings {
    KeyRange range = kRange61; // every note asked lies inside it
    EarQuestionKind kind = EarQuestionKind::Note;
    int min_interval = 1; // semitones, for Interval; 1..36
    int max_interval = 12;
    bool allow_descending = true;
    bool harmonic = false;        // Interval: the two notes sound together
    bool white_keys_only = false; // every note asked is a white key
};

struct EarQuestion {
    EarQuestionKind kind = EarQuestionKind::Note;
    int first = 60;  // the note played first; for Note, the note to find
    int second = 60; // Interval: the note that makes the interval; for Note, equal to `first`
    bool harmonic = false;

    // Signed size: positive when the second note is higher.
    int semitones() const { return second - first; }
};

struct EarQuestionResult {
    bool ok = false;
    std::string message; // why no question could be made
    EarQuestion question;
};

// Draws ear-training questions from a seeded generator: the same seed and settings always give the
// same questions, in order. A single-note question is a uniformly drawn note of the range; an
// interval question draws the interval size uniformly first (so a small interval is as likely as a
// large one), then its direction, then a first note for which both notes fit. The same question
// is never asked twice in a row when the settings allow another.
//
// Errors, not questions: a range that is empty or outside MIDI, an interval range outside 1..36 or
// reversed, no pair of notes that fits (a range narrower than the smallest interval), and white
// keys only on a range with none.
class EarTrainer {
  public:
    EarTrainer(std::uint64_t seed, const EarSettings& settings) : rng_(seed), settings_(settings) {}

    EarQuestionResult next();

  private:
    EarRng rng_;
    EarSettings settings_;
    bool have_previous_ = false;
    EarQuestion previous_;
};

// The pitch the player is asked to find: `first` for a note question, `second` for an interval.
inline int expected_pitch(const EarQuestion& q) {
    return q.kind == EarQuestionKind::Interval ? q.second : q.first;
}

// True when `played_note` is that pitch, or with `ignore_octave` any note of its pitch class.
// Notes outside 0..127 are never right.
bool check_pitch_answer(const EarQuestion& q, int played_note, bool ignore_octave = false);

// True when `answered_semitones` names the interval of an interval question. Direction counts
// (+7 is a fifth up, -7 a fifth down) unless `check_direction` is false. Always false for a note
// question.
bool check_interval_answer(const EarQuestion& q, int answered_semitones,
                           bool check_direction = true);

// "unison", "minor 2nd", ..., "tritone", ..., "octave" for 0..12 (the sign is ignored); anything
// larger is "N semitones".
std::string interval_name(int semitones);

// The question as a Song to play: one note, two notes one after the other (`note_beats` each), or
// two notes together for `2 * note_beats`. Uses the tempo, tick rate, note length and velocity of
// `play`; its keyboard range is not used.
Song ear_question_song(const EarQuestion& q, const ExerciseSettings& play = {});

} // namespace toonval
