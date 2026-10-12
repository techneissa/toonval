// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "toonval/song.hpp"

#include <cstddef>
#include <cstdint>

namespace toonval {

// The most bars finish_song() builds, the same cap the MIDI loader uses.
inline constexpr std::size_t kMaxSongMeasures = 100'000;

// Microseconds per quarter note for a tempo, rounded to the nearest and clamped to what a MIDI
// tempo event holds (1..0xFFFFFF). A tempo that is zero, negative or NaN gives the slowest value.
std::uint32_t us_per_quarter_from_bpm(double bpm);

// A tempo map with one tempo from tick 0.
TempoMap make_tempo_map(int ticks_per_quarter, double bpm);

// Finishes a Song a generator or recorder filled in tick terms, so it looks like one load_midi()
// returns and NoteWindow, Playback and Transport work on it. The caller sets, in ticks: `tempo`,
// `notes` (start_tick, end_tick, pitch, velocity, hand, finger, track, channel), `events`
// (tick and the event fields), `time_signatures` (empty means 4/4) and the `parts` it wants named.
//
// This sorts notes the way the loader does (start tick, pitch, track, channel), then fills
// seconds from ticks through every tempo change (tempo map, notes, events), the bar list with
// bar numbers (a first bar shorter than the next is a pickup, numbered 0), each note's measure,
// end_tick, duration_s, Note::drum (channel 10), and a Part for every track and channel with notes
// (the caller's named parts are kept, and every part gets its note count and, if it has none, the
// program of its first program change). Notes with end_tick <= start_tick are dropped. Returns
// false if the song needs more than kMaxSongMeasures bars; the bar list then stops at the cap and
// the rest of the song is still sorted and timed.
bool finish_song(Song& song);

} // namespace toonval
