// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/transport.hpp"

#include "toonval/midi_file.hpp"

#include "smf_builder.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <limits>
#include <vector>

using namespace toonval;
using namespace toonval::test;
using Kind = TransportOp::Kind;

namespace {

// Whole seconds and halves are exact in binary, so these tests can compare positions exactly.
constexpr std::int64_t sec(double s) { return static_cast<std::int64_t>(s * 1e9); }

Song plain_song(double duration_s) {
    Song s;
    s.duration_s = duration_s;
    return s;
}

// `bars` bars of 4/4 at `bpm`, one note across all of them, so bar n starts at (n - 1) * 240 / bpm.
Song bars_song(int bars, int bpm = 120, int num = 4, int den = 4) {
    Track t;
    t.bpm(0, bpm).time_sig(0, num, den);
    const std::uint32_t bar_ticks = static_cast<std::uint32_t>(480 * 4 * num / den);
    t.on(0, 0, 60).off(bar_ticks * static_cast<std::uint32_t>(bars), 0, 60).end();
    MidiLoadResult r = load_midi(smf({t}));
    REQUIRE(r.ok);
    return r.song;
}

std::vector<TransportOp> drain(Transport& t) {
    std::vector<TransportOp> ops;
    t.take_ops(ops);
    return ops;
}

std::vector<TransportOp> only(const std::vector<TransportOp>& ops, Kind kind) {
    std::vector<TransportOp> out;
    std::copy_if(ops.begin(), ops.end(), std::back_inserter(out),
                 [&](const TransportOp& o) { return o.kind == kind; });
    return out;
}

Transport ready(const Song& song) {
    Transport t;
    t.set_song(song);
    return t;
}

} // namespace

TEST_CASE("transport: a new transport sits paused at the start") {
    Transport t = ready(plain_song(10));
    t.update(sec(5));
    CHECK(t.position_s() == 0.0);
    CHECK(t.state() == TransportState::Paused);
    CHECK_FALSE(t.playing());
    CHECK(t.speed_percent() == 100);
    CHECK(drain(t).empty());
}

TEST_CASE("transport: the position follows the caller's clock while playing") {
    Transport t = ready(plain_song(100));
    t.play(sec(1000));
    t.update(sec(1001));
    CHECK(t.position_s() == 1.0);
    CHECK(t.state() == TransportState::Playing);
    t.update(sec(1003.5));
    CHECK(t.position_s() == 3.5);
}

TEST_CASE("transport: a time that is not later than the last is ignored") {
    Transport t = ready(plain_song(100));
    t.play(sec(10));
    t.update(sec(12));
    t.update(sec(11));
    CHECK(t.position_s() == 2.0);
    t.update(sec(12));
    CHECK(t.position_s() == 2.0);
    t.update(sec(13));
    CHECK(t.position_s() == 3.0);
}

TEST_CASE("transport: speed scales the song clock") {
    Transport t = ready(plain_song(100));
    CHECK(t.set_speed_percent(0, 50) == 50);
    t.play(0);
    t.update(sec(2));
    CHECK(t.position_s() == 1.0);
    t.set_speed_percent(sec(2), 150);
    t.update(sec(4));
    CHECK(t.position_s() == 4.0);
}

TEST_CASE("transport: speed is clamped to 25..150 and kept to steps of 5") {
    Transport t = ready(plain_song(100));
    CHECK(t.set_speed_percent(0, 10) == 25);
    CHECK(t.set_speed_percent(0, 0) == 25);
    CHECK(t.set_speed_percent(0, -40) == 25);
    CHECK(t.set_speed_percent(0, 200) == 150);
    CHECK(t.set_speed_percent(0, 62) == 60);
    CHECK(t.set_speed_percent(0, 63) == 65);
    CHECK(t.set_speed_percent(0, 100) == 100);
    CHECK(t.set_speed_percent(0, 148) == 150);
    CHECK(snap_speed_percent(27) == 25);
    CHECK(snap_speed_percent(28) == 30);
    for (int p = -20; p < 300; ++p) {
        int s = snap_speed_percent(p);
        CAPTURE(p);
        CHECK(s >= kMinSpeedPercent);
        CHECK(s <= kMaxSpeedPercent);
        CHECK(s % kSpeedStepPercent == 0);
    }
}

TEST_CASE("transport: changing speed mid-play does not move the position") {
    Transport t = ready(plain_song(100));
    t.play(0);
    t.update(sec(3));
    REQUIRE(t.position_s() == 3.0);
    t.set_speed_percent(sec(3.25), 25);
    CHECK(t.position_s() == 3.25); // the 0.25 s before the change ran at the old speed
    t.set_speed_percent(sec(3.25), 25);
    CHECK(t.position_s() == 3.25);
    t.update(sec(5.25));
    CHECK(t.position_s() == 3.75);
    t.set_speed_percent(sec(5.25), 150);
    CHECK(t.position_s() == 3.75);
    t.update(sec(6.25));
    CHECK(t.position_s() == 5.25);
}

TEST_CASE("transport: pause freezes the position and play resumes without catching up") {
    Transport t = ready(plain_song(100));
    t.play(0);
    t.pause(sec(1));
    CHECK(t.position_s() == 1.0);
    CHECK(t.state() == TransportState::Paused);
    t.update(sec(5));
    CHECK(t.position_s() == 1.0);
    t.play(sec(5));
    t.update(sec(6));
    CHECK(t.position_s() == 2.0);
}

TEST_CASE("transport: pause settles the clock to the pause time first") {
    Transport t = ready(plain_song(100));
    t.play(0);
    t.pause(sec(2.5)); // no update() in between
    CHECK(t.position_s() == 2.5);
}

TEST_CASE("transport: seek jumps, clamps, and keeps playing from there") {
    Transport t = ready(plain_song(10));
    t.play(0);
    t.update(sec(1));
    t.seek(sec(1), 6);
    CHECK(t.position_s() == 6.0);
    t.update(sec(2));
    CHECK(t.position_s() == 7.0);
    t.seek(sec(2), -4);
    CHECK(t.position_s() == 0.0);
    t.seek(sec(2), 99);
    CHECK(t.position_s() == 10.0);
}

TEST_CASE("transport: seek while paused moves the position and stays paused") {
    Transport t = ready(plain_song(10));
    t.seek(sec(1), 4);
    t.update(sec(3));
    CHECK(t.position_s() == 4.0);
    CHECK_FALSE(t.playing());
}

TEST_CASE("transport: the end of the song stops the clock once") {
    Transport t = ready(plain_song(2));
    t.play(0);
    drain(t);
    t.update(sec(5));
    CHECK(t.position_s() == 2.0);
    CHECK(t.ended());
    CHECK_FALSE(t.playing());
    CHECK(t.state() == TransportState::Ended);
    std::vector<TransportOp> ops = drain(t);
    REQUIRE(only(ops, Kind::End).size() == 1);
    CHECK(only(ops, Kind::End)[0].at_ns == sec(2)); // when it got there, not when update() ran
    t.update(sec(9));
    CHECK(only(drain(t), Kind::End).empty());
}

TEST_CASE("transport: play after the end starts over, and so does play at the very end") {
    Transport t = ready(plain_song(2));
    t.play(0);
    t.update(sec(5));
    REQUIRE(t.ended());
    t.play(sec(5));
    CHECK_FALSE(t.ended());
    CHECK(t.position_s() == 0.0);
    t.update(sec(6));
    CHECK(t.position_s() == 1.0);

    t.pause(sec(6));
    t.seek(sec(6), 2);
    t.play(sec(7));
    CHECK(t.position_s() == 0.0);
}

TEST_CASE("transport: seeking away from the end clears the ended state") {
    Transport t = ready(plain_song(2));
    t.play(0);
    t.update(sec(5));
    t.seek(sec(5), 1);
    CHECK_FALSE(t.ended());
    CHECK(t.state() == TransportState::Paused);
}

TEST_CASE("transport: seeking to the end while playing ends the song") {
    Transport t = ready(plain_song(2));
    t.play(0);
    t.seek(sec(0.5), 2);
    t.update(sec(1));
    CHECK(t.ended());
}

TEST_CASE("transport: ops record what the clock did, with wall and song times") {
    Transport t = ready(plain_song(100));
    t.set_speed_percent(0, 50);
    t.play(sec(10));
    t.update(sec(11));
    t.update(sec(12)); // contiguous: merges into the same Move
    t.seek(sec(12), 20);
    t.pause(sec(13));
    std::vector<TransportOp> ops = drain(t);
    REQUIRE(ops.size() == 5);
    CHECK(ops[0].kind == Kind::Play);
    CHECK(ops[0].at_ns == sec(10));
    CHECK(ops[0].to_s == 0.0);
    CHECK(ops[1].kind == Kind::Move);
    CHECK(ops[1].from_s == 0.0);
    CHECK(ops[1].to_s == 1.0);
    CHECK(ops[1].at_ns == sec(10));
    CHECK(ops[1].end_ns == sec(12));
    CHECK(ops[1].speed_percent == 50);
    CHECK(ops[2].kind == Kind::Seek);
    CHECK(ops[2].from_s == 1.0);
    CHECK(ops[2].to_s == 20.0);
    CHECK(ops[3].kind == Kind::Move);
    CHECK(ops[3].from_s == 20.0);
    CHECK(ops[3].to_s == 20.5);
    CHECK(ops[4].kind == Kind::Pause);
    CHECK(ops[4].to_s == 20.5);
    CHECK(drain(t).empty());
}

TEST_CASE("transport: a speed change splits the Move, so each Move has one speed") {
    Transport t = ready(plain_song(100));
    t.play(0);
    t.set_speed_percent(sec(1), 50);
    t.update(sec(3));
    std::vector<TransportOp> moves = only(drain(t), Kind::Move);
    REQUIRE(moves.size() == 2);
    CHECK(moves[0].speed_percent == 100);
    CHECK(moves[0].to_s == 1.0);
    CHECK(moves[1].speed_percent == 50);
    CHECK(moves[1].from_s == 1.0);
    CHECK(moves[1].to_s == 2.0);
    CHECK(moves[1].end_ns == sec(3));
}

TEST_CASE("transport: set_song resets the clock") {
    Transport t = ready(plain_song(10));
    t.play(0);
    t.update(sec(4));
    drain(t);
    t.set_song(plain_song(20));
    CHECK(t.position_s() == 0.0);
    CHECK_FALSE(t.playing());
    CHECK(t.duration_s() == 20.0);
}

TEST_CASE("transport: set_song drops and counts undrained ops, then reports the reset (F15)") {
    Transport t = ready(plain_song(10));
    t.play(0);
    t.update(sec(4)); // Play and a Move still queued
    CHECK(t.set_song(plain_song(20)) == 2);
    std::vector<TransportOp> ops = drain(t);
    REQUIRE(ops.size() == 2);
    CHECK(ops[0].kind == Kind::Pause);
    CHECK(ops[0].at_ns == sec(4));
    CHECK(ops[1].kind == Kind::Seek);
    CHECK(ops[1].at_ns == sec(4));
    CHECK(ops[1].from_s == 4.0);
    CHECK(ops[1].to_s == 0.0);

    // Paused at the start, nothing to report.
    CHECK(t.set_song(plain_song(5)) == 0);
    CHECK(drain(t).empty());
}

TEST_CASE("transport: a NaN seek is ignored and the clock keeps moving (F12)") {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    Transport t = ready(plain_song(10));
    t.play(0);
    t.update(sec(1));
    drain(t);
    t.seek(sec(1), nan);
    CHECK(t.position_s() == 1.0);
    CHECK(only(drain(t), Kind::Seek).empty());
    t.update(sec(2));
    CHECK(t.position_s() == 2.0);
    t.seek(sec(2), std::numeric_limits<double>::infinity());
    CHECK(t.position_s() == 10.0);
    CHECK_FALSE(t.set_loop(sec(2), nan, 4.0));
    CHECK_FALSE(t.set_loop(sec(2), 1.0, nan));
    CHECK_FALSE(t.loop_enabled());
}

TEST_CASE("transport: a seek forgets the hold point armed before it (F16)") {
    Transport t = ready(plain_song(100));
    t.play(0);
    t.update(sec(1));
    t.hold_at(sec(1), 2.0);
    t.seek(sec(1), 1.99);
    t.update(sec(2));
    CHECK_FALSE(t.held());
    CHECK(t.position_s() == doctest::Approx(2.99));
}

// ---- A-B loop -------------------------------------------------------------------------------

TEST_CASE("transport loop: the position wraps from B back to A") {
    Transport t = ready(plain_song(10));
    REQUIRE(t.set_loop(0, 2, 4));
    t.seek(0, 3.5);
    t.play(0);
    drain(t);
    t.update(sec(1)); // 0.5 s to B, then 0.5 s into the second pass
    CHECK(t.position_s() == 2.5);
    CHECK(t.loop_count() == 1);
    std::vector<TransportOp> ops = drain(t);
    REQUIRE(ops.size() == 3);
    CHECK(ops[0].kind == Kind::Move);
    CHECK(ops[0].from_s == 3.5);
    CHECK(ops[0].to_s == 4.0);
    CHECK(ops[0].end_ns == sec(0.5));
    CHECK(ops[1].kind == Kind::Wrap);
    CHECK(ops[1].from_s == 4.0);
    CHECK(ops[1].to_s == 2.0);
    CHECK(ops[1].at_ns == sec(0.5));
    CHECK(ops[2].kind == Kind::Move);
    CHECK(ops[2].from_s == 2.0);
    CHECK(ops[2].to_s == 2.5);
    CHECK(ops[2].at_ns == sec(0.5));
}

TEST_CASE("transport loop: landing exactly on B wraps to exactly A") {
    Transport t = ready(plain_song(10));
    REQUIRE(t.set_loop(0, 2, 4));
    t.seek(0, 3.5);
    t.play(0);
    t.update(sec(0.5));
    CHECK(t.position_s() == 2.0);
    CHECK(t.loop_count() == 1);
}

TEST_CASE("transport loop: the position is never seen at B or beyond while looping") {
    Transport t = ready(plain_song(10));
    REQUIRE(t.set_loop(0, 2.2, 3.7));
    t.seek(0, 2.2);
    t.play(0);
    for (int i = 1; i <= 2000; ++i) {
        t.update(i * 7'000'000LL); // 7 ms steps, not a divisor of the loop
        CHECK(t.position_s() >= 2.2);
        CHECK(t.position_s() < 3.7);
    }
    CHECK(t.loop_count() > 0);
}

TEST_CASE("transport loop: a long stall wraps several times and keeps the position inside") {
    Transport t = ready(plain_song(10));
    REQUIRE(t.set_loop(0, 2, 3));
    t.seek(0, 2);
    t.play(0);
    t.update(sec(3.5));
    CHECK(t.position_s() == 2.5);
    CHECK(t.loop_count() == 3);
}

TEST_CASE("transport loop: speed changes how long a pass takes") {
    Transport t = ready(plain_song(10));
    REQUIRE(t.set_loop(0, 2, 4));
    t.set_speed_percent(0, 50);
    t.seek(0, 2);
    t.play(0);
    t.update(sec(3.9));
    CHECK(t.loop_count() == 0);
    t.update(sec(4));
    CHECK(t.loop_count() == 1);
    CHECK(t.position_s() == 2.0);
}

TEST_CASE("transport loop: a position past B just plays on to the end") {
    Transport t = ready(plain_song(10));
    REQUIRE(t.set_loop(0, 2, 4));
    t.seek(0, 6);
    t.play(0);
    t.update(sec(2));
    CHECK(t.position_s() == 8.0);
    CHECK(t.loop_count() == 0);
}

TEST_CASE("transport loop: clearing or disabling it lets the song run through") {
    Transport t = ready(plain_song(10));
    REQUIRE(t.set_loop(0, 2, 4));
    t.seek(0, 3);
    t.play(0);
    t.update(sec(0.5));
    t.clear_loop(sec(0.5));
    t.update(sec(2));
    CHECK(t.position_s() == 5.0);

    REQUIRE(t.set_loop(sec(2), 2, 4));
    CHECK(t.loop_enabled());
    t.set_loop_enabled(sec(2), false);
    t.seek(sec(2), 3);
    t.update(sec(4));
    CHECK(t.position_s() == 5.0);
    t.set_loop_enabled(sec(4), true);
    t.seek(sec(4), 3);
    t.update(sec(5.5));
    CHECK(t.position_s() == 2.5);
}

TEST_CASE("transport loop: set_loop swaps reversed times, clamps, and rejects tiny loops") {
    Transport t = ready(plain_song(10));
    CHECK(t.set_loop(0, 6, 2));
    CHECK(t.loop_start_s() == 2.0);
    CHECK(t.loop_end_s() == 6.0);
    CHECK(t.set_loop(0, -5, 99));
    CHECK(t.loop_start_s() == 0.0);
    CHECK(t.loop_end_s() == 10.0);
    CHECK_FALSE(t.set_loop(0, 3, 3));
    CHECK_FALSE(t.set_loop(0, 3, 3.05));
    CHECK(t.loop_start_s() == 0.0); // the last good loop is untouched
    CHECK(t.loop_end_s() == 10.0);
    CHECK_FALSE(t.set_loop(0, 20, 30)); // clamps to an empty loop
}

TEST_CASE("transport loop: a loop that ends at the end of the song wraps instead of ending") {
    Transport t = ready(plain_song(4));
    REQUIRE(t.set_loop(0, 2, 4));
    t.seek(0, 3);
    t.play(0);
    t.update(sec(1.5));
    CHECK_FALSE(t.ended());
    CHECK(t.position_s() == 2.5);
}

TEST_CASE("transport loop: snapping to bar starts") {
    Song s = bars_song(8); // bars are 2 s long
    REQUIRE(s.measures.size() == 8);
    CHECK(snap_to_measure(s, 4.9, SnapMode::Floor) == 4.0);
    CHECK(snap_to_measure(s, 4.0, SnapMode::Floor) == 4.0);
    CHECK(snap_to_measure(s, 4.1, SnapMode::Ceil) == 6.0);
    CHECK(snap_to_measure(s, 4.0, SnapMode::Ceil) == 4.0);
    CHECK(snap_to_measure(s, 4.9, SnapMode::Nearest) == 4.0);
    CHECK(snap_to_measure(s, 5.1, SnapMode::Nearest) == 6.0);
    CHECK(snap_to_measure(s, 5.0, SnapMode::Nearest) == 6.0); // halfway goes up
    CHECK(snap_to_measure(s, 4.2, SnapMode::Nearest) == 4.0);
    CHECK(snap_to_measure(s, -3.0, SnapMode::Floor) == 0.0);
    CHECK(snap_to_measure(s, 15.5, SnapMode::Ceil) == s.duration_s); // after the last bar start
    CHECK(snap_to_measure(s, 99.0, SnapMode::Floor) == 14.0);
    CHECK(snap_to_measure(plain_song(10), 3.3) == 3.3);
}

TEST_CASE("transport loop: bar ranges become loop spans by bar number") {
    Song s = bars_song(8);
    LoopSpan span = measures_span(s, 3, 4);
    REQUIRE(span.ok);
    CHECK(span.start_s == 4.0);
    CHECK(span.end_s == 8.0);
    LoopSpan last = measures_span(s, 7, 8);
    REQUIRE(last.ok);
    CHECK(last.start_s == 12.0);
    CHECK(last.end_s == s.duration_s);
    CHECK(measures_span(s, 4, 3).ok == false);
    CHECK(measures_span(s, 3, 9).ok == false);
    CHECK(measures_span(s, 0, 2).ok == false);
    LoopSpan one = measures_span(s, 5, 5);
    REQUIRE(one.ok);
    CHECK(one.end_s - one.start_s == 2.0);

    Transport t = ready(s);
    REQUIRE(t.set_loop(0, span.start_s, span.end_s));
    CHECK(t.loop_start_s() == 4.0);
}

namespace {
// A one-beat pickup (bar 0, beat 4 of a 4/4 bar), then 4/4 bars from tick 480: 0.5 s, then 2 s
// bars.
Song pickup_song() {
    MidiLoadResult r = load_midi(smf({Track()
                                          .time_sig(0, 1, 4)
                                          .on(0, 0, 67)
                                          .off(480, 0, 67)
                                          .time_sig(0, 4, 4)
                                          .on(0, 0, 60)
                                          .off(1920 * 2, 0, 60)
                                          .end()}));
    REQUIRE(r.ok);
    REQUIRE(r.song.measures.size() >= 3);
    REQUIRE(r.song.measures[0].number == 0);
    return r.song;
}
} // namespace

TEST_CASE("transport loop: a pickup bar is bar 0 in a bar range") {
    Song s = pickup_song();
    LoopSpan span = measures_span(s, 0, 1);
    REQUIRE(span.ok);
    CHECK(span.start_s == 0.0);
    CHECK(span.end_s == 0.5 + 2.0);
}

// ---- Auto speed-up --------------------------------------------------------------------------

TEST_CASE("auto speed-up: start 60%, +5% after each clean pass, up to 100%") {
    AutoSpeedUp policy{true, 60, 5, 100};
    int speed = policy.start_percent;
    std::vector<int> seen{speed};
    for (int i = 0; i < 12; ++i) {
        speed = policy.next_percent(speed, true);
        seen.push_back(speed);
    }
    CHECK(seen == std::vector<int>{60, 65, 70, 75, 80, 85, 90, 95, 100, 100, 100, 100, 100});
    CHECK(policy.next_percent(80, false) == 80);
    CHECK(policy.next_percent(120, true) == 120); // already above the maximum: never lowered
}

TEST_CASE("auto speed-up: the transport follows clean passes reported by the caller") {
    Transport t = ready(plain_song(20));
    t.set_auto_speed_up(0, AutoSpeedUp{true, 60, 5, 100});
    CHECK(t.speed_percent() == 60);
    REQUIRE(t.set_loop(0, 2, 4));
    t.report_pass(0, true);
    CHECK(t.speed_percent() == 65);
    t.report_pass(0, false);
    CHECK(t.speed_percent() == 65);
    for (int i = 0; i < 20; ++i) {
        t.report_pass(0, true);
    }
    CHECK(t.speed_percent() == 100);
    CHECK(t.clean_passes() == 21);
}

TEST_CASE("auto speed-up: off means reported passes change nothing") {
    Transport t = ready(plain_song(20));
    t.set_speed_percent(0, 80);
    t.report_pass(0, true);
    CHECK(t.speed_percent() == 80);
    t.set_auto_speed_up(0, AutoSpeedUp{true, 60, 5, 100});
    t.set_auto_speed_up(0, AutoSpeedUp{false, 60, 5, 100});
    CHECK(t.speed_percent() == 60); // turning it off leaves the speed where it is
    t.report_pass(0, true);
    CHECK(t.speed_percent() == 60);
}

TEST_CASE("auto speed-up: a speed change from a pass keeps the position continuous") {
    Transport t = ready(plain_song(20));
    t.set_auto_speed_up(0, AutoSpeedUp{true, 60, 5, 100});
    t.play(0);
    t.update(sec(2)); // 1.2 s at 60%
    double before = t.position_s();
    t.report_pass(sec(2), true);
    CHECK(t.position_s() == before);
}

// ---- Count-in -------------------------------------------------------------------------------

TEST_CASE("count-in: the position waits N beats, and the beats are exposed as clicks") {
    Transport t = ready(bars_song(4)); // 120 BPM: a beat is 0.5 s
    t.set_count_in_beats(4);
    t.play(sec(10));
    CHECK(t.state() == TransportState::CountIn);
    CHECK(t.counting_in());
    CHECK(t.count_in_beats_left() == 4.0);
    t.update(sec(11.9));
    CHECK(t.position_s() == 0.0);
    CHECK(t.counting_in());
    t.update(sec(12.5));
    CHECK(t.position_s() == doctest::Approx(0.5));
    CHECK_FALSE(t.counting_in());
    CHECK(t.state() == TransportState::Playing);

    std::vector<TransportOp> clicks = only(drain(t), Kind::Click);
    REQUIRE(clicks.size() == 4);
    for (int i = 0; i < 4; ++i) {
        CAPTURE(i);
        CHECK(clicks[static_cast<std::size_t>(i)].count_in);
        CHECK(clicks[static_cast<std::size_t>(i)].beat == i + 1);
        CHECK(clicks[static_cast<std::size_t>(i)].accent == (i == 0));
        CHECK(clicks[static_cast<std::size_t>(i)].at_ns == sec(10 + 0.5 * i));
        CHECK(clicks[static_cast<std::size_t>(i)].to_s == doctest::Approx(-0.5 * (4 - i)));
    }
}

TEST_CASE("count-in: clicks come out the same whatever the update rate") {
    for (std::int64_t step_ms : {1000, 16, 250, 7}) {
        Transport t = ready(bars_song(4));
        t.set_count_in_beats(4);
        t.play(0);
        for (std::int64_t ms = step_ms; ms <= 3500; ms += step_ms) {
            t.update(ms * 1'000'000);
        }
        std::vector<TransportOp> clicks = only(drain(t), Kind::Click);
        CAPTURE(step_ms);
        REQUIRE(clicks.size() >= 4);
        std::vector<TransportOp> count_in;
        std::copy_if(clicks.begin(), clicks.end(), std::back_inserter(count_in),
                     [](const TransportOp& c) { return c.count_in; });
        REQUIRE(count_in.size() == 4);
        for (int i = 0; i < 4; ++i) {
            CHECK(count_in[static_cast<std::size_t>(i)].at_ns == sec(0.5 * i));
        }
        CHECK(t.position_s() > 0.0);
    }
}

TEST_CASE("count-in: half speed doubles the wall time of each beat") {
    Transport t = ready(bars_song(4));
    t.set_speed_percent(0, 50);
    t.set_count_in_beats(2);
    t.play(0);
    t.update(sec(1.9));
    CHECK(t.position_s() == 0.0);
    t.update(sec(2));
    CHECK(t.position_s() == 0.0);
    t.update(sec(3));
    CHECK(t.position_s() == doctest::Approx(0.5));
    std::vector<TransportOp> clicks = only(drain(t), Kind::Click);
    REQUIRE(clicks.size() == 2);
    CHECK(clicks[1].at_ns == sec(1));
}

TEST_CASE("count-in: a speed change mid-count keeps the beats already counted") {
    Transport t = ready(bars_song(4));
    t.set_count_in_beats(4);
    t.play(0);
    t.update(sec(1)); // two beats done
    CHECK(t.count_in_beats_left() == 2.0);
    t.set_speed_percent(sec(1), 50);
    CHECK(t.count_in_beats_left() == 2.0);
    t.update(sec(2.5)); // two beats take 2 s at half speed: still counting
    CHECK(t.position_s() == 0.0);
    CHECK(t.count_in_beats_left() == doctest::Approx(0.5));
    t.update(sec(3.5)); // the count ended at 3 s; 0.5 s of song time at 50% is 0.25 s
    CHECK(t.position_s() == 0.25);
    CHECK(t.count_in_beats_left() == 0.0);
}

TEST_CASE("count-in: uses the tempo and time signature where the song starts") {
    // 120 BPM in 4/4, then from tick 1920 (bar 2) 60 BPM in 3/4.
    Track t;
    t.bpm(0, 120).time_sig(0, 4, 4).on(0, 0, 60).bpm(1920, 60).time_sig(0, 3, 4);
    t.off(1440 * 2, 0, 60).end();
    MidiLoadResult r = load_midi(smf({t}));
    REQUIRE(r.ok);
    REQUIRE(r.song.measures.size() == 3);
    const double bar2 = r.song.measures[1].start_s;
    REQUIRE(bar2 == doctest::Approx(2.0));

    Transport tr = ready(r.song);
    tr.set_count_in_beats(6);
    tr.seek(0, bar2);
    tr.play(0);
    std::vector<TransportOp> clicks;
    tr.update(sec(10));
    clicks = only(drain(tr), Kind::Click);
    REQUIRE(clicks.size() >= 6);
    // 60 BPM: one-second beats; accent on beats 1 and 4 of a 3/4 count (bar count: 1 2 3 1 2 3).
    std::vector<bool> accents;
    for (std::size_t i = 0; i < 6; ++i) {
        CHECK(clicks[i].at_ns == sec(static_cast<double>(i)));
        accents.push_back(clicks[i].accent);
    }
    CHECK(accents == std::vector<bool>{true, false, false, true, false, false});
    CHECK(tr.position_s() == doctest::Approx(bar2 + 4.0));
}

TEST_CASE("count-in: pause cancels it, and a new play counts in again") {
    Transport t = ready(bars_song(4));
    t.set_count_in_beats(4);
    t.play(0);
    t.update(sec(1));
    t.pause(sec(1));
    CHECK_FALSE(t.counting_in());
    CHECK(t.state() == TransportState::Paused);
    drain(t);
    t.play(sec(5));
    CHECK(t.count_in_beats_left() == 4.0);
    CHECK(only(drain(t), Kind::Click).size() == 1); // the first beat sounds at once
}

TEST_CASE("count-in: seeking while counting in cancels it") {
    Transport t = ready(bars_song(4));
    t.set_count_in_beats(4);
    t.play(0);
    t.seek(sec(0.5), 2);
    CHECK_FALSE(t.counting_in());
    t.update(sec(1));
    CHECK(t.position_s() == 2.5);
}

TEST_CASE("count-in: zero beats means the clock moves at once") {
    Transport t = ready(bars_song(4));
    t.set_count_in_beats(0);
    t.play(0);
    CHECK(t.state() == TransportState::Playing);
    t.update(sec(1));
    CHECK(t.position_s() == 1.0);
    CHECK(only(drain(t), Kind::Click).empty());
}

TEST_CASE("count-in: a hold and release afterwards does not count in again") {
    Transport t = ready(bars_song(4));
    t.set_count_in_beats(2);
    t.play(0);
    t.update(sec(2));
    t.hold(sec(2));
    t.release(sec(3));
    CHECK_FALSE(t.counting_in());
    drain(t);
    t.update(sec(4));
    CHECK(t.position_s() == 2.0);
}

TEST_CASE("count-in: the song's end can't come before the count-in is over") {
    Transport t = ready(plain_song(1));
    t.set_count_in_beats(4);
    t.play(0);
    t.update(sec(1.9)); // plain_song has the default 120 BPM: the count-in lasts 2 s
    CHECK_FALSE(t.ended());
    t.update(sec(3.5));
    CHECK(t.ended());
}

// ---- Metronome ------------------------------------------------------------------------------

TEST_CASE("metronome: clicks on every beat while the song moves, bar start accented") {
    Transport t = ready(bars_song(4));
    t.set_metronome(true);
    t.play(0);
    t.update(sec(2.25));
    std::vector<TransportOp> clicks = only(drain(t), Kind::Click);
    REQUIRE(clicks.size() == 5); // beats at 0, .5, 1, 1.5, 2
    std::vector<int> beats;
    for (const TransportOp& c : clicks) {
        beats.push_back(c.beat);
        CHECK_FALSE(c.count_in);
        CHECK(c.accent == (c.beat == 1));
    }
    CHECK(beats == std::vector<int>{1, 2, 3, 4, 1});
    CHECK(clicks[3].at_ns == sec(1.5));
    CHECK(clicks[3].to_s == 1.5);
}

TEST_CASE("metronome: off by default, and clicks follow the speed") {
    Transport t = ready(bars_song(4));
    t.play(0);
    t.update(sec(1));
    CHECK(only(drain(t), Kind::Click).empty());

    Transport u = ready(bars_song(4));
    u.set_metronome(true);
    u.set_speed_percent(0, 50);
    u.play(0);
    u.update(sec(2.5));
    std::vector<TransportOp> clicks = only(drain(u), Kind::Click);
    REQUIRE(clicks.size() == 3); // song beats at 0, .5, 1 are at 0, 1, 2 s
    CHECK(clicks[2].at_ns == sec(2));
}

TEST_CASE("metronome: no click is lost or doubled across updates, seeks and loop wraps") {
    Transport t = ready(bars_song(4));
    t.set_metronome(true);
    t.play(0);
    for (int i = 1; i <= 8; ++i) {
        t.update(sec(0.3 * i));
    }
    std::vector<TransportOp> clicks = only(drain(t), Kind::Click);
    // 2.4 s of song: beats at 0, .5, 1, 1.5, 2
    CHECK(clicks.size() == 5);

    REQUIRE(t.set_loop(sec(2.4), 2, 3));
    t.seek(sec(2.4), 2);
    t.update(sec(2.4 + 1.25)); // one wrap
    clicks = only(drain(t), Kind::Click);
    std::vector<double> times;
    for (const TransportOp& c : clicks) {
        times.push_back(c.to_s);
    }
    // beats 2, 2.5 of the first pass, then 2, 2.5 again (3 is the loop end, not played)
    CHECK(times == std::vector<double>{2.0, 2.5, 2.0});
}

TEST_CASE("metronome: a pickup bar counts its beats from the end of the bar") {
    Transport t = ready(pickup_song());
    t.set_metronome(true);
    t.play(0);
    t.update(sec(1.25));
    std::vector<TransportOp> clicks = only(drain(t), Kind::Click);
    REQUIRE(clicks.size() == 3);
    CHECK(clicks[0].beat == 4); // the pickup is the last beat of a 4/4 bar, not an accented beat 1
    CHECK_FALSE(clicks[0].accent);
    CHECK(clicks[1].beat == 1); // bar 1 starts at 0.5 s
    CHECK(clicks[1].accent);
    CHECK(clicks[1].to_s == 0.5);
    CHECK(clicks[2].beat == 2);
    CHECK(clicks[2].to_s == 1.0);
}

// ---- Hold (Wait Mode) -----------------------------------------------------------------------

TEST_CASE("hold: stops the clock without a pause, release resumes with no catch-up") {
    Transport t = ready(plain_song(100));
    t.play(0);
    t.hold(sec(1));
    CHECK(t.state() == TransportState::Held);
    CHECK(t.held());
    CHECK(t.playing()); // not a user pause
    t.update(sec(5));
    CHECK(t.position_s() == 1.0);
    t.release(sec(5));
    CHECK(t.state() == TransportState::Playing);
    t.update(sec(6));
    CHECK(t.position_s() == 2.0);
    std::vector<TransportOp> ops = drain(t);
    CHECK(only(ops, Kind::Pause).empty());
    CHECK(only(ops, Kind::Hold).size() == 1);
    CHECK(only(ops, Kind::Release).size() == 1);
}

TEST_CASE("hold: hold_at stops the clock exactly at the song time, not a frame later") {
    Transport t = ready(plain_song(100));
    t.play(0);
    t.hold_at(0, 1.5);
    t.update(sec(3)); // would be 3.0 without the hold point
    CHECK(t.position_s() == 1.5);
    CHECK(t.held());
    std::vector<TransportOp> ops = drain(t);
    REQUIRE(only(ops, Kind::Hold).size() == 1);
    CHECK(only(ops, Kind::Hold)[0].at_ns == sec(1.5));
    CHECK(only(ops, Kind::Hold)[0].to_s == 1.5);
    t.release(sec(3));
    t.update(sec(4));
    CHECK(t.position_s() == 2.5);
    t.update(sec(5)); // the hold point was used up
    CHECK(t.position_s() == 3.5);
}

TEST_CASE("hold: hold_at ignores a time that is not ahead of the position") {
    Transport t = ready(plain_song(100));
    t.play(0);
    t.update(sec(2));
    t.hold_at(sec(2), 2.0);
    t.hold_at(sec(2), 1.0);
    t.update(sec(3));
    CHECK(t.position_s() == 3.0);
    CHECK_FALSE(t.held());
}

TEST_CASE("hold: a hold point and a loop end on the same time wrap first") {
    Transport t = ready(plain_song(10));
    REQUIRE(t.set_loop(0, 2, 4));
    t.seek(0, 3);
    t.play(0);
    t.hold_at(0, 4);
    t.update(sec(1.5));
    CHECK(t.position_s() == 2.5);
    CHECK_FALSE(t.held());
}

TEST_CASE("hold: pausing while held keeps the hold for when play comes back") {
    Transport t = ready(plain_song(100));
    t.play(0);
    t.hold(sec(1));
    t.pause(sec(2));
    CHECK(t.state() == TransportState::Paused);
    t.play(sec(3));
    t.update(sec(4));
    CHECK(t.position_s() == 1.0);
    CHECK(t.state() == TransportState::Held);
    t.release(sec(4));
    t.update(sec(5));
    CHECK(t.position_s() == 2.0);
}

TEST_CASE("hold: the hold point survives a speed change") {
    Transport t = ready(plain_song(100));
    t.play(0);
    t.hold_at(0, 2.0);
    t.set_speed_percent(sec(1), 50);
    t.update(sec(10));
    CHECK(t.position_s() == 2.0);
    std::vector<TransportOp> ops = drain(t);
    CHECK(only(ops, Kind::Hold)[0].at_ns == sec(3)); // 1 s at 100%, then 1 song second at 50%
}
