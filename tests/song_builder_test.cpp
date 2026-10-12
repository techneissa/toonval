// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/song_builder.hpp"

#include <doctest/doctest.h>

using namespace toonval;

namespace {

Note make_note(std::int64_t start, std::int64_t end, int pitch, std::uint16_t track = 1) {
    Note n;
    n.start_tick = start;
    n.end_tick = end;
    n.pitch = static_cast<std::uint8_t>(pitch);
    n.track = track;
    return n;
}

Song empty_song(int ppq = 480, double bpm = 120.0) {
    Song s;
    s.tempo = make_tempo_map(ppq, bpm);
    return s;
}

} // namespace

TEST_CASE("song_builder: bpm becomes microseconds per quarter, rounded and clamped") {
    CHECK(us_per_quarter_from_bpm(120.0) == 500'000);
    CHECK(us_per_quarter_from_bpm(60.0) == 1'000'000);
    CHECK(us_per_quarter_from_bpm(100.0) == 600'000);
    CHECK(us_per_quarter_from_bpm(0.0) == 0xFFFFFF);
    CHECK(us_per_quarter_from_bpm(-5.0) == 0xFFFFFF);
    CHECK(us_per_quarter_from_bpm(1e12) == 1);
}

TEST_CASE("song_builder: make_tempo_map starts at tick 0 with one tempo") {
    const TempoMap m = make_tempo_map(960, 90.0);
    CHECK(m.ticks_per_quarter == 960);
    REQUIRE(m.changes.size() == 1);
    CHECK(m.changes[0].tick == 0);
    CHECK(m.changes[0].us_per_quarter == us_per_quarter_from_bpm(90.0));
}

TEST_CASE("song_builder: finish_song sorts notes and fills seconds from ticks") {
    Song s = empty_song();
    s.notes = {make_note(480, 960, 64), make_note(0, 480, 72), make_note(0, 240, 60)};
    CHECK(finish_song(s));
    REQUIRE(s.notes.size() == 3);
    // Sorted by start tick, then pitch.
    CHECK(s.notes[0].pitch == 60);
    CHECK(s.notes[1].pitch == 72);
    CHECK(s.notes[2].pitch == 64);
    CHECK(s.notes[0].duration_s == doctest::Approx(0.25));
    CHECK(s.notes[1].duration_s == doctest::Approx(0.5));
    CHECK(s.notes[2].start_s == doctest::Approx(0.5));
    CHECK(s.end_tick == 960);
    CHECK(s.duration_s == doctest::Approx(1.0));
}

TEST_CASE("song_builder: seconds go through every tempo change") {
    Song s = empty_song();
    s.tempo.changes.push_back({480, 0.0, 250'000}); // 240 BPM from beat 1
    s.notes = {make_note(480, 960, 60)};
    CHECK(finish_song(s));
    CHECK(s.tempo.changes[1].seconds == doctest::Approx(0.5));
    CHECK(s.notes[0].start_s == doctest::Approx(0.5));
    CHECK(s.notes[0].duration_s == doctest::Approx(0.25));
}

TEST_CASE("song_builder: bars come from the time signature and number from 1") {
    Song s = empty_song();
    s.notes = {make_note(0, 100, 60), make_note(1920, 2000, 62), make_note(3840 + 10, 3900, 64)};
    CHECK(finish_song(s));
    REQUIRE(s.time_signatures.size() == 1);
    CHECK(s.time_signatures[0].numerator == 4);
    REQUIRE(s.measures.size() == 3);
    CHECK(s.measures[0].number == 1);
    CHECK(s.measures[1].start_tick == 1920);
    CHECK(s.measures[1].start_s == doctest::Approx(2.0));
    CHECK(s.measures[2].number == 3);
    CHECK(s.notes[0].measure == 1);
    CHECK(s.notes[1].measure == 2);
    CHECK(s.notes[2].measure == 3);
}

TEST_CASE("song_builder: a first bar shorter than the next is a pickup, numbered 0") {
    Song s = empty_song();
    s.time_signatures = {{0, 4, 4}, {480, 3, 4}}; // one beat, then 3/4 bars
    s.notes = {make_note(0, 100, 60), make_note(480, 900, 62), make_note(1920, 2000, 64)};
    CHECK(finish_song(s));
    REQUIRE(s.measures.size() >= 3);
    CHECK(s.measures[0].number == 0);
    CHECK(s.measures[1].number == 1);
    CHECK(s.measures[1].numerator == 3);
    CHECK(s.notes[0].measure == 0);
    CHECK(s.notes[1].measure == 1);
}

TEST_CASE("song_builder: parts are made for every track and channel with notes") {
    Song s = empty_song();
    Note a = make_note(0, 100, 60, 1);
    a.hand = Hand::Right;
    Note b = make_note(0, 100, 48, 2);
    b.channel = 3;
    Note d = make_note(100, 200, 62, 1);
    d.hand = Hand::Right;
    s.notes = {a, b, d};
    Part named;
    named.track = 1;
    named.channel = 0;
    named.name = "Right Hand";
    s.parts = {named};
    CHECK(finish_song(s));
    REQUIRE(s.parts.size() == 2);
    CHECK(s.parts[0].track == 1);
    CHECK(s.parts[0].name == "Right Hand");
    CHECK(s.parts[0].note_count == 2);
    CHECK(s.parts[1].track == 2);
    CHECK(s.parts[1].channel == 3);
    CHECK(s.parts[1].note_count == 1);
}

TEST_CASE("song_builder: events get seconds and are sorted by tick, keeping order within a tick") {
    Song s = empty_song();
    ChannelEvent pedal_up;
    pedal_up.tick = 960;
    pedal_up.number = 64;
    pedal_up.value = 0;
    ChannelEvent pedal_down;
    pedal_down.tick = 480;
    pedal_down.number = 64;
    pedal_down.value = 127;
    ChannelEvent vol;
    vol.tick = 480;
    vol.number = 7;
    vol.value = 100;
    s.events = {pedal_up, pedal_down, vol};
    s.notes = {make_note(0, 960, 60)};
    CHECK(finish_song(s));
    REQUIRE(s.events.size() == 3);
    CHECK(s.events[0].value == 127);
    CHECK(s.events[1].number == 7);
    CHECK(s.events[2].tick == 960);
    CHECK(s.events[0].time_s == doctest::Approx(0.5));
}

TEST_CASE("song_builder: a song with no notes finishes with no bars and zero length") {
    Song s = empty_song();
    CHECK(finish_song(s));
    CHECK(s.notes.empty());
    CHECK(s.duration_s == 0.0);
    CHECK(s.end_tick == 0);
}

TEST_CASE("song_builder: more bars than the cap stop at the cap and say so") {
    Song s = empty_song(1, 120.0); // 1 tick per quarter: a 4/4 bar is 4 ticks
    s.notes = {make_note(0, 4 * 200'000, 60)};
    CHECK_FALSE(finish_song(s));
    CHECK(s.measures.size() == kMaxSongMeasures);
}

TEST_CASE("song_builder: channel 10 notes are drums and a part takes its first program change") {
    Song s = empty_song();
    Note drum = make_note(0, 100, 36);
    drum.channel = 9;
    Note piano = make_note(0, 100, 60, 2);
    s.notes = {drum, piano};
    ChannelEvent late;
    late.tick = 50;
    late.track = 2;
    late.kind = ChannelEventKind::Program;
    late.value = 40;
    ChannelEvent first = late;
    first.tick = 10;
    first.value = 5;
    s.events = {late, first};
    CHECK(finish_song(s));
    REQUIRE(s.parts.size() == 2);
    CHECK(s.parts[0].drum);
    CHECK_FALSE(s.parts[0].has_program);
    CHECK(s.parts[1].has_program);
    CHECK(s.parts[1].program == 5);
    CHECK(s.notes[0].drum);
}
