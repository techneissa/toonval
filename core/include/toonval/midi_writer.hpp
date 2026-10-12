// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "toonval/song.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace toonval {

struct MidiWriteOptions {
    // 1: track 0 holds the tempo map and time signatures, and every source track of the song
    // follows with its own number (an empty chunk fills a gap), so a Song loads back with the same
    // track and channel of every note. 0: one track holds everything, in tick order; track
    // numbers, and the names of channels that share a number, are then lost.
    int format = 1;
    // Leave out a channel message's status byte when it repeats the one before (SMF running
    // status). Smaller file, same notes. A meta event ends the run, as the SMF spec says.
    bool running_status = false;
};

struct MidiWriteResult {
    bool ok = false;
    std::string message;             // English detail for logs when ok is false
    std::vector<std::uint8_t> bytes; // the file; empty unless ok
    int notes_written = 0;
    int notes_skipped = 0;   // pitch above 127, channel above 15, negative start, no length, or
                             // a second note starting on the same tick and pitch as another
    int notes_shortened = 0; // ended where the same pitch and channel starts again
    int events_written = 0;
    int events_skipped = 0; // channel above 15, or a controller number above 127
};

// Writes a Song as a Standard MIDI File (format 1 or 0), working from ticks, so what load_midi()
// reads back has the same notes (pitch, velocity, start and end tick), tempo map, time
// signatures, bars, parts and channel events, and so the same times in seconds. Finger is not in
// a MIDI file and is dropped. Never throws.
//
// What it writes: a track name (and instrument name) per source track from the song's Parts, with
// a channel-prefix name for a channel whose part is named differently, so a hand split by name
// survives; the tempo map and time signatures; note-ons and note-offs (real 0x8n note-offs, release
// velocity 64); program changes, controllers (the sustain pedal CC64 among them) and pitch bends
// from Song::events. At one tick the order is names, tempo, note-offs, other events, note-ons, so
// a re-struck pitch and a program change before its first note read back right.
//
// Repairs, counted in the result: a note's velocity is held to 1..127 (0 would be a note-off); a
// note that overlaps a later one of the same track, channel and pitch ends where that one starts;
// event values are held to their range. Errors, not a file: ticks per quarter outside 1..32767, a
// tempo outside 1..0xFFFFFF microseconds, a time signature with a zero numerator or a denominator
// that is not a power of two up to 64, a track number the header cannot count (65535 or more), a
// gap between events too long for a variable-length quantity, or a format other than 0 or 1.
MidiWriteResult write_midi(const Song& song, const MidiWriteOptions& options = {});

} // namespace toonval
