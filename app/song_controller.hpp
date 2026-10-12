// SPDX-License-Identifier: GPL-3.0-or-later
// The song half of the app (practice modes): one loaded song
// and everything that plays, shows and grades it. The UI binds its buttons to the actions below;
// until it does, keyboard shortcuts in main.cpp call them. No SDL, no MIDI, no files: the frame
// loop hands in the time, the player's events and the song, and takes out the MIDI messages to
// send and the finished runs to save, so the tests drive it the same way.
//
// Each frame, in this order:
//   update(now, player_events)    the clock, the player's notes, Wait Mode, the countdown
//   take_due_messages(now, out)   the song's MIDI to send now, in order
//   take_finished_runs()          runs to save and show results for
//
// Stuck-note safety: every action that stops or moves the song (pause, seek, replay, song change,
// opening the settings, focus loss, mute, quit) leaves Playback with nothing sounding, and the
// outbox keeps the order Playback produced, so a note-off is never sent before its note-on.
// shutdown() silences and hands back everything still queued; after it needs_silence() is false.
#pragma once

#include "toonval/evaluator.hpp"
#include "toonval/keyboard.hpp"
#include "toonval/midi_event.hpp"
#include "toonval/note_window.hpp"
#include "toonval/playback.hpp"
#include "toonval/progress.hpp"
#include "toonval/run_record.hpp"
#include "toonval/session_state.hpp"
#include "toonval/song.hpp"
#include "toonval/transport.hpp"
#include "toonval/transpose.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace toonval::app {

// What you hear of the song (the song sound setting). Your own playing always sounds.
enum class SongSound : std::uint8_t {
    Mute,      // default: nothing from the song
    OtherHand, // the hand you are not practising; nothing when you practise both
    BothHands, // the whole song
};
const char* song_sound_name(SongSound sound);
PianoPlays to_piano_plays(SongSound sound);
// Left and Right (an older choice) read as OtherHand.
SongSound song_sound_from(PianoPlays plays);
const char* hands_name(HandFilter hands);
const char* mode_name(PracticeMode mode);

// A song ready to play: hands assigned (names, else the smart split), with what identifies it.
struct LoadedSong {
    Song song;
    std::string title;
    std::uint64_t hash = 0;    // content_hash() of the file's bytes
    std::string library_path;  // relative to a library folder, '/' separated; empty when outside
    std::string display_path;  // as the player named it, for messages
    bool hands_split = false;  // the split assigned some hands
};

// A note result as it became final, for the brief grade shown where the note met the hit line.
struct GradeMark {
    std::uint32_t note_index = 0;
    NoteGrade grade = NoteGrade::Missed;
    int key = 0; // the key it was expected on, after transposition
    Hand hand = Hand::Unknown;
    std::int64_t at_ns = 0; // when it became final
};

// A run that ended with graded notes. The record's wall times and day are left at 0: the caller
// knows the time zone. `started_ns` and `ended_ns` are on the frame clock, to place it.
struct FinishedRun {
    RunRecord record;
    std::int64_t started_ns = 0;
    std::int64_t ended_ns = 0;
};

// How long one grade stays on screen.
inline constexpr std::int64_t kGradeShowNs = 900'000'000;
// A countdown tick: a soft high wood block on the piano (channel 10), the same note on the
// built-in piano, which has no drums.
inline constexpr std::uint8_t kTickChannel = 9;
inline constexpr std::uint8_t kTickNote = 76;
inline constexpr std::uint8_t kTickVelocity = 36;
inline constexpr std::int64_t kTickLengthNs = 60'000'000;

struct SongOptions {
    double hit_window_ms = 80.0;   // AppSettings::hit_window_ms
    double latency_ms = 0.0;       // AppSettings::latency_correction_ms
    double lookahead_s = 4.0;      // LookSettings::visible_seconds: what the player can see
    int resume_countdown_s = 3;    // AppSettings::resume_countdown_s, 1..6
    int skip_back_seconds = 5;     // AppSettings::skip_back_seconds, 1..30
    int skip_forward_seconds = 5;  // AppSettings::skip_forward_seconds, 1..30
    friend bool operator==(const SongOptions&, const SongOptions&) = default;
};

class SongController {
  public:
    SongController();
    ~SongController();
    SongController(const SongController&) = delete;
    SongController& operator=(const SongController&) = delete;

    // ---- Song

    // Finishes the run in progress, stops what the old song sounds and starts the new one paused
    // at 0. The practice choices (mode, hands, sound, speed, transposition) stay; the loop goes.
    void load(LoadedSong song, std::int64_t now_ns);
    // Back to free play.
    void unload(std::int64_t now_ns);
    bool has_song() const { return loaded_; }
    const LoadedSong& song() const { return song_; }

    // ---- Transport actions

    void play(std::int64_t now_ns);
    void pause(std::int64_t now_ns);
    // Play, or pause; ends a countdown (paused); at the end of the song, replay().
    void toggle_play(std::int64_t now_ns);
    // From the loop start when a loop is on, else from the start, and play.
    void replay(std::int64_t now_ns);
    // One step back by options().skip_back_seconds, stopping at the loop start (when a loop is
    // on and the position is in it) or at 0. A held button calls it again for each repeat.
    void skip_back(std::int64_t now_ns);
    // One step forward by options().skip_forward_seconds, stopping at the song's end.
    void skip_forward(std::int64_t now_ns);
    // The step sizes, for drawing the buttons ("5 s").
    int skip_back_seconds() const { return options_.skip_back_seconds; }
    int skip_forward_seconds() const { return options_.skip_forward_seconds; }
    void seek(double song_s, std::int64_t now_ns);
    void change_speed(int delta_percent, std::int64_t now_ns);
    void set_speed(int percent, std::int64_t now_ns);

    // A-B loop at bar boundaries: A at the start of the current bar, B at its end. With both set
    // the loop plays; B before A swaps them.
    void set_loop_start_here(std::int64_t now_ns);
    void set_loop_end_here(std::int64_t now_ns);
    // Exact times, as the overview will set them. False when too short.
    bool set_loop(double start_s, double end_s, std::int64_t now_ns);
    void clear_loop(std::int64_t now_ns);

    // ---- Practice choices

    void set_practice_hands(HandFilter hands, std::int64_t now_ns);
    HandFilter practice_hands() const { return hands_; }
    void set_song_sound(SongSound sound, std::int64_t now_ns);
    void cycle_song_sound(std::int64_t now_ns);
    SongSound song_sound() const { return sound_; }
    // Flow or Wait (FreePlay with a song is watching it: nothing graded).
    void set_mode(PracticeMode mode, std::int64_t now_ns);
    void toggle_mode(std::int64_t now_ns); // Flow <-> Wait
    PracticeMode mode() const { return mode_; }
    void set_transpose(int semitones, std::int64_t now_ns);
    int transpose() const { return semitones_; }
    // The player's keyboard: transposed notes outside it fold by octaves.
    void set_keyboard(KeyRange keyboard, std::int64_t now_ns);
    void set_options(const SongOptions& options, std::int64_t now_ns);
    const SongOptions& options() const { return options_; }

    // ---- App events

    // The settings (or any menu) opened: pause and silence. Closed: if the song was playing, or the
    // player asked to play while it was open, count down options().resume_countdown_s seconds
    // with a tick each, then play.
    void menu_opened(std::int64_t now_ns);
    void menu_closed(std::int64_t now_ns);
    bool menu_open() const { return menu_open_; }
    // The window lost focus: pause and silence.
    void lost_focus(std::int64_t now_ns);
    // Quit: the run is finished, the piano silenced, and every queued message handed to `out`.
    void shutdown(std::int64_t now_ns, std::vector<PlaybackMessage>& out);
    // Stops every song note now (a different output took over).
    void silence(std::int64_t now_ns);

    // ---- Per frame

    // The player's NoteOn/NoteOff events of this frame, stamped, in order.
    void update(std::int64_t now_ns, std::span<const MidiEvent> player_events);
    // Messages due by `now_ns`, in the order Playback made them.
    void take_due_messages(std::int64_t now_ns, std::vector<PlaybackMessage>& out);
    std::vector<FinishedRun> take_finished_runs();

    // ---- What the screen shows

    double position_s() const { return transport_.position_s(); }
    double duration_s() const { return transport_.duration_s(); }
    int speed_percent() const { return transport_.speed_percent(); }
    TransportState state() const { return transport_.state(); }
    bool playing() const { return transport_.playing(); }
    bool loop_enabled() const { return transport_.loop_enabled(); }
    double loop_start_s() const { return transport_.loop_start_s(); }
    double loop_end_s() const { return transport_.loop_end_s(); }
    std::optional<double> pending_loop_start() const { return loop_a_; }
    // Bar number at the position (Measure::number).
    int bar() const;
    int bar_count() const;
    const Transposition& transposition() const { return source_.value(); }
    // The notes on screen, `ahead_s` song seconds above the hit line. Every hand is shown; the
    // caller dims the hands not practised.
    void visible_notes(double ahead_s, std::vector<VisibleNote>& out) const;
    // The song's notes after transposition, for Fit song and the key view; empty when none.
    std::optional<KeyRange> key_range() const;
    // Wait Mode while the clock waits at a chord: its keys, with the hand of each (Unknown when
    // both hands share a key). Empty otherwise.
    struct HintKey {
        int key = 0;
        Hand hand = Hand::Unknown;
    };
    std::vector<HintKey> waiting_keys() const;
    bool waiting() const;
    // Grades that became final within kGradeShowNs before `now_ns`, oldest first.
    std::span<const GradeMark> recent_grades() const { return grades_; }
    // The final grade of each note this run, or -1; indexed like Song::notes.
    int grade_of(std::uint32_t note_index) const;
    int combo() const;
    int longest_combo() const;
    const EvaluationSummary* summary() const;
    // Seconds left of the resume countdown (1..6), or 0 when none runs.
    int countdown() const;
    bool counting_down() const { return countdown_end_ns_.has_value(); }
    // One line for the status row: title, mode, hands, speed, bar, sound, combo.
    std::string status_line() const;

    // ---- Saved state

    // `base` with the song's fields set: path (relative to the library), hash, position, speed,
    // hands, sound, loop, mode, transposition. Without a library path the song is not saved.
    SessionState session_state(const SessionState& base) const;
    // Applies a restored session (plan_restore's state) to the loaded song; paused.
    void apply_session(const SessionState& state, std::int64_t now_ns);

    // ---- For tests

    bool needs_silence() const { return playback_.needs_silence(); }
    std::size_t queued_messages() const { return outbox_.size(); }

  private:
    void pump(std::int64_t now_ns);
    void enqueue(std::vector<PlaybackMessage>& msgs);
    void apply_mutes(std::int64_t now_ns);
    void rebuild_evaluators(std::int64_t now_ns);
    void restart_here(std::int64_t now_ns); // seek in place: evaluators start fresh
    // Wait Mode: hold, release or arm the clock as the evaluator says.
    void follow_wait(std::int64_t now_ns);
    void collect_grades(std::int64_t now_ns);
    void finish_run(std::int64_t now_ns);
    void start_run_if_needed(std::int64_t now_ns);
    void start_countdown(std::int64_t now_ns);
    void cancel_countdown();
    double bar_start_at(double song_s) const;

    bool loaded_ = false;
    LoadedSong song_;
    NoteWindow window_;
    Transport transport_;
    Playback playback_;
    FlowEvaluator flow_;
    WaitEvaluator wait_;
    TranspositionSource source_;
    KeyRange keyboard_{0, 127};
    int semitones_ = 0;

    SongOptions options_;
    PracticeMode mode_ = PracticeMode::Flow;
    HandFilter hands_ = HandFilter::Both;
    SongSound sound_ = SongSound::Mute;
    std::optional<double> loop_a_;
    std::optional<double> loop_b_;

    std::vector<TransportOp> ops_;
    std::vector<PlaybackMessage> scratch_;
    std::deque<PlaybackMessage> outbox_;
    std::int64_t outbox_last_ns_ = INT64_MIN;
    std::int64_t last_now_ns_ = 0;

    // The run: from the first play after a load or a finish, to the end, a replay, a song or
    // choice change, or quit.
    bool run_active_ = false;
    std::int64_t run_started_ns_ = 0;
    std::int64_t run_playing_ns_ = 0;
    std::vector<PassResult> passes_;
    std::vector<FinishedRun> finished_;

    std::vector<GradeMark> grades_;
    std::vector<std::int8_t> grade_by_note_;
    std::size_t results_seen_ = 0;

    bool menu_open_ = false;
    bool resume_after_menu_ = false;
    std::optional<std::int64_t> countdown_end_ns_;
    int countdown_ticks_left_ = 0;
};

// "Right 92%, Left 85% · longest combo 41 · up 4% on last time": the results of a run, accuracy for
// each hand that had graded notes.
std::string results_line(const RunRecord& record, const AttemptChange& change);

} // namespace toonval::app
