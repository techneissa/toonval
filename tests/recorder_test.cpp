// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/recorder.hpp"

#include "toonval/midi_event.hpp"
#include "toonval/midi_file.hpp"
#include "toonval/midi_writer.hpp"

#include <doctest/doctest.h>

#include <cstdint>

using namespace toonval;

namespace {

constexpr std::int64_t kMs = 1'000'000;
constexpr std::int64_t kSec = 1'000'000'000;

MidiEvent on(std::int64_t t, int pitch, int velocity = 90, int channel = 0) {
    MidiEvent e;
    e.time_ns = t;
    e.type = MidiEventType::NoteOn;
    e.channel = static_cast<std::uint8_t>(channel);
    e.data1 = static_cast<std::uint8_t>(pitch);
    e.data2 = static_cast<std::uint8_t>(velocity);
    return e;
}

MidiEvent off(std::int64_t t, int pitch, int channel = 0) {
    MidiEvent e = on(t, pitch, 0, channel);
    e.type = MidiEventType::NoteOff;
    return e;
}

MidiEvent cc(std::int64_t t, int number, int value, int channel = 0) {
    MidiEvent e;
    e.time_ns = t;
    e.type = MidiEventType::ControlChange;
    e.channel = static_cast<std::uint8_t>(channel);
    e.data1 = static_cast<std::uint8_t>(number);
    e.data2 = static_cast<std::uint8_t>(value);
    return e;
}

} // namespace

TEST_CASE("recorder: one note at 120 BPM and 480 PPQ, with the default lead before it") {
    Recorder rec;
    rec.feed(on(10 * kSec, 60, 100));
    rec.feed(off(10 * kSec + 500 * kMs, 60));
    const RecordedSong r = rec.finish(12 * kSec);
    REQUIRE(r.has_notes);
    const Song& s = r.song;
    CHECK(s.tempo.ticks_per_quarter == 480);
    REQUIRE(s.tempo.changes.size() == 1);
    CHECK(s.tempo.changes[0].us_per_quarter == 500'000);
    REQUIRE(s.notes.size() == 1);
    // The song starts 0.25 s before the first note: 240 ticks at 120 BPM.
    CHECK(s.notes[0].start_tick == 240);
    CHECK(s.notes[0].end_tick == 240 + 480);
    CHECK(s.notes[0].start_s == doctest::Approx(0.25));
    CHECK(s.notes[0].duration_s == doctest::Approx(0.5));
    CHECK(s.notes[0].pitch == 60);
    CHECK(s.notes[0].velocity == 100);
    CHECK(s.notes[0].hand == Hand::Unknown);
    CHECK(s.notes[0].measure == 1);
    CHECK(r.notes == 1);
    CHECK_FALSE(r.truncated);
}

TEST_CASE("recorder: the start is trimmed to the first note minus the lead") {
    RecorderOptions opt;
    opt.lead_s = 1.0;
    Recorder rec(opt);
    rec.feed(on(3600 * kSec, 64));
    rec.feed(off(3601 * kSec, 64));
    const RecordedSong r = rec.finish(3602 * kSec);
    REQUIRE(r.has_notes);
    CHECK(r.song.notes[0].start_tick == 960);
    SUBCASE("a lead of zero starts the song on the first note") {
        RecorderOptions none;
        none.lead_s = 0.0;
        Recorder rec0(none);
        rec0.feed(on(5 * kSec, 64));
        rec0.feed(off(6 * kSec, 64));
        const RecordedSong r0 = rec0.finish(7 * kSec);
        CHECK(r0.song.notes[0].start_tick == 0);
    }
}

TEST_CASE("recorder: the tempo option sets the tick scale") {
    RecorderOptions opt;
    opt.bpm = 90.0;
    opt.ticks_per_quarter = 960;
    opt.lead_s = 0.0;
    Recorder rec(opt);
    rec.feed(on(0, 60));
    rec.feed(off(2 * kSec, 60));
    const RecordedSong r = rec.finish(3 * kSec);
    REQUIRE(r.has_notes);
    CHECK(r.song.tempo.ticks_per_quarter == 960);
    CHECK(r.song.tempo.changes[0].us_per_quarter == 666'667);
    // 2 s at 90 BPM is 3 beats, 2880 ticks.
    CHECK(r.song.notes[0].end_tick == 2880);
    CHECK(r.song.notes[0].duration_s == doctest::Approx(2.0).epsilon(1e-5));
}

TEST_CASE("recorder: a note-on with velocity zero is a note-off") {
    RecorderOptions opt;
    opt.lead_s = 0.0;
    Recorder rec(opt);
    rec.feed(on(0, 60, 80));
    rec.feed(on(kSec, 60, 0)); // still typed NoteOn, as a raw feed might hand it over
    const RecordedSong r = rec.finish(5 * kSec);
    REQUIRE(r.has_notes);
    REQUIRE(r.song.notes.size() == 1);
    CHECK(r.song.notes[0].end_tick == 960);
}

TEST_CASE("recorder: the sustain pedal is kept as CC64 and never lengthens a note") {
    RecorderOptions opt;
    opt.lead_s = 0.0;
    Recorder rec(opt);
    rec.feed(on(0, 60));
    rec.feed(cc(100 * kMs, kCcSustain, 127));
    rec.feed(off(500 * kMs, 60));
    rec.feed(cc(3 * kSec, kCcSustain, 0));
    const RecordedSong r = rec.finish(4 * kSec);
    REQUIRE(r.has_notes);
    REQUIRE(r.song.notes.size() == 1);
    CHECK(r.song.notes[0].end_tick == 480);
    REQUIRE(r.song.events.size() == 2);
    CHECK(r.song.events[0].kind == ChannelEventKind::Control);
    CHECK(r.song.events[0].number == 64);
    CHECK(r.song.events[0].value == 127);
    CHECK(r.song.events[0].tick == 96);
    CHECK(r.song.events[0].time_s == doctest::Approx(0.1));
    CHECK(r.song.events[1].value == 0);
    CHECK(r.song.events[1].tick == 2880);
    // The song is as long as its last note, not its last pedal event.
    CHECK(r.song.end_tick == 480);
}

TEST_CASE("recorder: the pedal state before the first note is kept once, at the start") {
    RecorderOptions opt;
    opt.lead_s = 0.5;
    Recorder rec(opt);
    for (int i = 0; i < 1000; ++i) {
        rec.feed(cc(i * kMs, kCcSustain, i % 2 == 0 ? 127 : 0));
    }
    rec.feed(cc(1000 * kMs, kCcSustain, 127));
    rec.feed(on(2 * kSec, 60));
    rec.feed(off(3 * kSec, 60));
    const RecordedSong r = rec.finish(4 * kSec);
    REQUIRE(r.has_notes);
    REQUIRE(r.song.events.size() == 1);
    CHECK(r.song.events[0].tick == 0);
    CHECK(r.song.events[0].value == 127);
}

TEST_CASE("recorder: a note still held when recording stops ends at the stop time") {
    RecorderOptions opt;
    opt.lead_s = 0.0;
    Recorder rec(opt);
    rec.feed(on(0, 60));
    rec.feed(on(kSec, 64));
    rec.feed(off(2 * kSec, 60));
    const RecordedSong r = rec.finish(5 * kSec);
    REQUIRE(r.has_notes);
    REQUIRE(r.song.notes.size() == 2);
    const Note& held = r.song.notes[1];
    CHECK(held.pitch == 64);
    CHECK(held.start_tick == 960);
    CHECK(held.end_tick == 4800);
}

TEST_CASE("recorder: a note held at a stop time before its start still has one tick") {
    RecorderOptions opt;
    opt.lead_s = 0.0;
    Recorder rec(opt);
    rec.feed(on(kSec, 60));
    const RecordedSong r = rec.finish(kSec);
    REQUIRE(r.has_notes);
    REQUIRE(r.song.notes.size() == 1);
    CHECK(r.song.notes[0].end_tick == r.song.notes[0].start_tick + 1);
}

TEST_CASE("recorder: a note-off that arrives a few ms late does not end the next note") {
    RecorderOptions opt;
    opt.lead_s = 0.0;
    Recorder rec(opt);
    rec.feed(on(0, 60));
    // Re-struck pitch: the new note-on (t = 1.002 s) reaches us before the old note-off
    // (t = 1.000 s).
    rec.feed(on(kSec + 2 * kMs, 60));
    rec.feed(off(kSec, 60));
    rec.feed(off(2 * kSec, 60));
    const RecordedSong r = rec.finish(3 * kSec);
    REQUIRE(r.has_notes);
    REQUIRE(r.song.notes.size() == 2);
    CHECK(r.song.notes[0].start_tick == 0);
    CHECK(r.song.notes[0].end_tick == 960);
    CHECK(r.song.notes[1].start_tick == 962);
    CHECK(r.song.notes[1].end_tick == 1920);
}

TEST_CASE("recorder: the first note is the earliest one even when it arrives second") {
    Recorder rec; // default lead 0.25 s
    rec.feed(on(10 * kSec + 3 * kMs, 64));
    rec.feed(on(10 * kSec, 60));
    rec.feed(off(11 * kSec, 60));
    rec.feed(off(11 * kSec, 64));
    const RecordedSong r = rec.finish(12 * kSec);
    REQUIRE(r.has_notes);
    REQUIRE(r.song.notes.size() == 2);
    CHECK(r.song.notes[0].pitch == 60);
    CHECK(r.song.notes[0].start_tick == 240);
}

TEST_CASE("recorder: an event later than the reorder window is placed at the latest time handled") {
    RecorderOptions opt;
    opt.lead_s = 0.0;
    Recorder rec(opt);
    rec.feed(on(0, 60));
    rec.feed(on(kSec, 62));
    rec.feed(on(2 * kSec, 64));   // by now everything up to 1 s has been handled
    rec.feed(off(500 * kMs, 60)); // 1.5 s late: placed at 1 s, where the song already is
    rec.feed(off(3 * kSec, 62));
    rec.feed(off(3 * kSec, 64));
    const RecordedSong r = rec.finish(4 * kSec);
    REQUIRE(r.has_notes);
    REQUIRE(r.song.notes.size() == 3);
    CHECK(r.song.notes[0].pitch == 60);
    CHECK(r.song.notes[0].end_tick == 960);
    for (const Note& n : r.song.notes) {
        CHECK(n.end_tick > n.start_tick);
    }
}

TEST_CASE("recorder: a second note-on of a sounding pitch ends the first one there") {
    RecorderOptions opt;
    opt.lead_s = 0.0;
    Recorder rec(opt);
    rec.feed(on(0, 60));
    rec.feed(on(kSec, 60));
    rec.feed(off(2 * kSec, 60));
    const RecordedSong r = rec.finish(3 * kSec);
    REQUIRE(r.song.notes.size() == 2);
    CHECK(r.song.notes[0].end_tick == 960);
    CHECK(r.song.notes[1].start_tick == 960);
    CHECK(r.song.notes[1].end_tick == 1920);
}

TEST_CASE("recorder: a note-off with no note sounding changes nothing") {
    RecorderOptions opt;
    opt.lead_s = 0.0;
    Recorder rec(opt);
    rec.feed(off(0, 60));
    rec.feed(on(kSec, 62));
    rec.feed(off(2 * kSec, 62));
    rec.feed(off(2 * kSec + 10 * kMs, 62));
    const RecordedSong r = rec.finish(3 * kSec);
    REQUIRE(r.song.notes.size() == 1);
    CHECK(r.song.notes[0].pitch == 62);
}

TEST_CASE(
    "recorder: All Notes Off, real or from a queue gap, ends every held note and is not saved") {
    for (int panic : {static_cast<int>(kCcAllNotesOff), static_cast<int>(kCcAllSoundOff)}) {
        RecorderOptions opt;
        opt.lead_s = 0.0;
        Recorder rec(opt);
        rec.feed(on(0, 60));
        rec.feed(on(0, 64, 90, 3));
        MidiEvent stop = cc(kSec, panic, 0);
        stop.synthetic = panic == kCcAllNotesOff;
        rec.feed(stop);
        const RecordedSong r = rec.finish(5 * kSec);
        REQUIRE(r.song.notes.size() == 2);
        for (const Note& n : r.song.notes) {
            CHECK(n.end_tick == 960);
        }
        CHECK(r.song.events.empty());
    }
}

TEST_CASE("recorder: notes on different channels become different parts of one track") {
    RecorderOptions opt;
    opt.lead_s = 0.0;
    Recorder rec(opt);
    rec.feed(on(0, 60, 90, 0));
    rec.feed(on(0, 60, 90, 4));
    rec.feed(off(kSec, 60, 0));
    rec.feed(off(kSec, 60, 4));
    const RecordedSong r = rec.finish(2 * kSec);
    REQUIRE(r.song.notes.size() == 2);
    REQUIRE(r.song.parts.size() == 2);
    CHECK(r.song.parts[0].name == "Free play");
    CHECK(r.song.parts[0].track == 1);
    CHECK(r.song.parts[1].channel == 4);
}

TEST_CASE("recorder: events that are not notes or controllers, and bad values, are ignored") {
    RecorderOptions opt;
    opt.lead_s = 0.0;
    Recorder rec(opt);
    MidiEvent other;
    other.time_ns = 0;
    rec.feed(other);
    rec.feed(on(0, 200));
    rec.feed(on(0, 60, 90, 16));
    rec.feed(cc(0, 200, 5));
    rec.feed(on(0, 60));
    rec.feed(off(kSec, 60));
    const RecordedSong r = rec.finish(2 * kSec);
    REQUIRE(r.song.notes.size() == 1);
    CHECK(r.song.events.empty());
}

TEST_CASE("recorder: nothing played gives no song") {
    Recorder rec;
    rec.feed(cc(0, kCcSustain, 127));
    const RecordedSong r = rec.finish(kSec);
    CHECK_FALSE(r.has_notes);
    CHECK(r.song.notes.empty());
    CHECK(r.notes == 0);
}

TEST_CASE("recorder: it can record again after finish, from a clean start") {
    RecorderOptions opt;
    opt.lead_s = 0.0;
    Recorder rec(opt);
    rec.feed(on(100 * kSec, 60));
    rec.feed(off(101 * kSec, 60));
    CHECK(rec.finish(102 * kSec).has_notes);
    rec.feed(on(500 * kSec, 72));
    rec.feed(off(501 * kSec, 72));
    const RecordedSong r = rec.finish(502 * kSec);
    REQUIRE(r.song.notes.size() == 1);
    CHECK(r.song.notes[0].pitch == 72);
    CHECK(r.song.notes[0].start_tick == 0);
}

TEST_CASE("recorder: a ten-hour session of 200000 notes keeps counts, order and small buffers") {
    RecorderOptions opt;
    opt.lead_s = 0.0;
    Recorder rec(opt);
    constexpr int kNotes = 200'000;
    const std::int64_t spacing = 10 * 3600 * kSec / kNotes; // 180 ms
    std::size_t most_buffered = 0;
    for (int i = 0; i < kNotes; ++i) {
        const std::int64_t t = i * spacing;
        const int pitch = 36 + i % 61;
        rec.feed(on(t, pitch));
        rec.feed(off(t + spacing / 2, pitch));
        if (i % 7 == 0) {
            rec.feed(cc(t + 1 * kMs, kCcSustain, (i / 7) % 2 == 0 ? 127 : 0));
        }
        most_buffered = std::max(most_buffered, rec.buffered());
    }
    CHECK(most_buffered < 200);
    const RecordedSong r = rec.finish(10 * 3600 * kSec);
    REQUIRE(r.has_notes);
    CHECK(r.notes == kNotes);
    REQUIRE(r.song.notes.size() == static_cast<std::size_t>(kNotes));
    CHECK(r.song.notes.back().start_s ==
          doctest::Approx(((kNotes - 1) * spacing) / 1e9).epsilon(1e-6));
    for (std::size_t i = 1; i < r.song.notes.size(); ++i) {
        REQUIRE(r.song.notes[i - 1].start_tick <= r.song.notes[i].start_tick);
    }
    CHECK_FALSE(r.truncated);
}

TEST_CASE(
    "recorder: a note held for three hours is one note and the tick count does not overflow") {
    RecorderOptions opt;
    opt.lead_s = 0.0;
    opt.ticks_per_quarter = 32767;
    Recorder rec(opt);
    rec.feed(on(0, 60));
    rec.feed(off(3 * 3600 * kSec, 60));
    const RecordedSong r = rec.finish(3 * 3600 * kSec);
    REQUIRE(r.song.notes.size() == 1);
    // 10800 s is 21600 beats at 120 BPM.
    CHECK(r.song.notes[0].end_tick == std::int64_t{21600} * 32767);
}

TEST_CASE("recorder: past the note limit it stops adding notes and says so") {
    RecorderOptions opt;
    opt.lead_s = 0.0;
    opt.max_notes = 5;
    Recorder rec(opt);
    for (int i = 0; i < 10; ++i) {
        rec.feed(on(i * kSec, 60 + i));
        rec.feed(off(i * kSec + 100 * kMs, 60 + i));
    }
    const RecordedSong r = rec.finish(11 * kSec);
    CHECK(r.song.notes.size() == 5);
    CHECK(r.notes == 5);
    CHECK(r.truncated);
}

TEST_CASE("recorder: past the event limit it stops adding events and says so") {
    RecorderOptions opt;
    opt.lead_s = 0.0;
    opt.max_events = 3;
    Recorder rec(opt);
    rec.feed(on(0, 60));
    for (int i = 0; i < 10; ++i) {
        rec.feed(cc((i + 1) * 100 * kMs, kCcSustain, i % 2 == 0 ? 127 : 0));
    }
    rec.feed(off(2 * kSec, 60));
    const RecordedSong r = rec.finish(3 * kSec);
    CHECK(r.song.events.size() == 3);
    CHECK(r.truncated);
}

TEST_CASE("recorder: the recording writes to a .mid file and reads back the same") {
    Recorder rec;
    rec.feed(cc(0, kCcSustain, 127));
    rec.feed(on(2 * kSec, 60, 70));
    rec.feed(on(2 * kSec + 250 * kMs, 64, 80));
    rec.feed(off(3 * kSec, 60));
    rec.feed(cc(3 * kSec + 100 * kMs, kCcSustain, 0));
    rec.feed(off(4 * kSec, 64));
    const RecordedSong r = rec.finish(5 * kSec);
    REQUIRE(r.has_notes);
    const MidiWriteResult w = write_midi(r.song);
    REQUIRE(w.ok);
    MidiLoadOptions load_opt;
    load_opt.keep_ghost_notes = true;
    const MidiLoadResult loaded = load_midi(w.bytes, load_opt);
    REQUIRE(loaded.ok);
    REQUIRE(loaded.song.notes.size() == 2);
    for (std::size_t i = 0; i < 2; ++i) {
        CHECK(loaded.song.notes[i].pitch == r.song.notes[i].pitch);
        CHECK(loaded.song.notes[i].velocity == r.song.notes[i].velocity);
        CHECK(loaded.song.notes[i].start_tick == r.song.notes[i].start_tick);
        CHECK(loaded.song.notes[i].end_tick == r.song.notes[i].end_tick);
    }
    REQUIRE(loaded.song.events.size() == 2);
    CHECK(loaded.song.events[0].number == 64);
    CHECK(loaded.song.events[0].value == 127);
    CHECK(loaded.song.events[0].tick == 0);
    CHECK(loaded.song.events[1].value == 0);
}
