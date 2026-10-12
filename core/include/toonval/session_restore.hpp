// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "toonval/session_state.hpp"
#include "toonval/song.hpp"

namespace toonval {

enum class RestoreOutcome : std::uint8_t {
    Restored,       // the saved song is there and unchanged: back where you left off
    NoSong,         // the session had no song (free play); only the preferences come back
    SongMissing,    // the file is gone or can't be opened: nothing of the song comes back
    SongUnreadable, // the file is there but did not load as a song
    SongChanged,    // the file is there but its content differs from the saved hash
};

// What the app may apply on start. `state` is always valid (clamped, and consistent with the song);
// the app applies it without further checks.
struct RestorePlan {
    RestoreOutcome outcome = RestoreOutcome::NoSong;
    // Load the song named by state.song_path. False for NoSong, SongMissing and SongUnreadable.
    bool load_song = false;
    SessionState state;
    // Restored only: the saved position was beyond the song's end and moved to it.
    bool position_clamped = false;
    // Restored only: the loop reached past the song and was cut to fit, or was dropped because
    // nothing usable of it is left (outside the song, or shorter than kMinLoopSeconds).
    bool loop_adjusted = false;
    bool loop_dropped = false;

    // The saved song-bound state could not be used and was reset; the app says why on screen.
    bool was_reset() const {
        return outcome == RestoreOutcome::SongMissing ||
               outcome == RestoreOutcome::SongUnreadable || outcome == RestoreOutcome::SongChanged;
    }
};

// Decides what of a saved session can be restored.
//
// `song_found`: the file at state.song_path exists and was read. `song_hash_matches`: its
// content_hash equals state.song_hash; ignored when no hash was saved (song_hash == 0).
// `song`: the loaded song, or null when it did not load. The checks run in this order:
//
// - no song saved: NoSong.
// - not found: SongMissing. Not loaded: SongUnreadable.
// - hash differs: SongChanged. The song is loaded (the file is the player's), from the start.
//   `state.song_hash` is 0 until the app records the new one.
// - otherwise Restored: the position moves to the song's end if past it, and the loop is cut to
//   the song or dropped (loop_enabled off).
//
// Whenever the song-bound part is not restored it goes back to its defaults: position 0, speed
// 100%, both hands practised and the song muted, no loop, no transposition. What
// belongs to the player or the device, not the song, always comes back: mode, count-in,
// metronome, the auto speed-up policy and the keyboard choice. The session never restores to
// "playing": the transport starts paused.
RestorePlan plan_restore(const SessionState& saved, bool song_found, bool song_hash_matches,
                         const Song* song);

} // namespace toonval
