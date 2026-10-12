// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "toonval/keyboard.hpp"
#include "toonval/midi_event.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace toonval {

// Playing the piano from the computer keyboard, for when no MIDI keyboard is connected.
//
// Keys are named by physical position, so the mapping is the same on QWERTY, AZERTY or Dvorak:
// the integers are USB HID usage ids, which are also SDL's scancode values (SDL_SCANCODE_A is 4).
// The app checks that the two agree.
namespace scancode {
inline constexpr int kA = 4;
inline constexpr int kC = 6;
inline constexpr int kD = 7;
inline constexpr int kE = 8;
inline constexpr int kF = 9;
inline constexpr int kG = 10;
inline constexpr int kH = 11;
inline constexpr int kJ = 13;
inline constexpr int kK = 14;
inline constexpr int kL = 15;
inline constexpr int kO = 18;
inline constexpr int kP = 19;
inline constexpr int kS = 22;
inline constexpr int kT = 23;
inline constexpr int kU = 24;
inline constexpr int kV = 25;
inline constexpr int kW = 26;
inline constexpr int kX = 27;
inline constexpr int kY = 28;
inline constexpr int kZ = 29;
inline constexpr int kTab = 43;
inline constexpr int kSemicolon = 51;
inline constexpr int kApostrophe = 52;
} // namespace scancode

// The mapping, with `base` the note the A key plays (middle C, 60, at octave shift 0). Each key
// plays `base` plus its offset in semitones:
//
//   white keys, the middle row:  A S D F G H J K  L  ;  '
//                       offset:  0 2 4 5 7 9 11 12 14 16 17   (C D E F G A B C D E F)
//   black keys, the row above:   W E   T Y U   O  P
//                       offset:  1 3   6 8 10  13 15          (C# D# F# G# A# C# D#)
//
//   Z / X   octave down / up, in steps of 12 notes
//   C / V   softer / louder, along the ladder `kVelocityLevels`
//   Tab     sustain pedal (CC64) down while held
inline constexpr int kTypingBaseNote = 60;
inline constexpr int kTypingNoteSpan = 18; // offsets 0..17: A up to ' is 17 semitones
inline constexpr std::array<int, 8> kVelocityLevels{16, 32, 48, 64, 80, 96, 112, 127};
inline constexpr int kDefaultVelocityLevel = 4; // 80

// What the typing keys do. Per physical key it remembers which note that press started, so a key
// held while the octave changes still releases the note it began, never a different one.
class ComputerKeyboard {
  public:
    explicit ComputerKeyboard(KeyRange keyboard = kRange88);

    // The player's keyboard. The octave shift moves inside the range where the whole mapped span
    // (18 notes from `base_note()`) fits on it; keys already held keep their notes.
    void set_keyboard(KeyRange keyboard);

    // One physical key event. `repeat` is the OS auto-repeat of a held key and does nothing. Fills
    // `out` (appends) with the events at `time_ns`: a NoteOn on a playing key's first press, a
    // NoteOff on its release, CC64 for Tab. Octave and velocity keys produce no event. A key that
    // plays a note outside the keyboard (only on a keyboard narrower than the span) plays nothing.
    // Other scancodes are ignored; `is_typing_key` tells which are not.
    void key(int scancode, bool down, bool repeat, std::int64_t time_ns,
             std::vector<MidiEvent>& out);

    // Releases every note the typing keys hold and lifts the pedal. For lost focus and quitting,
    // when the key-up events never arrive.
    void release_all(std::int64_t time_ns, std::vector<MidiEvent>& out);

    // True for the keys this class reacts to, so the caller can leave every other key alone.
    static bool is_typing_key(int scancode);

    // The note a playing key would start now, or nullopt for a non-playing key or a note off the
    // keyboard.
    std::optional<int> note_for(int scancode) const;
    // The key's own label for the note it plays now ("A", ";", "'"), empty when none plays it. For
    // the letters shown on the piano keys.
    std::string_view label_for_note(int note) const;

    int octave_shift() const { return octave_shift_; }
    int base_note() const { return kTypingBaseNote + 12 * octave_shift_; }
    int velocity() const { return kVelocityLevels[static_cast<std::size_t>(velocity_level_)]; }
    bool sustain_down() const { return sustain_down_; }
    // Notes the typing keys hold right now.
    int held_count() const { return held_count_; }
    // Bounds of the octave shift on the current keyboard.
    int min_octave_shift() const { return min_shift_; }
    int max_octave_shift() const { return max_shift_; }

  private:
    struct Held {
        bool down = false;
        std::int16_t note = -1; // -1: a press that plays nothing
    };

    void update_bounds();
    void emit(MidiEventType type, int data1, int data2, std::int64_t time_ns,
              std::vector<MidiEvent>& out) const;
    void press_note(Held& slot, int note, std::int64_t time_ns, std::vector<MidiEvent>& out);
    void release_note(int note, std::int64_t time_ns, std::vector<MidiEvent>& out);
    static int offset_of(int scancode); // -1 when the key does not play

    KeyRange keyboard_;
    int octave_shift_ = 0;
    int min_shift_ = 0;
    int max_shift_ = 0;
    int velocity_level_ = kDefaultVelocityLevel;
    bool sustain_down_ = false;
    int held_count_ = 0;
    std::array<Held, 256> keys_{};           // by scancode (all the mapped ones are below 256)
    std::array<std::uint8_t, 128> shared_{}; // how many physical keys hold each note
};

} // namespace toonval
