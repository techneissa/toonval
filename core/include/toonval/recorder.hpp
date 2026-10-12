// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "toonval/midi_event.hpp"
#include "toonval/song.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <queue>
#include <string>
#include <vector>

namespace toonval {

struct RecorderOptions {
    double bpm = 120.0;
    int ticks_per_quarter = 480;
    // The song starts this long before the first note-on, so a recording left running for ten
    // minutes before the first key does not open with ten minutes of silence.
    double lead_s = 0.25;
    // Events may reach feed() out of time order by a few milliseconds (two input ports, a thread
    // switch). They are held this long and handled in time order.
    std::int64_t reorder_window_ns = 20'000'000;
    // Limits that keep a session left running for days from filling memory. Past a limit the
    // recorder drops what it would add and sets RecordedSong::truncated.
    std::size_t max_notes = 2'000'000;
    std::size_t max_events = 4'000'000;
    std::string part_name = "Free play";
};

struct RecordedSong {
    bool has_notes = false; // false: nothing was played, and `song` is empty
    Song song;
    int notes = 0;
    int events = 0;         // controller events kept (the sustain pedal and other CCs)
    bool truncated = false; // a limit was hit, or the song has more bars than finish_song builds
};

// Records free play into a Song, then lets the caller write it with write_midi().
//
// Feed it the same timestamped MidiEvents the live path produces (time_ns from the MIDI thread's
// clock; only differences matter). Rules, each with a test in tests/recorder_test.cpp:
// - A NoteOn with velocity 0 is a NoteOff. A NoteOn of a sounding pitch and channel ends that
//   note there and starts a new one. A NoteOff with no note sounding is ignored.
// - The sustain pedal and other controllers are kept as ChannelEvents (CC64 included) and never
//   change a note's length. Controllers set before the first note are kept as one event per
//   channel and controller, at the start, holding the last value.
// - All Sound Off and All Notes Off (CC120, CC123; the synthetic one from a dropped-event gap too)
//   end every sounding note on every channel, like KeyState releases every key. They are not saved.
// - Notes still sounding at finish() end at the stop time. A note is at least one tick long.
// - The song's tick 0 is the first note-on minus `lead_s`. Events before it sit at tick 0.
// - Out-of-order events within the reorder window are handled in time order (equal times keep
//   arrival order). One later than that is handled at the latest time already handled.
// - Memory is a fixed table of sounding notes plus a small buffer of events inside the reorder
//   window, then 24 bytes per finished note and 16 per controller event; work is O(log window)
//   per event and O(n log n) once at finish().
// - Notes go to track 1, one part per channel named `part_name`, hand Unknown; track 0 holds only
//   the tempo and time signature when written. Bars are 4/4.
//
// One thread: the one that owns the frame loop's MIDI drain. Not thread-safe.
class Recorder {
  public:
    explicit Recorder(const RecorderOptions& options = {});

    void feed(const MidiEvent& ev);

    // Builds the Song and starts a fresh recording.
    RecordedSong finish(std::int64_t stop_time_ns);

    // Forgets everything recorded so far.
    void reset();

    // Finished notes so far (not those still sounding).
    std::size_t note_count() const { return notes_.size(); }
    // Events held in the reorder window.
    std::size_t buffered() const { return pending_.size(); }

  private:
    struct Pending {
        std::int64_t time_ns;
        std::uint64_t seq;
        MidiEvent ev;
    };
    struct PendingLater {
        bool operator()(const Pending& a, const Pending& b) const {
            return a.time_ns != b.time_ns ? a.time_ns > b.time_ns : a.seq > b.seq;
        }
    };
    struct Sounding {
        bool active = false;
        std::uint8_t velocity = 0;
        std::int64_t start_ns = 0;
    };
    struct RawNote {
        std::int64_t start_ns;
        std::int64_t end_ns;
        std::uint8_t channel;
        std::uint8_t pitch;
        std::uint8_t velocity;
    };
    struct RawControl {
        std::int64_t time_ns;
        std::uint8_t channel;
        std::uint8_t number;
        std::uint8_t value;
    };

    void handle(const MidiEvent& ev, std::int64_t time_ns);
    void end_note(int channel, int pitch, std::int64_t time_ns);
    void end_all(std::int64_t time_ns);
    void add_control(std::int64_t time_ns, int channel, int number, int value);

    RecorderOptions options_;
    std::priority_queue<Pending, std::vector<Pending>, PendingLater> pending_;
    std::uint64_t seq_ = 0;
    bool any_event_ = false;
    std::int64_t max_seen_ns_ = 0;
    std::int64_t handled_ns_ = 0; // time of the latest event handled; nothing is handled earlier
    std::array<std::array<Sounding, 128>, 16> sounding_{};
    std::vector<RawNote> notes_;
    std::vector<RawControl> controls_;
    // Controllers before the first note: last value per channel and controller, -1 when unset.
    std::vector<std::int16_t> pre_note_;
    std::int64_t pre_note_time_ns_ = 0;
    bool started_ = false;
    std::int64_t first_note_ns_ = 0;
    bool truncated_ = false;
};

} // namespace toonval
