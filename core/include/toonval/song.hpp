// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "toonval/hand.hpp"

namespace toonval {

// One note of a song. Times have the tempo map applied; ticks are kept next to them so beats
// (tick / ticks_per_quarter) and bar positions stay exact.
struct Note {
    double start_s = 0.0; // seconds from song start
    double duration_s = 0.0;
    std::uint8_t pitch = 60; // MIDI note number, before transposition
    std::uint8_t velocity = 64;
    Hand hand = Hand::Unknown;
    std::int8_t finger = 0; // 1..5, or 0 when unknown
    int measure = 1;        // bar number (see Measure), for loops and trouble-spot stats

    std::int64_t start_tick = 0;
    std::int64_t end_tick = 0; // always > start_tick
    std::uint16_t track = 0;   // index of the source track
    std::uint8_t channel = 0;  // 0..15
    bool drum = false;         // channel 10 (index 9): kept for playback, skipped by hands and
                               // evaluation
};

struct TempoChange {
    std::int64_t tick = 0;
    double seconds = 0.0; // time at `tick`, through every earlier change
    std::uint32_t us_per_quarter = 500'000;
};

// Converts between ticks and seconds through every tempo change. `changes` is sorted by tick and
// starts at tick 0; with no tempo event the file plays at 120 BPM.
struct TempoMap {
    int ticks_per_quarter = 480;
    std::vector<TempoChange> changes{TempoChange{}};

    double seconds_at(std::int64_t tick) const;
    // Fractional tick at a time; times before 0 map before tick 0 at the first tempo.
    double tick_at(double seconds) const;
    double beats_at(std::int64_t tick) const {
        return static_cast<double>(tick) / ticks_per_quarter;
    }
};

struct TimeSignature {
    std::int64_t tick = 0;
    std::uint8_t numerator = 4;
    std::uint8_t denominator = 4; // a power of two
};

// One bar. A bar ends where the next one starts.
struct Measure {
    int number = 1; // 1 for the first full bar; a pickup bar is 0
    std::int64_t start_tick = 0;
    double start_s = 0.0;
    std::uint8_t numerator = 4;
    std::uint8_t denominator = 4;
};

enum class ChannelEventKind : std::uint8_t { Program, Control, PitchBend };

// A program change, control change or pitch bend of the file, kept so MIDI out can reproduce the
// sound the file asks for. Control changes include the sustain pedal (CC64), volume and
// expression (CC7, CC11) and bank select (CC0, CC32); none of them change a note's length.
struct ChannelEvent {
    double time_s = 0.0; // seconds from song start, tempo map applied
    std::int64_t tick = 0;
    std::uint16_t track = 0;  // index of the source track
    std::uint8_t channel = 0; // 0..15
    ChannelEventKind kind = ChannelEventKind::Control;
    std::uint8_t number = 0; // controller number for Control, 0 for the other kinds
    std::uint16_t value = 0; // program 0..127, controller value 0..127, bend 0..16383 (8192 is
                             // centre)
};

// The notes of one channel in one track, with what a hand split needs to know about them.
struct Part {
    std::uint16_t track = 0;
    std::uint8_t channel = 0;
    std::string name;            // channel name (meta 0x20 prefix) if set, else the track name
    std::string instrument_name; // meta 0x04, may be empty
    std::uint8_t program = 0;    // first General MIDI program change, 0 (piano) if none
    bool has_program = false;
    bool drum = false;
    int note_count = 0;
    Hand hand = Hand::Unknown;
};

struct Song {
    std::vector<Note> notes; // sorted by start tick, then pitch
    std::vector<Part> parts; // sorted by track, then channel
    // Program changes, controllers and pitch bends, sorted by tick; events at one tick keep the
    // order of the file (track by track).
    std::vector<ChannelEvent> events;
    TempoMap tempo;
    std::vector<TimeSignature> time_signatures; // sorted, first at tick 0
    std::vector<Measure> measures;              // cover every note, sorted
    std::int64_t end_tick = 0;                  // last note end
    double duration_s = 0.0;

    // Bar number at a tick (the bar containing it). Ticks before the first bar get its number.
    int measure_at(std::int64_t tick) const;
};

// Hand named by a track or channel name, case-insensitive: "Right", "RH", "R.H.", "Piano R",
// "Right Hand", "Treble" give Right; "Left", "LH", "L.H.", "Piano L", "Bass" give Left. A name
// that names both hands, or neither, gives Unknown.
Hand hand_from_name(std::string_view name);

// Sets each part's hand from its name, and every note of that part to match. Drum parts stay
// Unknown, and so does a part named only "Bass" when its program is a General MIDI bass
// instrument (33..40 in 1-based numbering), since that is a bass guitar, not a left hand.
// Parts and notes with no clear name keep Unknown; the split point decides them later.
void assign_hands_from_names(Song& song);

} // namespace toonval
