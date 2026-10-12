// SPDX-License-Identifier: GPL-3.0-or-later
// What the touch UI needs from the song transport: a read-only view of it and a few actions. The
// UI holds no song state of its own; app/song_transport.hpp binds it to the SongController, so the
// bar's buttons, the song overview and the resume countdown always show and change the real thing.
#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace toonval::app::ui {

// What you hear of the song. Your own playing always sounds.
enum class SongSound : std::uint8_t { Mute, OtherHand, BothHands };

struct SongView {
    bool has_song = false; // free play: no transport, no overview, no countdown
    std::string title;
    double position_s = 0.0;
    double duration_s = 0.0;
    bool playing = false; // the clock moves, or is about to (a count-in)
    bool loop_enabled = false;
    double loop_start_s = 0.0;
    double loop_end_s = 0.0;
    // Start of every bar, in seconds, for the faint bar lines on the overview. May be empty.
    std::span<const double> bar_starts_s;
    SongSound sound = SongSound::Mute;
    int skip_back_s = 5; // AppSettings::skip_back_seconds
    int skip_forward_s = 5;
    // The resume countdown after the settings close: seconds left (fractional, 0 when none runs)
    // and how long it was.
    double countdown_left_s = 0.0;
    int countdown_total_s = 3;
};

class SongPort {
  public:
    virtual ~SongPort() = default;

    virtual const SongView& view() const = 0;

    // Play or pause. During the resume countdown, stops it (paused).
    virtual void toggle_play(std::int64_t now_ns) = 0;
    // From the loop start when a loop is on, else from the start, and play.
    virtual void replay(std::int64_t now_ns) = 0;
    // One step of skip_back_s / skip_forward_s. A held button calls these again on each repeat.
    virtual void skip_back(std::int64_t now_ns) = 0;
    virtual void skip_forward(std::int64_t now_ns) = 0;
    virtual void seek(double song_s, std::int64_t now_ns) = 0;
    virtual void set_sound(SongSound sound, std::int64_t now_ns) = 0;
};

} // namespace toonval::app::ui
