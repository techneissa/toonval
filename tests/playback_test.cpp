// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/playback.hpp"

#include "toonval/evaluator.hpp"
#include "toonval/keyboard.hpp"
#include "toonval/midi_file.hpp"
#include "toonval/note_window.hpp"
#include "toonval/transport.hpp"
#include "toonval/transpose.hpp"

#include "smf_builder.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <utility>
#include <vector>

using namespace toonval;
using namespace toonval::test;
using MsgKind = PlaybackMessage::Kind;
using OpKind = TransportOp::Kind;

namespace {

constexpr std::int64_t sec(double s) { return static_cast<std::int64_t>(s * 1e9); }
// 120 BPM and 480 ticks per quarter: a second is 960 ticks.
constexpr std::uint32_t tk(double s) { return static_cast<std::uint32_t>(s * 960.0); }

// One track written with absolute ticks, so a test reads as a list of what happens when.
class AbsTrack {
  public:
    explicit AbsTrack(std::string name) : name_(std::move(name)) {}
    AbsTrack& on(std::uint32_t tick, int ch, int pitch, int vel = 100) {
        return add(tick, {u8(0x90 | ch), u8(pitch), u8(vel)});
    }
    AbsTrack& off(std::uint32_t tick, int ch, int pitch) {
        return add(tick, {u8(0x80 | ch), u8(pitch), 64});
    }
    AbsTrack& note(double start_s, double end_s, int ch, int pitch, int vel = 100) {
        return on(tk(start_s), ch, pitch, vel).off(tk(end_s), ch, pitch);
    }
    AbsTrack& cc(double at_s, int ch, int controller, int value) {
        return add(tk(at_s), {u8(0xB0 | ch), u8(controller), u8(value)});
    }
    AbsTrack& program(double at_s, int ch, int program) {
        return add(tk(at_s), {u8(0xC0 | ch), u8(program)});
    }
    AbsTrack& bend(double at_s, int ch, int value14) {
        return add(tk(at_s), {u8(0xE0 | ch), u8(value14 & 0x7F), u8(value14 >> 7)});
    }
    Bytes body() const {
        std::vector<std::pair<std::uint32_t, Bytes>> sorted = events_;
        std::stable_sort(sorted.begin(), sorted.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        Bytes out;
        put_vlq(out, 0);
        out.insert(out.end(), {0xFF, 0x03, static_cast<std::uint8_t>(name_.size())});
        out.insert(out.end(), name_.begin(), name_.end());
        std::uint32_t last = 0;
        for (const auto& [tick, bytes] : sorted) {
            put_vlq(out, tick - last);
            out.insert(out.end(), bytes.begin(), bytes.end());
            last = tick;
        }
        out.insert(out.end(), {0x00, 0xFF, 0x2F, 0x00});
        return out;
    }

  private:
    static std::uint8_t u8(int v) { return static_cast<std::uint8_t>(v); }
    AbsTrack& add(std::uint32_t tick, Bytes bytes) {
        events_.emplace_back(tick, std::move(bytes));
        return *this;
    }
    std::string name_;
    std::vector<std::pair<std::uint32_t, Bytes>> events_;
};

Song make_song(const std::vector<AbsTrack>& tracks) {
    Bytes file = header(1, static_cast<int>(tracks.size()), 480);
    for (const AbsTrack& t : tracks) {
        put_chunk(file, "MTrk", t.body());
    }
    MidiLoadResult r = load_midi(file.data(), file.size());
    REQUIRE(r.ok);
    return std::move(r.song);
}

// A stand-in for the piano: what a real General MIDI keyboard would end up doing with the stream.
class FakePiano {
  public:
    FakePiano() {
        for (std::size_t ch = 0; ch < 16; ++ch) {
            bend_[ch] = 8192;
            cc_[ch].fill(0);
            cc_[ch][7] = 100;
            cc_[ch][10] = 64;
            cc_[ch][11] = 127;
            cc_[ch][91] = 40;
            key_[ch].fill(false);
            held_[ch].fill(false);
        }
    }
    void feed(const std::vector<PlaybackMessage>& messages) {
        for (const PlaybackMessage& m : messages) {
            CHECK(m.channel < 16);
            apply(m);
            log.push_back(m);
        }
    }
    bool ringing() const {
        for (std::size_t ch = 0; ch < 16; ++ch) {
            for (std::size_t k = 0; k < 128; ++k) {
                if (key_[ch][k] || held_[ch][k]) {
                    return true;
                }
            }
        }
        return false;
    }
    bool key_down(int ch, int note) const { return key_[idx(ch)][idx(note)]; }
    int program(int ch) const { return program_[idx(ch)]; }
    int controller(int ch, int n) const { return cc_[idx(ch)][idx(n)]; }
    int bend(int ch) const { return bend_[idx(ch)]; }
    std::vector<PlaybackMessage> log;

  private:
    static std::size_t idx(int v) { return static_cast<std::size_t>(v); }
    void apply(const PlaybackMessage& m) {
        const std::size_t ch = m.channel;
        switch (m.kind) {
        case MsgKind::NoteOn:
            key_[ch][m.data] = true;
            break;
        case MsgKind::NoteOff:
            key_[ch][m.data] = false;
            if (cc_[ch][64] >= 64) {
                held_[ch][m.data] = true; // the pedal keeps it ringing
            }
            break;
        case MsgKind::ProgramChange:
            program_[ch] = m.data;
            break;
        case MsgKind::PitchBend:
            bend_[ch] = m.value;
            break;
        case MsgKind::ControlChange:
            if (m.data == 123) {
                key_[ch].fill(false);
                held_[ch].fill(false);
                break;
            }
            if (m.data == 121) {
                for (std::size_t n = 1; n < 120; ++n) {
                    if (n != 32) {
                        cc_[ch][n] = n == 7 ? 100 : n == 10 ? 64 : n == 11 ? 127 : n == 91 ? 40 : 0;
                    }
                }
                bend_[ch] = 8192;
                held_[ch].fill(false);
                break;
            }
            cc_[ch][m.data] = m.value;
            if (m.data == 64 && m.value < 64) {
                held_[ch].fill(false);
            }
            break;
        }
    }
    std::array<std::array<bool, 128>, 16> key_{};
    std::array<std::array<bool, 128>, 16> held_{};
    std::array<std::array<int, 128>, 16> cc_{};
    std::array<int, 16> program_{};
    std::array<int, 16> bend_{};
};

// A transport, a playback and a piano wired together the way the app will wire them.
struct Rig {
    Song song;
    Transport transport;
    Playback playback;
    FakePiano piano;
    std::vector<PlaybackMessage> last; // what the last pump() produced

    explicit Rig(Song s) : song(std::move(s)), playback(song) { transport.set_song(song); }

    void pump() {
        std::vector<TransportOp> ops;
        transport.take_ops(ops);
        last.clear();
        for (const TransportOp& op : ops) {
            playback.apply(op, last);
        }
        piano.feed(last);
    }
    void play(double now_s) { transport.play(sec(now_s)), pump(); }
    void pause(double now_s) { transport.pause(sec(now_s)), pump(); }
    void update(double now_s) { transport.update(sec(now_s)), pump(); }
    void seek(double now_s, double to_s) { transport.seek(sec(now_s), to_s), pump(); }
};

std::vector<PlaybackMessage> of_kind(const std::vector<PlaybackMessage>& v, MsgKind kind) {
    std::vector<PlaybackMessage> out;
    std::copy_if(v.begin(), v.end(), std::back_inserter(out),
                 [&](const PlaybackMessage& m) { return m.kind == kind; });
    return out;
}

// "on 60", "off 60", "pc 5", "cc 64=127", "bend 8192", in order, ignoring times.
std::vector<std::string> describe(const std::vector<PlaybackMessage>& v) {
    std::vector<std::string> out;
    for (const PlaybackMessage& m : v) {
        std::string d;
        switch (m.kind) {
        case MsgKind::NoteOn:
            d = "on " + std::to_string(m.data);
            break;
        case MsgKind::NoteOff:
            d = "off " + std::to_string(m.data);
            break;
        case MsgKind::ProgramChange:
            d = "pc " + std::to_string(m.data);
            break;
        case MsgKind::ControlChange:
            d = "cc " + std::to_string(m.data) + "=" + std::to_string(m.value);
            break;
        case MsgKind::PitchBend:
            d = "bend " + std::to_string(m.value);
            break;
        }
        d += "@" + std::to_string(m.channel);
        out.push_back(d);
    }
    return out;
}

using Strings = std::vector<std::string>;

std::vector<PlaybackMessage> note_log(const std::vector<PlaybackMessage>& v) {
    std::vector<PlaybackMessage> notes;
    std::copy_if(v.begin(), v.end(), std::back_inserter(notes), [](const PlaybackMessage& m) {
        return m.kind == MsgKind::NoteOn || m.kind == MsgKind::NoteOff;
    });
    return notes;
}

// The notes only: the all-notes-off at the end of a song is not what these tests are about.
Strings note_messages(const std::vector<PlaybackMessage>& v) {
    std::vector<PlaybackMessage> notes;
    std::copy_if(v.begin(), v.end(), std::back_inserter(notes), [](const PlaybackMessage& m) {
        return m.kind == MsgKind::NoteOn || m.kind == MsgKind::NoteOff;
    });
    return describe(notes);
}

} // namespace

// ---- Notes, timing and order --------------------------------------------------------------------

TEST_CASE("playback: notes are sent at their start and stopped at their end") {
    Rig r(make_song({AbsTrack("Right").note(0.0, 1.0, 0, 60, 90).note(1.0, 2.0, 0, 64, 70)}));
    r.play(10.0);
    r.update(14.0);
    CHECK(note_messages(r.piano.log) == Strings{"on 60@0", "off 60@0", "on 64@0", "off 64@0"});
    const std::vector<PlaybackMessage> log = note_log(r.piano.log);
    REQUIRE(log.size() == 4);
    CHECK(log[0].value == 90);
    CHECK(log[0].at_ns == sec(10.0));
    CHECK(log[1].at_ns == sec(11.0));
    CHECK(log[2].value == 70);
    CHECK(log[2].at_ns == sec(11.0));
    CHECK(log[3].at_ns == sec(12.0));
    CHECK(log[3].song_s == doctest::Approx(2.0));
    CHECK_FALSE(r.piano.ringing());
}

TEST_CASE("playback: the notes between two updates are sent once, whatever the update rate") {
    for (double step : {0.016, 0.1, 0.7, 5.0}) {
        Rig r(make_song(
            {AbsTrack("Right").note(0.0, 1.0, 0, 60).note(1.5, 2.5, 0, 62).note(3.0, 3.2, 0, 64)}));
        r.play(0.0);
        for (double t = step; t < 4.0 + step; t += step) {
            r.update(t);
        }
        CAPTURE(step);
        CHECK(of_kind(r.piano.log, MsgKind::NoteOn).size() == 3);
        CHECK(of_kind(r.piano.log, MsgKind::NoteOff).size() == 3);
        CHECK_FALSE(r.piano.ringing());
    }
}

TEST_CASE("playback: speed changes the timing of messages, not their content") {
    Rig r(make_song({AbsTrack("Right").note(1.0, 2.0, 0, 60)}));
    r.transport.set_speed_percent(0, 50);
    r.play(0.0);
    r.update(6.0);
    const std::vector<PlaybackMessage> log = note_log(r.piano.log);
    REQUIRE(log.size() == 2);
    CHECK(log[0].data == 60);
    CHECK(log[0].at_ns == sec(2.0)); // song second 1 at half speed
    CHECK(log[1].at_ns == sec(4.0));
}

TEST_CASE("playback: a speed change in the middle of a note keeps its times consistent") {
    Rig r(make_song({AbsTrack("Right").note(1.0, 3.0, 0, 60)}));
    r.play(0.0);
    r.update(2.0); // song 2.0: the note has started at wall 1.0
    r.transport.set_speed_percent(sec(2.0), 50);
    r.update(
        5.0); // 3 s wall at half speed = 1.5 song seconds: the note ends at song 3.0 = wall 4.0
    const std::vector<PlaybackMessage> log = note_log(r.piano.log);
    REQUIRE(log.size() == 2);
    CHECK(log[0].at_ns == sec(1.0));
    CHECK(log[1].at_ns == sec(4.0));
}

TEST_CASE("playback: at one time the note-off comes first, then events, then the note-on") {
    Rig r(make_song({AbsTrack("Right")
                         .note(0.0, 1.0, 0, 60)
                         .program(1.0, 0, 40)
                         .cc(1.0, 0, 7, 50)
                         .note(1.0, 2.0, 0, 60) // the same key struck again at the same instant
                         .note(0.5, 1.0, 0, 62)}));
    r.play(0.0);
    r.update(3.0);
    std::vector<std::string> d = describe(r.piano.log);
    auto pos = [&](const std::string& s, std::size_t from = 0) {
        return static_cast<std::size_t>(
            std::find(d.begin() + static_cast<std::ptrdiff_t>(from), d.end(), s) - d.begin());
    };
    std::size_t first_on = pos("on 60@0");
    std::size_t off = pos("off 60@0");
    std::size_t pc = pos("pc 40@0");
    std::size_t vol = pos("cc 7=50@0");
    std::size_t second_on = pos("on 60@0", first_on + 1);
    REQUIRE(second_on < d.size());
    CHECK(off < pc);
    CHECK(pc < vol); // file order inside one tick
    CHECK(vol < second_on);
    CHECK_FALSE(r.piano.ringing());
}

TEST_CASE("playback: two overlapping notes on one key do not stop each other early") {
    // Two tracks both play C4 on channel 1: 0-2 s and 1-3 s.
    Rig r(make_song({AbsTrack("A").note(0.0, 2.0, 0, 60), AbsTrack("B").note(1.0, 3.0, 0, 60)}));
    r.play(0.0);
    r.update(2.5);
    CHECK(r.piano.key_down(0, 60)); // the second note is still sounding after the first one ended
    r.update(3.5);
    CHECK_FALSE(r.piano.ringing());
}

TEST_CASE("playback: program changes and controllers from the file are sent in time") {
    Rig r(make_song({AbsTrack("Right")
                         .program(0.0, 0, 5)
                         .cc(0.0, 0, 0, 1)
                         .note(0.0, 4.0, 0, 60)
                         .cc(1.0, 0, 64, 127)
                         .bend(2.0, 0, 9000)
                         .program(3.0, 0, 73)}));
    r.play(0.0);
    // The state at the start is sent when play starts, before any Move.
    CHECK(r.piano.program(0) == 5);
    CHECK(r.piano.controller(0, 0) == 1);
    r.update(0.5);
    CHECK(r.piano.controller(0, 64) == 0);
    r.update(1.5);
    CHECK(r.piano.controller(0, 64) == 127);
    r.update(2.5);
    CHECK(r.piano.bend(0) == 9000);
    CHECK(r.piano.program(0) == 5);
    r.update(3.5);
    CHECK(r.piano.program(0) == 73);
}

// ---- Pause, resume, end ------------------------------------------------------------------------

TEST_CASE("playback: pause stops every sounding note, lifts the pedal and sends all-notes-off") {
    Rig r(make_song(
        {AbsTrack("Right").cc(0.0, 0, 64, 127).note(0.0, 4.0, 0, 60).note(0.5, 4.0, 0, 64),
         AbsTrack("Left").note(0.0, 4.0, 1, 36)}));
    r.play(0.0);
    r.update(1.0);
    REQUIRE(r.piano.key_down(0, 60));
    REQUIRE(r.piano.key_down(1, 36));
    r.pause(1.25);
    CHECK_FALSE(r.piano.ringing());
    CHECK(r.piano.controller(0, 64) == 0);
    std::vector<std::string> d = describe(r.last);
    CHECK(of_kind(r.last, MsgKind::NoteOff).size() == 3);
    CHECK(std::count(d.begin(), d.end(), "cc 123=0@0") == 1);
    CHECK(std::count(d.begin(), d.end(), "cc 123=0@1") == 1);
    CHECK(std::count(d.begin(), d.end(), "cc 64=0@0") == 1);
    for (const PlaybackMessage& m : r.last) {
        CHECK(m.at_ns == sec(1.25));
    }
    // The note-offs come before the all-notes-off.
    CHECK(d.front().rfind("off", 0) == 0);
    CHECK(d.back() == "cc 123=0@1");
}

TEST_CASE("playback: a second pause with nothing sounding sends nothing") {
    Rig r(make_song({AbsTrack("Right").note(0.0, 1.0, 0, 60)}));
    r.play(0.0);
    r.update(0.5);
    r.pause(0.5);
    r.play(1.0);
    r.pause(1.0);
    CHECK(r.last.empty());
}

TEST_CASE("playback: resuming restores the pedal and carries on without restarting the note") {
    Rig r(make_song(
        {AbsTrack("Right").cc(0.0, 0, 64, 127).note(0.0, 4.0, 0, 60).note(2.0, 3.0, 0, 64)}));
    r.play(0.0);
    r.update(1.0);
    r.pause(1.0);
    REQUIRE_FALSE(r.piano.ringing());
    r.play(5.0);
    CHECK(r.piano.controller(0, 64) == 127); // pedal state of the file at this position
    CHECK_FALSE(r.piano.key_down(0, 60));    // the note in progress is not restarted
    r.update(6.5);
    CHECK(r.piano.key_down(0, 64)); // the next note plays on time
    r.update(9.0);
    CHECK_FALSE(r.piano.ringing());
}

TEST_CASE("playback: the end of the song leaves nothing ringing, pedal included") {
    Rig r(make_song({AbsTrack("Right").cc(0.0, 0, 64, 127).note(0.0, 2.0, 0, 60)}));
    r.play(0.0);
    r.update(10.0);
    CHECK(r.transport.ended());
    CHECK_FALSE(r.piano.ringing());
    CHECK(r.piano.controller(0, 64) == 0);
}

// ---- Seek --------------------------------------------------------------------------------------

TEST_CASE("playback: seeking silences the piano and restores the state at the new position") {
    Rig r(make_song({AbsTrack("Right")
                         .program(0.0, 0, 40)
                         .cc(0.0, 0, 7, 90)
                         .note(0.0, 10.0, 0, 60)
                         .program(5.0, 0, 73)
                         .cc(5.0, 0, 7, 40)
                         .cc(6.0, 0, 64, 127)
                         .bend(6.5, 0, 12000)
                         .note(8.0, 9.0, 0, 67)}));
    r.play(0.0);
    r.update(7.0); // the piano now has program 73, volume 40, pedal down, bend 12000
    REQUIRE(r.piano.program(0) == 73);
    REQUIRE(r.piano.key_down(0, 60));

    r.seek(7.0, 2.0);
    CHECK_FALSE(r.piano.ringing()); // note 60 began before 2.0: not started again
    CHECK(r.piano.program(0) == 40);
    CHECK(r.piano.controller(0, 7) == 90);
    CHECK(r.piano.controller(0, 64) == 0);
    CHECK(r.piano.bend(0) == 8192); // the file's bend comes later: back to centre

    r.update(8.0); // 3.0 s in
    r.update(10.5);
    r.pause(10.5);
    CHECK_FALSE(r.piano.ringing());

    r.seek(11.0, 7.0);
    r.play(11.0);
    CHECK(r.piano.program(0) == 73);
    CHECK(r.piano.controller(0, 7) == 40);
    CHECK(r.piano.controller(0, 64) == 127);
    CHECK(r.piano.bend(0) == 12000);
}

TEST_CASE("playback: a seek while playing starts the notes at the target, not the ones before it") {
    Rig r(make_song(
        {AbsTrack("Right").note(0.0, 1.0, 0, 60).note(5.0, 6.0, 0, 62).note(5.0, 6.0, 0, 65)}));
    r.play(0.0);
    r.update(0.5);
    r.seek(0.5, 5.0); // exactly on two notes
    r.piano.log.clear();
    r.update(1.0);
    CHECK(describe(r.piano.log) == Strings{"on 62@0", "on 65@0"});
}

TEST_CASE("playback: the state is restored in order: bank select, program, controllers, bend") {
    Rig r(make_song({AbsTrack("Right")
                         .bend(0.5, 0, 9999)
                         .cc(0.5, 0, 7, 77)
                         .program(0.5, 0, 12)
                         .cc(0.5, 0, 32, 3)
                         .cc(0.5, 0, 0, 1)
                         .note(5.0, 6.0, 0, 60)}));
    r.play(0.0);
    r.update(0.1);
    r.seek(0.1, 3.0);
    // The piano has what the file sets; nothing is wasted on what it already has.
    r.pause(0.1);
    r.seek(0.2, 0.0);
    r.play(0.2);
    r.seek(0.3, 3.0);
    std::vector<std::string> d = describe(r.last);
    auto at = [&](const std::string& s) { return std::find(d.begin(), d.end(), s) - d.begin(); };
    REQUIRE(at("cc 0=1@0") < static_cast<std::ptrdiff_t>(d.size()));
    CHECK(at("cc 0=1@0") < at("pc 12@0"));
    CHECK(at("cc 32=3@0") < at("pc 12@0"));
    CHECK(at("pc 12@0") < at("cc 7=77@0"));
    CHECK(at("cc 7=77@0") < at("bend 9999@0"));
}

TEST_CASE("playback: only what the piano doesn't already have is sent") {
    Rig r(
        make_song({AbsTrack("Right").program(0.5, 0, 12).cc(0.5, 0, 7, 77).note(5.0, 6.0, 0, 60)}));
    r.play(0.0);
    r.seek(0.0, 3.0);
    CHECK_FALSE(r.last.empty());
    r.seek(0.1, 4.0);
    CHECK(r.last.empty()); // same state again: no program change, no volume
    r.seek(0.2, 0.0);      // back before the changes: defaults for what was set
    CHECK(describe(r.last) == Strings{"pc 0@0", "cc 7=100@0"});
}

TEST_CASE("playback: program changes at time 0 and mid-song") {
    Rig r(make_song({AbsTrack("Right")
                         .program(0.0, 2, 40)
                         .note(0.0, 1.0, 2, 60)
                         .program(3.0, 2, 73)
                         .note(3.0, 4.0, 2, 62)}));
    r.play(0.0);
    CHECK(r.piano.program(2) == 40); // set up before the first note
    CHECK(r.piano.log.size() == 1);
    r.update(0.001);
    CHECK(r.piano.key_down(2, 60));
    r.seek(0.001, 3.5);
    CHECK(r.piano.program(2) == 73);
    r.seek(0.002, 0.0);
    CHECK(r.piano.program(2) == 40);
    r.seek(0.003, 3.0); // the change sits exactly at the target
    CHECK(r.piano.program(2) == 73);
    r.update(0.5);
    CHECK(r.piano.key_down(2, 62));
    CHECK(of_kind(r.last, MsgKind::ProgramChange).empty()); // not sent twice
}

TEST_CASE("playback: a seek while paused sends nothing until play") {
    Rig r(make_song(
        {AbsTrack("Right").program(0.0, 0, 40).program(2.0, 0, 73).note(3.0, 4.0, 0, 60)}));
    r.seek(0.0, 2.5);
    CHECK(r.last.empty());
    CHECK(r.piano.log.empty());
    r.play(1.0);
    CHECK(r.piano.program(0) == 73);
}

// ---- Loop --------------------------------------------------------------------------------------

TEST_CASE("playback: a loop wrap stops what B left sounding and restores the state at A") {
    Rig r(make_song({AbsTrack("Right")
                         .cc(0.0, 0, 64, 127)
                         .cc(2.5, 0, 64, 0)
                         .program(3.0, 0, 73)
                         .note(0.0, 10.0, 0, 48) // spans the whole loop
                         .note(2.0, 2.5, 0, 60)
                         .note(3.5, 4.5, 0, 62) // crosses B
                         .note(4.0, 5.0, 0, 64)}));
    REQUIRE(r.transport.set_loop(0, 2.0, 4.0));
    r.transport.seek(0, 2.0);
    r.play(0.0);
    r.update(1.75); // song 3.75: the note at 3.5 sounds, the program is 73, the pedal is up
    REQUIRE(r.piano.key_down(0, 62));
    REQUIRE(r.piano.program(0) == 73);

    r.update(2.25); // wraps at song 4.0 (wall 2.0), then 0.25 s into the second pass
    CHECK(r.transport.loop_count() == 1);
    CHECK_FALSE(r.piano.key_down(0, 62));
    CHECK_FALSE(r.piano.key_down(0, 48));    // started before A: not retriggered
    CHECK(r.piano.program(0) == 0);          // back to the program the file has at A
    CHECK(r.piano.controller(0, 64) == 127); // the pedal state at A
    CHECK(r.piano.key_down(0, 60));          // starts exactly at A, so it plays on every pass
    r.update(5.0);
    r.pause(5.0);
    CHECK_FALSE(r.piano.ringing());
}

TEST_CASE("playback: every wrap of a tight loop leaves the piano clean, at any update rate") {
    for (double step : {0.016, 0.3, 1.1}) {
        Rig r(make_song(
            {AbsTrack("Right").cc(0.0, 0, 64, 127).note(0.0, 20.0, 0, 48).note(1.0, 2.2, 0, 60)}));
        REQUIRE(r.transport.set_loop(0, 1.0, 2.0));
        r.transport.seek(0, 1.0);
        r.play(0.0);
        int wraps_seen = 0;
        for (double t = step; t < 12.0; t += step) {
            r.update(t);
            wraps_seen = r.transport.loop_count();
            // After any update, the only thing that can ring is the loop's own note.
            for (int k = 0; k < 128; ++k) {
                if (k != 60) {
                    CHECK_FALSE(r.piano.key_down(0, k));
                }
            }
        }
        CAPTURE(step);
        CHECK(wraps_seen >= 5);
        r.pause(12.0);
        CHECK_FALSE(r.piano.ringing());
    }
}

// ---- Hold --------------------------------------------------------------------------------------

TEST_CASE("playback: a hold sends nothing, and notes sounding stay sounding") {
    Rig r(make_song({AbsTrack("Right").note(0.0, 4.0, 0, 60).note(2.0, 3.0, 0, 64)}));
    r.play(0.0);
    r.transport.hold_at(0, 1.0);
    r.update(3.0);
    REQUIRE(r.transport.held());
    CHECK(r.piano.key_down(0, 60));
    std::size_t before = r.piano.log.size();
    r.update(9.0);
    CHECK(r.piano.log.size() == before);
    r.transport.release(sec(9.0));
    r.pump();
    CHECK(r.last.empty());
    r.update(10.5); // song 2.5
    CHECK(r.piano.key_down(0, 64));
    r.pause(10.5);
    CHECK_FALSE(r.piano.ringing());
}

TEST_CASE("playback: a pause during a hold still silences the piano") {
    Rig r(make_song({AbsTrack("Right").cc(0.0, 0, 64, 127).note(0.0, 4.0, 0, 60)}));
    r.play(0.0);
    r.update(1.0);
    r.transport.hold(sec(1.0));
    r.pump();
    REQUIRE(r.piano.key_down(0, 60));
    r.pause(2.0);
    CHECK_FALSE(r.piano.ringing());
}

// ---- Mute --------------------------------------------------------------------------------------

namespace {
Song two_hands() {
    return make_song(
        {AbsTrack("Right Hand").note(0.0, 1.0, 0, 72).note(2.0, 3.0, 0, 74).note(4.0, 5.0, 0, 76),
         AbsTrack("Left Hand").note(0.0, 2.0, 1, 48).note(2.0, 4.0, 1, 50)});
}
} // namespace

TEST_CASE("playback: a muted hand is not sent, and the piano plays the other hand") {
    Rig r(two_hands());
    r.playback.set_hand_muted(Hand::Right, true, 0, r.last);
    r.play(0.0);
    r.update(10.0);
    CHECK(note_messages(r.piano.log) == Strings{"on 48@1", "off 48@1", "on 50@1", "off 50@1"});
    CHECK(r.playback.hand_muted(Hand::Right));
    CHECK_FALSE(r.playback.hand_muted(Hand::Left));

    Rig left_muted(two_hands());
    left_muted.playback.set_hand_muted(Hand::Left, true, 0, left_muted.last);
    left_muted.play(0.0);
    left_muted.update(10.0);
    CHECK(of_kind(left_muted.piano.log, MsgKind::NoteOn).size() == 3);
    for (const PlaybackMessage& m : of_kind(left_muted.piano.log, MsgKind::NoteOn)) {
        CHECK(m.channel == 0);
    }
}

TEST_CASE("playback: muting a hand mid-note stops its sounding notes at once") {
    Rig r(two_hands());
    r.play(0.0);
    r.update(0.5);
    REQUIRE(r.piano.key_down(0, 72));
    REQUIRE(r.piano.key_down(1, 48));
    std::vector<PlaybackMessage> out;
    r.playback.set_hand_muted(Hand::Right, true, sec(0.5), out);
    r.piano.feed(out);
    CHECK(describe(out) == Strings{"off 72@0"});
    CHECK(out[0].at_ns == sec(0.5));
    CHECK_FALSE(r.piano.key_down(0, 72));
    CHECK(r.piano.key_down(1, 48)); // the other hand keeps playing
    r.update(0.9);
    r.update(10.0);
    CHECK_FALSE(r.piano.ringing());
}

TEST_CASE("playback: unmuting in the middle of a note starts nothing and leaves nothing stuck") {
    Rig r(two_hands());
    std::vector<PlaybackMessage> out;
    r.playback.set_hand_muted(Hand::Right, true, 0, out);
    r.play(0.0);
    r.update(0.5);
    r.playback.set_hand_muted(Hand::Right, false, sec(0.5), out);
    CHECK(out.empty());
    r.update(1.5); // the muted note's note-off comes by: nothing to stop
    CHECK_FALSE(r.piano.key_down(0, 72));
    r.update(2.5); // the next right-hand note plays
    CHECK(r.piano.key_down(0, 74));
    r.update(10.0);
    CHECK_FALSE(r.piano.ringing());
}

TEST_CASE("playback: muting is kept across seeks, loops and pauses") {
    Rig r(two_hands());
    std::vector<PlaybackMessage> out;
    r.playback.set_hand_muted(Hand::Left, true, 0, out);
    r.play(0.0);
    r.seek(0.0, 2.0);
    r.update(1.0);
    r.pause(1.0);
    for (const PlaybackMessage& m : r.piano.log) {
        if (m.kind == MsgKind::NoteOn) {
            CHECK(m.channel == 0);
        }
    }
}

TEST_CASE("playback: drums can be muted on their own") {
    Rig r(make_song(
        {AbsTrack("Right").note(0.0, 1.0, 0, 60), AbsTrack("Drums").note(0.0, 0.5, 9, 36)}));
    std::vector<PlaybackMessage> out;
    r.playback.set_drums_muted(true, 0, out);
    r.play(0.0);
    r.update(2.0);
    CHECK(note_messages(r.piano.log) == Strings{"on 60@0", "off 60@0"});
    CHECK(r.playback.drums_muted());
}

// ---- Panic and song change ----------------------------------------------------------------------

TEST_CASE("playback: silence() stops everything for quit or a lost connection") {
    Rig r(make_song({AbsTrack("Right").cc(0.0, 0, 64, 127).note(0.0, 4.0, 0, 60)}));
    r.play(0.0);
    r.update(1.0);
    std::vector<PlaybackMessage> out;
    r.playback.silence(sec(1.0), out);
    r.piano.feed(out);
    CHECK_FALSE(r.piano.ringing());
    CHECK(r.playback.sounding_count() == 0);
    CHECK(r.piano.controller(0, 64) == 0);
}

TEST_CASE("playback: loading another song stops the old one first") {
    Rig r(make_song({AbsTrack("Right").note(0.0, 4.0, 0, 60)}));
    r.play(0.0);
    r.update(1.0);
    std::vector<PlaybackMessage> out;
    r.playback.set_song(make_song({AbsTrack("Right").note(0.0, 1.0, 0, 70)}), sec(1.0), out);
    r.piano.feed(out);
    CHECK_FALSE(r.piano.ringing());
    CHECK_FALSE(r.playback.running());
}

// ---- Metronome ---------------------------------------------------------------------------------

TEST_CASE("playback: count-in and metronome clicks become percussion notes when asked") {
    Rig r(make_song({AbsTrack("Right").note(0.0, 8.0, 0, 60)}));
    ClickSound sound;
    sound.enabled = true;
    r.playback.set_click_sound(sound);
    r.transport.set_count_in_beats(4);
    r.play(0.0);
    r.update(2.5);
    std::vector<PlaybackMessage> ons = of_kind(r.piano.log, MsgKind::NoteOn);
    std::vector<PlaybackMessage> clicks;
    std::copy_if(ons.begin(), ons.end(), std::back_inserter(clicks),
                 [](const PlaybackMessage& m) { return m.channel == 9; });
    REQUIRE(clicks.size() == 4);
    CHECK(clicks[0].data == sound.accent_note);
    CHECK(clicks[0].value == sound.accent_velocity);
    CHECK(clicks[1].data == sound.note);
    CHECK(clicks[1].at_ns == sec(0.5));
    // Each click note ends by itself shortly after.
    std::vector<PlaybackMessage> offs = of_kind(r.piano.log, MsgKind::NoteOff);
    CHECK(std::count_if(offs.begin(), offs.end(),
                        [](const PlaybackMessage& m) { return m.channel == 9; }) == 4);
    CHECK_FALSE(r.piano.key_down(9, sound.accent_note));
    CHECK_FALSE(r.piano.key_down(9, sound.note));
}

TEST_CASE("playback: clicks are silent unless a click sound is enabled") {
    Rig r(make_song({AbsTrack("Right").note(0.0, 8.0, 0, 60)}));
    r.transport.set_count_in_beats(4);
    r.play(0.0);
    r.update(3.0);
    for (const PlaybackMessage& m : r.piano.log) {
        CHECK(m.channel == 0);
    }
}

// ---- Reset by CC121
// ------------------------------------------------------------------------------

TEST_CASE("playback: a reset-all-controllers in the file is remembered when restoring state") {
    Rig r(make_song({AbsTrack("Right")
                         .cc(0.5, 0, 7, 50)
                         .cc(1.0, 0, 121, 0) // the piano puts the volume back to 100 itself
                         .note(0.0, 5.0, 0, 60)}));
    r.play(0.0);
    r.update(1.5);
    REQUIRE(r.piano.controller(0, 7) == 100);
    // The file sets 50 at 0.5 s. Playback must know the piano is at 100 now, not at 50.
    r.seek(1.5, 0.7);
    CHECK(r.piano.controller(0, 7) == 50);
}

// ---- Stuck-note safety across every transition, with random commands ----------------------------

namespace {

struct FileState {
    int program = 0;
    int bend = 8192;
    std::array<int, 128> cc{};
    FileState() {
        cc.fill(0);
        cc[7] = 100;
        cc[10] = 64;
        cc[11] = 127;
        cc[91] = 40;
    }
};

// What the file says each channel's state is at `position`: events at or before it.
std::array<FileState, 16> file_state_at(const Song& song, double position) {
    std::array<FileState, 16> st{};
    for (const ChannelEvent& e : song.events) {
        if (e.time_s > position) {
            break;
        }
        FileState& s = st[e.channel];
        switch (e.kind) {
        case ChannelEventKind::Program:
            s.program = e.value;
            break;
        case ChannelEventKind::PitchBend:
            s.bend = e.value;
            break;
        case ChannelEventKind::Control:
            s.cc[e.number] = e.value;
            break;
        }
    }
    return st;
}

Song random_song(std::mt19937& rng) {
    std::uniform_real_distribution<double> start(0.0, 20.0);
    std::uniform_real_distribution<double> len(0.05, 6.0);
    std::uniform_int_distribution<int> pitch(36, 84);
    std::uniform_int_distribution<int> chan(0, 3);
    std::uniform_int_distribution<int> value(0, 127);
    std::uniform_int_distribution<int> coin(0, 3);
    AbsTrack right("Right Hand");
    AbsTrack left("Left Hand");
    for (int i = 0; i < 60; ++i) {
        double s = start(rng);
        int ch = chan(rng);
        (coin(rng) < 2 ? right : left).note(s, s + len(rng), ch == 3 ? 9 : ch, pitch(rng));
    }
    for (int i = 0; i < 40; ++i) {
        double s = start(rng);
        int ch = chan(rng);
        switch (coin(rng)) {
        case 0:
            right.program(s, ch, value(rng));
            break;
        case 1:
            right.cc(s, ch, 64, value(rng));
            break;
        case 2:
            left.cc(s, ch,
                    std::array<int, 5>{0, 7, 11, 32, 91}[static_cast<std::size_t>(value(rng) % 5)],
                    value(rng));
            break;
        default:
            right.bend(s, ch, value(rng) * 128);
            break;
        }
    }
    return make_song({right, left});
}

} // namespace

TEST_CASE("playback: no stuck notes and the right state after any sequence of transport commands") {
    std::mt19937 rng(2024);
    for (int round = 0; round < 25; ++round) {
        Song song = random_song(rng);
        Rig r(song);
        std::uniform_real_distribution<double> dt(0.001, 1.5);
        std::uniform_real_distribution<double> where(0.0, song.duration_s);
        std::uniform_int_distribution<int> action(0, 11);
        std::uniform_int_distribution<int> speed(25, 150);
        double now = 0.0;
        for (int step = 0; step < 120; ++step) {
            now += dt(rng);
            std::vector<TransportOp> ops;
            switch (action(rng)) {
            case 0:
                r.transport.play(sec(now));
                break;
            case 1:
                r.transport.pause(sec(now));
                break;
            case 2:
                r.transport.seek(sec(now), where(rng));
                break;
            case 3:
                r.transport.set_speed_percent(sec(now), speed(rng));
                break;
            case 4: {
                double a = where(rng);
                r.transport.set_loop(sec(now), a, a + 0.2 + dt(rng) * 2);
                break;
            }
            case 5:
                r.transport.clear_loop(sec(now));
                break;
            case 6:
                r.transport.hold(sec(now));
                break;
            case 7:
                r.transport.release(sec(now));
                break;
            case 8: {
                std::vector<PlaybackMessage> out;
                Hand h = static_cast<Hand>(action(rng) % 3);
                r.playback.set_hand_muted(h, action(rng) % 2 == 0, sec(now), out);
                r.piano.feed(out);
                break;
            }
            default:
                r.transport.update(sec(now));
                break;
            }
            // Walk the ops one at a time to check the piano between them.
            r.transport.take_ops(ops);
            for (const TransportOp& op : ops) {
                std::vector<PlaybackMessage> out;
                r.playback.apply(op, out);
                r.piano.feed(out);
                for (const PlaybackMessage& m : out) {
                    CHECK(m.data < 128);
                    CHECK(m.value < 16384);
                }
                switch (op.kind) {
                case OpKind::Pause:
                case OpKind::Seek:
                case OpKind::Wrap:
                case OpKind::End: {
                    CAPTURE(round);
                    CAPTURE(step);
                    CHECK_FALSE(r.piano.ringing());
                    break;
                }
                default:
                    break;
                }
                if (op.kind == OpKind::Play || op.kind == OpKind::Wrap ||
                    (op.kind == OpKind::Seek && r.playback.running())) {
                    auto expected = file_state_at(song, op.to_s);
                    for (int ch = 0; ch < 16; ++ch) {
                        CAPTURE(ch);
                        CAPTURE(op.to_s);
                        CHECK(r.piano.program(ch) ==
                              expected[static_cast<std::size_t>(ch)].program);
                        CHECK(r.piano.bend(ch) == expected[static_cast<std::size_t>(ch)].bend);
                        for (int n : {0, 7, 10, 11, 32, 64, 91}) {
                            CAPTURE(n);
                            CHECK(r.piano.controller(ch, n) ==
                                  expected[static_cast<std::size_t>(ch)]
                                      .cc[static_cast<std::size_t>(n)]);
                        }
                    }
                }
            }
        }
        r.pause(now + 1.0);
        CHECK_FALSE(r.piano.ringing());
        std::vector<PlaybackMessage> out;
        r.playback.silence(sec(now + 2.0), out);
        r.piano.feed(out);
        CHECK_FALSE(r.piano.ringing());
    }
}

// ---- Transposition ------------------------------------------------------------------------------

namespace {

// Sets the transposition the way the app will, and lets the piano hear what came out.
void transpose(Rig& r, const Transposition& t, double now_s) {
    std::vector<PlaybackMessage> out;
    r.playback.set_transposition(t, sec(now_s), out);
    r.last = out;
    r.piano.feed(out);
}

} // namespace

TEST_CASE("playback transposition: the piano gets the shifted pitch, speed never changes it") {
    Rig r(make_song({AbsTrack("Right").note(0.0, 1.0, 0, 60).note(1.0, 2.0, 0, 64)}));
    transpose(r, Transposition(5, kRange61), 0.0);
    r.play(0.0);
    r.update(3.0);
    CHECK(note_messages(r.piano.log) == Strings{"on 65@0", "off 65@0", "on 69@0", "off 69@0"});
    CHECK_FALSE(r.piano.ringing());
}

TEST_CASE("playback transposition: a note folded onto the keyboard plays folded") {
    // 94 + 5 = 99 is above a 61-key keyboard (top key 96); the display folds it to 87.
    Rig r(make_song({AbsTrack("Right").note(0.0, 1.0, 0, 94)}));
    transpose(r, Transposition(5, kRange61), 0.0);
    r.play(0.0);
    r.update(2.0);
    CHECK(note_messages(r.piano.log) == Strings{"on 87@0", "off 87@0"});
}

TEST_CASE("playback transposition: a note no key can play is not sent, and nothing sticks") {
    Rig r(make_song({AbsTrack("Right").note(0.0, 1.0, 0, 66).note(1.0, 2.0, 0, 62)}));
    transpose(r, Transposition(0, KeyRange{60, 64}), 0.0); // no F# between C4 and E4
    r.play(0.0);
    r.update(3.0);
    CHECK(note_messages(r.piano.log) == Strings{"on 62@0", "off 62@0"});
    CHECK_FALSE(r.piano.ringing());
}

TEST_CASE("playback transposition: drum notes and metronome clicks keep their numbers") {
    Rig r(make_song({AbsTrack("Drums").note(0.0, 0.5, 9, 38)}));
    ClickSound click;
    click.enabled = true;
    r.playback.set_click_sound(click);
    r.transport.set_metronome(true);
    transpose(r, Transposition(7, kRange61), 0.0);
    r.play(0.0);
    r.update(1.0);
    const Strings d = describe(r.piano.log);
    CHECK(std::count(d.begin(), d.end(), "on 38@9") == 1);
    CHECK(std::count(d.begin(), d.end(), "off 38@9") == 1);
    CHECK(std::count(d.begin(), d.end(), "on 76@9") == 1); // the accented click
}

TEST_CASE("playback transposition: notes folded onto one key do not stop each other early") {
    // On a one-octave keyboard (C4 to B4) the song's C5 folds down onto the C4 the other track
    // plays.
    Rig r(make_song({AbsTrack("A").note(0.0, 2.0, 0, 60), AbsTrack("B").note(1.0, 3.0, 0, 72)}));
    transpose(r, Transposition(0, KeyRange{60, 71}), 0.0);
    r.play(0.0);
    r.update(2.5);
    CHECK(r.piano.key_down(0, 60)); // the second note still sounds after the first one ended
    r.update(3.5);
    CHECK_FALSE(r.piano.ringing());
}

TEST_CASE("playback transposition: changing it mid-note stops the note at its old pitch") {
    Rig r(make_song({AbsTrack("Right").note(0.0, 4.0, 0, 60).note(2.0, 3.0, 0, 62)}));
    transpose(r, Transposition(2, kRange61), 0.0);
    r.play(0.0);
    r.update(1.0);
    REQUIRE(r.piano.key_down(0, 62)); // 60 + 2
    transpose(r, Transposition(-3, kRange61), 1.0);
    // The panic: the old key goes off at once, with all-notes-off after it, none left ringing.
    CHECK(describe(r.last) == Strings{"off 62@0", "cc 123=0@0"});
    for (const PlaybackMessage& m : r.last) {
        CHECK(m.at_ns == sec(1.0));
    }
    CHECK_FALSE(r.piano.ringing());
    CHECK(r.playback.sounding_count() == 0);
    // The note in progress is not restarted; the next one comes at the new pitch.
    r.update(2.5);
    CHECK(r.piano.key_down(0, 59)); // 62 - 3
    r.update(5.0);
    CHECK_FALSE(r.piano.ringing());
    const Strings d = note_messages(r.piano.log);
    CHECK(std::count(d.begin(), d.end(), "on 59@0") == 1);
    CHECK(std::count(d.begin(), d.end(), "off 59@0") == 1);
    CHECK(std::count(d.begin(), d.end(), "off 62@0") == 1); // the stuck-note check: one off per on
}

TEST_CASE("playback transposition: the pedal is down again after the change, as the file has it") {
    Rig r(make_song({AbsTrack("Right").cc(0.0, 0, 64, 127).note(0.0, 4.0, 0, 60)}));
    r.play(0.0);
    r.update(1.0);
    REQUIRE(r.piano.controller(0, 64) == 127);
    transpose(r, Transposition(1, kRange61), 1.0);
    CHECK(r.piano.controller(0, 64) == 127);
    CHECK_FALSE(r.piano.key_down(0, 60));
    CHECK_FALSE(r.piano.key_down(0, 61));
}

TEST_CASE("playback transposition: the same transposition again sends nothing") {
    Rig r(make_song({AbsTrack("Right").note(0.0, 4.0, 0, 60)}));
    transpose(r, Transposition(2, kRange61), 0.0);
    r.play(0.0);
    r.update(1.0);
    transpose(r, Transposition(2, kRange61), 1.0);
    CHECK(r.last.empty());
    CHECK(r.piano.key_down(0, 62));
    transpose(r, Transposition(14, kRange61), 1.1); // clamps to 12
    CHECK_FALSE(r.last.empty());
    transpose(r, Transposition(12, kRange61), 1.2); // the same after the clamp
    CHECK(r.last.empty());
}

TEST_CASE("playback transposition: a change while paused sends nothing and applies on play") {
    Rig r(make_song({AbsTrack("Right").note(0.0, 1.0, 0, 60)}));
    transpose(r, Transposition(4, kRange61), 0.0);
    CHECK(r.last.empty());
    r.play(0.0);
    r.update(2.0);
    CHECK(note_messages(r.piano.log) == Strings{"on 64@0", "off 64@0"});
}

TEST_CASE("playback transposition: the shown, played and expected pitch are always the same") {
    std::mt19937 rng(77);
    const KeyRange ranges[] = {kRange61,         kRange88,         kRange49,        kRange76,
                               KeyRange{60, 64}, KeyRange{48, 59}, KeyRange{0, 127}};
    for (int round = 0; round < 60; ++round) {
        // Notes with distinct start times 50 ms apart, any pitch, some on the drum channel.
        Song song;
        std::uniform_int_distribution<int> pitch(0, 127);
        std::uniform_int_distribution<int> channel(0, 15);
        const int count = 40;
        for (int i = 0; i < count; ++i) {
            Note n;
            n.start_s = i * 0.05;
            n.duration_s = 0.3;
            n.start_tick = i * 48;
            n.end_tick = n.start_tick + 288;
            n.pitch = static_cast<std::uint8_t>(pitch(rng));
            n.channel = static_cast<std::uint8_t>(channel(rng));
            n.drum = n.channel == 9;
            n.hand = Hand::Right;
            song.notes.push_back(n);
        }
        song.duration_s = count * 0.05 + 0.5;
        song.end_tick = song.notes.back().end_tick;

        std::uniform_int_distribution<int> shift(-14, 14); // past 12 to include the clamp
        const Transposition t(shift(rng), ranges[round % 7]);

        Rig r(song);
        transpose(r, t, 0.0);
        r.play(0.0);
        r.update(song.duration_s + 1.0);

        NoteWindow window(song);
        NoteWindowOptions options;
        options.ahead_s = 1000.0;
        options.include_drums = true;
        options.transposition = t;
        std::vector<VisibleNote> shown;
        window.query(0.0, options, shown);
        REQUIRE(shown.size() == song.notes.size());

        // The pitch each song note was played at, found by its start time.
        std::vector<int> played(song.notes.size(), -1);
        for (const PlaybackMessage& m : r.piano.log) {
            if (m.kind == MsgKind::NoteOn) {
                const auto i = static_cast<std::size_t>(std::llround(m.song_s / 0.05));
                REQUIRE(i < played.size());
                played[i] = m.data;
            }
        }
        for (const VisibleNote& v : shown) {
            const Note& n = song.notes[v.index];
            const TransposedPitch expected = t.expected_pitch(n);
            CAPTURE(round);
            CAPTURE(v.index);
            CAPTURE(int{n.pitch});
            CAPTURE(t.semitones());
            CHECK(v.pitch == expected.pitch);
            CHECK(v.folded == expected.folded);
            CHECK(v.fold_octaves == expected.fold_octaves);
            CHECK(v.playable == expected.playable);
            CHECK(played[v.index] == expected.pitch); // -1 (not sent) exactly when unplayable
            // And the answer is musically right, whatever the three paths agree on.
            if (n.drum) {
                CHECK(expected.pitch == n.pitch);
            } else if (expected.playable) {
                CHECK(t.keyboard().contains(expected.pitch));
                CHECK((expected.pitch - (n.pitch + t.semitones())) % 12 == 0);
            }
        }
        CHECK_FALSE(r.piano.ringing());
    }
}

// ---- Playback edge cases: pedal on pause, silence before teardown, restoring state at a position,
// and a transposition shared by display, playback and both evaluators -------------------------

namespace {

Song hand_song(const std::vector<std::pair<double, double>>& starts_and_lengths, int pitch = 60) {
    Song song;
    for (const auto& [start, length] : starts_and_lengths) {
        Note n;
        n.start_s = start;
        n.duration_s = length;
        n.pitch = static_cast<std::uint8_t>(pitch);
        n.hand = Hand::Right;
        song.notes.push_back(n);
    }
    song.duration_s = 5.0;
    return song;
}

} // namespace

TEST_CASE(
    "playback: a zero- or negative-length note still stops, it never sounds until pause (F6)") {
    for (double length : {0.0, -0.5, std::numeric_limits<double>::quiet_NaN(),
                          std::numeric_limits<double>::infinity()}) {
        CAPTURE(length);
        Rig r(hand_song({{1.0, length}}));
        r.play(0.0);
        r.update(2.0);
        CHECK(of_kind(r.piano.log, MsgKind::NoteOn).size() == 1);
        const std::vector<PlaybackMessage> offs = of_kind(r.piano.log, MsgKind::NoteOff);
        REQUIRE(offs.size() == 1);
        CHECK(offs[0].at_ns <= sec(1.1));
        CHECK(r.playback.sounding_count() == 0);
        CHECK_FALSE(r.piano.ringing());
    }
    // A note with no start time can't be scheduled: it is not sent at all.
    Rig r(hand_song({{std::numeric_limits<double>::quiet_NaN(), 0.5}, {1.0, 0.5}}));
    r.play(0.0);
    r.update(3.0);
    CHECK(of_kind(r.piano.log, MsgKind::NoteOn).size() == 1);
    CHECK_FALSE(r.piano.ringing());
}

TEST_CASE("playback: pause lifts the pedal on a channel that never played a note (F7)") {
    Rig r(make_song(
        {AbsTrack("Right").note(0.0, 1.5, 0, 60), AbsTrack("Pedal").cc(0.0, 1, 64, 127)}));
    r.play(0.0);
    r.update(1.0);
    REQUIRE(r.piano.controller(1, 64) == 127);
    r.pause(1.25);
    CHECK(r.piano.controller(1, 64) == 0);
    CHECK_FALSE(r.piano.ringing());
    // A song change and silence() lift it too.
    r.play(2.0);
    REQUIRE(r.piano.controller(1, 64) == 127);
    std::vector<PlaybackMessage> out;
    r.playback.silence(sec(2.5), out);
    r.piano.feed(out);
    CHECK(r.piano.controller(1, 64) == 0);
}

namespace {
int g_teardown_calls = 0;
} // namespace

TEST_CASE("playback: needs_silence says when silence() must come before teardown (F8)") {
    Song song = make_song({AbsTrack("Right").cc(0.0, 0, 64, 127).note(0.5, 4.0, 0, 60)});
    g_teardown_calls = 0;
    {
        Rig r(song);
        r.playback.set_teardown_check([](const Playback&) { ++g_teardown_calls; });
        CHECK_FALSE(r.playback.needs_silence());
        r.play(0.0);
        CHECK(r.playback.needs_silence()); // the pedal is down
        r.update(1.0);
        CHECK(r.playback.needs_silence());
        std::vector<PlaybackMessage> out;
        r.playback.silence(sec(1.0), out);
        r.piano.feed(out);
        CHECK_FALSE(r.playback.needs_silence());
    }
    CHECK(g_teardown_calls == 0);
    {
        Rig r(song);
        r.playback.set_teardown_check([](const Playback&) { ++g_teardown_calls; });
        r.play(0.0);
        r.update(1.0); // dropped while the note sounds
    }
    CHECK(g_teardown_calls == 1);
}

TEST_CASE("playback: restoring state at a position uses checkpoints and matches the file (F14)") {
    std::mt19937 rng(14);
    Song song;
    song.duration_s = 2000.0;
    // Few channels and controllers, so a seek sends a handful of messages and the time measured is
    // finding the state, not sending it.
    std::uniform_int_distribution<int> channel(0, 2);
    const int controllers[] = {1, 7, 10, 11, 91};
    std::uniform_int_distribution<int> controller_index(0, 4);
    std::uniform_int_distribution<int> value(0, 127);
    std::uniform_int_distribution<int> kind(0, 9);
    const int count = 200000;
    for (int i = 0; i < count; ++i) {
        ChannelEvent e;
        e.time_s = i * 0.01;
        e.channel = static_cast<std::uint8_t>(channel(rng));
        const int k = kind(rng);
        e.kind = k == 0   ? ChannelEventKind::Program
                 : k == 1 ? ChannelEventKind::PitchBend
                          : ChannelEventKind::Control;
        e.number = e.kind == ChannelEventKind::Control
                       ? static_cast<std::uint8_t>(controllers[controller_index(rng)])
                       : 0;
        e.value = static_cast<std::uint16_t>(
            e.kind == ChannelEventKind::PitchBend ? value(rng) * 128 : value(rng));
        song.events.push_back(e);
    }
    Rig r(song);
    r.play(0.0);
    std::uniform_real_distribution<double> where(0.0, 1999.0);
    double now = 0.0;
    std::vector<double> targets;
    // The best of three rounds of 300 seeks, so a busy machine doesn't fail the test.
    double took_ms = 1e9;
    for (int round = 0; round < 3; ++round) {
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 300; ++i) {
            now += 0.001;
            targets.push_back(where(rng));
            r.seek(now, targets.back());
        }
        took_ms = std::min(took_ms, std::chrono::duration<double, std::milli>(
                                        std::chrono::steady_clock::now() - t0)
                                        .count());
    }
    CAPTURE(took_ms);
    CHECK(took_ms < 75.0); // about 6 ms (45 under ASan); a scan from the song start takes 150

    // The piano holds, at the last target, what the file set at or before it, or the default.
    FakePiano fresh;
    std::array<std::array<int, 120>, 16> cc{};
    std::array<int, 16> program{};
    std::array<int, 16> bend{};
    for (std::size_t ch = 0; ch < 16; ++ch) {
        for (int n = 0; n < 120; ++n) {
            cc[ch][static_cast<std::size_t>(n)] = fresh.controller(static_cast<int>(ch), n);
        }
        program[ch] = fresh.program(static_cast<int>(ch));
        bend[ch] = fresh.bend(static_cast<int>(ch));
    }
    for (const ChannelEvent& e : song.events) {
        if (e.time_s > targets.back()) {
            break;
        }
        switch (e.kind) {
        case ChannelEventKind::Program:
            program[e.channel] = e.value;
            break;
        case ChannelEventKind::PitchBend:
            bend[e.channel] = e.value;
            break;
        case ChannelEventKind::Control:
            cc[e.channel][e.number] = e.value;
            break;
        }
    }
    for (int ch = 0; ch < 16; ++ch) {
        CAPTURE(ch);
        CHECK(r.piano.program(ch) == program[static_cast<std::size_t>(ch)]);
        CHECK(r.piano.bend(ch) == bend[static_cast<std::size_t>(ch)]);
        for (int n = 0; n < 120; ++n) {
            CAPTURE(n);
            CHECK(r.piano.controller(ch, n) ==
                  cc[static_cast<std::size_t>(ch)][static_cast<std::size_t>(n)]);
        }
    }
}

TEST_CASE("transposition source: display, playback and both evaluators change together (F10)") {
    const Song song = make_song(
        {AbsTrack("Right").note(1.0, 1.5, 0, 60).note(3.0, 3.5, 0, 60).note(5.0, 5.5, 0, 60)});
    TranspositionSource source(Transposition(0, kRange61));
    Rig r(song);
    r.playback.follow(&source);
    FlowEvaluator flow(song);
    flow.follow(&source);
    WaitEvaluator wait(song);
    wait.follow(&source);
    NoteWindow window(song);

    const auto pump = [&](double now_s) {
        r.transport.update(sec(now_s));
        std::vector<TransportOp> ops;
        r.transport.take_ops(ops);
        r.last.clear();
        for (const TransportOp& op : ops) {
            r.playback.apply(op, r.last);
            flow.apply(op);
            wait.apply(op);
        }
        r.piano.feed(r.last);
        flow.update(sec(now_s));
        wait.update(sec(now_s));
    };
    const auto press = [](double wall_s, int key) {
        MidiEvent e;
        e.time_ns = sec(wall_s);
        e.type = MidiEventType::NoteOn;
        e.data1 = static_cast<std::uint8_t>(key);
        e.data2 = 90;
        return e;
    };
    const auto shown_at = [&](double song_s) {
        NoteWindowOptions o;
        o.transposition = source.value();
        std::vector<VisibleNote> shown;
        window.query(song_s, o, shown);
        return shown.empty() ? -1 : shown.front().pitch;
    };

    r.transport.play(0);
    pump(0.0);
    flow.apply(press(1.0, 60));
    pump(2.0);
    CHECK(shown_at(2.0) == 60);
    CHECK(wait.waiting_keys() == std::vector<int>{60});

    source.set(Transposition(2, kRange61), sec(2.0));
    CHECK(shown_at(2.0) == 62);
    flow.apply(press(3.0, 62));
    pump(4.0);
    CHECK(wait.waiting_keys() == std::vector<int>{62});
    CHECK(r.playback.transposition() == source.value());
    const std::vector<PlaybackMessage> ons = of_kind(r.piano.log, MsgKind::NoteOn);
    REQUIRE(ons.size() == 2);
    CHECK(ons[0].data == 60);
    CHECK(ons[1].data == 62);
    flow.finish();
    const EvaluationSummary& s = flow.summary();
    REQUIRE(s.notes.size() == 2);
    CHECK(s.notes[0].expected_pitch == 60);
    CHECK(s.notes[1].expected_pitch == 62);
    CHECK(s.notes[1].grade == NoteGrade::Perfect);
    CHECK(s.wrong == 0);
    CHECK_FALSE(r.piano.ringing());
}
