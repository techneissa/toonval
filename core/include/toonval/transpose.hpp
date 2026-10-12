// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "toonval/keyboard.hpp"
#include "toonval/song.hpp"

#include <cstdint>

namespace toonval {

inline constexpr int kMaxTransposeSemitones = 12;

// What a song pitch becomes on this keyboard.
struct TransposedPitch {
    int pitch = -1;       // the key to show, expect and play; -1 when `playable` is false
    bool folded = false;  // moved by whole octaves to land on the keyboard: draw the marker
    int fold_octaves = 0; // +1 raised one octave, -2 lowered two; 0 when not folded
    bool playable = true; // false: no key of the keyboard has this note's pitch class
};

// The one place a song pitch is turned into the key that is shown, expected and played. The
// display (NoteWindow), the evaluator (expected_pitch) and MIDI out (Playback) all call map(), so
// they cannot disagree about a note.
//
// The semitone shift is clamped to -12..+12. A shifted pitch outside the keyboard moves by the
// fewest whole octaves that bring it inside; a keyboard with no key of that pitch class (narrower
// than an octave, or an empty range) leaves the note unplayable instead of guessing a key.
class Transposition {
  public:
    // No shift and no keyboard limit beyond MIDI itself (0..127).
    Transposition() = default;
    Transposition(int semitones, KeyRange keyboard);

    int semitones() const { return semitones_; }
    const KeyRange& keyboard() const { return keyboard_; }

    TransposedPitch map(int song_pitch) const;
    // map() for a note of a song: drum notes name instruments, not keys, so they are never moved.
    // NoteWindow and Playback call this, so what is drawn and what is played come from one rule.
    TransposedPitch map_note(int song_pitch, bool drum) const;
    // What the evaluator waits for: the key to press for a note. Same answer as the display shows
    // and the piano plays.
    TransposedPitch expected_pitch(const Note& note) const {
        return map_note(note.pitch, note.drum);
    }

    bool operator==(const Transposition& other) const {
        return semitones_ == other.semitones_ && keyboard_.lowest == other.keyboard_.lowest &&
               keyboard_.highest == other.keyboard_.highest;
    }

  private:
    int semitones_ = 0;
    KeyRange keyboard_{0, 127};
};

// The session's one transposition, so the shown, expected and played keys can't drift apart. The
// app owns one and changes it only here; Playback, FlowEvaluator and WaitEvaluator follow() it and
// the display copies value() into NoteWindowOptions each frame. A follower picks up a change at its
// next call that carries a time, acting as if the change came at changed_at_ns(): Playback stops
// the notes sounding at their old keys, and the evaluators expect the new keys from then on.
class TranspositionSource {
  public:
    TranspositionSource() = default;
    explicit TranspositionSource(const Transposition& value) : value_(value) {}
    const Transposition& value() const { return value_; }
    // Goes up by one per change; followers compare it with the last one they applied.
    std::uint64_t version() const { return version_; }
    std::int64_t changed_at_ns() const { return changed_at_ns_; }
    // The same value again changes nothing.
    void set(const Transposition& value, std::int64_t now_ns) {
        if (value == value_) {
            return;
        }
        value_ = value;
        changed_at_ns_ = now_ns;
        ++version_;
    }

  private:
    Transposition value_;
    std::uint64_t version_ = 0;
    std::int64_t changed_at_ns_ = 0;
};

} // namespace toonval
