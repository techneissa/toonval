// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/evaluator.hpp"

#include "toonval/keyboard.hpp"
#include "toonval/transport.hpp"
#include "toonval/transpose.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <random>
#include <vector>

using namespace toonval;
using OpKind = TransportOp::Kind;

namespace {

constexpr std::int64_t sec(double s) { return static_cast<std::int64_t>(std::llround(s * 1e9)); }
constexpr std::int64_t ms(double m) { return static_cast<std::int64_t>(std::llround(m * 1e6)); }

struct N {
    double start;
    int pitch;
    Hand hand = Hand::Right;
    double dur = 0.25;
};

// Bars of 2 s each (4/4 at 120 BPM), so a note's bar is floor(start / 2) + 1.
Song song_of(const std::vector<N>& list, double tail_s = 2.0) {
    Song s;
    double end = 0.0;
    for (const N& n : list) {
        Note note;
        note.start_s = n.start;
        note.duration_s = n.dur;
        note.pitch = static_cast<std::uint8_t>(n.pitch);
        note.hand = n.hand;
        note.measure = static_cast<int>(std::floor(n.start / 2.0)) + 1;
        note.start_tick = static_cast<std::int64_t>(std::llround(n.start * 960.0));
        note.end_tick = note.start_tick + std::max<std::int64_t>(1, std::llround(n.dur * 960.0));
        s.notes.push_back(note);
        end = std::max(end, n.start + n.dur);
    }
    std::stable_sort(s.notes.begin(), s.notes.end(), [](const Note& a, const Note& b) {
        return a.start_s < b.start_s || (a.start_s == b.start_s && a.pitch < b.pitch);
    });
    s.duration_s = end + tail_s;
    for (int bar = 0; bar * 2.0 < s.duration_s; ++bar) {
        Measure m;
        m.number = bar + 1;
        m.start_s = bar * 2.0;
        m.start_tick = bar * 1920;
        s.measures.push_back(m);
    }
    return s;
}

Song song_of(std::initializer_list<N> list, double tail_s = 2.0) {
    return song_of(std::vector<N>(list), tail_s);
}

MidiEvent note_on(double wall_s, int key, int vel = 90) {
    MidiEvent e;
    e.time_ns = sec(wall_s);
    e.type = MidiEventType::NoteOn;
    e.data1 = static_cast<std::uint8_t>(key);
    e.data2 = static_cast<std::uint8_t>(vel);
    return e;
}

[[maybe_unused]] MidiEvent note_off(double wall_s, int key) {
    MidiEvent e;
    e.time_ns = sec(wall_s);
    e.type = MidiEventType::NoteOff;
    e.data1 = static_cast<std::uint8_t>(key);
    return e;
}

[[maybe_unused]] MidiEvent cc(double wall_s, int number, int value) {
    MidiEvent e;
    e.time_ns = sec(wall_s);
    e.type = MidiEventType::ControlChange;
    e.data1 = static_cast<std::uint8_t>(number);
    e.data2 = static_cast<std::uint8_t>(value);
    return e;
}

// A Transport and an evaluator driven like the app's frame loop: each frame updates the clock,
// feeds its ops, then the MIDI events stamped up to now, then updates the evaluator. Play starts
// at wall time 1 s, so at 100% a note at song time s arrives at wall time 1 + s.
template <typename Evaluator, typename Options> struct Rig {
    Song song;
    Transport transport;
    Evaluator eval;
    std::vector<MidiEvent> inbox; // sorted by stamp before each frame
    std::int64_t now = 0;
    std::int64_t frame = 16'666'667;
    std::vector<WaitDirective> directives;

    Rig(const Song& s, const Options& o) : song(s) {
        transport.set_song(song);
        eval.set_song(song, o);
    }

    void send(const MidiEvent& e) { inbox.push_back(e); }
    void play_at(double wall_s, int speed = 100) {
        transport.set_speed_percent(sec(wall_s), speed);
        transport.play(sec(wall_s));
        now = sec(wall_s);
        flush_ops();
    }
    void flush_ops() {
        std::vector<TransportOp> ops;
        transport.take_ops(ops);
        for (const TransportOp& op : ops) {
            eval.apply(op);
        }
    }
    void step(std::int64_t to) {
        now = to;
        transport.update(now);
        flush_ops();
        std::stable_sort(inbox.begin(), inbox.end(), [](const MidiEvent& a, const MidiEvent& b) {
            return a.time_ns < b.time_ns;
        });
        std::size_t used = 0;
        while (used < inbox.size() && inbox[used].time_ns <= now) {
            eval.apply(inbox[used]);
            ++used;
        }
        inbox.erase(inbox.begin(), inbox.begin() + static_cast<std::ptrdiff_t>(used));
        after_update();
    }
    void after_update();
    void run_until(double wall_s) {
        const std::int64_t end = sec(wall_s);
        while (now + frame <= end) {
            step(now + frame);
        }
        if (now < end) {
            step(end);
        }
    }
};

using FlowRig = Rig<FlowEvaluator, FlowOptions>;
using WaitRig = Rig<WaitEvaluator, WaitOptions>;

template <> void FlowRig::after_update() { eval.update(now); }

template <> void WaitRig::after_update() {
    const WaitDirective d = eval.update(now);
    directives.push_back(d);
    if (d.hold_now) {
        transport.hold(now);
    }
    if (d.release) {
        transport.release(now);
    }
    if (d.arm_hold) {
        transport.hold_at(now, d.hold_at_s);
    }
    flush_ops();
}

const NoteResult* result_for(const EvaluationSummary& s, std::uint32_t note, int pass = -1) {
    for (const NoteResult& r : s.notes) {
        if (r.note_index == note && (pass < 0 || r.pass == pass)) {
            return &r;
        }
    }
    return nullptr;
}

[[maybe_unused]] int hits(const EvaluationSummary& s) {
    return s.counts.perfect + s.counts.good + s.counts.early + s.counts.late;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// TransportTimeline

TEST_CASE("timeline: a press is placed in the song through the moves, at any speed") {
    TransportTimeline tl;
    TransportOp move;
    move.kind = OpKind::Move;
    move.at_ns = sec(10);
    move.end_ns = sec(12);
    move.from_s = 0.0;
    move.to_s = 1.0;
    move.speed_percent = 50;
    tl.apply(move);
    TransportOp hold;
    hold.kind = OpKind::Hold;
    hold.at_ns = sec(12);
    hold.to_s = 1.0;
    tl.apply(hold);
    CHECK(tl.song_at(sec(11)) == doctest::Approx(0.5));
    CHECK(tl.song_at(sec(15)) == doctest::Approx(1.0)); // held
    CHECK(tl.song_at(sec(5)) == doctest::Approx(0.0));  // before the first op
    CHECK(tl.rate_at(sec(11)) == doctest::Approx(0.5));
}

// ---------------------------------------------------------------------------------------------
// Flow Mode: grades

TEST_CASE("flow: a press on time is Perfect and the combo counts it") {
    FlowRig rig(song_of({{1.0, 60}}), FlowOptions{});
    rig.play_at(1.0);
    rig.send(note_on(2.0, 60));
    rig.run_until(4.0);
    const EvaluationSummary& s = rig.eval.summary();
    REQUIRE(s.notes.size() == 1);
    CHECK(s.notes[0].grade == NoteGrade::Perfect);
    CHECK(s.notes[0].arrival_ns == sec(2.0));
    CHECK(s.counts.perfect == 1);
    CHECK(s.combo == 1);
    CHECK(s.longest_combo == 1);
    CHECK(s.wrong == 0);
}

TEST_CASE("flow: grades follow the window: Perfect, Good, Early, Late and Missed") {
    // Window 80 ms: Perfect within 28 ms, Good within 56 ms, Early/Late within 80 ms.
    FlowRig rig(song_of({{1.0, 60}, {2.0, 62}, {3.0, 64}, {4.0, 65}, {5.0, 67}, {6.0, 69}}),
                FlowOptions{});
    rig.play_at(1.0);
    rig.send(note_on(2.0 + 0.020, 60)); // Perfect
    rig.send(note_on(3.0 - 0.050, 62)); // Good (early side)
    rig.send(note_on(4.0 - 0.070, 64)); // Early
    rig.send(note_on(5.0 + 0.070, 65)); // Late
    rig.send(note_on(6.0 + 0.200, 67)); // too late: Missed, and a wrong press
    rig.run_until(9.0);
    const EvaluationSummary& s = rig.eval.summary();
    REQUIRE(s.notes.size() == 6);
    CHECK(result_for(s, 0)->grade == NoteGrade::Perfect);
    CHECK(result_for(s, 1)->grade == NoteGrade::Good);
    CHECK(result_for(s, 2)->grade == NoteGrade::Early);
    CHECK(result_for(s, 3)->grade == NoteGrade::Late);
    CHECK(result_for(s, 3)->error_ns == ms(70));
    CHECK(result_for(s, 4)->grade == NoteGrade::Missed);
    CHECK(result_for(s, 5)->grade == NoteGrade::Missed);
    CHECK(s.wrong == 1);
    CHECK(s.counts.missed == 2);
    CHECK(s.longest_combo == 4);
    CHECK(s.combo == 0);
}

TEST_CASE("flow: the window edge is inside, one nanosecond past it is outside") {
    FlowOptions o;
    o.window_ms = 80.0;
    FlowRig rig(song_of({{1.0, 60}, {2.0, 62}}), o);
    rig.play_at(1.0);
    rig.send(note_on(2.0 + 0.080, 60));
    MidiEvent past = note_on(3.0, 62);
    past.time_ns = sec(3.0) - ms(80) - 1;
    rig.send(past);
    MidiEvent late = note_on(2.0, 60);
    late.time_ns = sec(2.0) + ms(80) + 1; // one past the late edge of the first note: wrong
    rig.send(late);
    rig.run_until(5.0);
    const EvaluationSummary& s = rig.eval.summary();
    CHECK(result_for(s, 0)->grade == NoteGrade::Late);
    CHECK(result_for(s, 1)->grade == NoteGrade::Missed);
    CHECK(s.wrong == 1); // the early 62 is pressed early for its note, never wrong (F5)
}

TEST_CASE("flow: the window is adjustable and the thresholds scale with it") {
    FlowOptions o;
    o.window_ms = 200.0; // Perfect within 70 ms, Good within 140 ms
    FlowRig rig(song_of({{1.0, 60}, {2.0, 62}}), o);
    rig.play_at(1.0);
    rig.send(note_on(2.0 + 0.060, 60));
    rig.send(note_on(3.0 - 0.150, 62));
    rig.run_until(5.0);
    const EvaluationSummary& s = rig.eval.summary();
    CHECK(result_for(s, 0)->grade == NoteGrade::Perfect);
    CHECK(result_for(s, 1)->grade == NoteGrade::Early);
}

TEST_CASE("flow: latency correction is subtracted from each press before grading") {
    FlowOptions o;
    o.latency_ms = 60.0;
    FlowRig rig(song_of({{1.0, 60}}), o);
    rig.play_at(1.0);
    rig.send(note_on(2.0 + 0.060, 60)); // the player's lag: really on time
    rig.run_until(4.0);
    const EvaluationSummary& s = rig.eval.summary();
    CHECK(result_for(s, 0)->grade == NoteGrade::Perfect);
    CHECK(result_for(s, 0)->error_ns == 0);
}

// ---------------------------------------------------------------------------------------------
// Flow Mode: matching

TEST_CASE("flow: the assignment maximises matches where nearest-first would lose one") {
    // Same key at song 1.0 and 1.1. Presses 70 ms and 170 ms after the first: nearest-first
    // gives the first press to the second note and leaves the first note missed.
    FlowRig rig(song_of({{1.0, 60}, {1.1, 60}}), FlowOptions{});
    rig.play_at(1.0);
    rig.send(note_on(2.070, 60));
    rig.send(note_on(2.170, 60));
    rig.run_until(4.0);
    const EvaluationSummary& s = rig.eval.summary();
    CHECK(result_for(s, 0)->grade == NoteGrade::Late);
    CHECK(result_for(s, 1)->grade == NoteGrade::Late);
    CHECK(s.counts.missed == 0);
    CHECK(s.wrong == 0);
}

TEST_CASE("flow: an extra press near a note never steals it, the closer press grades it") {
    FlowRig rig(song_of({{1.0, 60}}), FlowOptions{});
    rig.play_at(1.0);
    rig.send(note_on(2.0 - 0.070, 60)); // extra, early
    rig.send(note_on(2.0 + 0.005, 60));
    rig.run_until(4.0);
    const EvaluationSummary& s = rig.eval.summary();
    CHECK(result_for(s, 0)->grade == NoteGrade::Perfect);
    CHECK(result_for(s, 0)->error_ns == ms(5));
    CHECK(s.wrong == 0); // the extra press is the right key, early: not wrong (F5)
    CHECK(s.longest_combo == 1);
}

TEST_CASE("flow: an extra press after a note is wrong and breaks the combo") {
    FlowRig rig(song_of({{1.0, 60}}), FlowOptions{});
    rig.play_at(1.0);
    rig.send(note_on(2.0 + 0.005, 60));
    rig.send(note_on(2.0 + 0.070, 60)); // extra, late
    rig.run_until(4.0);
    const EvaluationSummary& s = rig.eval.summary();
    CHECK(result_for(s, 0)->grade == NoteGrade::Perfect);
    CHECK(s.wrong == 1); // after the note made the combo 1
    CHECK(s.longest_combo == 1);
    CHECK(s.combo == 0);
}

TEST_CASE("flow: a wrong key is counted apart and resets the combo") {
    FlowRig rig(song_of({{1.0, 60}, {2.0, 62}, {3.0, 64}}), FlowOptions{});
    rig.play_at(1.0);
    rig.send(note_on(2.0, 60));
    rig.send(note_on(2.5, 61)); // matches nothing
    rig.send(note_on(3.0, 62));
    rig.send(note_on(4.0, 64));
    rig.run_until(6.0);
    const EvaluationSummary& s = rig.eval.summary();
    CHECK(hits(s) == 3);
    CHECK(s.wrong == 1);
    CHECK(s.longest_combo == 2);
    CHECK(s.combo == 2);
}

TEST_CASE("flow: a chord rolled inside the window is all hits") {
    FlowRig rig(song_of({{1.0, 60}, {1.0, 64}, {1.0, 67}, {1.0, 72}}), FlowOptions{});
    rig.play_at(1.0);
    rig.send(note_on(2.0 - 0.060, 72)); // rolled top down
    rig.send(note_on(2.0 - 0.020, 67));
    rig.send(note_on(2.0 + 0.025, 64));
    rig.send(note_on(2.0 + 0.075, 60));
    rig.run_until(4.0);
    const EvaluationSummary& s = rig.eval.summary();
    CHECK(hits(s) == 4);
    CHECK(s.counts.missed == 0);
    CHECK(s.wrong == 0);
    CHECK(result_for(s, 0)->grade == NoteGrade::Late);  // 60, +75 ms
    CHECK(result_for(s, 3)->grade == NoteGrade::Early); // 72, -60 ms
}

TEST_CASE("flow: a fast trill of repeated keys at 150% speed matches every note") {
    // 60 and 62 alternating every 100 ms of song: at 150% each key repeats every 133 ms of wall
    // time, under two windows apart, so every press could reach two notes of its key.
    FlowRig rig(song_of({{1.0, 60},
                         {1.1, 62},
                         {1.2, 60},
                         {1.3, 62},
                         {1.4, 60},
                         {1.5, 62},
                         {1.6, 60},
                         {1.7, 62}}),
                FlowOptions{});
    rig.play_at(1.0, 150);
    const double jitter[] = {0.030, -0.035, 0.040, -0.030, 0.035, -0.040, 0.030, -0.025};
    const double starts[] = {1.0, 1.1, 1.2, 1.3, 1.4, 1.5, 1.6, 1.7};
    for (int i = 0; i < 8; ++i) {
        rig.send(note_on(1.0 + starts[i] / 1.5 + jitter[i], i % 2 == 0 ? 60 : 62));
    }
    rig.run_until(4.0);
    const EvaluationSummary& s = rig.eval.summary();
    CHECK(hits(s) == 8);
    CHECK(s.counts.missed == 0);
    CHECK(s.wrong == 0);
    for (std::uint32_t i = 0; i < 8; ++i) {
        CHECK(std::llabs(result_for(s, i)->error_ns - ms(jitter[i] * 1000.0)) <= 2);
    }
}

TEST_CASE("flow: both hands on one key at once take one press and both are hits") {
    FlowRig rig(song_of({{1.0, 60, Hand::Left}, {1.0, 60, Hand::Right}}), FlowOptions{});
    rig.play_at(1.0);
    rig.send(note_on(2.010, 60));
    rig.run_until(4.0);
    const EvaluationSummary& s = rig.eval.summary();
    REQUIRE(s.notes.size() == 2);
    CHECK(s.notes[0].grade == NoteGrade::Perfect);
    CHECK(s.notes[1].grade == NoteGrade::Perfect);
    CHECK(s.left.hit == 1);
    CHECK(s.right.hit == 1);
    CHECK(s.wrong == 0);
}

TEST_CASE("flow: only the practised hand is graded; the piano's notes never are") {
    FlowOptions o;
    o.hands = HandFilter::Right;
    FlowRig rig(song_of({{1.0, 60, Hand::Right}, {1.0, 48, Hand::Left}, {2.0, 50, Hand::Unknown}}),
                o);
    rig.play_at(1.0);
    rig.send(note_on(2.0, 60));
    rig.run_until(5.0);
    const EvaluationSummary& s = rig.eval.summary();
    REQUIRE(s.notes.size() == 1);
    CHECK(s.notes[0].note_index == 1); // the right-hand C4 (sorted after the left-hand C3)
    CHECK(s.counts.missed == 0);
    CHECK(s.left.graded == 0);
    CHECK(s.right.accuracy() == 1.0);
}

TEST_CASE("flow: drum notes are never graded") {
    Song song = song_of({{1.0, 60}, {1.5, 38}});
    song.notes[1].drum = true;
    FlowRig rig(song, FlowOptions{});
    rig.play_at(1.0);
    rig.send(note_on(2.0, 60));
    rig.run_until(4.0);
    CHECK(rig.eval.summary().notes.size() == 1);
    CHECK(rig.eval.summary().counts.missed == 0);
}

TEST_CASE("flow: the expected key comes from the transposition") {
    FlowOptions o;
    o.transposition = Transposition(2, kRange61);
    FlowRig rig(song_of({{1.0, 60}, {2.0, 95}}), o); // 95 + 2 = 97 folds down to 85
    rig.play_at(1.0);
    rig.send(note_on(2.0, 60)); // the untransposed key is wrong
    rig.send(note_on(2.0, 62));
    rig.send(note_on(3.0, 85));
    rig.run_until(5.0);
    const EvaluationSummary& s = rig.eval.summary();
    CHECK(result_for(s, 0)->grade == NoteGrade::Perfect);
    CHECK(result_for(s, 0)->expected_pitch == 62);
    CHECK(result_for(s, 1)->grade == NoteGrade::Perfect);
    CHECK(result_for(s, 1)->expected_pitch == 85);
    CHECK(s.wrong == 1);
}

TEST_CASE("flow: an unplayable note is neither hit nor missed, and is counted") {
    FlowOptions o;
    o.transposition = Transposition(0, KeyRange{60, 64}); // narrower than an octave
    FlowRig rig(song_of({{1.0, 60}, {2.0, 66}}), o);
    rig.play_at(1.0);
    rig.send(note_on(2.0, 60));
    rig.run_until(5.0);
    const EvaluationSummary& s = rig.eval.summary();
    CHECK(s.notes.size() == 1);
    CHECK(s.counts.missed == 0);
    CHECK(s.unplayable == 1);
}

TEST_CASE("flow: a speed change mid-song keeps arrival times exact") {
    FlowRig rig(song_of({{1.0, 60}, {3.0, 62}}), FlowOptions{});
    rig.play_at(1.0);
    rig.send(note_on(2.0, 60));
    rig.run_until(3.0); // song 2.0
    rig.transport.set_speed_percent(sec(3.0), 50);
    // Song 3.0 is one song second later at half speed: wall 5.0.
    rig.send(note_on(5.0 + 0.010, 62));
    rig.run_until(7.0);
    const EvaluationSummary& s = rig.eval.summary();
    CHECK(result_for(s, 1)->arrival_ns == sec(5.0));
    CHECK(result_for(s, 1)->grade == NoteGrade::Perfect);
    CHECK(s.counts.missed == 0);
}

TEST_CASE("flow: events stamped out of order across ports by a few ms grade the same") {
    FlowRig rig(song_of({{1.0, 60}, {1.0, 64}, {1.05, 60}}), FlowOptions{});
    rig.play_at(1.0);
    rig.run_until(2.06);
    // Port B's 64 (stamped 2.004) and second 60 (stamped 2.052) are delivered after port A's
    // later stamps.
    rig.eval.apply(note_on(2.001, 60));
    rig.eval.apply(note_on(2.055, 60));
    rig.eval.apply(note_on(2.004, 64));
    rig.eval.apply(note_on(2.052, 60));
    rig.run_until(4.0);
    const EvaluationSummary& s = rig.eval.summary();
    CHECK(s.counts.missed == 0);
    CHECK(s.wrong == 1);
    CHECK(result_for(s, 0)->error_ns == ms(1));
    CHECK(result_for(s, 2)->error_ns == ms(2)); // the 2.052 press, not the later 2.055
}

TEST_CASE("flow: releases, the pedal, velocity-0 note-ons and synthetic events are not presses") {
    FlowRig rig(song_of({{1.0, 60}}), FlowOptions{});
    rig.play_at(1.0);
    rig.send(note_on(2.0, 60, 0));
    rig.send(note_off(2.01, 60));
    rig.send(cc(2.02, kCcSustain, 127));
    MidiEvent synth = cc(2.03, kCcAllNotesOff, 0);
    synth.synthetic = true;
    rig.send(synth);
    rig.run_until(4.0);
    const EvaluationSummary& s = rig.eval.summary();
    CHECK(s.counts.missed == 1);
    CHECK(s.wrong == 0);
}

TEST_CASE("flow: presses while paused or before play are ignored") {
    FlowRig rig(song_of({{1.0, 60}, {3.0, 62}}), FlowOptions{});
    rig.send(note_on(0.5, 70)); // before play
    rig.play_at(1.0);
    rig.send(note_on(2.0, 60));
    rig.run_until(2.5);
    rig.transport.pause(sec(2.5));
    rig.send(note_on(3.0, 71)); // noodling while paused
    rig.run_until(4.0);
    rig.play_at(4.0);
    rig.send(note_on(5.5, 62)); // song 3.0 at wall 4 + 1.5
    rig.run_until(7.0);
    const EvaluationSummary& s = rig.eval.summary();
    CHECK(s.wrong == 0);
    CHECK(hits(s) == 2);
}

TEST_CASE("flow: a seek drops undecided notes and presses, neither hit nor missed") {
    FlowRig rig(song_of({{1.0, 60}, {5.0, 62}}), FlowOptions{});
    rig.play_at(1.0);
    rig.run_until(2.02); // note 0 arrived at 2.0, its window still open
    rig.transport.seek(sec(2.02), 4.5);
    rig.send(note_on(2.03, 60)); // stamped after the seek: the old note is gone, so wrong
    rig.send(note_on(2.52, 62)); // song 5.0 arrives at 2.02 + 0.5
    rig.run_until(4.0);
    const EvaluationSummary& s = rig.eval.summary();
    CHECK(result_for(s, 0) == nullptr);
    CHECK(result_for(s, 1)->grade == NoteGrade::Perfect);
    CHECK(s.counts.missed == 0);
    CHECK(s.wrong == 1);
}

TEST_CASE("flow: per-hand accuracy, grade counts and the per-bar miss map") {
    // Bars are 2 s: notes at 1.0 and 1.5 are bar 1, 3.0 is bar 2, 5.0 is bar 3.
    FlowRig rig(song_of({{1.0, 60, Hand::Right},
                         {1.5, 48, Hand::Left},
                         {3.0, 62, Hand::Right},
                         {5.0, 64, Hand::Right}}),
                FlowOptions{});
    rig.play_at(1.0);
    rig.send(note_on(2.0, 60));
    rig.send(note_on(2.5, 48));
    rig.send(note_on(4.5, 70)); // wrong, song 3.5: bar 2
    rig.send(note_on(6.0, 64));
    rig.run_until(8.0);
    const EvaluationSummary& s = rig.eval.summary();
    CHECK(s.right.graded == 3);
    CHECK(s.right.hit == 2);
    CHECK(s.right.accuracy() == doctest::Approx(2.0 / 3.0));
    CHECK(s.left.accuracy() == 1.0);
    CHECK(s.all.graded == 4);
    CHECK(s.counts.perfect == 3);
    CHECK(s.counts.missed == 1);
    REQUIRE(s.measures.size() == 3);
    CHECK(s.measures[0].measure == 1);
    CHECK(s.measures[0].graded == 2);
    CHECK(s.measures[0].missed == 0);
    CHECK(s.measures[1].measure == 2);
    CHECK(s.measures[1].missed == 1);
    CHECK(s.measures[1].wrong == 1);
    CHECK(s.measures[2].missed == 0);
}

TEST_CASE("flow: loop wraps start a new pass; a clean pass is reported for auto speed-up") {
    FlowRig rig(song_of({{0.5, 60}, {1.5, 62}, {3.0, 64}}), FlowOptions{});
    REQUIRE(rig.transport.set_loop(0, 0.0, 2.0));
    rig.transport.set_loop_enabled(0, true);
    rig.play_at(1.0);
    // Pass 0: wall 1..3, both notes. Pass 1: wall 3..5, misses the second note. Pass 2: 5..7.
    rig.send(note_on(1.5, 60));
    rig.send(note_on(2.5, 62));
    rig.send(note_on(3.5, 60));
    rig.send(note_on(5.5, 60));
    rig.send(note_on(6.5 + 0.075, 62)); // late, still a hit
    rig.run_until(7.3);
    std::vector<PassResult> passes;
    rig.eval.take_passes(passes);
    REQUIRE(passes.size() == 3);
    CHECK(passes[0].pass == 0);
    CHECK(passes[0].clean);
    CHECK(passes[0].graded == 2);
    CHECK(passes[1].pass == 1);
    CHECK_FALSE(passes[1].clean);
    CHECK(passes[1].missed == 1);
    CHECK(passes[2].clean);
    CHECK(passes[2].start_s == 0.0);
    CHECK(passes[2].end_s == 2.0);
    CHECK(result_for(rig.eval.summary(), 1, 2)->grade == NoteGrade::Late);
    passes.clear();
    rig.eval.take_passes(passes);
    CHECK(passes.empty()); // each pass is handed out once
}

TEST_CASE("flow: a pass that began inside the loop is never clean") {
    FlowRig rig(song_of({{0.5, 60}, {1.5, 62}}), FlowOptions{});
    REQUIRE(rig.transport.set_loop(0, 0.0, 2.0));
    rig.transport.set_loop_enabled(0, true);
    rig.transport.seek(0, 1.0);
    rig.play_at(1.0);
    rig.send(note_on(1.5, 62));
    rig.run_until(2.5);
    std::vector<PassResult> passes;
    rig.eval.take_passes(passes);
    REQUIRE(passes.size() == 1);
    CHECK(passes[0].missed == 0);
    CHECK_FALSE(passes[0].clean);
}

TEST_CASE("flow: finish decides everything still open") {
    FlowRig rig(song_of({{1.0, 60}}), FlowOptions{});
    rig.play_at(1.0);
    rig.run_until(2.01);
    CHECK(rig.eval.pending_notes() == 1);
    rig.eval.finish();
    CHECK(rig.eval.summary().counts.missed == 1);
    CHECK(rig.eval.pending_notes() == 0);
}

// ---------------------------------------------------------------------------------------------
// Wait Mode

TEST_CASE("wait: the song holds at a chord and goes on when all its keys are down at once") {
    WaitRig rig(song_of({{1.0, 60}, {1.0, 64}, {1.0, 67}, {2.0, 72}}), WaitOptions{});
    rig.play_at(1.0);
    rig.run_until(2.5);
    CHECK(rig.transport.held());
    CHECK(rig.transport.position_s() == 1.0);
    CHECK(rig.eval.waiting_for_s() == 1.0);
    CHECK(rig.eval.waiting_keys() == std::vector<int>{60, 64, 67});
    rig.send(note_on(2.5, 60));
    rig.send(note_on(2.6, 64));
    rig.run_until(2.8);
    CHECK(rig.transport.held()); // two of three
    rig.send(note_on(2.8, 67));
    rig.run_until(2.9);
    CHECK_FALSE(rig.transport.held());
    CHECK(rig.transport.position_s() > 1.0);
    const EvaluationSummary& s = rig.eval.summary();
    REQUIRE(s.notes.size() == 3);
    CHECK(s.counts.played == 3);
    CHECK(s.notes[0].grade == NoteGrade::Played);
    CHECK(s.notes[0].arrival_ns == sec(2.0));
    CHECK(s.notes[0].error_ns == ms(800)); // how long the song waited
    CHECK(rig.eval.waiting_for_s() == 2.0);
}

TEST_CASE("wait: a chord note let go before the last one lands does not count") {
    WaitRig rig(song_of({{1.0, 60}, {1.0, 64}, {3.0, 72}}), WaitOptions{});
    rig.play_at(1.0);
    rig.run_until(2.5);
    rig.send(note_on(2.5, 60));
    rig.send(note_off(2.6, 60));
    rig.send(note_on(2.7, 64));
    rig.run_until(3.0);
    CHECK(rig.transport.held());
    rig.send(note_on(3.0, 60));
    rig.run_until(3.1);
    CHECK_FALSE(rig.transport.held());
    CHECK(rig.eval.summary().wrong == 0);
}

TEST_CASE("wait: a key already held does not count until it is pressed again") {
    WaitRig rig(song_of({{1.0, 60}, {2.0, 60}, {4.0, 72}}), WaitOptions{});
    rig.play_at(1.0);
    rig.run_until(2.5);
    rig.send(note_on(2.5, 60)); // plays the first chord and stays down
    rig.run_until(4.5);
    CHECK(rig.transport.held());
    CHECK(rig.transport.position_s() == 2.0);
    rig.send(note_off(4.5, 60));
    rig.send(note_on(4.6, 60));
    rig.run_until(4.7);
    CHECK_FALSE(rig.transport.held());
    CHECK(rig.eval.summary().counts.played == 2);
}

TEST_CASE("wait: a key pressed before play does not play the first chord") {
    WaitRig rig(song_of({{1.0, 60}, {3.0, 72}}), WaitOptions{});
    rig.send(note_on(0.5, 60));
    rig.play_at(1.0);
    rig.run_until(3.0);
    CHECK(rig.transport.held());
    CHECK(rig.eval.summary().counts.played == 0);
}

TEST_CASE("wait: a press slightly before the chord arrives counts and the song never stops") {
    WaitOptions o;
    o.early_ms = 150.0;
    WaitRig rig(song_of({{1.0, 60}, {3.0, 72}}), o);
    rig.play_at(1.0);
    rig.send(note_on(2.0 - 0.100, 60));
    rig.run_until(2.7);
    CHECK_FALSE(rig.transport.held());
    CHECK(rig.transport.position_s() == doctest::Approx(1.7));
    const EvaluationSummary& s = rig.eval.summary();
    REQUIRE(s.notes.size() == 1);
    CHECK(s.notes[0].error_ns == 0);
}

TEST_CASE("wait: a press too early does not count, and is not wrong either") {
    WaitOptions o;
    o.early_ms = 150.0;
    WaitRig rig(song_of({{1.0, 60}, {3.0, 72}}), o);
    rig.play_at(1.0);
    rig.send(note_on(2.0 - 0.300, 60));
    rig.run_until(2.5);
    CHECK(rig.transport.held());
    CHECK(rig.transport.position_s() == 1.0);
    CHECK(rig.eval.summary().wrong == 0);
}

TEST_CASE("wait: wrong notes are counted in the waiting chord's bar and never block it") {
    WaitRig rig(song_of({{3.0, 60}, {5.0, 72}}), WaitOptions{});
    rig.play_at(1.0);
    rig.run_until(4.5);
    rig.send(note_on(4.5, 61));
    rig.send(note_on(4.6, 60)); // 61 is still down
    rig.run_until(4.7);
    CHECK_FALSE(rig.transport.held());
    const EvaluationSummary& s = rig.eval.summary();
    CHECK(s.wrong == 1);
    REQUIRE_FALSE(s.measures.empty());
    CHECK(s.measures[0].measure == 2);
    CHECK(s.measures[0].wrong == 1);
}

TEST_CASE("wait: the sustain pedal never holds a key") {
    WaitRig rig(song_of({{1.0, 60}, {1.0, 64}, {3.0, 72}}), WaitOptions{});
    rig.play_at(1.0);
    rig.run_until(2.5);
    rig.send(cc(2.5, kCcSustain, 127));
    rig.send(note_on(2.6, 60));
    rig.send(note_off(2.7, 60)); // the pedal keeps it sounding, not held
    rig.send(note_on(2.8, 64));
    rig.run_until(3.0);
    CHECK(rig.transport.held());
}

TEST_CASE("wait: a velocity-0 note-on is a release") {
    WaitRig rig(song_of({{1.0, 60}, {1.0, 64}, {3.0, 72}}), WaitOptions{});
    rig.play_at(1.0);
    rig.run_until(2.5);
    rig.send(note_on(2.6, 60));
    rig.send(note_on(2.7, 60, 0));
    rig.send(note_on(2.8, 64));
    rig.run_until(3.0);
    CHECK(rig.transport.held());
}

TEST_CASE("wait: All Notes Off releases every key") {
    WaitRig rig(song_of({{1.0, 60}, {1.0, 64}, {3.0, 72}}), WaitOptions{});
    rig.play_at(1.0);
    rig.run_until(2.5);
    rig.send(note_on(2.6, 60));
    MidiEvent gap = cc(2.7, kCcAllNotesOff, 0);
    gap.synthetic = true;
    rig.send(gap);
    rig.send(note_on(2.8, 64));
    rig.run_until(3.0);
    CHECK(rig.transport.held());
}

TEST_CASE("wait: out-of-order stamps across ports: a release stamped before a press counts") {
    WaitRig rig(song_of({{1.0, 60}, {1.0, 64}, {3.0, 72}}), WaitOptions{});
    rig.play_at(1.0);
    rig.run_until(2.5);
    rig.send(note_on(2.5, 60));
    rig.run_until(2.6);
    // Port B's press of 64 (stamped 2.605) is delivered before port A's release of 60 (stamped
    // 2.600): the two keys were never down together.
    rig.eval.apply(note_on(2.605, 64));
    rig.eval.apply(note_off(2.600, 60));
    rig.run_until(2.8);
    CHECK(rig.transport.held());
    CHECK(rig.eval.summary().counts.played == 0);
}

TEST_CASE("wait: both hands on one key are one press") {
    WaitRig rig(song_of({{1.0, 60, Hand::Left}, {1.0, 60, Hand::Right}, {3.0, 72}}), WaitOptions{});
    rig.play_at(1.0);
    rig.run_until(2.5);
    CHECK(rig.eval.waiting_keys() == std::vector<int>{60});
    rig.send(note_on(2.5, 60));
    rig.run_until(2.6);
    CHECK_FALSE(rig.transport.held());
    CHECK(rig.eval.summary().counts.played == 2);
}

TEST_CASE("wait: only the practised hand is waited for") {
    WaitOptions o;
    o.hands = HandFilter::Left;
    WaitRig rig(song_of({{1.0, 72, Hand::Right}, {2.0, 48, Hand::Left}, {4.0, 50, Hand::Left}}), o);
    rig.play_at(1.0);
    rig.run_until(3.5);
    CHECK(rig.transport.held());
    CHECK(rig.transport.position_s() == 2.0);
}

TEST_CASE("wait: with hold_full_length, letting go early holds the song until pressed again") {
    WaitOptions o;
    o.hold_full_length = true;
    WaitRig rig(song_of({{1.0, 60, Hand::Right, 1.0}, {4.0, 72}}), o);
    rig.play_at(1.0);
    rig.run_until(2.5);
    rig.send(note_on(2.5, 60));
    rig.send(note_off(2.8, 60)); // song 1.3, the note ends at 2.0
    rig.run_until(3.0);
    CHECK(rig.transport.held());
    CHECK(rig.transport.position_s() == doctest::Approx(1.3).epsilon(0.02));
    REQUIRE(rig.eval.summary().notes.size() == 1);
    CHECK(rig.eval.summary().notes[0].released_early);
    rig.send(note_on(3.0, 60));
    rig.run_until(3.2);
    CHECK_FALSE(rig.transport.held());
    rig.send(note_off(3.0 + 0.7 - 0.05, 60)); // within release_slack_ms of the end: fine
    rig.run_until(4.5);
    CHECK_FALSE(rig.transport.held());
}

TEST_CASE("wait: without hold_full_length a short press is enough") {
    WaitRig rig(song_of({{1.0, 60, Hand::Right, 1.0}, {4.0, 72}}), WaitOptions{});
    rig.play_at(1.0);
    rig.run_until(2.5);
    rig.send(note_on(2.5, 60));
    rig.send(note_off(2.6, 60));
    rig.run_until(4.0);
    CHECK_FALSE(rig.transport.held());
    CHECK_FALSE(rig.eval.summary().notes[0].released_early);
}

TEST_CASE("wait: a seek waits for the first chord after it and keys down must be pressed again") {
    WaitRig rig(song_of({{1.0, 60}, {2.0, 64}, {3.0, 60}, {5.0, 72}}), WaitOptions{});
    rig.play_at(1.0);
    rig.run_until(2.5);
    rig.send(note_on(2.5, 60)); // plays bar 1's chord, stays down
    rig.run_until(2.6);
    rig.transport.seek(sec(2.6), 2.5);
    rig.run_until(4.0);
    CHECK(rig.eval.waiting_for_s() == 3.0);
    CHECK(rig.transport.held());
    CHECK(rig.transport.position_s() == 3.0);
    rig.send(note_off(4.0, 60));
    rig.send(note_on(4.1, 60));
    rig.run_until(4.2);
    CHECK_FALSE(rig.transport.held());
}

TEST_CASE("wait: each loop pass waits again from the loop start and reports clean passes") {
    WaitRig rig(song_of({{0.0, 60}, {1.0, 62}, {3.0, 64}}), WaitOptions{});
    REQUIRE(rig.transport.set_loop(0, 0.0, 2.0));
    rig.transport.set_loop_enabled(0, true);
    rig.play_at(1.0);
    rig.run_until(1.2);
    CHECK(rig.transport.held()); // the chord at the loop start holds at once
    rig.send(note_on(1.2, 60));
    rig.send(note_off(1.3, 60));
    rig.run_until(2.5);
    rig.send(note_on(2.5, 62));
    rig.send(note_off(2.6, 62));
    rig.run_until(3.7); // played at 2.515, so the wrap came at 3.515
    CHECK(rig.transport.loop_count() == 1);
    CHECK(rig.transport.held());
    CHECK(rig.transport.position_s() == doctest::Approx(0.0).epsilon(0.02));
    rig.send(note_on(3.7, 61)); // wrong in pass 1
    rig.send(note_on(3.8, 60));
    rig.send(note_off(3.9, 60));
    rig.run_until(4.9);
    rig.send(note_on(5.0, 62));
    rig.send(note_off(5.1, 62));
    rig.run_until(6.3);
    std::vector<PassResult> passes;
    rig.eval.take_passes(passes);
    REQUIRE(passes.size() == 2);
    CHECK(passes[0].clean);
    CHECK(passes[0].graded == 2);
    CHECK_FALSE(passes[1].clean);
    CHECK(passes[1].wrong == 1);
}

// ---------------------------------------------------------------------------------------------
// Properties

namespace {

// A random song: single notes, chords of 2 to 4 keys, quick repeats of one key, trills, and both
// hands on one key at once. Two notes on one key either start together or at least 60 ms apart,
// as a real player needs to strike a key twice.
std::vector<N> random_song(std::mt19937& rng, int groups) {
    std::uniform_real_distribution<double> gap(0.05, 0.4);
    std::uniform_real_distribution<double> dur(0.05, 0.5);
    std::uniform_int_distribution<int> pitch(40, 90);
    std::uniform_int_distribution<int> pattern(0, 4);
    std::uniform_int_distribution<int> hand(0, 2);
    const Hand hands[] = {Hand::Left, Hand::Right, Hand::Unknown};
    std::vector<N> out;
    std::array<double, 128> last{};
    last.fill(-1.0);
    const auto add = [&](double t, int p, Hand h) {
        if (last[static_cast<std::size_t>(p)] >= 0.0 && t != last[static_cast<std::size_t>(p)] &&
            t - last[static_cast<std::size_t>(p)] < 0.060) {
            return;
        }
        last[static_cast<std::size_t>(p)] = t;
        out.push_back({t, p, h, dur(rng)});
    };
    double t = 0.5;
    for (int g = 0; g < groups; ++g) {
        const Hand h = hands[hand(rng)];
        const int p = pitch(rng);
        switch (pattern(rng)) {
        case 0:
            add(t, p, h);
            break;
        case 1: {
            std::uniform_int_distribution<int> size(2, 4);
            const int n = size(rng);
            for (int i = 0; i < n; ++i) {
                add(t, std::min(127, p + i * 4), h);
            }
            break;
        }
        case 2: // quick repeats
            add(t, p, h);
            add(t + 0.08, p, h);
            add(t + 0.16, p, h);
            t += 0.16;
            break;
        case 3: // trill
            for (int i = 0; i < 6; ++i) {
                add(t + i * 0.07, i % 2 == 0 ? p : p + 2, h);
            }
            t += 0.35;
            break;
        case 4: // both hands on one key
            add(t, p, Hand::Left);
            add(t, p, Hand::Right);
            break;
        }
        t += gap(rng);
    }
    return out;
}

struct Strike {
    double wall_s;
    int key;
};

// One press per key and start: what a perfect player plays, with jitter inside the Perfect window
// and chords struck in random order.
std::vector<Strike> perfect_strikes(std::mt19937& rng, const std::vector<N>& song, int semitones,
                                    double rate, double max_jitter_s) {
    std::uniform_real_distribution<double> jitter(-max_jitter_s, max_jitter_s);
    std::vector<Strike> out;
    std::vector<std::pair<double, int>> seen;
    for (const N& n : song) {
        const std::pair<double, int> id{n.start, n.pitch};
        if (std::find(seen.begin(), seen.end(), id) != seen.end()) {
            continue;
        }
        seen.push_back(id);
        out.push_back({1.0 + n.start / rate + jitter(rng), n.pitch + semitones});
    }
    std::shuffle(out.begin(), out.end(), rng);
    return out;
}

} // namespace

TEST_CASE("property: a perfect player on random songs gets every note Perfect and no wrong") {
    for (unsigned seed = 1; seed <= 40; ++seed) {
        CAPTURE(seed);
        std::mt19937 rng(seed);
        const std::vector<N> notes = random_song(rng, 60);
        const int speeds[] = {25, 50, 75, 100, 125, 150};
        const int speed = speeds[seed % 6];
        const int semitones = static_cast<int>(seed % 25) - 12;
        FlowOptions o;
        // Pitches 40..102 shifted by up to an octave stay inside 0..127, so nothing folds.
        o.transposition = Transposition(semitones, KeyRange{0, 127});
        FlowRig rig(song_of(notes), o);
        rig.play_at(1.0, speed);
        // Inside the Perfect window (28 ms) with room for rounding.
        for (const Strike& st : perfect_strikes(rng, notes, semitones, speed / 100.0, 0.025)) {
            rig.send(note_on(st.wall_s, st.key));
        }
        rig.run_until(1.0 + rig.song.duration_s * 100.0 / speed + 1.0);
        rig.eval.finish();
        const EvaluationSummary& s = rig.eval.summary();
        CHECK(s.notes.size() == notes.size());
        CHECK(s.counts.perfect == static_cast<int>(notes.size()));
        CHECK(s.wrong == 0);
        CHECK(s.longest_combo == static_cast<int>(notes.size()));
        CHECK(rig.eval.pending_notes() == 0);
        CHECK(rig.eval.pending_presses() == 0);
    }
}

TEST_CASE("property: a sloppy player's extra notes never reduce the correct count") {
    for (unsigned seed = 1; seed <= 40; ++seed) {
        CAPTURE(seed);
        std::mt19937 rng(seed * 7919u);
        const std::vector<N> notes = random_song(rng, 60);
        const int speed = 50 + static_cast<int>(seed % 11) * 10;
        const double rate = speed / 100.0;
        FlowRig rig(song_of(notes), FlowOptions{});
        rig.play_at(1.0, speed);
        // Every note struck anywhere inside the window, plus extra presses: near misses on the
        // right key, neighbours, and random keys at random times.
        std::vector<Strike> strikes = perfect_strikes(rng, notes, 0, rate, 0.079);
        const std::size_t real = strikes.size();
        std::uniform_int_distribution<std::size_t> pick(0, real - 1);
        std::uniform_real_distribution<double> near(-0.079, 0.079);
        std::uniform_int_distribution<int> any_key(30, 100);
        std::uniform_real_distribution<double> any_time(1.0, 1.0 + rig.song.duration_s / rate);
        for (std::size_t i = 0; i < real / 2; ++i) {
            const Strike base = strikes[pick(rng)]; // a copy: push_back moves the vector
            strikes.push_back({base.wall_s + near(rng), base.key});
            strikes.push_back({base.wall_s + near(rng), base.key + 1});
            strikes.push_back({any_time(rng), any_key(rng)});
        }
        // Keys pressed early for a later note (up to 3.5 song seconds ahead, inside the 4 s
        // lookahead) where no note of that key is within a window: never a match, never wrong.
        int early = 0;
        std::uniform_real_distribution<double> ahead(0.2, 3.5);
        for (std::size_t i = 0; i < real / 4; ++i) {
            const N& n = notes[pick(rng) % notes.size()];
            const double song_s = n.start - ahead(rng);
            const bool near_same_key = std::any_of(notes.begin(), notes.end(), [&](const N& m) {
                return m.pitch == n.pitch && std::abs(m.start - song_s) <= 0.080 * rate + 0.01;
            });
            if (song_s < 0.01 || near_same_key) {
                continue;
            }
            strikes.push_back({1.0 + song_s / rate, n.pitch});
            ++early;
        }
        // Random keys no note of the song uses are always wrong.
        int foreign = 0;
        for (std::size_t i = real; i < strikes.size(); ++i) {
            const int key = strikes[i].key;
            if (std::none_of(notes.begin(), notes.end(),
                             [&](const N& m) { return m.pitch == key; })) {
                ++foreign;
            }
        }
        const int extras = static_cast<int>(strikes.size() - real);
        for (const Strike& st : strikes) {
            rig.send(note_on(st.wall_s, st.key));
        }
        rig.run_until(1.0 + rig.song.duration_s / rate + 1.0);
        rig.eval.finish();
        const EvaluationSummary& s = rig.eval.summary();
        CHECK(s.notes.size() == notes.size());
        CHECK(s.counts.missed == 0);
        CHECK(hits(s) == static_cast<int>(notes.size()));
        CHECK(early > 0);
        CHECK(s.wrong <= extras - early);
        CHECK(s.wrong >= foreign);
    }
}

TEST_CASE("property: a Wait Mode player who plays each chord when it stops never gets stuck") {
    for (unsigned seed = 1; seed <= 15; ++seed) {
        CAPTURE(seed);
        std::mt19937 rng(seed * 104729u);
        const std::vector<N> notes = random_song(rng, 30);
        WaitRig rig(song_of(notes), WaitOptions{});
        rig.play_at(1.0, 50 + static_cast<int>(seed % 3) * 50);
        std::vector<int> down;
        std::uniform_real_distribution<double> roll(0.0, 0.040);
        std::bernoulli_distribution lose(0.3); // a key-up lost on the way
        std::bernoulli_distribution peek(0.3); // a key of the next chord pressed early
        double struck = -1.0;                  // the chord already struck
        int guard = 0;
        while (!rig.transport.ended() && guard++ < 20000) {
            rig.step(rig.now + rig.frame);
            if (rig.transport.held() && rig.eval.waiting_for_s() != struck) {
                struck = rig.eval.waiting_for_s();
                const double t = static_cast<double>(rig.now) * 1e-9 + 0.010;
                for (int key : down) {
                    if (!lose(rng)) {
                        rig.send(note_off(t, key));
                    }
                }
                down = rig.eval.waiting_keys();
                std::shuffle(down.begin(), down.end(), rng);
                for (int key : down) {
                    rig.send(note_on(t + 0.001 + roll(rng), key));
                }
                // The first key of the chord after this one, struck early and let go (or not).
                const auto later = std::find_if(notes.begin(), notes.end(), [&](const N& n) {
                    return n.start > struck + kChordSeconds + 1e-9;
                });
                if (later != notes.end() && peek(rng)) {
                    rig.send(note_on(t + 0.0005, later->pitch));
                    if (!lose(rng)) {
                        rig.send(note_off(t + 0.0008, later->pitch));
                    }
                }
            }
        }
        CHECK(rig.transport.ended());
        const EvaluationSummary& s = rig.eval.summary();
        CHECK(s.counts.played == static_cast<int>(notes.size()));
        CHECK(s.wrong == 0);
    }
}

TEST_CASE("flow: reset_results mid-loop keeps grading, with passes counted from 0 again") {
    FlowRig rig(song_of({{0.5, 60}, {1.5, 62}}), FlowOptions{});
    REQUIRE(rig.transport.set_loop(0, 0.0, 2.0));
    rig.transport.set_loop_enabled(0, true);
    rig.play_at(1.0);
    rig.send(note_on(1.5, 60));
    rig.send(note_on(2.5, 62));
    rig.send(note_on(3.5, 60));
    rig.send(note_on(4.5, 62));
    rig.run_until(4.51); // pass 1, its second note still undecided
    rig.eval.reset_results();
    rig.run_until(5.3);
    const EvaluationSummary& s = rig.eval.summary();
    REQUIRE(s.notes.size() == 1);
    CHECK(s.notes[0].note_index == 1);
    CHECK(s.notes[0].pass == 0);
    std::vector<PassResult> passes;
    rig.eval.take_passes(passes);
    REQUIRE(passes.size() == 1);
    CHECK(passes[0].pass == 0);
    CHECK(passes[0].graded == 1);
    CHECK_FALSE(passes[0].clean); // it lost the results before the reset
}

// ---------------------------------------------------------------------------------------------
// Flow and Wait edge cases: song changes mid-play, lost key-ups, early presses, tied strikes,
// huge or invalid options, and passes dropped on loops and seeks.

TEST_CASE("flow: set_song while the transport keeps playing grades the next notes (F1)") {
    FlowRig rig(song_of({{3.0, 60}}), FlowOptions{});
    rig.play_at(1.0);
    rig.run_until(3.0); // song 2.0, still playing
    rig.eval.set_song(rig.song, FlowOptions{});
    rig.send(note_on(4.0, 60)); // on time
    rig.run_until(6.0);
    const EvaluationSummary& s = rig.eval.summary();
    CHECK(hits(s) == 1);
    CHECK(s.counts.missed == 0);
    CHECK(s.wrong == 0);
}

TEST_CASE("wait: set_song while the transport keeps playing arms the next hold (F1)") {
    WaitRig rig(song_of({{2.0, 60}, {4.0, 62}}), WaitOptions{});
    rig.play_at(1.0);
    rig.run_until(2.0); // song 1.0
    rig.eval.set_song(rig.song, WaitOptions{});
    rig.run_until(3.5);
    CHECK(rig.transport.held());
    CHECK(rig.transport.position_s() == 2.0);
    CHECK(rig.eval.waiting_for_s() == 2.0);
    rig.send(note_on(3.5, 60));
    rig.run_until(3.6);
    CHECK_FALSE(rig.transport.held());
    CHECK(rig.eval.summary().counts.played == 1);
}

TEST_CASE("wait: set_song while held keeps the song held at the chord (F1)") {
    WaitRig rig(song_of({{1.0, 60}, {3.0, 62}}), WaitOptions{});
    rig.play_at(1.0);
    rig.run_until(2.5); // held at 1.0
    REQUIRE(rig.transport.held());
    rig.eval.set_song(rig.song, WaitOptions{});
    rig.run_until(2.7);
    CHECK(rig.transport.held());
    CHECK(rig.transport.position_s() == 1.0);
    rig.send(note_on(2.7, 60));
    rig.run_until(2.8);
    CHECK_FALSE(rig.transport.held());
}

TEST_CASE("flow and wait: a song change through the Transport grades from the next play (F1)") {
    const Song next = song_of({{1.0, 64}, {3.0, 65}});
    {
        FlowRig rig(song_of({{1.0, 60}, {5.0, 62}}), FlowOptions{});
        rig.play_at(1.0);
        rig.run_until(2.5);
        rig.transport.set_song(next);
        rig.song = next;
        rig.eval.set_song(next, FlowOptions{});
        rig.run_until(3.0);
        CHECK_FALSE(rig.transport.playing());
        rig.play_at(3.0);
        rig.send(note_on(4.0, 64));
        rig.run_until(7.0);
        const EvaluationSummary& s = rig.eval.summary();
        CHECK(hits(s) == 1);
        CHECK(s.wrong == 0);
    }
    {
        WaitRig rig(song_of({{1.0, 60}, {5.0, 62}}), WaitOptions{});
        rig.play_at(1.0);
        rig.run_until(2.5); // held at 1.0
        rig.transport.set_song(next);
        rig.song = next;
        rig.eval.set_song(next, WaitOptions{});
        rig.run_until(3.0);
        rig.play_at(3.0);
        rig.run_until(4.5);
        CHECK(rig.transport.held());
        CHECK(rig.transport.position_s() == 1.0);
        CHECK(rig.eval.waiting_for_s() == 1.0);
        rig.send(note_on(4.5, 64));
        rig.run_until(4.6);
        CHECK_FALSE(rig.transport.held());
        CHECK(rig.eval.summary().counts.played == 1);
    }
}

TEST_CASE("wait: a lost key-up does not swallow the next strike of that key (F2)") {
    WaitRig rig(song_of({{0.5, 60}, {1.0, 60}, {3.0, 72}}), WaitOptions{});
    rig.play_at(1.0);
    rig.send(note_on(1.5, 60)); // plays the chord at 0.5; its note-off never comes
    rig.run_until(2.5);
    CHECK(rig.transport.held());
    CHECK(rig.eval.waiting_for_s() == 1.0);
    rig.send(note_on(2.5, 60)); // a note-on for a key already down is a new strike
    rig.run_until(2.6);
    CHECK_FALSE(rig.transport.held());
    CHECK(rig.eval.summary().counts.played == 2);
    CHECK(rig.eval.summary().wrong == 0);
}

TEST_CASE("wait: release_all lets go of every key, for a lost connection (F2)") {
    WaitRig rig(song_of({{1.0, 60}, {1.0, 64}, {3.0, 72}}), WaitOptions{});
    rig.play_at(1.0);
    rig.run_until(2.5);
    rig.send(note_on(2.5, 60));
    rig.run_until(2.6);
    rig.eval.release_all(sec(2.6)); // the keyboard was unplugged with 60 down
    rig.send(note_on(2.7, 64));
    rig.run_until(2.8);
    CHECK(rig.transport.held()); // 60 is no longer down
    rig.send(note_on(2.8, 60));
    rig.run_until(2.9);
    CHECK_FALSE(rig.transport.held());
}

TEST_CASE("wait: a press while paused or before play never plays a chord (F3)") {
    {
        WaitRig rig(song_of({{1.0, 60}, {3.0, 72}}), WaitOptions{});
        rig.play_at(1.0);
        rig.run_until(1.9); // song 0.9
        rig.transport.pause(sec(1.9));
        rig.send(note_on(1.91, 60));
        rig.run_until(2.5);
        rig.play_at(2.5);
        rig.run_until(3.0);
        CHECK(rig.transport.held());
        CHECK(rig.transport.position_s() == 1.0);
        CHECK(rig.eval.waiting_for_s() == 1.0);
        CHECK(rig.eval.summary().counts.played == 0);
    }
    {
        WaitRig rig(song_of({{0.0, 60}, {2.0, 72}}), WaitOptions{});
        rig.send(note_on(0.5, 60)); // no play yet
        rig.run_until(0.9);
        rig.play_at(1.0);
        rig.run_until(1.5);
        CHECK(rig.transport.held()); // within a frame of the start
        CHECK(rig.eval.waiting_for_s() == 0.0);
        CHECK(rig.eval.summary().counts.played == 0);
    }
}

TEST_CASE("flow and wait agree on one key struck for two notes 21 ms apart (F4)") {
    const Song close = song_of({{1.000, 60, Hand::Right}, {1.021, 60, Hand::Left}, {3.0, 72}});
    {
        FlowRig rig(close, FlowOptions{});
        rig.play_at(1.0);
        rig.send(note_on(2.0105, 60));
        rig.send(note_on(4.0, 72));
        rig.run_until(6.0);
        CHECK(hits(rig.eval.summary()) == 3);
        CHECK(rig.eval.summary().counts.missed == 0);
    }
    {
        WaitRig rig(close, WaitOptions{});
        rig.play_at(1.0);
        rig.run_until(2.5);
        rig.send(note_on(2.5, 60));
        rig.run_until(3.0);
        CHECK(rig.eval.summary().counts.played == 2);
        CHECK(rig.eval.waiting_for_s() == 3.0);
    }
    CHECK(kUnisonSeconds == kChordSeconds);
}

TEST_CASE("flow and wait agree that one key 40 ms apart needs two strikes (F4)") {
    const Song apart = song_of({{1.000, 60}, {1.040, 60}, {3.0, 72}});
    {
        FlowRig rig(apart, FlowOptions{});
        rig.play_at(1.0);
        rig.send(note_on(2.02, 60));
        rig.run_until(6.0);
        CHECK(hits(rig.eval.summary()) == 1);
        CHECK(rig.eval.summary().counts.missed == 2);
    }
    {
        WaitRig rig(apart, WaitOptions{});
        rig.play_at(1.0);
        rig.run_until(2.5);
        rig.send(note_on(2.5, 60));
        rig.run_until(3.0);
        CHECK(rig.eval.summary().counts.played == 1);
        CHECK(rig.eval.waiting_for_s() == doctest::Approx(1.040));
    }
}

TEST_CASE("wait: a key of a later chord pressed early is not wrong (F5)") {
    const Song song = song_of({{1.0, 60}, {3.0, 64}, {5.0, 72}});
    {
        WaitRig rig(song, WaitOptions{});
        rig.play_at(1.0);
        rig.run_until(2.5); // held at 1.0
        rig.send(note_on(2.5, 64));
        rig.send(note_off(2.6, 64));
        rig.run_until(2.7);
        CHECK(rig.eval.summary().wrong == 0);
        CHECK(rig.transport.held()); // and it plays nothing
        CHECK(rig.eval.waiting_for_s() == 1.0);
    }
    {
        WaitOptions o;
        o.lookahead_s = 1.5; // the later chord is past the lookahead: a wrong key
        WaitRig rig(song, o);
        rig.play_at(1.0);
        rig.run_until(2.5);
        rig.send(note_on(2.5, 64));
        rig.run_until(2.7);
        CHECK(rig.eval.summary().wrong == 1);
    }
}

TEST_CASE("flow: a key of a later note pressed early is not wrong (F5)") {
    const Song song = song_of({{1.0, 60}, {3.0, 64}});
    {
        FlowRig rig(song, FlowOptions{});
        rig.play_at(1.0);
        rig.send(note_on(2.0, 60));
        rig.send(note_on(2.0, 64)); // 2 s early for the note at 3.0
        rig.send(note_on(4.0, 64));
        rig.run_until(6.0);
        const EvaluationSummary& s = rig.eval.summary();
        CHECK(hits(s) == 2);
        CHECK(s.wrong == 0);
    }
    {
        FlowOptions o;
        o.lookahead_s = 1.5;
        FlowRig rig(song, o);
        rig.play_at(1.0);
        rig.send(note_on(2.0, 64));
        rig.run_until(6.0);
        CHECK(rig.eval.summary().wrong == 1);
    }
}

TEST_CASE("flow: a long run of one key is decided in bounded time (F9)") {
    const int n = 2500;
    std::vector<N> list;
    for (int i = 0; i < n; ++i) {
        list.push_back({0.1 + i * 0.040, 60, Hand::Right, 0.02});
    }
    // The best of three runs, so a busy machine doesn't fail the test.
    double took_ms = 1e9;
    for (int round = 0; round < 3; ++round) {
        FlowRig rig(song_of(list), FlowOptions{});
        rig.play_at(1.0);
        rig.transport.update(sec(1.0 + rig.song.duration_s));
        rig.flush_ops();
        for (int i = 0; i < n; ++i) {
            rig.eval.apply(note_on(1.0 + 0.1 + i * 0.040, 60));
        }
        const auto t0 = std::chrono::steady_clock::now();
        rig.eval.update(sec(2.0 + rig.song.duration_s));
        const auto t1 = std::chrono::steady_clock::now();
        took_ms = std::min(took_ms, std::chrono::duration<double, std::milli>(t1 - t0).count());
        CHECK(rig.eval.summary().counts.perfect == n);
        CHECK(rig.eval.pending_notes() == 0);
    }
    CAPTURE(took_ms);
    CHECK(took_ms < 20.0); // about 2 ms; the full table took 80 ms for 2000 notes
}

TEST_CASE("flow: the banded matching keeps the optimal assignment (F9)") {
    // Random runs of one key with one jittered press per note: a full assignment exists, so the
    // optimal one hits every note.
    for (unsigned seed = 1; seed <= 20; ++seed) {
        CAPTURE(seed);
        std::mt19937 rng(seed);
        std::uniform_real_distribution<double> gap(0.06, 0.2);
        std::uniform_real_distribution<double> jitter(-0.075, 0.075);
        std::vector<N> list;
        double t = 0.2;
        for (int i = 0; i < 300; ++i) {
            list.push_back({t, 60, Hand::Right, 0.03});
            t += gap(rng);
        }
        FlowRig rig(song_of(list), FlowOptions{});
        rig.play_at(1.0);
        for (const N& note : list) {
            rig.send(note_on(1.0 + note.start + jitter(rng), 60));
        }
        rig.run_until(1.0 + rig.song.duration_s + 1.0);
        rig.eval.finish();
        const EvaluationSummary& s = rig.eval.summary();
        CHECK(hits(s) == static_cast<int>(list.size()));
        CHECK(s.wrong == 0);
    }
}

TEST_CASE("flow: NaN, infinite and huge options fall back or are clamped (F11)") {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    FlowOptions o;
    o.window_ms = nan;
    o.perfect_fraction = nan;
    o.good_fraction = nan;
    o.latency_ms = nan;
    o.settle_ms = 1e13;
    o.lookahead_s = inf;
    FlowEvaluator e(song_of({{1.0, 60}}), o);
    CHECK(e.options().window_ms == FlowOptions{}.window_ms);
    CHECK(e.options().perfect_fraction == FlowOptions{}.perfect_fraction);
    CHECK(e.options().good_fraction == FlowOptions{}.good_fraction);
    CHECK(e.options().latency_ms == 0.0);
    CHECK(e.options().settle_ms <= 1000.0);
    CHECK(std::isfinite(e.options().lookahead_s));
    FlowRig rig(song_of({{1.0, 60}}), o);
    rig.play_at(1.0);
    rig.send(note_on(2.005, 60)); // 5 ms late
    rig.run_until(4.0);
    rig.eval.finish();
    CHECK(rig.eval.summary().counts.perfect == 1);
    CHECK(rig.eval.summary().wrong == 0);

    o.window_ms = inf;
    o.latency_ms = -inf;
    FlowEvaluator f(song_of({{1.0, 60}}), o);
    CHECK(f.options().window_ms == 500.0);
    CHECK(std::isfinite(f.options().latency_ms));

    WaitOptions w;
    w.early_ms = nan;
    w.release_slack_ms = inf;
    w.settle_ms = nan;
    w.lookahead_s = nan;
    WaitEvaluator we(song_of({{1.0, 60}}), w);
    CHECK(we.options().early_ms == WaitOptions{}.early_ms);
    CHECK(std::isfinite(we.options().release_slack_ms));
    CHECK(we.options().settle_ms == WaitOptions{}.settle_ms);
    CHECK(we.options().lookahead_s == WaitOptions{}.lookahead_s);
}

TEST_CASE("flow and wait: passes taken each frame are forgotten, however many loops (F13)") {
    {
        FlowRig rig(song_of({{0.05, 60}}, 1.0), FlowOptions{});
        REQUIRE(rig.transport.set_loop(0, 0.0, 0.1));
        rig.transport.set_loop_enabled(0, true);
        rig.play_at(1.0);
        std::vector<PassResult> passes;
        std::size_t most = 0;
        while (rig.transport.loop_count() < 3000) {
            rig.step(rig.now + rig.frame);
            rig.eval.take_passes(passes);
            most = std::max(most, rig.eval.kept_passes());
        }
        CHECK(most <= 4);
        CHECK(passes.size() >= 2990);
    }
    {
        WaitRig rig(song_of({{0.05, 60}}, 1.0), WaitOptions{});
        REQUIRE(rig.transport.set_loop(0, 0.0, 0.1));
        rig.transport.set_loop_enabled(0, true);
        rig.play_at(1.0);
        std::vector<PassResult> passes;
        std::size_t most = 0;
        bool down = false;
        while (rig.transport.loop_count() < 1000) {
            rig.step(rig.now + rig.frame);
            if (rig.transport.held()) {
                const double t = static_cast<double>(rig.now) * 1e-9;
                rig.send(down ? note_off(t, 60) : note_on(t, 60));
                down = !down;
            }
            rig.eval.take_passes(passes);
            most = std::max(most, rig.eval.kept_passes());
        }
        CHECK(most <= 4);
        CHECK(passes.size() >= 990);
    }
}

TEST_CASE("flow: a pass cut short by a seek is dropped, not kept forever (F13)") {
    FlowRig rig(song_of({{1.0, 60}, {5.0, 62}}), FlowOptions{});
    rig.play_at(1.0);
    for (int i = 0; i < 50; ++i) {
        rig.run_until(1.5 + i * 0.1);
        rig.transport.seek(rig.now, 0.5);
    }
    rig.run_until(7.0);
    std::vector<PassResult> passes;
    rig.eval.take_passes(passes);
    CHECK(rig.eval.kept_passes() <= 2);
}
