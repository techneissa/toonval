// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "toonval/keyboard.hpp"
#include "toonval/note_window.hpp"
#include "toonval/settings_doc.hpp"
#include "toonval/transport.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace toonval {

// What the app keeps between runs, in two pieces: the session (where you left off
// in a song) and the app settings (what does not depend on the song). The look and the user preset
// slots are a third piece with their own model; they join the same file as the `[look]` and
// `[user_presets]` sections (see LoadedSettings).
//
// Every field has a default, a valid range, and a clamp. `clamped()` is the one place that knows
// the ranges: the file reader, the writer and the restore check all go through it, so no state
// that came from a file, however damaged, can reach the app out of range.

inline constexpr std::string_view kAppSection = "app";
inline constexpr std::string_view kSessionSection = "session";

inline constexpr double kMaxSessionSeconds =
    24.0 * 60.0 * 60.0; // a song longer than a day is a bug
inline constexpr std::size_t kMaxSongPathBytes = 4096;
inline constexpr std::size_t kMaxNameBytes = 256; // keyboard and MIDI port names
inline constexpr int kMaxCountInBeats = 16;
inline constexpr int kMaxSessionSectionId = 2'000'000'000; // as kMaxSectionId in sections.hpp
inline constexpr int kMaxSessionPlanItems = 64;            // as kMaxPlanItems in sections.hpp
inline constexpr int kMinKeyboardKeys = 12; // a manual range narrower than an octave is a typo

// Index 0 is the default of every enum below.
enum class PracticeMode : std::uint8_t {
    FreePlay, // no grading: the song plays or waits for nothing
    Wait,     // the song stops at the hit line until the right notes are down
    Flow,     // the song keeps going and each note is graded
};

// What you hear of the song while you practise (Playback mutes the rest).
// Your own playing always sounds.
enum class PianoPlays : std::uint8_t {
    Nothing,   // Mute: you hear only what you play
    OtherHand, // the hand you are not practising; nothing when you practise both
    Left,
    Right,
    Both, // the whole song: watching it
};

// How the control bar looks when revealed: a full-width strip (the default), floating groups, or a
// centred dock. A UI preference, not part of the look: presets leave it alone.
enum class BarStyle : std::uint8_t {
    Strip,  // a full-width translucent strip
    Groups, // floating button groups with captions under the icons
    Dock,   // a centred floating dock
};

enum class KeyboardChoice : std::uint8_t {
    Auto,    // match the connected device by name (find_profile), else ask
    Profile, // the built-in profile called `keyboard_name`
    Manual,  // `keyboard_lowest` .. `keyboard_highest`
};

struct SessionState {
    // Song identity. The path is relative to the song library folder, with '/' separators, and
    // never absolute or containing "..". Empty means no song (free play). The hash is
    // content_hash() of the file's bytes; 0 means none was recorded.
    std::string song_path;       // default empty; valid UTF-8 without control characters
    std::uint64_t song_hash = 0; // default 0

    double position_s = 0.0; // 0 .. kMaxSessionSeconds; default 0. Always paused.
    int speed_percent = 100; // 25 .. 150 in steps of 5; default 100
    HandFilter practice_hands = HandFilter::Both; // the hand(s) you practise; default Both
    PianoPlays piano_plays = PianoPlays::Nothing; // default Mute

    // The A-B loop. A loop exists when both ends are in 0 .. kMaxSessionSeconds and are at least
    // kMinLoopSeconds apart (start before end). Without one both ends are 0 and it is off.
    double loop_start_s = 0.0;
    double loop_end_s = 0.0;
    bool loop_enabled = false;

    // The auto speed-up policy (AutoSpeedUp in transport.hpp).
    bool auto_speed_enabled = false;
    int auto_speed_start_percent = 60; // 25 .. 150, steps of 5
    int auto_speed_step_percent = 5;   // 5 .. 25, steps of 5
    int auto_speed_max_percent = 100;  // start .. 150, steps of 5

    // Where the player was in the song's sections (sections.hpp), so a restart returns to it:
    // the section looped (a SectionId of `song_hash`'s SongSections; 0 is none) and the item of the
    // practice plan that was playing (1-based place in SongSections::plan; 0 is no plan). Both
    // belong to the song: without one they are 0. The app checks them against the sections it
    // loads, since this layer does not know which sections exist, and drops what is gone.
    int section_id = 0; // 0 .. kMaxSessionSectionId
    int plan_item = 0;  // 0 .. kMaxSessionPlanItems

    int count_in_beats = 0; // 0 .. 16; 0 is off
    bool metronome = false;
    PracticeMode mode = PracticeMode::FreePlay;
    int transpose_semitones = 0; // -12 .. +12

    // The keyboard. In Profile mode the name picks a built-in profile and the range is the one
    // saved with it, used when this build does not know the name. A usable range is always kept,
    // even in Auto mode: ordered, inside 0..127, both ends white keys, at least 12 keys.
    KeyboardChoice keyboard = KeyboardChoice::Auto;
    std::string keyboard_name; // <= 256 bytes; empty in Profile mode becomes Auto
    int keyboard_lowest = 36;
    int keyboard_highest = 96;

    bool has_song() const { return !song_path.empty(); }
    bool has_loop() const { return loop_end_s - loop_start_s >= kMinLoopSeconds; }
    AutoSpeedUp auto_speed_up() const {
        return AutoSpeedUp{auto_speed_enabled, auto_speed_start_percent, auto_speed_step_percent,
                           auto_speed_max_percent};
    }
    KeyRange keyboard_range() const { return KeyRange{keyboard_lowest, keyboard_highest}; }

    // The same state with every field forced into its valid range (see above). A path that is
    // absolute, climbs out with "..", is not UTF-8, holds a control character, or is too long
    // becomes empty, and the song fields of an empty path go back to their defaults. Idempotent.
    SessionState clamped() const;

    friend bool operator==(const SessionState&, const SessionState&) = default;
};

struct AppSettings {
    int hit_window_ms = 80;        // 20 .. 250: Flow Mode's window either side of a note
    int latency_correction_ms = 0; // -250 .. 250: the calibrated offset of this setup
    std::string midi_in_port;      // <= 256 bytes; empty picks the keyboard automatically
    std::string midi_out_port;     // <= 256 bytes; empty picks the output automatically
    int daily_goal_minutes = 15;   // 0 .. 720; 0 turns the goal off
    // Closing the settings while a song was playing counts down this many seconds, one soft tick
    // each, before the song plays on: 1 .. 6, default 3.
    int resume_countdown_s = 3;
    // Skip back and skip forward each move by their own number of seconds: 1 .. 30, default 5.
    // A tap moves one step; holding the button (or key) repeats it.
    int skip_back_seconds = 5;
    int skip_forward_seconds = 5;
    BarStyle bar_style = BarStyle::Strip;

    AppSettings clamped() const;

    friend bool operator==(const AppSettings&, const AppSettings&) = default;
};

// Reading never fails. A missing key keeps the default. A value that does not parse keeps the
// default (BadValue); one outside its range is clamped (OutOfRange); keys this build does not know,
// malformed lines and repeated keys are reported too. The result is always clamped. Line numbers in
// the issues refer to the text the document was parsed from.
SessionState read_session(const SettingsDocument& doc,
                          std::vector<SettingsIssue>* issues = nullptr);
AppSettings read_app_settings(const SettingsDocument& doc,
                              std::vector<SettingsIssue>* issues = nullptr);

// Writes every key of the (clamped) value into its section, creating it when missing. Other keys
// and sections of the document are not touched, and a key already holding the same value keeps its
// line as typed.
void write_session(SettingsDocument& doc, const SessionState& state);
void write_app_settings(SettingsDocument& doc, const AppSettings& settings);

// A whole file: the document with everything in it, and the typed sections read from it. The app
// edits `app` and `session` and calls serialize(); sections this build does not know, like a
// newer build's, come back byte for byte.
struct LoadedSettings {
    SettingsDocument document;
    AppSettings app;
    SessionState session;
    std::vector<SettingsIssue> issues;

    // Newer major version: load it, but writing it back could damage what the newer build means.
    bool read_only_recommended() const { return document.read_only_recommended(); }

    // The document with `app` and `session` written into a copy of it, with a format line.
    std::string serialize() const;
};

// Parses `text` and reads the `[app]` and `[session]` sections. Sections with other names are
// reported as UnknownSection unless listed in `other_known_sections`: a reader that owns another
// section (`look`, `user_presets`) names it there, takes its text with
// `document.section_text(name)`, and writes it back with `document.set_section_text`.
LoadedSettings load_settings(std::string_view text,
                             std::span<const std::string_view> other_known_sections = {});

} // namespace toonval
