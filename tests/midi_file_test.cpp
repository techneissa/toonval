// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/midi_file.hpp"

#include "smf_builder.hpp"

#include <doctest/doctest.h>

#include <fstream>
#include <iterator>
#include <random>
#include <string>

using namespace toonval;
using namespace toonval::test;

namespace {
MidiLoadResult load(const Bytes& bytes, const MidiLoadOptions& options = {}) {
    return load_midi(bytes.data(), bytes.size(), options);
}
} // namespace

TEST_CASE("one note at the default 120 BPM") {
    Bytes file = smf({Track().on(0, 0, 60, 90).off(480, 0, 60).end()});
    MidiLoadResult r = load(file);
    REQUIRE(r.ok);
    REQUIRE(r.song.notes.size() == 1);
    const Note& n = r.song.notes[0];
    CHECK(n.pitch == 60);
    CHECK(n.velocity == 90);
    CHECK(n.start_s == doctest::Approx(0.0));
    CHECK(n.duration_s == doctest::Approx(0.5));
    CHECK(n.start_tick == 0);
    CHECK(n.end_tick == 480);
    CHECK(n.measure == 1);
}

TEST_CASE("tempo changes mid-song shift note times correctly") {
    // 480 ticks per quarter. 120 BPM from tick 0, 60 BPM from 960, 240 BPM from 1920.
    // Hand-computed: tick 960 = 1.0 s, 1440 = 2.0 s, 1920 = 3.0 s, 2400 = 3.25 s.
    Bytes file = smf({Track().bpm(0, 120).bpm(960, 60).bpm(960, 240).end(),
                      Track()
                          .on(0, 0, 60)
                          .off(480, 0, 60) // 0 .. 0.5 s
                          .on(480, 0, 62)  // tick 960
                          .off(480, 0, 62) // 1.0 .. 2.0 s
                          .on(0, 0, 64)    // tick 1440, held across the change at 1920
                          .off(960, 0, 64) // 2.0 .. 3.25 s
                          .end()});
    MidiLoadResult r = load(file);
    REQUIRE(r.ok);
    REQUIRE(r.song.notes.size() == 3);
    const auto& n = r.song.notes;
    CHECK(n[0].start_s == doctest::Approx(0.0));
    CHECK(n[0].duration_s == doctest::Approx(0.5));
    CHECK(n[1].start_s == doctest::Approx(1.0));
    CHECK(n[1].duration_s == doctest::Approx(1.0));
    CHECK(n[2].start_s == doctest::Approx(2.0));
    CHECK(n[2].duration_s == doctest::Approx(1.25));
    CHECK(r.song.duration_s == doctest::Approx(3.25));

    const TempoMap& map = r.song.tempo;
    REQUIRE(map.changes.size() == 3);
    CHECK(map.changes[2].tick == 1920);
    CHECK(map.changes[2].seconds == doctest::Approx(3.0));
    CHECK(map.seconds_at(1200) == doctest::Approx(1.5));
    CHECK(map.tick_at(1.5) == doctest::Approx(1200.0));
    CHECK(map.tick_at(3.25) == doctest::Approx(2400.0));
    CHECK(map.tick_at(0.25) == doctest::Approx(240.0));
    CHECK(map.beats_at(2400) == doctest::Approx(5.0));
}

TEST_CASE("tempo events count from any track, and the later of two at one tick wins") {
    Bytes file = smf({Track().bpm(0, 30).bpm(0, 120).end(),
                      Track().bpm(480, 60).on(0, 0, 60).off(480, 0, 60).end()});
    MidiLoadResult r = load(file);
    REQUIRE(r.ok);
    REQUIRE(r.song.notes.size() == 1);
    CHECK(r.song.notes[0].start_s == doctest::Approx(0.5));
    CHECK(r.song.notes[0].duration_s == doctest::Approx(1.0));
}

TEST_CASE("running status in MIDI files parses correctly") {
    Track t;
    t.raw(0, {0x90, 60, 100})
        .raw(0, {64, 90}) // running status: another note-on
        .raw(480, {60, 0})
        .raw(0, {64, 0})
        .raw(0, {0x80, 67, 0}) // a new status byte
        .raw(0, {0x90, 67, 80})
        .raw(480, {67, 0})
        .end();
    MidiLoadResult r = load(smf({t}));
    REQUIRE(r.ok);
    REQUIRE(r.song.notes.size() == 3);
    CHECK(r.song.notes[0].pitch == 60);
    CHECK(r.song.notes[1].pitch == 64);
    CHECK(r.song.notes[1].velocity == 90);
    CHECK(r.song.notes[1].duration_s == doctest::Approx(0.5));
    CHECK(r.song.notes[2].pitch == 67);
    CHECK(r.song.notes[2].start_s == doctest::Approx(0.5));

    SUBCASE("a meta event cancels running status") {
        Track bad;
        bad.raw(0, {0x90, 60, 100}).name(0, "x").raw(480, {60, 0}).end();
        MidiLoadResult b = load(smf({bad}));
        CHECK_FALSE(b.ok);
        CHECK(b.error == MidiFileError::Corrupt);
    }
    SUBCASE("a data byte with no status before it is an error") {
        Track bad;
        bad.raw(0, {60, 100}).end();
        CHECK(load(smf({bad})).error == MidiFileError::Corrupt);
    }
}

TEST_CASE("a note-on with velocity 0 is a note-off") {
    Bytes file = smf({Track().on(0, 3, 72).on(240, 3, 72, 0).on(240, 3, 72).off(480, 3, 72).end()});
    MidiLoadResult r = load(file);
    REQUIRE(r.ok);
    REQUIRE(r.song.notes.size() == 2);
    CHECK(r.song.notes[0].duration_s == doctest::Approx(0.25));
    CHECK(r.song.notes[1].start_s == doctest::Approx(0.5));
    CHECK(r.song.notes[1].duration_s == doctest::Approx(0.5));
    CHECK(r.song.notes[1].channel == 3);
    CHECK(r.stats.overlaps_split == 0);
}

TEST_CASE("the sustain pedal never extends note durations") {
    Bytes file = smf({Track()
                          .cc(0, 0, 64, 127) // pedal down
                          .on(0, 0, 60)
                          .off(480, 0, 60)
                          .on(0, 0, 62)
                          .off(240, 0, 62)
                          .cc(960, 0, 64, 0) // pedal up long after
                          .end()});
    MidiLoadResult r = load(file);
    REQUIRE(r.ok);
    REQUIRE(r.song.notes.size() == 2);
    CHECK(r.song.notes[0].duration_s == doctest::Approx(0.5));
    CHECK(r.song.notes[1].duration_s == doctest::Approx(0.25));
}

TEST_CASE("overlapping notes of the same pitch split cleanly, never with zero or negative length") {
    SUBCASE("a second note-on ends the first; the extra note-off is ignored") {
        Bytes file =
            smf({Track().on(0, 0, 60).on(240, 0, 60).off(240, 0, 60).off(240, 0, 60).end()});
        MidiLoadResult r = load(file);
        REQUIRE(r.ok);
        REQUIRE(r.song.notes.size() == 2);
        CHECK(r.song.notes[0].start_tick == 0);
        CHECK(r.song.notes[0].end_tick == 240);
        CHECK(r.song.notes[1].start_tick == 240);
        CHECK(r.song.notes[1].end_tick == 480);
        CHECK(r.stats.overlaps_split == 1);
    }
    SUBCASE("a re-strike written note-on before note-off at one tick keeps both notes") {
        Bytes file = smf({Track().on(0, 0, 60).on(480, 0, 60).off(0, 0, 60).off(480, 0, 60).end()});
        MidiLoadResult r = load(file);
        REQUIRE(r.ok);
        REQUIRE(r.song.notes.size() == 2);
        CHECK(r.song.notes[0].end_tick == 480);
        CHECK(r.song.notes[1].start_tick == 480);
        CHECK(r.song.notes[1].end_tick == 960);
    }
    SUBCASE("two note-ons at one tick merge into one note") {
        Bytes file = smf({Track().on(0, 0, 60, 70).on(0, 0, 60, 90).off(480, 0, 60).end()});
        MidiLoadResult r = load(file);
        REQUIRE(r.ok);
        REQUIRE(r.song.notes.size() == 1);
        CHECK(r.song.notes[0].velocity == 70);
        CHECK(r.song.notes[0].end_tick == 480);
        CHECK(r.stats.duplicates_merged == 1);
    }
    SUBCASE("a note-off at the note-on tick drops the note") {
        Bytes file = smf({Track().on(0, 0, 60).off(0, 0, 60).on(480, 0, 62).off(480, 0, 62).end()});
        MidiLoadResult r = load(file);
        REQUIRE(r.ok);
        REQUIRE(r.song.notes.size() == 1);
        CHECK(r.song.notes[0].pitch == 62);
        CHECK(r.stats.zero_length_dropped == 1);
    }
    SUBCASE("the same pitch on another channel or track does not split") {
        Bytes file = smf({Track().on(0, 0, 60).on(240, 1, 60).off(240, 0, 60).off(240, 1, 60).end(),
                          Track().on(120, 0, 60).off(600, 0, 60).end()});
        MidiLoadResult r = load(file);
        REQUIRE(r.ok);
        REQUIRE(r.song.notes.size() == 3);
        CHECK(r.stats.overlaps_split == 0);
    }
    SUBCASE("every note in a tangle of on and off events has positive length") {
        Track t;
        int deltas[] = {0, 0, 10, 0, 0, 30, 0, 5, 0, 0, 100};
        for (int i = 0; i < 11; ++i) {
            if (i % 3 == 1) {
                t.off(static_cast<std::uint32_t>(deltas[i]), 0, 60);
            } else {
                t.on(static_cast<std::uint32_t>(deltas[i]), 0, 60);
            }
        }
        MidiLoadResult r = load(smf({t.end(7)}), {.keep_ghost_notes = true});
        REQUIRE(r.ok);
        for (const Note& n : r.song.notes) {
            CHECK(n.end_tick > n.start_tick);
            CHECK(n.duration_s > 0.0);
        }
    }
}

TEST_CASE("ghost notes under 30 ms are dropped, with a setting that keeps them") {
    // 480 ticks per quarter at 120 BPM: 1 tick = 1/960 s. 28 ticks = 29.2 ms, 29 ticks = 30.2 ms.
    Bytes file = smf({Track()
                          .on(0, 0, 60)
                          .off(28, 0, 60)
                          .on(0, 0, 62)
                          .off(29, 0, 62)
                          .on(0, 9, 42) // drum hit, short on purpose
                          .off(10, 9, 42)
                          .end()});
    MidiLoadResult r = load(file);
    REQUIRE(r.ok);
    REQUIRE(r.song.notes.size() == 2);
    CHECK(r.song.notes[0].pitch == 62);
    CHECK(r.song.notes[1].pitch == 42);
    CHECK(r.stats.ghost_notes_dropped == 1);

    MidiLoadResult kept = load(file, {.keep_ghost_notes = true});
    REQUIRE(kept.ok);
    CHECK(kept.song.notes.size() == 3);
    CHECK(kept.stats.ghost_notes_dropped == 0);

    MidiLoadResult strict = load(file, {.ghost_threshold_s = 0.05});
    REQUIRE(strict.ok);
    CHECK(strict.song.notes.size() == 1);
    CHECK(strict.stats.ghost_notes_dropped == 2);
}

TEST_CASE("program changes, controllers and pitch bends are kept for playback") {
    // Quarter = 0.5 s at 120 BPM until tick 480, then 1 s at 60 BPM.
    Track conductor;
    conductor.bpm(0, 120).bpm(480, 60).end();
    Track piano;
    piano.program(0, 0, 5)
        .cc(0, 0, 0, 1)               // bank select MSB, after the program change in the file
        .cc(0, 0, 32, 2)              // bank select LSB
        .cc(240, 0, 7, 90)            // volume at 0.25 s
        .raw(240, {0xE0, 0x10, 0x50}) // bend 0x50 * 128 + 0x10 at tick 480 = 0.5 s
        .cc(480, 1, 64, 127)          // pedal down on channel 2 at tick 960 = 1.5 s
        .cc(0, 0, 11, 100)            // expression, same tick as the pedal
        .on(0, 0, 60)
        .off(480, 0, 60)
        .end();
    MidiLoadResult r = load(smf({conductor, piano}));
    REQUIRE(r.ok);
    const std::vector<ChannelEvent>& e = r.song.events;
    REQUIRE(e.size() == 7);

    CHECK(e[0].kind == ChannelEventKind::Program);
    CHECK(e[0].channel == 0);
    CHECK(e[0].value == 5);
    CHECK(e[0].track == 1);
    CHECK(e[0].time_s == doctest::Approx(0.0));
    // Equal ticks keep the order of the file.
    CHECK(e[1].kind == ChannelEventKind::Control);
    CHECK(e[1].number == 0);
    CHECK(e[1].value == 1);
    CHECK(e[2].number == 32);
    CHECK(e[2].value == 2);

    CHECK(e[3].number == 7);
    CHECK(e[3].value == 90);
    CHECK(e[3].tick == 240);
    CHECK(e[3].time_s == doctest::Approx(0.25));

    CHECK(e[4].kind == ChannelEventKind::PitchBend);
    CHECK(e[4].value == 0x50 * 128 + 0x10);
    CHECK(e[4].time_s == doctest::Approx(0.5));

    // Across the tempo change: 0.5 s + one quarter at 60 BPM.
    CHECK(e[5].number == 64);
    CHECK(e[5].channel == 1);
    CHECK(e[5].value == 127);
    CHECK(e[5].tick == 960);
    CHECK(e[5].time_s == doctest::Approx(1.5));
    CHECK(e[6].number == 11);
    CHECK(e[6].time_s == doctest::Approx(1.5));
}

TEST_CASE("channel events from several tracks are merged by time, then by track") {
    Track a;
    a.cc(100, 0, 7, 1).cc(100, 0, 7, 3).end();
    Track b;
    b.cc(100, 1, 7, 2).cc(200, 1, 7, 4).end();
    MidiLoadResult r = load(smf({a, b}));
    REQUIRE(r.ok);
    REQUIRE(r.song.events.size() == 4);
    std::vector<int> values;
    for (const ChannelEvent& ev : r.song.events) {
        values.push_back(static_cast<int>(ev.value));
    }
    // tick 100: track 0 then track 1; tick 200: track 0, then track 1's second event at tick 300.
    CHECK(values == std::vector<int>{1, 2, 3, 4});
    for (std::size_t i = 1; i < r.song.events.size(); ++i) {
        CHECK(r.song.events[i - 1].tick <= r.song.events[i].tick);
    }
}

TEST_CASE("a program change at time 0 and one mid-song are both kept with their times") {
    Track t;
    t.program(0, 2, 40)
        .on(0, 2, 60)
        .off(480, 2, 60)
        .program(0, 2, 73)
        .on(0, 2, 62)
        .off(480, 2, 62)
        .end();
    MidiLoadResult r = load(smf({t}));
    REQUIRE(r.ok);
    REQUIRE(r.song.events.size() == 2);
    CHECK(r.song.events[0].value == 40);
    CHECK(r.song.events[0].time_s == doctest::Approx(0.0));
    CHECK(r.song.events[1].value == 73);
    CHECK(r.song.events[1].time_s == doctest::Approx(0.5));
    // Part.program still reports the first one.
    REQUIRE(r.song.parts.size() == 1);
    CHECK(r.song.parts[0].program == 40);
}

TEST_CASE("channel events do not change notes, and running status carries them") {
    Track t;
    t.on(0, 0, 60)
        .raw(0, {0xB0, 64, 127}) // pedal down
        .raw(0, {64, 0})         // running status: pedal up again
        .raw(0, {7, 80})         // running status: volume
        .off(480, 0, 60)
        .end();
    MidiLoadResult r = load(smf({t}));
    REQUIRE(r.ok);
    REQUIRE(r.song.notes.size() == 1);
    CHECK(r.song.notes[0].duration_s == doctest::Approx(0.5));
    REQUIRE(r.song.events.size() == 3);
    CHECK(r.song.events[0].value == 127);
    CHECK(r.song.events[1].value == 0);
    CHECK(r.song.events[2].number == 7);
}

TEST_CASE("channel pressure and note aftertouch are not kept as channel events") {
    Track t;
    t.on(0, 0, 60).raw(10, {0xA0, 60, 30}).raw(10, {0xD0, 20}).off(480, 0, 60).end();
    MidiLoadResult r = load(smf({t}));
    REQUIRE(r.ok);
    CHECK(r.song.events.empty());
}

TEST_CASE("a note-on with no note-off ends at the end of its track") {
    Bytes file = smf(
        {Track().on(0, 0, 60).on(480, 0, 64).end(480), Track().on(0, 0, 67).on(960, 0, 69).end()});
    MidiLoadResult r = load(file);
    REQUIRE(r.ok);
    REQUIRE(r.song.notes.size() == 3);
    CHECK(r.song.notes[0].pitch == 60);
    CHECK(r.song.notes[0].end_tick == 960);
    CHECK(r.song.notes[1].pitch == 67);
    CHECK(r.song.notes[1].end_tick == 960);
    CHECK(r.song.notes[2].pitch == 64);
    CHECK(r.song.notes[2].end_tick == 960);
    CHECK(r.stats.unterminated_notes == 3);
    // Pitch 69 starts on its track's last tick: zero length, dropped.
    CHECK(r.stats.zero_length_dropped == 1);
}

TEST_CASE("a track with no End-of-Track event ends at its last event") {
    Track t;
    t.on(0, 0, 60).off(480, 0, 60).on(0, 0, 62).cc(240, 0, 7, 100);
    MidiLoadResult r = load(smf({t, Track().on(0, 0, 50).off(10, 0, 50).end()}));
    REQUIRE(r.ok);
    REQUIRE(r.song.notes.size() == 2);
    CHECK(r.song.notes[1].pitch == 62);
    CHECK(r.song.notes[1].end_tick == 720);
}

TEST_CASE("notes on channel 10 are kept and flagged as drums") {
    Bytes file = smf({Track().on(0, 9, 36).off(240, 9, 36).on(0, 0, 60).off(240, 0, 60).end()});
    MidiLoadResult r = load(file);
    REQUIRE(r.ok);
    REQUIRE(r.song.notes.size() == 2);
    CHECK(r.song.notes[0].drum);
    CHECK(r.song.notes[0].channel == 9);
    CHECK_FALSE(r.song.notes[1].drum);
    REQUIRE(r.song.parts.size() == 2);
    CHECK_FALSE(r.song.parts[0].drum);
    CHECK(r.song.parts[1].drum);
}

TEST_CASE("measures come from time signatures") {
    SUBCASE("4/4 then 3/4 from bar 3") {
        // Bars start at 0, 1920, 3840 (3/4 from here), 5280.
        Bytes file = smf({Track().time_sig(0, 4, 4).time_sig(3840, 3, 4).end(),
                          Track()
                              .on(0, 0, 60)
                              .off(480, 0, 60)
                              .on(1440, 0, 61) // 1920
                              .off(480, 0, 61)
                              .on(1440, 0, 62) // 3840
                              .off(480, 0, 62)
                              .on(680, 0, 63) // 5000, still bar 3
                              .off(280, 0, 63)
                              .on(0, 0, 64) // 5280
                              .off(480, 0, 64)
                              .end()});
        MidiLoadResult r = load(file);
        REQUIRE(r.ok);
        const auto& n = r.song.notes;
        REQUIRE(n.size() == 5);
        CHECK(n[0].measure == 1);
        CHECK(n[1].measure == 2);
        CHECK(n[2].measure == 3);
        CHECK(n[3].measure == 3);
        CHECK(n[4].measure == 4);
        const auto& m = r.song.measures;
        REQUIRE(m.size() == 4);
        CHECK(m[2].start_tick == 3840);
        CHECK(m[2].start_s == doctest::Approx(4.0));
        CHECK(m[2].numerator == 3);
        CHECK(m[3].start_tick == 5280);
        CHECK(m[3].start_s == doctest::Approx(5.5));
    }
    SUBCASE("no time signature means 4/4") {
        MidiLoadResult r = load(smf({Track().on(1920, 0, 60).off(480, 0, 60).end()}));
        REQUIRE(r.ok);
        CHECK(r.song.notes[0].measure == 2);
        REQUIRE(r.song.time_signatures.size() == 1);
        CHECK(r.song.time_signatures[0].numerator == 4);
    }
    SUBCASE("6/8 bars are three quarter notes long") {
        MidiLoadResult r =
            load(smf({Track().time_sig(0, 6, 8).on(1440, 0, 60).off(480, 0, 60).end()}));
        REQUIRE(r.ok);
        CHECK(r.song.notes[0].measure == 2);
        CHECK(r.song.measures[1].denominator == 8);
    }
    SUBCASE("a time signature in the middle of a bar starts a new bar there") {
        MidiLoadResult r = load(smf({Track()
                                         .time_sig(0, 4, 4)
                                         .on(1920, 0, 60)
                                         .off(480, 0, 60)
                                         .time_sig(480, 4, 4) // tick 2880, mid bar 2
                                         .on(0, 0, 62)
                                         .off(480, 0, 62)
                                         .end()}));
        REQUIRE(r.ok);
        CHECK(r.song.notes[1].measure == 3);
        CHECK(r.song.measures[2].start_tick == 2880);
    }
    SUBCASE("a short first bar under its own time signature is a pickup, bar 0") {
        MidiLoadResult r = load(smf({Track()
                                         .time_sig(0, 1, 4)
                                         .on(0, 0, 67)
                                         .off(480, 0, 67)
                                         .time_sig(0, 4, 4) // tick 480
                                         .on(0, 0, 60)
                                         .off(1920, 0, 60)
                                         .on(0, 0, 62) // tick 2400
                                         .off(480, 0, 62)
                                         .end()}));
        REQUIRE(r.ok);
        CHECK(r.song.notes[0].measure == 0);
        CHECK(r.song.notes[1].measure == 1);
        CHECK(r.song.notes[2].measure == 2);
        CHECK(r.song.measures[0].number == 0);
    }
    SUBCASE("a first bar cut short by a time signature is a pickup") {
        MidiLoadResult r = load(smf({Track()
                                         .on(0, 0, 67)
                                         .off(960, 0, 67)
                                         .time_sig(0, 3, 4) // tick 960, half a 4/4 bar
                                         .on(0, 0, 60)
                                         .off(480, 0, 60)
                                         .end()}));
        REQUIRE(r.ok);
        CHECK(r.song.notes[0].measure == 0);
        CHECK(r.song.notes[1].measure == 1);
    }
}

TEST_CASE("SMPTE time division is rejected with a clear error") {
    Bytes file = header(0, 1, 0xE728); // -25 fps, 40 ticks per frame
    put_chunk(file, "MTrk", Track().on(0, 0, 60).off(40, 0, 60).end().data());
    MidiLoadResult r = load(file);
    CHECK_FALSE(r.ok);
    CHECK(r.error == MidiFileError::SmpteDivision);
    CHECK(r.message.find("SMPTE") != std::string::npos);
    CHECK(r.song.notes.empty());
}

TEST_CASE("SMF format 2 returns an error") {
    Bytes file = header(2, 1, 480);
    put_chunk(file, "MTrk", Track().on(0, 0, 60).off(480, 0, 60).end().data());
    MidiLoadResult r = load(file);
    CHECK_FALSE(r.ok);
    CHECK(r.error == MidiFileError::UnsupportedFormat);
}

TEST_CASE("unknown chunks such as Yamaha XF are skipped") {
    Bytes file = header(1, 2, 480);
    put_chunk(file, "MTrk", Track().bpm(0, 60).end().data());
    put_chunk(file, "XFIH", {1, 2, 3, 4, 5});
    put_chunk(file, "MTrk", Track().on(0, 0, 60).off(480, 0, 60).end().data());
    put_chunk(file, "XFKM", {9});
    MidiLoadResult r = load(file);
    REQUIRE(r.ok);
    REQUIRE(r.song.notes.size() == 1);
    CHECK(r.song.notes[0].duration_s == doctest::Approx(1.0));
}

TEST_CASE("parts describe each track and channel for hand assignment") {
    Bytes file = smf({Track().name(0, "Conductor").bpm(0, 120).end(), Track()
                                                                          .name(0, "Piano")
                                                                          .instrument(0, "Grand")
                                                                          .program(0, 0, 1)
                                                                          .channel_prefix(0, 1)
                                                                          .name(0, "Left")
                                                                          .on(0, 0, 72)
                                                                          .off(480, 0, 72)
                                                                          .on(0, 1, 48)
                                                                          .off(480, 1, 48)
                                                                          .on(0, 1, 50)
                                                                          .off(480, 1, 50)
                                                                          .end()});
    MidiLoadResult r = load(file, {.assign_hands = false});
    REQUIRE(r.ok);
    REQUIRE(r.song.parts.size() == 2);
    const Part& a = r.song.parts[0];
    CHECK(a.track == 1);
    CHECK(a.channel == 0);
    CHECK(a.name == "Piano");
    CHECK(a.instrument_name == "Grand");
    CHECK(a.has_program);
    CHECK(a.program == 1);
    CHECK(a.note_count == 1);
    const Part& b = r.song.parts[1];
    CHECK(b.channel == 1);
    CHECK(b.name == "Left");
    CHECK_FALSE(b.has_program);
    CHECK(b.note_count == 2);
    CHECK(b.hand == Hand::Unknown);
}

TEST_CASE("pitches stay untransposed, from the lowest to the highest MIDI note") {
    MidiLoadResult r =
        load(smf({Track().on(0, 0, 0).on(0, 0, 127).off(480, 0, 0).off(0, 0, 127).end()}));
    REQUIRE(r.ok);
    REQUIRE(r.song.notes.size() == 2);
    CHECK(r.song.notes[0].pitch == 0);
    CHECK(r.song.notes[1].pitch == 127);
}

TEST_CASE("the loader sets hands from track names unless told not to") {
    Bytes file = smf({Track().name(0, "Right Hand").on(0, 0, 72).off(480, 0, 72).end(),
                      Track().name(0, "Left Hand").on(0, 0, 48).off(480, 0, 48).end()});
    MidiLoadResult r = load(file);
    REQUIRE(r.ok);
    REQUIRE(r.song.notes.size() == 2);
    CHECK(r.song.notes[0].hand == Hand::Left);
    CHECK(r.song.notes[1].hand == Hand::Right);
    CHECK(load(file, {.assign_hands = false}).song.notes[0].hand == Hand::Unknown);
}

namespace {
// A file that uses most of the format: two tracks, running status, meta, SysEx, pedal, drums.
Bytes rich_file() {
    Track conductor;
    conductor.name(0, "Song").bpm(0, 100).time_sig(0, 3, 4).bpm(960, 140).end();
    Track piano;
    piano.name(0, "Piano RH")
        .program(0, 0, 0)
        .raw(0, {0xF0, 0x03, 0x7E, 0x7F, 0xF7})
        .on(0, 0, 60)
        .raw(0, {64, 80})
        .cc(100, 0, 64, 127)
        .raw(380, {0x90, 60, 0})
        .raw(0, {64, 0})
        .on(0, 9, 36)
        .off(20, 9, 36)
        .on(500, 0, 67)
        .end(480);
    return smf({conductor, piano}, 96);
}

// Every field of a successful result is well formed.
void check_well_formed(const MidiLoadResult& r) {
    if (!r.ok) {
        CHECK(r.error != MidiFileError::None);
        CHECK(r.song.notes.empty());
        return;
    }
    for (const Note& n : r.song.notes) {
        CHECK(n.end_tick > n.start_tick);
        CHECK(n.duration_s > 0.0);
        CHECK(n.pitch < 128);
    }
    CHECK_FALSE(r.song.measures.empty());
}
} // namespace

TEST_CASE("truncated or corrupt files return an error, never crash") {
    const Bytes good = rich_file();
    MidiLoadResult full = load(good);
    REQUIRE(full.ok);
    REQUIRE(full.song.notes.size() == 4);

    SUBCASE("a file cut inside its last track is read up to the cut; any other cut is an error") {
        // The last track's chunk header ends 8 bytes after its "MTrk".
        std::size_t last_track = 0;
        for (std::size_t i = 0; i + 4 <= good.size(); ++i) {
            if (std::equal(good.begin() + static_cast<std::ptrdiff_t>(i),
                           good.begin() + static_cast<std::ptrdiff_t>(i) + 4, "MTrk")) {
                last_track = i;
            }
        }
        REQUIRE(last_track > 0);
        for (std::size_t len = 0; len < good.size(); ++len) {
            Bytes cut(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(len));
            MidiLoadResult r = load(cut);
            CAPTURE(len);
            if (len < last_track + 8) {
                CHECK_FALSE(r.ok);
                CHECK(r.error != MidiFileError::None);
                CHECK_FALSE(r.message.empty());
            } else {
                CHECK(r.ok);
                CHECK(r.stats.tracks_truncated == 1);
                check_well_formed(r);
            }
        }
        CHECK(load_midi(nullptr, 0).error == MidiFileError::NotMidi);
    }
    SUBCASE("random bytes, with and without a valid header") {
        std::mt19937 rng(1234);
        std::uniform_int_distribution<int> byte(0, 255);
        std::uniform_int_distribution<std::size_t> length(0, 300);
        for (int i = 0; i < 4000; ++i) {
            Bytes junk = i % 2 == 0 ? Bytes{} : Bytes(good.begin(), good.begin() + 22);
            std::size_t n = length(rng);
            for (std::size_t k = 0; k < n; ++k) {
                junk.push_back(static_cast<std::uint8_t>(byte(rng)));
            }
            check_well_formed(load(junk));
        }
    }
    SUBCASE("a valid file with bytes flipped") {
        std::mt19937 rng(99);
        std::uniform_int_distribution<std::size_t> where(0, good.size() - 1);
        std::uniform_int_distribution<int> byte(0, 255);
        for (int i = 0; i < 4000; ++i) {
            Bytes bad = good;
            for (int k = 0; k < 1 + i % 4; ++k) {
                bad[where(rng)] = static_cast<std::uint8_t>(byte(rng));
            }
            check_well_formed(load(bad));
        }
    }
    SUBCASE("lengths that point past the end of the data") {
        Bytes bad = good;
        bad[18] = 0x7F; // first MTrk length: the second track is swallowed, so a track is missing
        CHECK_FALSE(load(bad).ok);
        Bytes huge_meta = header(0, 1, 480);
        put_chunk(huge_meta, "MTrk", {0x00, 0xFF, 0x03, 0x8F, 0xFF, 0xFF, 0x7F, 'x'});
        CHECK(load(huge_meta).error == MidiFileError::Corrupt);
    }
}

TEST_CASE("a track chunk longer than the file is read up to the end of the data") {
    SUBCASE("the declared length is too big, the events are whole") {
        Bytes file = header(0, 1, 480);
        Bytes body = Track().on(0, 0, 60).off(480, 0, 60).on(0, 0, 62).off(480, 0, 62).data();
        file.insert(file.end(), {'M', 'T', 'r', 'k'});
        put_be(file, static_cast<std::uint32_t>(body.size() + 1000), 4);
        file.insert(file.end(), body.begin(), body.end());
        MidiLoadResult r = load(file);
        REQUIRE(r.ok);
        CHECK(r.stats.tracks_truncated == 1);
        REQUIRE(r.song.notes.size() == 2);
        CHECK(r.song.notes[1].start_s == doctest::Approx(0.5));
        CHECK(r.song.notes[1].duration_s == doctest::Approx(0.5));
    }
    SUBCASE("a file whose length is right is not counted") {
        MidiLoadResult r = load(smf({Track().on(0, 0, 60).off(480, 0, 60).end()}));
        REQUIRE(r.ok);
        CHECK(r.stats.tracks_truncated == 0);
    }
    SUBCASE("a cut in the middle of an event ends the track cleanly") {
        Bytes file = header(0, 1, 480);
        Bytes body = Track().on(0, 0, 60).on(480, 0, 62).off(480, 0, 62).data();
        body.insert(body.end(), {0x81, 0x00, 0x90, 60}); // delta 128, note-on missing its velocity
        file.insert(file.end(), {'M', 'T', 'r', 'k'});
        put_be(file, static_cast<std::uint32_t>(body.size() + 50), 4);
        file.insert(file.end(), body.begin(), body.end());
        MidiLoadResult r = load(file);
        REQUIRE(r.ok);
        CHECK(r.stats.tracks_truncated == 1);
        REQUIRE(r.song.notes.size() == 2);
        // The first note had no note-off: it ends at the last whole event (tick 960), not at the
        // cut event.
        CHECK(r.song.notes[0].end_tick == 960);
        CHECK(r.stats.unterminated_notes == 1);
    }
    SUBCASE("a cut inside a meta event or SysEx ends the track cleanly") {
        for (Bytes tail : {Bytes{0x00, 0xFF, 0x03, 0x10, 'a'}, Bytes{0x00, 0xF0, 0x20, 0x01},
                           Bytes{0x00, 0xFF}, Bytes{0x80}}) {
            Bytes file = header(0, 1, 480);
            Bytes body = Track().on(0, 0, 60).off(480, 0, 60).data();
            body.insert(body.end(), tail.begin(), tail.end());
            file.insert(file.end(), {'M', 'T', 'r', 'k'});
            put_be(file, static_cast<std::uint32_t>(body.size() + 9), 4);
            file.insert(file.end(), body.begin(), body.end());
            MidiLoadResult r = load(file);
            CAPTURE(tail.size());
            REQUIRE(r.ok);
            CHECK(r.stats.tracks_truncated == 1);
            CHECK(r.song.notes.size() == 1);
        }
    }
    SUBCASE("bytes that do not parse inside the cut-off chunk are still an error") {
        Bytes file = header(0, 1, 480);
        Bytes body = {0x00, 0x3C, 0x40}; // a data byte with no running status
        file.insert(file.end(), {'M', 'T', 'r', 'k'});
        put_be(file, 100, 4);
        file.insert(file.end(), body.begin(), body.end());
        CHECK(load(file).error == MidiFileError::Corrupt);
    }
    SUBCASE("a track chunk with a whole header and no data is an empty track") {
        Bytes file = header(0, 1, 480);
        file.insert(file.end(), {'M', 'T', 'r', 'k', 0, 0, 0, 20});
        MidiLoadResult r = load(file);
        REQUIRE(r.ok);
        CHECK(r.stats.tracks_truncated == 1);
        CHECK(r.song.notes.empty());
    }
    SUBCASE("a missing track is still an error") {
        Bytes file = header(1, 2, 480);
        put_chunk(file, "MTrk", Track().bpm(0, 120).end().data());
        file.insert(file.end(), {'M', 'T', 'r'}); // cut inside the next chunk header
        CHECK(load(file).error == MidiFileError::Truncated);
    }
    SUBCASE("an unknown chunk that runs past the end is still an error") {
        Bytes file = header(1, 2, 480);
        file.insert(file.end(), {'X', 'F', 'I', 'H', 0, 0, 0, 50, 1, 2});
        CHECK(load(file).error == MidiFileError::Truncated);
    }
}

TEST_CASE("a song absurdly many bars long is refused, not allocated") {
    // A 1/64 bar at 1 tick per quarter is 1/16 of a tick, so it is ignored; 1/4 bars of one
    // tick each over 2^27 ticks would be 134 million bars.
    Bytes file = header(0, 1, 1);
    put_chunk(file, "MTrk",
              Track().time_sig(0, 1, 4).on(0, 0, 60).off(0x7FFFFFF, 0, 60).end().data());
    MidiLoadResult r = load(file);
    CHECK_FALSE(r.ok);
    CHECK(r.error == MidiFileError::TooLarge);
}

namespace {
Bytes read_test_file(const std::string& name) {
    std::ifstream in(std::string(TOONVAL_TEST_DATA_DIR) + "/" + name, std::ios::binary);
    REQUIRE(in.good());
    return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}
} // namespace

TEST_CASE("real file: Bach, Prelude in C major BWV 846") {
    MidiLoadResult r = load(read_test_file("bach-bwv846-prelude.mid"));
    REQUIRE(r.ok);
    const Song& song = r.song;
    REQUIRE(song.time_signatures.size() == 1);
    CHECK(song.time_signatures[0].numerator == 4);
    CHECK(song.time_signatures[0].denominator == 4);
    CHECK(song.measures.size() == 35); // the prelude has 35 bars
    CHECK(song.measures.front().number == 1);
    // Bar 1 is C4 E4 | G4 C5 E5 G4 C5 E5, played twice.
    REQUIRE(song.notes.size() > 16);
    const int bar1[] = {60, 64, 67, 72, 76, 67, 72, 76};
    for (int i = 0; i < 8; ++i) {
        CAPTURE(i);
        CHECK(song.notes[static_cast<std::size_t>(i)].pitch == bar1[i]);
        CHECK(song.notes[static_cast<std::size_t>(i)].measure == 1);
    }
    CHECK(song.notes[16].measure == 2);
    CHECK(song.notes.back().measure == 35);
    REQUIRE(song.parts.size() == 2);
    CHECK(song.parts[0].hand == Hand::Left);  // staff "lower"
    CHECK(song.parts[1].hand == Hand::Right); // staff "upper"
    for (const Note& n : song.notes) {
        CHECK(n.duration_s > 0.0);
        CHECK(n.hand != Hand::Unknown);
    }
}

TEST_CASE("real file: Satie, Gymnopedie No. 1") {
    MidiLoadResult r = load(read_test_file("satie-gymnopedie-1.mid"));
    REQUIRE(r.ok);
    const Song& song = r.song;
    REQUIRE(song.time_signatures.size() == 1);
    CHECK(song.time_signatures[0].numerator == 3);
    CHECK(song.time_signatures[0].denominator == 4);
    // The left hand alternates G2 and D2 on each downbeat, under chords on beat 2.
    REQUIRE(song.notes.size() > 8);
    CHECK(song.notes[0].pitch == 43);
    CHECK(song.notes[0].hand == Hand::Left);
    CHECK(song.notes[1].pitch == 59);
    CHECK(song.notes[1].hand == Hand::Right);
    CHECK(song.notes[4].pitch == 38);
    CHECK(song.notes[4].measure == 2);
    CHECK(song.notes[1].start_s > song.notes[0].start_s);
    CHECK(song.notes.back().measure == song.measures.back().number);
    CHECK(r.stats.ghost_notes_dropped == 0);
}
