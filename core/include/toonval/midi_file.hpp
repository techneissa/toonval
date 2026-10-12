// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "toonval/song.hpp"

namespace toonval {

enum class MidiFileError : std::uint8_t {
    None,
    NotMidi,           // no "MThd" header
    Truncated,         // the data ends inside a chunk, or before every track was read
    Corrupt,           // bad event, length or header field
    UnsupportedFormat, // SMF format 2 (independent sequences)
    SmpteDivision,     // time division in SMPTE frames instead of ticks per quarter note
    TooLarge,          // more bars or memory than any real song needs
};

struct MidiLoadOptions {
    // Notes shorter than this are file artifacts and get dropped, unless kept.
    double ghost_threshold_s = 0.030;
    bool keep_ghost_notes = false;
    // Set hands from track and channel names (see assign_hands_from_names).
    bool assign_hands = true;
};

// What the loader changed or removed to keep every note well formed.
struct MidiLoadStats {
    int ghost_notes_dropped = 0; // shorter than the ghost threshold
    int overlaps_split = 0;      // a note-on that ended a sounding note of the same pitch
    int duplicates_merged = 0;   // a second note-on of a sounding pitch at the same tick
    int unterminated_notes = 0;  // no note-off; ended at the end of their track
    int zero_length_dropped = 0; // note-off at the note-on tick, or unterminated at track end
    int tracks_truncated = 0;    // track chunks whose length ran past the end of the file
};

struct MidiLoadResult {
    bool ok = false;
    MidiFileError error = MidiFileError::None;
    std::string message; // English detail for logs, with the byte offset when it helps
    Song song;           // empty unless ok
    MidiLoadStats stats;
};

// Parses a Standard MIDI File (format 0 or 1) from memory into a Song. Never throws and never
// reads outside `data`; a bad or truncated file gives ok == false and an error.
//
// Rules, each with a test in tests/midi_file_test.cpp:
// - Ticks become seconds through every tempo change of every track (format 1 puts them in track
//   0, but any track counts). Bars come from time-signature events; a time signature that falls
//   mid-bar starts a new bar there. A first bar shorter than its successor, cut short or under its
//   own shorter signature, is a pickup and numbered 0.
// - Running status is expanded; SysEx and meta events cancel it.
// - A note-on with velocity 0 is a note-off.
// - Only one note per pitch and channel sounds at a time. A note-on of a sounding pitch ends that
//   note at its tick and starts a new one. A note-off then ends the newest note; one more note-off
//   than note-ons is ignored. A second note-on at the same tick as the first is merged into it.
//   If a note-off lands on the tick where a note-on just ended an earlier note (a re-strike
//   written on-before-off), the off belongs to the earlier note and the new one keeps sounding.
// - A note with no note-off ends at its track's End-of-Track (or last event).
// - No note has zero or negative length: such notes are dropped and counted.
// - The sustain pedal (CC64) never lengthens a note.
// - Channel 10 notes are kept with drum = true and never filtered as ghost notes.
// - Unknown chunks (Yamaha XF "XFIH", ...) are skipped by their length.
// - A track chunk whose declared length runs past the end of the file is read up to the end of the
//   data and counted in stats.tracks_truncated. An event the data ends in the middle of is dropped,
//   and notes still sounding end at the last whole event. Bytes that don't parse are still an
//   error, and so is a missing track or an unknown chunk that runs past the end.
// - Program changes, control changes (CC64 sustain included) and pitch bends are kept in
//   Song::events with their tick and seconds, so MIDI out can reproduce the file.
MidiLoadResult load_midi(const std::uint8_t* data, std::size_t size,
                         const MidiLoadOptions& options = {});

inline MidiLoadResult load_midi(std::span<const std::uint8_t> data,
                                const MidiLoadOptions& options = {}) {
    return load_midi(data.data(), data.size(), options);
}

const char* to_string(MidiFileError error);

} // namespace toonval
