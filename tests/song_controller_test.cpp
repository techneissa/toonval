// SPDX-License-Identifier: GPL-3.0-or-later
// The app's song controller without SDL: actions in, transport, playback and evaluator state out,
// and the stuck-note rule over random action sequences.
#include "song_controller.hpp"
#include "song_library.hpp"

#include "toonval/song_builder.hpp"

#include <doctest/doctest.h>

#include <array>
#include <bitset>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

using namespace toonval;
using namespace toonval::app;

namespace {

constexpr std::int64_t kMs = 1'000'000;
constexpr std::int64_t kSec = 1'000'000'000;

// Two bars of 4/4 at 120 BPM (0.5 s a beat): the right hand plays quarter notes C5 D5 E5 F5 ...,
// the left hand holds a C3 per bar.
LoadedSong two_hand_song(int bars = 2) {
    Song s;
    s.tempo = make_tempo_map(480, 120.0);
    for (int bar = 0; bar < bars; ++bar) {
        for (int beat = 0; beat < 4; ++beat) {
            Note n;
            n.start_tick = (bar * 4 + beat) * 480;
            n.end_tick = n.start_tick + 400;
            n.pitch = static_cast<std::uint8_t>(72 + (beat * 2));
            n.velocity = 80;
            n.hand = Hand::Right;
            n.channel = 0;
            s.notes.push_back(n);
        }
        Note l;
        l.start_tick = bar * 4 * 480;
        l.end_tick = l.start_tick + 4 * 480 - 10;
        l.pitch = 48;
        l.velocity = 70;
        l.hand = Hand::Left;
        l.channel = 1;
        l.track = 1;
        s.notes.push_back(l);
    }
    REQUIRE(finish_song(s));
    LoadedSong out;
    out.song = std::move(s);
    out.title = "Test";
    out.hash = 0x1234;
    out.library_path = "test.mid";
    return out;
}

// A MIDI device as the piano sees it: what sounds after the bytes so far.
struct FakePiano {
    std::array<std::bitset<128>, 16> sounding{};
    std::bitset<16> pedal;
    int note_ons = 0;
    int note_offs = 0;

    void play(const PlaybackMessage& m) {
        REQUIRE(m.channel < 16);
        switch (m.kind) {
        case PlaybackMessage::Kind::NoteOn:
            if (m.value == 0) {
                sounding[m.channel].reset(m.data);
            } else {
                sounding[m.channel].set(m.data);
                ++note_ons;
            }
            break;
        case PlaybackMessage::Kind::NoteOff:
            sounding[m.channel].reset(m.data);
            ++note_offs;
            break;
        case PlaybackMessage::Kind::ControlChange:
            if (m.data == 64) {
                pedal.set(m.channel, m.value >= 64);
            } else if (m.data == 123 || m.data == 120) {
                sounding[m.channel].reset();
            }
            break;
        case PlaybackMessage::Kind::ProgramChange:
        case PlaybackMessage::Kind::PitchBend:
            break;
        }
    }
    std::size_t count() const {
        std::size_t n = 0;
        for (const auto& ch : sounding) {
            n += ch.count();
        }
        return n;
    }
    bool silent() const { return count() == 0 && pedal.none(); }
};

// Runs frames of ~16 ms from `now` to `until`, sending due messages to the piano.
void run(SongController& c, FakePiano& piano, std::int64_t& now, std::int64_t until,
         std::span<const MidiEvent> events = {}) {
    std::vector<PlaybackMessage> out;
    bool first = true;
    while (now < until) {
        now = std::min(until, now + 16 * kMs);
        c.update(now, first ? events : std::span<const MidiEvent>{});
        first = false;
        out.clear();
        c.take_due_messages(now, out);
        for (const PlaybackMessage& m : out) {
            piano.play(m);
        }
    }
}

void drain(SongController& c, FakePiano& piano, std::int64_t now) {
    std::vector<PlaybackMessage> out;
    c.take_due_messages(now, out);
    for (const PlaybackMessage& m : out) {
        piano.play(m);
    }
}

MidiEvent note_on(int key, std::int64_t at) {
    MidiEvent ev;
    ev.time_ns = at;
    ev.type = MidiEventType::NoteOn;
    ev.data1 = static_cast<std::uint8_t>(key);
    ev.data2 = 90;
    ev.source = InputSource::ComputerKeyboard;
    return ev;
}

MidiEvent note_off(int key, std::int64_t at) {
    MidiEvent ev = note_on(key, at);
    ev.type = MidiEventType::NoteOff;
    ev.data2 = 0;
    return ev;
}

} // namespace

TEST_CASE("a loaded song starts paused at 0 in Flow Mode, muted, and plays when asked") {
    SongController c;
    std::int64_t now = kSec;
    c.load(two_hand_song(), now);
    CHECK(c.has_song());
    CHECK(c.state() == TransportState::Paused);
    CHECK(c.position_s() == 0.0);
    CHECK(c.mode() == PracticeMode::Flow);
    CHECK(c.song_sound() == SongSound::Mute);
    FakePiano piano;
    c.toggle_play(now);
    run(c, piano, now, now + 1 * kSec);
    CHECK(c.position_s() == doctest::Approx(1.0).epsilon(0.02));
    CHECK(piano.note_ons == 0); // Mute: nothing of the song sounds
    c.toggle_play(now);
    CHECK(c.state() == TransportState::Paused);
    std::vector<PlaybackMessage> out;
    c.shutdown(now, out);
}

TEST_CASE("song sound: Mute plays nothing, Other hand the hand not practised, Both hands all") {
    const auto ons_by_channel = [](SongSound sound, HandFilter hands) {
        SongController c;
        std::int64_t now = kSec;
        c.load(two_hand_song(), now);
        c.set_practice_hands(hands, now);
        c.set_song_sound(sound, now);
        FakePiano piano;
        std::array<int, 2> ons{};
        c.play(now);
        std::vector<PlaybackMessage> out;
        for (int i = 0; i < 120; ++i) {
            now += 16 * kMs;
            c.update(now, {});
            out.clear();
            c.take_due_messages(now, out);
            for (const auto& m : out) {
                if (m.kind == PlaybackMessage::Kind::NoteOn && m.channel < 2) {
                    ++ons[m.channel];
                }
                piano.play(m);
            }
        }
        out.clear();
        c.shutdown(now, out);
        for (const auto& m : out) {
            piano.play(m);
        }
        CHECK(piano.silent());
        return ons; // channel 0 is the right hand, 1 the left
    };
    CHECK(ons_by_channel(SongSound::Mute, HandFilter::Both) == std::array<int, 2>{0, 0});
    CHECK(ons_by_channel(SongSound::OtherHand, HandFilter::Both) == std::array<int, 2>{0, 0});
    const auto other_of_right = ons_by_channel(SongSound::OtherHand, HandFilter::Right);
    CHECK(other_of_right[0] == 0);
    CHECK(other_of_right[1] > 0);
    const auto other_of_left = ons_by_channel(SongSound::OtherHand, HandFilter::Left);
    CHECK(other_of_left[0] > 0);
    CHECK(other_of_left[1] == 0);
    const auto both = ons_by_channel(SongSound::BothHands, HandFilter::Right);
    CHECK(both[0] > 0);
    CHECK(both[1] > 0);
}

TEST_CASE("skip back and forward step by their own seconds, stopping at 0, the loop start, the end") {
    SongController c;
    std::int64_t now = kSec;
    c.load(two_hand_song(8), now); // bars of 2 s, 16 s long
    CHECK(c.skip_back_seconds() == 5);
    CHECK(c.skip_forward_seconds() == 5);
    c.skip_forward(now);
    CHECK(c.position_s() == doctest::Approx(5.0));
    CHECK(c.bar() == 3);
    SongOptions o;
    o.skip_back_seconds = 2;
    o.skip_forward_seconds = 7;
    c.set_options(o, now);
    CHECK(c.skip_back_seconds() == 2);
    CHECK(c.skip_forward_seconds() == 7);
    c.skip_forward(now);
    CHECK(c.position_s() == doctest::Approx(12.0));
    c.skip_back(now);
    CHECK(c.position_s() == doctest::Approx(10.0));
    c.skip_forward(now); // 17 is past the end: stops at it
    CHECK(c.position_s() == doctest::Approx(c.duration_s()));
    c.seek(1.0, now);
    c.skip_back(now); // stops at 0
    CHECK(c.position_s() == 0.0);
    // Inside a loop, back stops at the loop start.
    REQUIRE(c.set_loop(4.0, 10.0, now));
    c.seek(5.0, now);
    c.skip_back(now);
    CHECK(c.position_s() == doctest::Approx(4.0));
    c.skip_back(now);
    CHECK(c.position_s() == doctest::Approx(4.0));
    // Before the loop, back goes on to 0.
    c.seek(1.5, now);
    c.skip_back(now);
    CHECK(c.position_s() == 0.0);
    c.clear_loop(now);

    // Every jump while the song sounds leaves the piano silent.
    c.set_song_sound(SongSound::BothHands, now);
    FakePiano piano;
    c.seek(2.0, now);
    c.play(now);
    run(c, piano, now, now + 700 * kMs);
    REQUIRE(piano.count() > 0);
    c.skip_forward(now);
    drain(c, piano, now);
    CHECK(piano.count() == 0);
    run(c, piano, now, now + 700 * kMs);
    REQUIRE(piano.count() > 0);
    c.skip_back(now);
    drain(c, piano, now);
    CHECK(piano.count() == 0);
    std::vector<PlaybackMessage> out;
    c.shutdown(now, out);
}

TEST_CASE("replay from the start or the loop start, loop at bars, and speed in steps of 5") {
    SongController c;
    std::int64_t now = kSec;
    c.load(two_hand_song(4), now); // bars of 2 s

    c.change_speed(5, now);
    CHECK(c.speed_percent() == 105);
    for (int i = 0; i < 40; ++i) {
        c.change_speed(-5, now);
    }
    CHECK(c.speed_percent() == 25);
    for (int i = 0; i < 40; ++i) {
        c.change_speed(5, now);
    }
    CHECK(c.speed_percent() == 150);

    c.seek(4.5, now);
    c.set_loop_start_here(now); // bar 3: 4..6 s
    CHECK(c.pending_loop_start() == doctest::Approx(4.0));
    CHECK_FALSE(c.loop_enabled());
    c.seek(5.2, now);
    c.set_loop_end_here(now);
    CHECK(c.loop_enabled());
    CHECK(c.loop_start_s() == doctest::Approx(4.0));
    CHECK(c.loop_end_s() == doctest::Approx(6.0));
    c.seek(5.9, now);
    c.replay(now);
    CHECK(c.position_s() == doctest::Approx(4.0));
    CHECK(c.playing());
    c.clear_loop(now);
    CHECK_FALSE(c.loop_enabled());
    c.replay(now);
    CHECK(c.position_s() == 0.0);
    std::vector<PlaybackMessage> out;
    c.shutdown(now, out);
}

TEST_CASE("Wait Mode holds at a chord, shows its keys, and moves on when they are played") {
    SongController c;
    std::int64_t now = kSec;
    c.load(two_hand_song(), now);
    c.set_mode(PracticeMode::Wait, now);
    CHECK(c.mode() == PracticeMode::Wait);
    FakePiano piano;
    c.play(now);
    run(c, piano, now, now + 500 * kMs);
    REQUIRE(c.waiting());
    CHECK(c.position_s() == doctest::Approx(0.0).epsilon(0.001));
    auto keys = c.waiting_keys();
    REQUIRE(keys.size() == 2);
    CHECK(keys[0].key == 48);
    CHECK(keys[0].hand == Hand::Left);
    CHECK(keys[1].key == 72);
    CHECK(keys[1].hand == Hand::Right);
    // Both keys down: the song goes on and waits at the next note, D5 at 0.5 s.
    const std::array<MidiEvent, 2> chord{note_on(48, now), note_on(72, now)};
    run(c, piano, now, now + 300 * kMs, chord);
    CHECK(c.position_s() > 0.0);
    run(c, piano, now, now + 800 * kMs);
    REQUIRE(c.waiting());
    CHECK(c.position_s() == doctest::Approx(0.5).epsilon(0.001));
    keys = c.waiting_keys();
    REQUIRE(keys.size() == 1);
    CHECK(keys[0].key == 74);
    // Practising the left hand: only its notes stop the song.
    c.set_practice_hands(HandFilter::Left, now);
    run(c, piano, now, now + 200 * kMs);
    const std::array<MidiEvent, 2> up{note_off(48, now), note_off(72, now)};
    run(c, piano, now, now + 2 * kSec, up);
    REQUIRE(c.waiting());
    CHECK(c.position_s() == doctest::Approx(2.0).epsilon(0.001));
    keys = c.waiting_keys();
    REQUIRE(keys.size() == 1);
    CHECK(keys[0].key == 48);
    std::vector<PlaybackMessage> out;
    c.shutdown(now, out);
}

TEST_CASE("Flow Mode grades notes as they pass, counts the combo, and a finished song is a run") {
    SongController c;
    std::int64_t now = kSec;
    c.load(two_hand_song(), now);
    c.set_practice_hands(HandFilter::Right, now);
    c.play(now);
    const std::int64_t start = now;
    // Press each right-hand note on time except the third (E5 at 1.0 s).
    std::vector<std::pair<std::int64_t, MidiEvent>> script;
    for (int i = 0; i < 8; ++i) {
        if (i == 2) {
            continue;
        }
        const std::int64_t t = start + i * 500 * kMs;
        const int key = 72 + (i % 4) * 2;
        script.push_back({t, note_on(key, t)});
        script.push_back({t + 200 * kMs, note_off(key, t + 200 * kMs)});
    }
    std::size_t next = 0;
    std::vector<MidiEvent> frame;
    std::vector<PlaybackMessage> out;
    int max_grades_seen = 0;
    while (c.state() != TransportState::Ended && now < start + 10 * kSec) {
        now += 16 * kMs;
        frame.clear();
        while (next < script.size() && script[next].first <= now) {
            frame.push_back(script[next++].second);
        }
        c.update(now, frame);
        max_grades_seen = std::max(max_grades_seen, static_cast<int>(c.recent_grades().size()));
        out.clear();
        c.take_due_messages(now, out);
    }
    CHECK(c.state() == TransportState::Ended);
    CHECK(max_grades_seen > 0);
    const auto runs = c.take_finished_runs();
    REQUIRE(runs.size() == 1);
    const RunRecord& r = runs[0].record;
    CHECK(r.mode == RunMode::Flow);
    CHECK(r.hands == HandFilter::Right);
    CHECK(r.song_hash == 0x1234);
    CHECK(r.right.graded == 8);
    CHECK(r.right.hit == 7);
    CHECK(r.counts.missed == 1);
    CHECK(r.longest_combo == 5);
    CHECK(r.left.graded == 0);
    CHECK(r.duration_ms > 3500);
    // The results line names the hand and the combo.
    const std::string line = results_line(r, compare_with_last_attempt({}, r));
    CHECK(line.find("right 88%") != std::string::npos);
    CHECK(line.find("longest combo 5") != std::string::npos);
    c.shutdown(now, out);
}

TEST_CASE("opening the settings pauses and silences; closing counts down with ticks, then plays") {
    SongController c;
    std::int64_t now = kSec;
    c.load(two_hand_song(4), now);
    c.set_song_sound(SongSound::BothHands, now);
    SongOptions o;
    o.resume_countdown_s = 3;
    c.set_options(o, now);
    FakePiano piano;
    c.play(now);
    run(c, piano, now, now + 600 * kMs);
    CHECK(piano.count() > 0);
    c.menu_opened(now);
    drain(c, piano, now);
    CHECK(c.state() == TransportState::Paused);
    CHECK(piano.silent());
    const double paused_at = c.position_s();
    // A seek while the menu is open still resumes through the countdown.
    c.seek(2.0, now);
    run(c, piano, now, now + 1 * kSec);
    c.menu_closed(now);
    CHECK(c.counting_down());
    CHECK(c.countdown() == 3);
    int ticks = 0;
    std::vector<PlaybackMessage> out;
    const std::int64_t closed = now;
    while (now < closed + 2900 * kMs) {
        now += 16 * kMs;
        c.update(now, {});
        out.clear();
        c.take_due_messages(now, out);
        for (const auto& m : out) {
            if (m.kind == PlaybackMessage::Kind::NoteOn && m.channel == kTickChannel) {
                ++ticks;
            }
            piano.play(m);
        }
        CHECK_FALSE(c.playing());
    }
    CHECK(ticks == 3);
    CHECK(c.countdown() == 1);
    CHECK(c.position_s() == doctest::Approx(2.0)); // the seek, not where it paused
    CHECK(paused_at < 2.0);
    run(c, piano, now, now + 300 * kMs);
    CHECK(c.playing());
    CHECK_FALSE(c.counting_down());

    // Closing a menu opened while paused does not start anything.
    c.pause(now);
    c.menu_opened(now);
    c.menu_closed(now);
    CHECK_FALSE(c.counting_down());
    out.clear();
    c.shutdown(now, out);
}

TEST_CASE("the session keeps the song's place and choices, and a restored one comes back") {
    SongController c;
    std::int64_t now = kSec;
    c.load(two_hand_song(4), now);
    c.set_mode(PracticeMode::Wait, now);
    c.set_practice_hands(HandFilter::Left, now);
    c.set_song_sound(SongSound::OtherHand, now);
    c.set_speed(70, now);
    c.set_transpose(-2, now);
    c.seek(2.5, now);
    c.set_loop_start_here(now);
    c.set_loop_end_here(now);
    c.seek(3.0, now);
    const SessionState s = c.session_state(SessionState{});
    CHECK(s.song_path == "test.mid");
    CHECK(s.song_hash == 0x1234);
    CHECK(s.position_s == doctest::Approx(3.0));
    CHECK(s.mode == PracticeMode::Wait);
    CHECK(s.practice_hands == HandFilter::Left);
    CHECK(s.piano_plays == PianoPlays::OtherHand);
    CHECK(s.speed_percent == 70);
    CHECK(s.transpose_semitones == -2);
    CHECK(s.loop_enabled);
    CHECK(s.loop_start_s == doctest::Approx(2.0));
    CHECK(s.loop_end_s == doctest::Approx(4.0));
    std::vector<PlaybackMessage> out;
    c.shutdown(now, out);

    SongController d;
    d.load(two_hand_song(4), now);
    d.apply_session(s, now);
    CHECK(d.session_state(SessionState{}) == s);
    CHECK(d.state() == TransportState::Paused);
    d.shutdown(now, out);

    // A song from outside the library is played but not saved.
    LoadedSong outside = two_hand_song();
    outside.library_path.clear();
    SongController e;
    e.load(std::move(outside), now);
    CHECK(e.session_state(s).song_path.empty());
    e.shutdown(now, out);
}

TEST_CASE("a real song: Bach's prelude loads, splits its hands and plays to the end") {
    const SongFileLoad load =
        load_song_file(std::string(TOONVAL_TEST_DATA_DIR) + "/bach-bwv846-prelude.mid");
    REQUIRE(load.ok);
    CHECK(load.song.hash != 0);
    CHECK_FALSE(load.song.title.empty());
    for (const Note& n : load.song.song.notes) {
        if (!n.drum) {
            REQUIRE(n.hand != Hand::Unknown);
        }
    }
    SongController c;
    std::int64_t now = kSec;
    c.load(load.song, now);
    CHECK(c.bar_count() >= 35);
    std::vector<VisibleNote> notes;
    c.visible_notes(4.0, notes);
    CHECK_FALSE(notes.empty());
    REQUIRE(c.key_range().has_value());
    c.set_song_sound(SongSound::BothHands, now);
    c.set_speed(150, now);
    FakePiano piano;
    c.play(now);
    run(c, piano, now, now + 5 * kSec);
    CHECK(piano.note_ons > 20);
    c.seek(c.duration_s() - 0.5, now);
    run(c, piano, now, now + 2 * kSec);
    CHECK(c.state() == TransportState::Ended);
    CHECK(piano.silent());
    std::vector<PlaybackMessage> out;
    c.shutdown(now, out);
}

TEST_CASE("a file that is not a song gives one line, and nothing loads") {
    const std::vector<std::uint8_t> junk{'h', 'e', 'l', 'l', 'o'};
    const SongFileLoad load = load_song_bytes(junk, "notes.mid");
    CHECK_FALSE(load.ok);
    CHECK_FALSE(load.error.empty());
    CHECK(load.error.find('\n') == std::string::npos);
    const SongFileLoad missing = load_song_file("/nonexistent/x.mid");
    CHECK_FALSE(missing.ok);
    CHECK(missing.error.find("x.mid") != std::string::npos);
}

TEST_CASE("the library finds the test songs, sorted by title, with relative paths") {
    SongLibrary lib({std::string(TOONVAL_TEST_DATA_DIR)});
    CHECK(lib.scan() >= 4);
    const auto songs = lib.songs();
    for (std::size_t i = 1; i < songs.size(); ++i) {
        CHECK(songs[i - 1].entry.title_key <= songs[i].entry.title_key);
    }
    for (const LibrarySong& s : songs) {
        CHECK(lib.resolve(s.relative) == s.path);
        CHECK(lib.relative_path(s.path) == s.relative);
    }
    CHECK(lib.relative_path("/etc/hosts").empty());
    CHECK_FALSE(lib.resolve("../tests/data/bach-bwv846-prelude.mid").has_value());
    CHECK_FALSE(lib.resolve("/etc/hosts").has_value());
    CHECK(lib.step(std::nullopt, 1) == std::size_t{0});
    CHECK(lib.step(std::size_t{0}, -1) == songs.size() - 1);
    CHECK(lib.step(songs.size() - 1, 1) == std::size_t{0});
}

// The stuck-note rule, over random action sequences: whatever the player does, every note the
// song starts is stopped by a later note-off, and after a pause, a seek, a song change, a menu or
// a lost focus nothing of the song is left sounding.
TEST_CASE("no action sequence leaves a note sounding (property)") {
    std::mt19937 rng(20261012);
    const LoadedSong songs[2] = {two_hand_song(6), two_hand_song(3)};
    int total_note_ons = 0;
    int silent_checks = 0;
    for (int trial = 0; trial < 300; ++trial) {
        SongController c;
        FakePiano piano;
        std::int64_t now = kSec;
        c.load(songs[0], now);
        c.set_song_sound(SongSound::BothHands, now);
        SongOptions o;
        o.resume_countdown_s = 1;
        c.set_options(o, now);
        bool menu = false;
        for (int step = 0; step < 60; ++step) {
            const int action = static_cast<int>(rng() % 22);
            bool must_be_silent = false;
            switch (action) {
            case 0:
            case 1:
            case 2:
                c.toggle_play(now);
                must_be_silent = !c.playing() && !c.counting_down();
                break;
            case 3:
                c.pause(now);
                must_be_silent = true;
                break;
            case 4:
                c.seek(std::uniform_real_distribution<double>(-1.0, 14.0)(rng), now);
                must_be_silent = true;
                break;
            case 5:
                c.skip_back(now);
                must_be_silent = true;
                break;
            case 6:
                c.skip_forward(now);
                must_be_silent = true;
                break;
            case 7:
                c.replay(now);
                break;
            case 8:
                c.change_speed(rng() % 2 == 0 ? 25 : -25, now);
                break;
            case 9:
                c.set_loop_start_here(now);
                break;
            case 10:
                c.set_loop_end_here(now);
                break;
            case 11:
                c.clear_loop(now);
                break;
            case 12:
                c.set_practice_hands(static_cast<HandFilter>(rng() % 3), now);
                break;
            case 13:
                c.cycle_song_sound(now);
                break;
            case 14:
                c.toggle_mode(now);
                break;
            case 15:
                c.set_transpose(static_cast<int>(rng() % 9) - 4, now);
                break;
            case 16:
                if (menu) {
                    c.menu_closed(now);
                } else {
                    c.menu_opened(now);
                    must_be_silent = true;
                }
                menu = !menu;
                break;
            case 17:
                c.lost_focus(now);
                must_be_silent = true;
                break;
            case 18:
                c.load(songs[rng() % 2], now);
                must_be_silent = true;
                break;
            case 19:
                c.set_keyboard(rng() % 2 == 0 ? KeyRange{36, 96} : KeyRange{21, 108}, now);
                break;
            case 20: {
                const std::array<MidiEvent, 2> presses{note_on(72, now), note_on(48, now)};
                run(c, piano, now, now + 50 * kMs, presses);
                break;
            }
            default:
                break;
            }
            drain(c, piano, now);
            if (must_be_silent) {
                // Messages may wait a few ms behind a tick's note-off; let them out.
                drain(c, piano, now + kTickLengthNs);
                INFO("trial " << trial << " step " << step << " action " << action);
                CHECK(piano.count() == 0);
                ++silent_checks;
            }
            run(c, piano, now, now + static_cast<std::int64_t>(rng() % 900) * kMs);
        }
        std::vector<PlaybackMessage> out;
        c.shutdown(now, out);
        for (const auto& m : out) {
            piano.play(m);
        }
        INFO("trial " << trial);
        total_note_ons += piano.note_ons;
        CHECK(piano.silent());
        CHECK_FALSE(c.needs_silence());
        CHECK(c.queued_messages() == 0);
    }
    // The sequences did make the song sound, and did stop it often.
    CHECK(total_note_ons > 1000);
    CHECK(silent_checks > 3000);
}

TEST_CASE("after a seek while playing, the song sounds again from the new place") {
    SongController c;
    std::int64_t now = kSec;
    c.load(two_hand_song(4), now);
    c.set_song_sound(SongSound::BothHands, now);
    FakePiano piano;
    c.play(now);
    run(c, piano, now, now + 1 * kSec);
    const int before = piano.note_ons;
    CHECK(before > 0);
    c.skip_forward(now);
    drain(c, piano, now);
    CHECK(piano.count() == 0);
    run(c, piano, now, now + 1 * kSec);
    CHECK(c.playing());
    CHECK(piano.note_ons > before + 1);
    std::vector<PlaybackMessage> out;
    c.shutdown(now, out);
}

TEST_CASE("a run where nothing was played is not saved") {
    SongController c;
    std::int64_t now = kSec;
    c.load(two_hand_song(), now);
    FakePiano piano;
    c.play(now);
    run(c, piano, now, now + 6 * kSec);
    CHECK(c.state() == TransportState::Ended);
    CHECK(c.take_finished_runs().empty());
    std::vector<PlaybackMessage> out;
    c.shutdown(now, out);
    CHECK(c.take_finished_runs().empty());
}

namespace {

// Right C5 (channel 0), left C3 (1), a note with no hand (2) and a drum hit (9), all at once and
// again each second for four seconds.
LoadedSong mixed_hand_song() {
    Song s;
    s.tempo = make_tempo_map(480, 120.0);
    const auto add = [&](int tick, int pitch, Hand hand, std::uint8_t channel) {
        Note n;
        n.start_tick = tick;
        n.end_tick = tick + 200;
        n.pitch = static_cast<std::uint8_t>(pitch);
        n.velocity = 80;
        n.hand = hand;
        n.channel = channel;
        n.track = channel;
        s.notes.push_back(n);
    };
    for (int beat = 0; beat < 8; beat += 2) {
        add(beat * 480, 72, Hand::Right, 0);
        add(beat * 480, 48, Hand::Left, 1);
        add(beat * 480, 60, Hand::Unknown, 2);
        add(beat * 480, 38, Hand::Unknown, 9);
    }
    REQUIRE(finish_song(s));
    LoadedSong out;
    out.song = std::move(s);
    out.title = "Mixed";
    out.hash = 0x77;
    out.library_path = "mixed.mid";
    return out;
}

// Note-ons per channel (0, 1, 2 and 9 as slots 0..3) after playing the whole song.
std::array<int, 4> ons_of_mixed(SongSound sound, HandFilter hands) {
    SongController c;
    std::int64_t now = kSec;
    c.load(mixed_hand_song(), now);
    c.set_practice_hands(hands, now);
    c.set_song_sound(sound, now);
    std::array<int, 4> ons{};
    c.play(now);
    std::vector<PlaybackMessage> out;
    for (int i = 0; i < 250; ++i) {
        now += 16 * kMs;
        c.update(now, {});
        out.clear();
        c.take_due_messages(now, out);
        for (const auto& m : out) {
            if (m.kind == PlaybackMessage::Kind::NoteOn && m.value != 0) {
                const int slot = m.channel == 9 ? 3 : (m.channel < 3 ? m.channel : -1);
                if (slot >= 0) {
                    ++ons[static_cast<std::size_t>(slot)];
                }
            }
        }
    }
    return ons;
}

} // namespace

TEST_CASE("Other hand: drums always sound; a note with no hand sounds unless it is graded") {
    // Both hands practised: the evaluator grades the hand-less notes, so they are the player's
    // and stay muted, along with both hands. Drums are not the player's: they sound.
    const auto both = ons_of_mixed(SongSound::OtherHand, HandFilter::Both);
    CHECK(both[0] == 0);
    CHECK(both[1] == 0);
    CHECK(both[2] == 0);
    CHECK(both[3] > 0);
    // One hand practised: the evaluator does not grade hand-less notes, so they sound.
    const auto right = ons_of_mixed(SongSound::OtherHand, HandFilter::Right);
    CHECK(right[0] == 0);
    CHECK(right[1] > 0);
    CHECK(right[2] > 0);
    CHECK(right[3] > 0);
    const auto left = ons_of_mixed(SongSound::OtherHand, HandFilter::Left);
    CHECK(left[0] > 0);
    CHECK(left[1] == 0);
    CHECK(left[2] > 0);
    CHECK(left[3] > 0);
    // Both hands sound: everything. Mute: nothing.
    const auto all = ons_of_mixed(SongSound::BothHands, HandFilter::Right);
    CHECK((all[0] > 0 && all[1] > 0 && all[2] > 0 && all[3] > 0));
    CHECK(ons_of_mixed(SongSound::Mute, HandFilter::Both) == std::array<int, 4>{0, 0, 0, 0});
}

TEST_CASE("unload and silence leave no note sounding on the piano") {
    SongController c;
    std::int64_t now = kSec;
    c.load(two_hand_song(4), now);
    c.set_song_sound(SongSound::BothHands, now);
    FakePiano piano;
    c.play(now);
    run(c, piano, now, now + 700 * kMs);
    REQUIRE(piano.count() > 0);

    c.silence(now);
    drain(c, piano, now);
    CHECK(piano.silent());
    CHECK_FALSE(c.needs_silence());

    run(c, piano, now, now + 700 * kMs); // still playing: sounds again
    c.unload(now);
    drain(c, piano, now);
    CHECK(piano.silent());
    CHECK_FALSE(c.needs_silence());
    CHECK_FALSE(c.has_song());
    CHECK(c.status_line().empty());
    CHECK(c.grade_of(0) == -1);
    run(c, piano, now, now + 500 * kMs);
    CHECK(piano.silent());
    c.unload(now); // twice is harmless
}

TEST_CASE("transpose is clamped, shows in transposition() and the status line, and notes stay silent-safe") {
    SongController c;
    std::int64_t now = kSec;
    c.load(two_hand_song(), now);
    CHECK(c.transpose() == 0);
    CHECK(c.transposition().semitones() == 0);
    c.set_transpose(3, now);
    CHECK(c.transpose() == 3);
    CHECK(c.transposition().semitones() == 3);
    CHECK(c.status_line().find("+3 st") != std::string::npos);
    c.set_transpose(-5, now);
    CHECK(c.status_line().find("-5 st") != std::string::npos);
    c.set_transpose(99, now);
    CHECK(c.transpose() == kMaxTransposeSemitones);
    c.set_transpose(-99, now);
    CHECK(c.transpose() == -kMaxTransposeSemitones);
    c.set_transpose(0, now);
    CHECK(c.status_line().find(" st") == std::string::npos);
}

TEST_CASE("menu_open follows menu_opened and menu_closed; closing twice does nothing") {
    SongController c;
    std::int64_t now = kSec;
    c.load(two_hand_song(), now);
    CHECK_FALSE(c.menu_open());
    c.menu_opened(now);
    CHECK(c.menu_open());
    c.menu_closed(now);
    CHECK_FALSE(c.menu_open());
    c.menu_closed(now);
    CHECK_FALSE(c.menu_open());
    CHECK_FALSE(c.counting_down()); // it was paused when the menu opened
}

TEST_CASE("grade_of is -1 until a note is final, then its grade; out of range is -1") {
    SongController c;
    std::int64_t now = kSec;
    c.load(two_hand_song(), now);
    CHECK(c.grade_of(0) == -1);
    CHECK(c.grade_of(100000) == -1);
    c.play(now);
    FakePiano piano;
    run(c, piano, now, now + 2500 * kMs); // nobody plays: notes that passed are missed
    int missed = 0;
    for (std::uint32_t i = 0; i < c.song().song.notes.size(); ++i) {
        if (c.grade_of(i) == static_cast<int>(NoteGrade::Missed)) {
            ++missed;
        }
    }
    CHECK(missed > 0);
    CHECK(c.grade_of(100000) == -1);
}

TEST_CASE("status_line names the song, mode, hands, speed, bar, sound and state") {
    SongController c;
    CHECK(c.status_line().empty());
    std::int64_t now = kSec;
    c.load(two_hand_song(), now);
    c.set_practice_hands(HandFilter::Left, now);
    c.set_song_sound(SongSound::BothHands, now);
    const std::string line = c.status_line();
    CHECK(line.find("Test") == 0);
    CHECK(line.find("Flow") != std::string::npos);
    CHECK(line.find("Left hand") != std::string::npos);
    CHECK(line.find("100%") != std::string::npos);
    CHECK(line.find("Bar 1/2") != std::string::npos);
    CHECK(line.find("Sound: Both hands") != std::string::npos);
    CHECK(line.find("Paused") != std::string::npos);
    CHECK(line.find("Combo 0") != std::string::npos);
    c.set_mode(PracticeMode::Wait, now);
    CHECK(c.status_line().find("Wait") != std::string::npos);
    CHECK(c.status_line().find("Combo") == std::string::npos);
}

TEST_CASE("the name helpers give the shown names and round-trip the saved choices") {
    CHECK(std::string(song_sound_name(SongSound::Mute)) == "Mute");
    CHECK(std::string(song_sound_name(SongSound::OtherHand)) == "Other hand");
    CHECK(std::string(song_sound_name(SongSound::BothHands)) == "Both hands");
    CHECK(std::string(hands_name(HandFilter::Left)) == "Left hand");
    CHECK(std::string(hands_name(HandFilter::Right)) == "Right hand");
    CHECK(std::string(hands_name(HandFilter::Both)) == "Both hands");
    CHECK(std::string(mode_name(PracticeMode::Flow)) == "Flow");
    CHECK(std::string(mode_name(PracticeMode::Wait)) == "Wait");
    CHECK(std::string(mode_name(PracticeMode::FreePlay)) == "Free play");
    for (const SongSound s : {SongSound::Mute, SongSound::OtherHand, SongSound::BothHands}) {
        CHECK(song_sound_from(to_piano_plays(s)) == s);
    }
    CHECK(to_piano_plays(SongSound::Mute) == PianoPlays::Nothing);
    CHECK(to_piano_plays(SongSound::BothHands) == PianoPlays::Both);
    // Older saved choices read as Other hand.
    CHECK(song_sound_from(PianoPlays::Left) == SongSound::OtherHand);
    CHECK(song_sound_from(PianoPlays::Right) == SongSound::OtherHand);
}
