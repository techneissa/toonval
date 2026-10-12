// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// TinySoundFont's API (tsf.h) plus helpers that need its internal structs. tsf_glue.cpp holds the
// implementation; include this header instead of tsf.h.
#include <tsf.h>

namespace toonval::app {

// Makes room for a note before tsf_channel_note_on: when fewer voices are free than the note's
// matching zones need, ends voices until there are enough. Takes, in order, the oldest voice in
// its release phase, then the oldest held only by the sustain pedal, then the oldest still held.
// A stolen voice is cut at once. Returns how many voices were stolen.
int tsf_glue_make_room(tsf* f, int preset_index, int key, float velocity);

// Voices sounding for `key` (any phase). For tests and diagnostics; same thread as render.
int tsf_glue_voices_for_key(const tsf* f, int key);

} // namespace toonval::app
