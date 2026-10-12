// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "toonval/hand.hpp"
#include "toonval/song.hpp"
#include "toonval/transport.hpp"
#include "toonval/transpose.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace toonval {

// One message for MIDI out. `at_ns` is when to send it, on the same clock as the Transport's
// `now_ns`: the time the song reaches the message at the speed it plays, or the time of the command
// for messages that react to one (stopping notes on a pause). `song_s` is where in the song it
// belongs.
struct PlaybackMessage {
    enum class Kind : std::uint8_t { NoteOn, NoteOff, ProgramChange, ControlChange, PitchBend };
    Kind kind = Kind::NoteOn;
    std::uint8_t channel = 0; // 0..15
    std::uint8_t data = 0;    // note number, program number or controller number; 0 for a bend
    std::uint16_t value = 0; // velocity, controller value, or 14-bit bend (8192 is centre); 0 for a
                             // program change
    std::int64_t at_ns = 0;
    double song_s = 0.0;
};

// The length given to a note whose own length is zero, negative or not a number, so its note-off
// still comes (the .mid loader drops such notes; a hand-built or MusicXML song may not).
inline constexpr double kMinNoteSeconds = 0.030;

// The metronome's voice: General MIDI percussion on channel 10 by default (Hi and Low Wood Block).
// Percussion notes are one-shot; the note-off follows `length_ns` later.
struct ClickSound {
    bool enabled = false;
    std::uint8_t channel = 9;
    std::uint8_t accent_note = 76;
    std::uint8_t note = 77;
    std::uint8_t accent_velocity = 127;
    std::uint8_t velocity = 90;
    std::int64_t length_ns = 20'000'000;
};

// Turns the Transport's ops into the MIDI messages that make the piano play the song.
//
// Rules, each with a test in tests/playback_test.cpp:
// - The piano never gets a stuck note. Pause, seek, loop wrap, the end of the song, a new song and
//   silence() send a note-off for every sounding note, the pedal up on every channel where it is
//   down (with or without notes of its own), and CC123 (all notes off) on every channel that has
//   played a note since the last time. A note of zero, negative or unknown length still stops:
//   it gets kMinNoteSeconds. A note with no start time is never sent.
// - After a seek, a loop wrap or a play, the piano gets the program change, controllers and pitch
//   bend the file has set at that position (events at or before it), bank select before program,
//   and the defaults for anything the file sets only later. Only what differs from what the piano
//   already has is sent. Notes already sounding at that position are not started.
// - A hand can be muted ("the piano plays the other hand"): its notes are not sent, and muting a
//   hand whose notes are sounding stops them at once. Unmuting does not start a note in progress.
// - A hold (Wait Mode) sends nothing: notes keep sounding until their note-off, or until a pause.
// - Notes are sent at the pitch Transposition::map_note gives, the same key the display shows and
//   the evaluator expects. A note folded onto the keyboard plays folded, a note no key can play is
//   not sent, and drum notes and clicks keep their numbers.
// - Changing the transposition while the piano sounds stops every sounding note at its old pitch
//   first (the panic), so none is left stuck. The notes in progress are not restarted.
// - Messages inside one Transport Move get their times from the Move's wall span, so speed
//   changes the timing and never the pitch.
//
// Order at one song time: note-offs, then program/controller/bend events, then note-ons.
// Copies what it needs from the Song and keeps no reference to it.
class Playback {
  public:
    Playback() = default;
    explicit Playback(const Song& song) { load(song); }

    // Switches to another song. Stops what the old song has sounding, as silence() does.
    void set_song(const Song& song, std::int64_t now_ns, std::vector<PlaybackMessage>& out);

    // Feeds one op of the Transport, in the order the Transport produced them. Appends to `out`.
    void apply(const TransportOp& op, std::vector<PlaybackMessage>& out);

    // The panic: stops everything now, for quit, a lost connection, or a song change.
    void silence(std::int64_t now_ns, std::vector<PlaybackMessage>& out);

    // Hand::Unknown can be muted too. A muted hand's sounding notes get their note-off now.
    void set_hand_muted(Hand hand, bool muted, std::int64_t now_ns,
                        std::vector<PlaybackMessage>& out);
    bool hand_muted(Hand hand) const { return hand_muted_[static_cast<std::size_t>(hand)]; }
    // Channel-10 notes (accompaniment drums).
    void set_drums_muted(bool muted, std::int64_t now_ns, std::vector<PlaybackMessage>& out);
    bool drums_muted() const { return drums_muted_; }

    // Applies from the next note on. A change of the shift or the keyboard range stops the sounding
    // notes first (as a pause does) and, while playing, puts the pedal and controllers back as the
    // file has them at the current position. The same transposition again does nothing.
    void set_transposition(const Transposition& transposition, std::int64_t now_ns,
                           std::vector<PlaybackMessage>& out);
    const Transposition& transposition() const { return transposition_; }

    void set_click_sound(const ClickSound& sound) { click_ = sound; }
    const ClickSound& click_sound() const { return click_; }

    // Notes that have been started and not stopped.
    std::size_t sounding_count() const { return active_.size(); }
    bool running() const { return running_; }

    // True while the piano may still sound because of this Playback: a note started and not
    // stopped, a pedal left down, or a channel that has played since the last panic.
    //
    // Playback has no MIDI output of its own, so its destructor can't stop the piano. Before
    // dropping a Playback (quit, closing the song, losing the output), the app calls silence() and
    // sends what it returns; needs_silence() is then false.
    bool needs_silence() const;
    // Called from the destructor when needs_silence() is still true, so the app can log the bug or
    // assert in a debug build. Moves with the Playback; a moved-from one never calls it.
    void set_teardown_check(void (*check)(const Playback&)) { teardown_.check = check; }

    // Reads the transposition from `source` (the session's one copy) instead of
    // set_transposition(): apply() picks up a change at the next op, as set_transposition() at that
    // op's time would. The source must outlive this Playback, or be unset with follow(nullptr).
    void follow(const TranspositionSource* source);

    Playback(const Playback&) = default;
    Playback(Playback&&) = default;
    Playback& operator=(const Playback&) = default;
    Playback& operator=(Playback&&) = default;
    ~Playback();

  private:
    struct Item {
        double start_s = 0.0;
        double end_s = 0.0;
        std::uint8_t channel = 0;
        std::uint8_t pitch = 0;
        std::uint8_t velocity = 0;
        Hand hand = Hand::Unknown;
        bool drum = false;
    };
    // What the piano has been given for one channel; -1 means never sent.
    struct ChannelState {
        int program = -1;
        int bend = -1;
        std::array<int, 128> cc;
        ChannelState() { cc.fill(-1); }
    };
    // What the file has set on one channel up to some time; -1 means not yet. Controllers from 120
    // up are channel mode messages, not settings, and are not kept.
    struct FileChannel {
        std::int16_t program = -1;
        std::int16_t bend = -1;
        std::array<std::int8_t, 120> cc;
        FileChannel() { cc.fill(-1); }
    };
    using FileState = std::array<FileChannel, 16>;
    struct TeardownSlot {
        void (*check)(const Playback&) = nullptr;
        TeardownSlot() = default;
        TeardownSlot(const TeardownSlot&) = default;
        TeardownSlot& operator=(const TeardownSlot&) = default;
        TeardownSlot(TeardownSlot&& other) noexcept : check(other.check) { other.check = nullptr; }
        TeardownSlot& operator=(TeardownSlot&& other) noexcept {
            check = other.check;
            other.check = nullptr;
            return *this;
        }
    };

    void load(const Song& song);
    void send(PlaybackMessage::Kind kind, std::uint8_t channel, std::uint8_t data,
              std::uint16_t value, std::int64_t at_ns, double song_s,
              std::vector<PlaybackMessage>& out);
    void send_event(const ChannelEvent& e, std::int64_t at_ns, std::vector<PlaybackMessage>& out);
    // Ends the note in `active_slot`; the note-off goes out unless another sounding note shares its
    // channel and key.
    void release(std::size_t active_slot, std::int64_t at_ns, double song_s,
                 std::vector<PlaybackMessage>& out);
    void panic(std::int64_t at_ns, double song_s, std::vector<PlaybackMessage>& out);
    void resync(double position_s, std::int64_t at_ns, std::vector<PlaybackMessage>& out);
    void restore_state(double position_s, std::int64_t at_ns, std::vector<PlaybackMessage>& out);
    void sync_transposition(std::int64_t at_ns, std::vector<PlaybackMessage>& out);
    static void apply_file_event(const ChannelEvent& e, FileState& state);
    void play_span(const TransportOp& move, std::vector<PlaybackMessage>& out);
    void click(const TransportOp& op, std::vector<PlaybackMessage>& out);
    bool muted(const Item& n) const;
    // The key the piano is given for a note, or -1 when the keyboard has none.
    int sounded_pitch(const Item& n) const;

    std::vector<Item> notes_;           // sorted by start
    std::vector<std::uint32_t> by_end_; // note indices sorted by end
    std::vector<ChannelEvent> events_;  // sorted by time
    // checkpoints_[k] is what the file has set before events_[k * kCheckpointEvents], so restoring
    // the state at a position replays at most kCheckpointEvents events, not the whole song.
    static constexpr std::size_t kCheckpointEvents = 512;
    std::vector<FileState> checkpoints_;

    std::size_t next_on_ = 0;
    std::size_t next_off_ = 0;
    std::size_t next_event_ = 0;
    bool running_ = false;
    double position_s_ = 0.0;
    std::vector<std::uint32_t> active_; // notes started and not yet ended
    std::array<ChannelState, 16> sent_;
    std::uint16_t used_channels_ = 0; // bit per channel that has had a note-on since the last panic

    std::array<bool, 3> hand_muted_{};
    bool drums_muted_ = false;
    Transposition transposition_;
    ClickSound click_;
    TeardownSlot teardown_;
    const TranspositionSource* source_ = nullptr;
    std::uint64_t source_version_ = 0;
    bool source_synced_ = false;
};

} // namespace toonval
