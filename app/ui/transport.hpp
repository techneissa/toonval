// SPDX-License-Identifier: GPL-3.0-or-later
// The song's transport in the revealed bar (replay, skip back, play/pause, skip forward, the
// song's sound), the thin song overview to drag along, and the resume countdown that follows
// closing the settings. Everything reads and acts on a SongPort.
#pragma once

#include "kit.hpp"
#include "song_port.hpp"

namespace toonval::app::ui {

// Width of the transport buttons for `cell` wide hit cells.
float transport_width(float cell);
// The buttons left to right from `x`, centred on `cy`, each `cell` wide and `hit_h` tall, drawn
// `drawn` big. `prominent` draws play/pause as a filled accent circle; `captions` puts a short
// caption under each icon (bar style B), else a long press names it.
void transport(Kit& k, SongPort& song, float x, float cy, float cell, float hit_h, float drawn,
               bool live, bool prominent, bool captions);
// The skip icon: an open circular arrow (anticlockwise for back, clockwise for forward) with the
// step's seconds in the middle, `size` points across.
void skip_icon(const Kit& k, ImDrawList* dl, ImVec2 centre, float size, int seconds, bool forward,
               ImVec4 colour);
// The song overview inside `min`..`max`: title, bar lines, loop, played part and playhead, times.
// Drag or tap to jump. Returns true while it is being dragged.
bool scrubber(Kit& k, SongPort& song, ImVec2 min, ImVec2 max, bool live, bool show_title = true);
// Height the overview needs, in points.
float scrubber_height(const Kit& k);

// The resume countdown: a big number in a ring in the middle of `min`..`max`. Draws nothing when
// no countdown runs.
void countdown_overlay(Kit& k, const SongView& song, ImVec2 min, ImVec2 max);

} // namespace toonval::app::ui
