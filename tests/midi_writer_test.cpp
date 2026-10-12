// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/midi_writer.hpp"

#include "smf_builder.hpp"
#include "song_compare.hpp"
#include "toonval/midi_file.hpp"
#include "toonval/song_builder.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <tuple>

using namespace toonval;
using test::Bytes;
using test::Track;

namespace {

Note make_note(std::int64_t start, std::int64_t end, int pitch, std::uint16_t track = 1,
               std::uint8_t channel = 0, int velocity = 100) {
    Note n;
    n.start_tick = start;
    n.end_tick = end;
    n.pitch = static_cast<std::uint8_t>(pitch);
    n.velocity = static_cast<std::uint8_t>(velocity);
    n.track = track;
    n.channel = channel;
    return n;
}

Song one_note_song() {
    Song s;
    s.tempo = make_tempo_map(480, 120.0);
    s.notes = {make_note(0, 480, 60)};
    Part p;
    p.track = 1;
    p.name = "Piano";
    s.parts = {p};
    finish_song(s);
    return s;
}

MidiLoadResult load_all(const Bytes& bytes) {
    MidiLoadOptions opt;
    opt.keep_ghost_notes = true;
    return load_midi(bytes, opt);
}

// Writes `song`, loads the bytes, and checks the loaded song is the one written.
void check_round_trip(const Song& song, const MidiWriteOptions& options = {}) {
    const MidiWriteResult w = write_midi(song, options);
    REQUIRE(w.ok);
    const MidiLoadResult r = load_all(w.bytes);
    REQUIRE(r.ok);
    test::expect_same_song(song, r.song);
}

// A small deterministic generator, so a failure repeats.
class Lcg {
  public:
    explicit Lcg(std::uint64_t seed) : state_(seed) {}
    int below(int n) {
        state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<int>((state_ >> 33) % static_cast<std::uint64_t>(n));
    }

  private:
    std::uint64_t state_;
};

// A random song: several tracks and channels, tempo and time-signature changes, notes that never
// overlap on one pitch, controllers, program changes and pitch bends, and named parts.
Song random_song(std::uint64_t seed) {
    Lcg rng(seed);
    Song s;
    const int ppq = 96 * (1 + rng.below(8)); // 96..768
    s.tempo.ticks_per_quarter = ppq;
    s.tempo.changes.assign(
        1, TempoChange{0, 0.0, static_cast<std::uint32_t>(300'000 + rng.below(500'000))});
    const std::int64_t length = std::int64_t{ppq} * 4 * (4 + rng.below(20));
    for (int i = rng.below(4); i > 0; --i) {
        s.tempo.changes.push_back({rng.below(static_cast<int>(length)), 0.0,
                                   static_cast<std::uint32_t>(250'000 + rng.below(750'000))});
    }
    s.time_signatures.push_back({0, 4, 4});
    for (int i = rng.below(3); i > 0; --i) {
        const int den = 1 << (1 + rng.below(4)); // 2..16
        s.time_signatures.push_back({std::int64_t{ppq} * 4 * rng.below(8),
                                     static_cast<std::uint8_t>(1 + rng.below(7)),
                                     static_cast<std::uint8_t>(den)});
    }
    const int tracks = 1 + rng.below(4);
    std::map<std::tuple<int, int, int>, std::int64_t> free_at; // track, channel, pitch -> tick
    for (int t = 0; t < tracks; ++t) {
        const int count = rng.below(60);
        for (int i = 0; i < count; ++i) {
            const int ch = rng.below(3) == 0 ? 9 : rng.below(3);
            const int pitch = 36 + rng.below(60);
            std::int64_t start = rng.below(static_cast<int>(length));
            auto& free = free_at[{t, ch, pitch}];
            start = std::max(start, free);
            const std::int64_t dur = ppq / 4 + rng.below(ppq * 2);
            s.notes.push_back(make_note(start, start + dur, pitch, static_cast<std::uint16_t>(t),
                                        static_cast<std::uint8_t>(ch), 1 + rng.below(127)));
            free = start + dur + 1;
        }
        for (int i = rng.below(12); i > 0; --i) {
            ChannelEvent e;
            e.tick = rng.below(static_cast<int>(length));
            e.track = static_cast<std::uint16_t>(t);
            e.channel = static_cast<std::uint8_t>(rng.below(3));
            switch (rng.below(3)) {
            case 0:
                e.kind = ChannelEventKind::Program;
                e.value = static_cast<std::uint16_t>(rng.below(128));
                break;
            case 1:
                e.kind = ChannelEventKind::Control;
                e.number = rng.below(2) == 0 ? 64 : static_cast<std::uint8_t>(rng.below(120));
                e.value = static_cast<std::uint16_t>(rng.below(128));
                break;
            default:
                e.kind = ChannelEventKind::PitchBend;
                e.value = static_cast<std::uint16_t>(rng.below(16384));
                break;
            }
            s.events.push_back(e);
        }
    }
    // Events are generated track by track, so a tick tie sorts the way the loader orders them.
    // Every channel of a track gets a name, and the instrument name is set on all of them or none:
    // a file cannot tell an unnamed part from one that takes its track's name.
    static const char* const kNames[] = {"Right Hand", "Left Hand", "Strings", "Melody"};
    for (int t = 0; t < tracks; ++t) {
        const bool instruments = rng.below(2) == 0;
        int index = 0;
        for (int ch : {0, 1, 2, 9}) {
            Part p;
            p.track = static_cast<std::uint16_t>(t);
            p.channel = static_cast<std::uint8_t>(ch);
            p.name = kNames[(t + index) % 4];
            if (instruments) {
                p.instrument_name = "Inst " + std::to_string(t * 4 + index);
            }
            s.parts.push_back(p);
            ++index;
        }
    }
    finish_song(s);
    // Only parts with notes survive a file, so drop the unused ones from the expectation.
    std::erase_if(s.parts, [](const Part& p) { return p.note_count == 0; });
    assign_hands_from_names(s);
    return s;
}

Bytes read_test_file(const std::string& name) {
    std::ifstream in(std::string(TOONVAL_TEST_DATA_DIR) + "/" + name, std::ios::binary);
    REQUIRE(in.good());
    return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("midi_writer: one note is exactly the bytes a reader expects") {
    const MidiWriteResult w = write_midi(one_note_song());
    REQUIRE(w.ok);
    const Bytes expected =
        test::smf({Track().tempo(0, 500'000).time_sig(0, 4, 4).end(),
                   Track().name(0, "Piano").on(0, 0, 60, 100).off(480, 0, 60, 64).end()});
    CHECK(w.bytes == expected);
    CHECK(w.notes_written == 1);
    CHECK(w.notes_skipped == 0);
}

TEST_CASE("midi_writer: format 0 puts everything in one track") {
    Song s = one_note_song();
    MidiWriteOptions opt;
    opt.format = 0;
    const MidiWriteResult w = write_midi(s, opt);
    REQUIRE(w.ok);
    const Bytes expected = test::smf({Track()
                                          .name(0, "Piano")
                                          .tempo(0, 500'000)
                                          .time_sig(0, 4, 4)
                                          .on(0, 0, 60, 100)
                                          .off(480, 0, 60, 64)
                                          .end()});
    CHECK(w.bytes == expected);
    const MidiLoadResult r = load_all(w.bytes);
    REQUIRE(r.ok);
    REQUIRE(r.song.notes.size() == 1);
    CHECK(r.song.notes[0].pitch == 60);
    CHECK(r.song.notes[0].end_tick == 480);
}

TEST_CASE("midi_writer: format 0 keeps notes of every track, on their channels") {
    Song s;
    s.tempo = make_tempo_map(480, 100.0);
    s.notes = {make_note(0, 240, 60, 1, 0), make_note(0, 480, 48, 2, 1),
               make_note(240, 480, 62, 1, 0)};
    finish_song(s);
    MidiWriteOptions opt;
    opt.format = 0;
    const MidiWriteResult w = write_midi(s, opt);
    REQUIRE(w.ok);
    const MidiLoadResult r = load_all(w.bytes);
    REQUIRE(r.ok);
    REQUIRE(r.song.notes.size() == 3);
    for (const Note& n : r.song.notes) {
        CHECK(n.track == 0);
    }
    CHECK(r.song.notes[0].pitch == 48);
    CHECK(r.song.notes[0].channel == 1);
    CHECK(r.song.notes[0].end_tick == 480);
    CHECK(r.song.tempo.changes[0].us_per_quarter == 600'000);
}

TEST_CASE("midi_writer: tempo, time signature and track names round-trip") {
    Song s;
    s.tempo = make_tempo_map(480, 90.0);
    s.tempo.changes.push_back({1920, 0.0, 400'000});
    s.time_signatures = {{0, 3, 4}, {2880, 6, 8}};
    s.notes = {make_note(0, 480, 60, 1), make_note(2000, 3000, 48, 2)};
    Part right;
    right.track = 1;
    right.name = "Right Hand";
    right.instrument_name = "Grand";
    Part left;
    left.track = 2;
    left.name = "Left Hand";
    s.parts = {right, left};
    finish_song(s);
    assign_hands_from_names(s);
    REQUIRE(s.notes[0].hand == Hand::Right);
    check_round_trip(s);
}

TEST_CASE("midi_writer: two channels with different names in one track keep their names") {
    Song s;
    s.tempo = make_tempo_map(480, 120.0);
    s.notes = {make_note(0, 480, 60, 1, 0), make_note(0, 480, 48, 1, 1)};
    Part a;
    a.track = 1;
    a.channel = 0;
    a.name = "Right Hand";
    Part b;
    b.track = 1;
    b.channel = 1;
    b.name = "Left Hand";
    s.parts = {a, b};
    finish_song(s);
    assign_hands_from_names(s);
    check_round_trip(s);
}

TEST_CASE("midi_writer: sustain pedal, volume, program and pitch bend are written") {
    Song s;
    s.tempo = make_tempo_map(480, 120.0);
    s.notes = {make_note(0, 480, 60, 1, 2), make_note(480, 960, 62, 1, 2)};
    auto ev = [](std::int64_t tick, ChannelEventKind kind, int number, int value) {
        ChannelEvent e;
        e.tick = tick;
        e.track = 1;
        e.channel = 2;
        e.kind = kind;
        e.number = static_cast<std::uint8_t>(number);
        e.value = static_cast<std::uint16_t>(value);
        return e;
    };
    s.events = {ev(0, ChannelEventKind::Program, 0, 19), ev(0, ChannelEventKind::Control, 7, 90),
                ev(100, ChannelEventKind::Control, 64, 127),
                ev(300, ChannelEventKind::PitchBend, 0, 12000),
                ev(960, ChannelEventKind::Control, 64, 0)};
    finish_song(s);
    check_round_trip(s);
    const MidiLoadResult r = load_all(write_midi(s).bytes);
    REQUIRE(r.ok);
    // The pedal never makes the notes longer.
    CHECK(r.song.notes[0].end_tick == 480);
}

TEST_CASE("midi_writer: running status gives a smaller file that loads the same") {
    const Song s = random_song(7);
    MidiWriteOptions plain;
    MidiWriteOptions running;
    running.running_status = true;
    const MidiWriteResult a = write_midi(s, plain);
    const MidiWriteResult b = write_midi(s, running);
    REQUIRE(a.ok);
    REQUIRE(b.ok);
    CHECK(b.bytes.size() < a.bytes.size());
    check_round_trip(s, running);
}

TEST_CASE("midi_writer: running status is not used across a meta event or another channel") {
    Song s;
    s.tempo = make_tempo_map(480, 120.0);
    s.notes = {make_note(0, 100, 60, 1, 0), make_note(0, 100, 64, 1, 1)};
    finish_song(s);
    MidiWriteOptions opt;
    opt.running_status = true;
    const MidiWriteResult w = write_midi(s, opt);
    REQUIRE(w.ok);
    const Bytes expected =
        test::smf({Track().tempo(0, 500'000).time_sig(0, 4, 4).end(), Track()
                                                                          .raw(0, {0x90, 60, 100})
                                                                          .raw(0, {0x91, 64, 100})
                                                                          .raw(100, {0x80, 60, 64})
                                                                          .raw(0, {0x81, 64, 64})
                                                                          .end()});
    // Note-on of channel 0 then channel 1 differ in status, so each keeps its status byte; the
    // note-off of channel 1 right after channel 0's is a new status too.
    CHECK(w.bytes == expected);
}

TEST_CASE("midi_writer: running status drops repeated status bytes") {
    Song s;
    s.tempo = make_tempo_map(480, 120.0);
    s.notes = {make_note(0, 100, 60), make_note(0, 100, 64)};
    finish_song(s);
    MidiWriteOptions opt;
    opt.running_status = true;
    const MidiWriteResult w = write_midi(s, opt);
    REQUIRE(w.ok);
    const Bytes expected =
        test::smf({Track().tempo(0, 500'000).time_sig(0, 4, 4).end(), Track()
                                                                          .raw(0, {0x90, 60, 100})
                                                                          .raw(0, {64, 100})
                                                                          .raw(100, {0x80, 60, 64})
                                                                          .raw(0, {64, 64})
                                                                          .end()});
    CHECK(w.bytes == expected);
}

TEST_CASE("midi_writer: notes that overlap on one pitch are shortened, never lost or negative") {
    Song s;
    s.tempo = make_tempo_map(480, 120.0);
    Note a = make_note(0, 1000, 60);
    Note b = make_note(500, 900, 60);
    Note dup = make_note(500, 700, 60); // same start as b: merged into it
    Note zero = make_note(2000, 2000, 62);
    Note high = make_note(0, 100, 200);
    s.notes = {a, b, dup, zero, high};
    // finish_song would drop `zero`, so write the unfinished song as a caller might.
    s.time_signatures = {{0, 4, 4}};
    const MidiWriteResult w = write_midi(s);
    REQUIRE(w.ok);
    CHECK(w.notes_written == 2);
    CHECK(w.notes_shortened == 1);
    CHECK(w.notes_skipped == 3);
    const MidiLoadResult r = load_all(w.bytes);
    REQUIRE(r.ok);
    REQUIRE(r.song.notes.size() == 2);
    CHECK(r.song.notes[0].start_tick == 0);
    CHECK(r.song.notes[0].end_tick == 500);
    CHECK(r.song.notes[1].start_tick == 500);
    CHECK(r.song.notes[1].end_tick == 900);
}

TEST_CASE("midi_writer: velocity is kept in 1..127 so a note-on is never a note-off") {
    Song s;
    s.tempo = make_tempo_map(480, 120.0);
    s.notes = {make_note(0, 100, 60, 1, 0, 0), make_note(200, 300, 62, 1, 0, 255)};
    const MidiWriteResult w = write_midi(s);
    REQUIRE(w.ok);
    const MidiLoadResult r = load_all(w.bytes);
    REQUIRE(r.ok);
    REQUIRE(r.song.notes.size() == 2);
    CHECK(r.song.notes[0].velocity == 1);
    CHECK(r.song.notes[1].velocity == 127);
}

TEST_CASE("midi_writer: a note ending where the same pitch starts again is two notes") {
    Song s;
    s.tempo = make_tempo_map(480, 120.0);
    s.notes = {make_note(0, 480, 60), make_note(480, 960, 60)};
    finish_song(s);
    check_round_trip(s);
}

TEST_CASE("midi_writer: a song with no notes is a valid file with its tempo") {
    Song s;
    s.tempo = make_tempo_map(480, 100.0);
    finish_song(s);
    const MidiWriteResult w = write_midi(s);
    REQUIRE(w.ok);
    const MidiLoadResult r = load_all(w.bytes);
    REQUIRE(r.ok);
    CHECK(r.song.notes.empty());
    CHECK(r.song.tempo.changes[0].us_per_quarter == 600'000);
}

TEST_CASE("midi_writer: a gap of empty source tracks keeps the track numbers") {
    Song s;
    s.tempo = make_tempo_map(480, 120.0);
    s.notes = {make_note(0, 480, 60, 3)};
    finish_song(s);
    const MidiWriteResult w = write_midi(s);
    REQUIRE(w.ok);
    const MidiLoadResult r = load_all(w.bytes);
    REQUIRE(r.ok);
    REQUIRE(r.song.notes.size() == 1);
    CHECK(r.song.notes[0].track == 3);
}

TEST_CASE("midi_writer: bad input gives an error, not a file") {
    Song s = one_note_song();
    SUBCASE("zero ticks per quarter") {
        s.tempo.ticks_per_quarter = 0;
        CHECK_FALSE(write_midi(s).ok);
    }
    SUBCASE("more ticks per quarter than the header holds") {
        s.tempo.ticks_per_quarter = 40000;
        CHECK_FALSE(write_midi(s).ok);
    }
    SUBCASE("a denominator that is not a power of two") {
        s.time_signatures = {{0, 4, 3}};
        const MidiWriteResult w = write_midi(s);
        CHECK_FALSE(w.ok);
        CHECK_FALSE(w.message.empty());
        CHECK(w.bytes.empty());
    }
    SUBCASE("a tempo of zero or one a tempo event cannot hold") {
        s.tempo.changes[0].us_per_quarter = 0;
        CHECK_FALSE(write_midi(s).ok);
        s.tempo.changes[0].us_per_quarter = 0x1000000;
        CHECK_FALSE(write_midi(s).ok);
    }
    SUBCASE("a format other than 0 or 1") {
        MidiWriteOptions opt;
        opt.format = 2;
        CHECK_FALSE(write_midi(s, opt).ok);
    }
    SUBCASE("a track number the header cannot count") {
        s.notes[0].track = 65535;
        CHECK_FALSE(write_midi(s).ok);
    }
}

TEST_CASE("midi_writer: random songs round-trip: notes, tempo map, bars, parts, events") {
    for (std::uint64_t seed = 1; seed <= 60; ++seed) {
        CAPTURE(seed);
        const Song s = random_song(seed);
        MidiWriteOptions opt;
        opt.running_status = seed % 2 == 0;
        opt.format = 1;
        check_round_trip(s, opt);
    }
}

TEST_CASE("midi_writer: real files round-trip: Bach and Satie") {
    for (const char* name : {"bach-bwv846-prelude.mid", "satie-gymnopedie-1.mid"}) {
        CAPTURE(name);
        const Bytes original = read_test_file(name);
        const MidiLoadResult first = load_all(original);
        REQUIRE(first.ok);
        for (bool running : {false, true}) {
            MidiWriteOptions opt;
            opt.running_status = running;
            const MidiWriteResult w = write_midi(first.song, opt);
            REQUIRE(w.ok);
            CHECK(w.notes_skipped == 0);
            CHECK(w.notes_shortened == 0);
            const MidiLoadResult second = load_all(w.bytes);
            REQUIRE(second.ok);
            test::expect_same_song(first.song, second.song);
        }
    }
}

TEST_CASE("midi_writer: a real file written as format 0 keeps every note") {
    const MidiLoadResult first = load_all(read_test_file("bach-bwv846-prelude.mid"));
    REQUIRE(first.ok);
    MidiWriteOptions opt;
    opt.format = 0;
    const MidiWriteResult w = write_midi(first.song, opt);
    REQUIRE(w.ok);
    const MidiLoadResult second = load_all(w.bytes);
    REQUIRE(second.ok);
    REQUIRE(second.song.notes.size() == first.song.notes.size());
    for (std::size_t i = 0; i < first.song.notes.size(); ++i) {
        CHECK(second.song.notes[i].pitch == first.song.notes[i].pitch);
        CHECK(second.song.notes[i].start_tick == first.song.notes[i].start_tick);
        CHECK(second.song.notes[i].end_tick == first.song.notes[i].end_tick);
    }
    CHECK(second.song.measures.size() == first.song.measures.size());
}
