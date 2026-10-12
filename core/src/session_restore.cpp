// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/session_restore.hpp"

#include <algorithm>
#include <cmath>

namespace toonval {

namespace {

// Everything that belongs to the song goes back to its defaults; what belongs to the player or the
// device (mode, count-in, metronome, auto speed-up policy, keyboard) stays.
SessionState song_state_reset(const SessionState& saved) {
    SessionState s = saved;
    const SessionState defaults;
    s.song_hash = defaults.song_hash;
    s.position_s = defaults.position_s;
    s.speed_percent = defaults.speed_percent;
    s.practice_hands = defaults.practice_hands;
    s.piano_plays = defaults.piano_plays;
    s.loop_start_s = defaults.loop_start_s;
    s.loop_end_s = defaults.loop_end_s;
    s.loop_enabled = defaults.loop_enabled;
    s.transpose_semitones = defaults.transpose_semitones;
    s.section_id = defaults.section_id;
    s.plan_item = defaults.plan_item;
    return s;
}

} // namespace

RestorePlan plan_restore(const SessionState& saved_state, bool song_found, bool song_hash_matches,
                         const Song* song) {
    const SessionState saved = saved_state.clamped();
    RestorePlan plan;

    if (!saved.has_song()) {
        plan.outcome = RestoreOutcome::NoSong;
        plan.state = song_state_reset(saved);
        return plan;
    }
    if (!song_found) {
        plan.outcome = RestoreOutcome::SongMissing;
        plan.state = song_state_reset(saved);
        plan.state.song_path.clear();
        return plan;
    }
    if (song == nullptr) {
        plan.outcome = RestoreOutcome::SongUnreadable;
        plan.state = song_state_reset(saved);
        plan.state.song_path.clear();
        return plan;
    }
    plan.load_song = true;
    if (saved.song_hash != 0 && !song_hash_matches) {
        plan.outcome = RestoreOutcome::SongChanged;
        plan.state = song_state_reset(saved);
        return plan;
    }

    plan.outcome = RestoreOutcome::Restored;
    plan.state = saved;
    const double duration = std::isfinite(song->duration_s) ? std::max(0.0, song->duration_s) : 0.0;
    if (plan.state.position_s > duration) {
        plan.state.position_s = duration;
        plan.position_clamped = true;
    }
    if (plan.state.has_loop()) {
        const double start = std::min(plan.state.loop_start_s, duration);
        const double end = std::min(plan.state.loop_end_s, duration);
        if (end - start < kMinLoopSeconds) {
            plan.state.loop_start_s = plan.state.loop_end_s = 0.0;
            plan.state.loop_enabled = false;
            plan.loop_dropped = true;
        } else if (start != plan.state.loop_start_s || end != plan.state.loop_end_s) {
            plan.state.loop_start_s = start;
            plan.state.loop_end_s = end;
            plan.loop_adjusted = true;
        }
    }
    return plan;
}

} // namespace toonval
