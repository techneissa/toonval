// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/run_record.hpp"

#include "toonval/midi_event.hpp"
#include "toonval/transport.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

using namespace toonval;

namespace {

EvaluationSummary sample_summary() {
    EvaluationSummary s;
    s.counts = {.perfect = 30, .good = 8, .early = 2, .late = 3, .missed = 5, .played = 0};
    s.wrong = 4;
    s.unplayable = 1;
    s.combo = 7;
    s.longest_combo = 21;
    s.left = {.graded = 20, .hit = 18};
    s.right = {.graded = 28, .hit = 25};
    s.unknown = {.graded = 0, .hit = 0};
    s.all = {.graded = 48, .hit = 43};
    s.measures = {{.measure = 1, .graded = 6, .missed = 0, .wrong = 0},
                  {.measure = 2, .graded = 6, .missed = 3, .wrong = 2},
                  {.measure = 4, .graded = 8, .missed = 1, .wrong = 0}};
    return s;
}

RunContext sample_context() {
    RunContext c;
    c.song_hash = 0x0102030405060708ULL;
    c.song_title = "Für Elise";
    c.mode = RunMode::Flow;
    c.hands = HandFilter::Right;
    c.speed_percent = 80;
    c.transpose_semitones = -2;
    c.start_utc_s = 1'760'000'000;
    c.end_utc_s = 1'760'000'300;
    c.local_day = 20'373;
    c.duration_ms = 287'500;
    return c;
}

RunRecord sample_record() { return make_run_record(sample_summary(), sample_context()); }

PassResult pass(bool clean) {
    PassResult p;
    p.clean = clean;
    return p;
}

} // namespace

TEST_CASE("make_run_record copies the context and the summary") {
    RunRecord r = sample_record();
    CHECK(r.song_hash == 0x0102030405060708ULL);
    CHECK(r.song_title == "Für Elise");
    CHECK(r.mode == RunMode::Flow);
    CHECK(r.hands == HandFilter::Right);
    CHECK(r.speed_percent == 80);
    CHECK(r.transpose_semitones == -2);
    CHECK(r.start_utc_s == 1'760'000'000);
    CHECK(r.end_utc_s == 1'760'000'300);
    CHECK(r.local_day == 20'373);
    CHECK(r.duration_ms == 287'500);
    CHECK(r.counts.perfect == 30);
    CHECK(r.counts.good == 8);
    CHECK(r.counts.early == 2);
    CHECK(r.counts.late == 3);
    CHECK(r.counts.missed == 5);
    CHECK(r.wrong == 4);
    CHECK(r.unplayable == 1);
    CHECK(r.longest_combo == 21);
    CHECK(r.left.graded == 20);
    CHECK(r.left.hit == 18);
    CHECK(r.right.graded == 28);
    CHECK(r.right.hit == 25);
    CHECK(r.all.graded == 48);
    CHECK(r.all.hit == 43);
    REQUIRE(r.measures.size() == 3);
    CHECK(r.measures[1].measure == 2);
    CHECK(r.measures[1].graded == 6);
    CHECK(r.measures[1].missed == 3);
    CHECK(r.measures[1].wrong == 2);
    CHECK(r.clean_passes == 0);
}

TEST_CASE("a real Flow run becomes a record") {
    // Four notes, bars of 2 s: three in bar 1, one in bar 2. The player hits the first three on
    // time and never plays the fourth.
    Song song;
    const double starts[] = {0.5, 1.0, 1.5, 2.5};
    for (double start : starts) {
        Note n;
        n.start_s = start;
        n.duration_s = 0.25;
        n.pitch = 60;
        n.hand = Hand::Right;
        n.measure = start < 2.0 ? 1 : 2;
        n.start_tick = static_cast<std::int64_t>(start * 960);
        n.end_tick = n.start_tick + 240;
        song.notes.push_back(n);
    }
    for (int bar = 0; bar < 2; ++bar) {
        Measure m;
        m.number = bar + 1;
        m.start_s = bar * 2.0;
        m.start_tick = bar * 1920;
        song.measures.push_back(m);
    }
    song.duration_s = 4.0;
    FlowEvaluator eval(song);
    Transport transport;
    transport.set_song(song);
    transport.play(1'000'000'000);
    auto feed = [&](std::int64_t now_ns) {
        transport.update(now_ns);
        std::vector<TransportOp> ops;
        transport.take_ops(ops);
        for (const TransportOp& op : ops) {
            eval.apply(op);
        }
    };
    feed(1'000'000'000);
    for (double start : {0.5, 1.0, 1.5}) {
        MidiEvent e;
        e.time_ns = static_cast<std::int64_t>((1.0 + start) * 1e9);
        e.type = MidiEventType::NoteOn;
        e.data1 = 60;
        e.data2 = 90;
        feed(e.time_ns);
        eval.apply(e);
        eval.update(e.time_ns);
    }
    feed(6'000'000'000);
    eval.update(6'000'000'000);
    eval.finish();
    std::vector<PassResult> passes;
    eval.take_passes(passes);

    RunRecord r = make_run_record(eval.summary(), sample_context(), passes);
    CHECK(r.counts.perfect == 3);
    CHECK(r.counts.missed == 1);
    CHECK(r.all.graded == 4);
    CHECK(r.all.hit == 3);
    CHECK(r.right.graded == 4);
    CHECK(r.left.graded == 0);
    CHECK(r.longest_combo == 3);
    CHECK(r.accuracy(HandFilter::Both) == doctest::Approx(0.75));
    REQUIRE(r.measures.size() == 2);
    CHECK(r.measures[0] == RunBar{1, 3, 0, 0});
    CHECK(r.measures[1] == RunBar{2, 1, 1, 0});
    RunRecord back;
    REQUIRE(parse_run_record(serialize_run_record(r), back) == RunParseStatus::Ok);
    CHECK(back == r);
}

TEST_CASE("accuracy per hand comes from the hit and graded counts") {
    RunRecord r = sample_record();
    CHECK(r.left.accuracy() == doctest::Approx(18.0 / 20.0));
    CHECK(r.right.accuracy() == doctest::Approx(25.0 / 28.0));
    CHECK(r.all.accuracy() == doctest::Approx(43.0 / 48.0));
    CHECK(r.accuracy(HandFilter::Left) == doctest::Approx(18.0 / 20.0));
    CHECK(r.accuracy(HandFilter::Right) == doctest::Approx(25.0 / 28.0));
    CHECK(r.accuracy(HandFilter::Both) == doctest::Approx(43.0 / 48.0));
    CHECK(r.graded_notes() == 48);
}

TEST_CASE("clean passes are counted from the pass results") {
    const std::vector<PassResult> passes = {pass(true), pass(false), pass(true), pass(true)};
    RunRecord r = make_run_record(sample_summary(), sample_context(), passes);
    CHECK(r.clean_passes == 3);
}

TEST_CASE("an empty summary makes a valid empty record") {
    RunRecord r = make_run_record(EvaluationSummary{}, RunContext{});
    CHECK(r.graded_notes() == 0);
    CHECK(r.measures.empty());
    CHECK(r.speed_percent == 100);
    CHECK(r == sanitized(r));
}

TEST_CASE("the clock going backwards cannot make a run end before it started") {
    RunContext c = sample_context();
    c.end_utc_s = c.start_utc_s - 3600;
    RunRecord r = make_run_record(sample_summary(), c);
    CHECK(r.start_utc_s == c.start_utc_s);
    CHECK(r.end_utc_s == c.start_utc_s);
    // The practised time comes from a monotonic clock in the app, so it survives the jump.
    CHECK(r.duration_ms == 287'500);
}

TEST_CASE("sanitized forces every field into its documented range") {
    RunRecord r = sample_record();
    r.speed_percent = 999;
    r.transpose_semitones = -40;
    r.duration_ms = -5;
    r.local_day = 2'000'000'000;
    r.wrong = -3;
    r.left.hit = 99; // more hits than graded notes
    r.all.graded = -1;
    r.counts.perfect = 2'000'000'000;
    RunRecord s = sanitized(r);
    CHECK(s.speed_percent == 150);
    CHECK(s.transpose_semitones == -12);
    CHECK(s.duration_ms == 0);
    CHECK(s.local_day == kMaxRunDay);
    CHECK(s.wrong == 0);
    CHECK(s.left.hit == s.left.graded);
    CHECK(s.all.graded == 0);
    CHECK(s.counts.perfect == kMaxRunCount);
    CHECK(sanitized(s) == s);

    r = sample_record();
    r.speed_percent = 83;
    CHECK(sanitized(r).speed_percent == 85); // steps of 5, as the Transport
    r.duration_ms = kMaxRunDurationMs + 1;
    CHECK(sanitized(r).duration_ms == kMaxRunDurationMs);
}

TEST_CASE("sanitized sorts the bars, merges repeated bars and caps misses at the graded notes") {
    RunRecord r = sample_record();
    r.measures = {{.measure = 9, .graded = 4, .missed = 1, .wrong = 0},
                  {.measure = 3, .graded = 4, .missed = 9, .wrong = 1},
                  {.measure = 9, .graded = 2, .missed = 1, .wrong = 2},
                  {.measure = -4, .graded = 1, .missed = 0, .wrong = 0},
                  {.measure = 1'000'000, .graded = 1, .missed = 0, .wrong = 0}};
    RunRecord s = sanitized(r);
    REQUIRE(s.measures.size() == 4);
    CHECK(s.measures[0].measure == 0); // out of range clamps to the nearest bar
    CHECK(s.measures[3].measure == kMaxRunMeasureNumber);
    CHECK(s.measures[1].measure == 3);
    CHECK(s.measures[1].missed == 4); // not more misses than graded notes
    CHECK(s.measures[1].wrong == 1);
    CHECK(s.measures[2].measure == 9);
    CHECK(s.measures[2].graded == 6);
    CHECK(s.measures[2].missed == 2);
    CHECK(s.measures[2].wrong == 2);
    CHECK(sanitized(s) == s);
}

TEST_CASE("a title is cut at a character boundary and loses control characters") {
    RunRecord r = sample_record();
    r.song_title = std::string("A\tB\nC") + '\x7f';
    CHECK(sanitized(r).song_title == "ABC");

    r.song_title = "bad \xC3\x28 utf8";
    CHECK(sanitized(r).song_title.empty());

    r.song_title.clear();
    for (int i = 0; i < 200; ++i) {
        r.song_title += "é"; // 2 bytes each: 400 bytes
    }
    std::string cut = sanitized(r).song_title;
    CHECK(cut.size() <= kMaxRunTitleBytes);
    CHECK(cut.size() % 2 == 0);
    CHECK(cut.size() == 256);
}

TEST_CASE("local_day_from_utc applies the offset and floors toward the past") {
    CHECK(local_day_from_utc(0, 0) == 0);
    CHECK(local_day_from_utc(86'399, 0) == 0);
    CHECK(local_day_from_utc(86'400, 0) == 1);
    CHECK(local_day_from_utc(-1, 0) == -1); // the second before 1970
    // 2026-10-12 23:30 UTC is already the 13th in Berlin (UTC+2 in summer) and still the 12th in
    // New York (UTC-4).
    const std::int64_t t = 1'791'847'800;
    CHECK(local_day_from_utc(t, 2 * 3600) == local_day_from_utc(t, 0) + 1);
    CHECK(local_day_from_utc(t, -4 * 3600) == local_day_from_utc(t, 0));
    // The 5 hours either side of midnight local time.
    CHECK(local_day_from_utc(86'400 * 100 + 5 * 3600 - 1, -5 * 3600) == 99);
    CHECK(local_day_from_utc(86'400 * 100 + 5 * 3600, -5 * 3600) == 100);
}

TEST_CASE("day keys convert to calendar dates and back") {
    CHECK(civil_from_day(0).year == 1970);
    CHECK(civil_from_day(0).month == 1);
    CHECK(civil_from_day(0).day == 1);
    CivilDate leap = civil_from_day(day_from_civil({2024, 2, 29}));
    CHECK(leap.year == 2024);
    CHECK(leap.month == 2);
    CHECK(leap.day == 29);
    CHECK(day_from_civil({2024, 3, 1}) - day_from_civil({2024, 2, 28}) == 2);
    CHECK(day_from_civil({2025, 3, 1}) - day_from_civil({2025, 2, 28}) == 1);
    CHECK(day_from_civil({2000, 3, 1}) - day_from_civil({1900, 3, 1}) == 36'525);
    CHECK(day_from_civil({1969, 12, 31}) == -1);
    for (std::int32_t d = -800; d < 60'000; d += 37) {
        CivilDate c = civil_from_day(d);
        CHECK(day_from_civil(c) == d);
    }
}

TEST_CASE("serialization round-trips a record exactly") {
    RunRecord r = sample_record();
    std::string bytes = serialize_run_record(r);
    RunRecord back;
    REQUIRE(parse_run_record(bytes, back) == RunParseStatus::Ok);
    CHECK(back == r);
}

TEST_CASE("serialization round-trips the extremes") {
    RunRecord r;
    r.song_hash = ~0ULL;
    r.song_title = std::string(kMaxRunTitleBytes, 'x');
    r.mode = RunMode::Wait;
    r.hands = HandFilter::Left;
    r.speed_percent = 25;
    r.transpose_semitones = 12;
    r.start_utc_s = kMaxRunUtcSeconds;
    r.end_utc_s = kMaxRunUtcSeconds;
    r.local_day = -kMaxRunDay;
    r.duration_ms = kMaxRunDurationMs;
    r.counts = {kMaxRunCount, kMaxRunCount, kMaxRunCount, kMaxRunCount, kMaxRunCount, kMaxRunCount};
    r.wrong = kMaxRunCount;
    r.unplayable = kMaxRunCount;
    r.longest_combo = kMaxRunCount;
    r.clean_passes = kMaxRunCount;
    r.left = {kMaxRunCount, kMaxRunCount};
    r.right = {kMaxRunCount, 0};
    r.unknown = {1, 1};
    r.all = {kMaxRunCount, kMaxRunCount};
    for (int m = 0; m < 3000; ++m) {
        r.measures.push_back({m * 30, 8, m % 9, m % 5});
    }
    r.measures.push_back({kMaxRunMeasureNumber, kMaxRunCount, kMaxRunCount, kMaxRunCount});
    r = sanitized(r);
    RunRecord back;
    REQUIRE(parse_run_record(serialize_run_record(r), back) == RunParseStatus::Ok);
    CHECK(back == r);
    CHECK(back.measures.size() == r.measures.size());
}

TEST_CASE("serialize sanitizes, so what is stored always parses back to a valid record") {
    RunRecord r = sample_record();
    r.speed_percent = 7;
    r.measures.push_back({.measure = 2, .graded = 1, .missed = 5, .wrong = 0});
    RunRecord back;
    REQUIRE(parse_run_record(serialize_run_record(r), back) == RunParseStatus::Ok);
    CHECK(back == sanitized(r));
}

TEST_CASE("the version 1 layout is fixed") {
    // An independent description of the bytes, written from the format comment in run_record.hpp,
    // so a change of layout cannot pass by editing the serializer alone.
    RunRecord zero;
    std::string expect = "TR";
    expect += '\x01';
    expect += std::string(8, '\0'); // song hash
    expect += '\0';                 // title length
    expect += '\0';                 // mode Flow
    expect += '\0';                 // hands Both
    expect += '\x64';               // speed 100
    expect += std::string(5, '\0'); // transpose, start, end-start, day, duration
    expect += std::string(6, '\0'); // grade counts
    expect += std::string(4, '\0'); // wrong, unplayable, longest combo, clean passes
    expect += std::string(8, '\0'); // left, right, unknown, all
    expect += '\0';                 // bar count
    CHECK(serialize_run_record(zero) == expect);
    CHECK(expect.size() == 39);

    RunRecord r;
    r.song_hash = 0x0102030405060708ULL;
    r.song_title = "ab";
    r.mode = RunMode::Wait;
    r.hands = HandFilter::Right;
    r.speed_percent = 150;
    r.transpose_semitones = -3; // zigzag 5
    r.start_utc_s = 300;        // zigzag 600: D8 04
    r.end_utc_s = 301;          // 1 after the start
    r.local_day = -1;           // zigzag 1
    r.duration_ms = 300;        // varint: AC 02
    r.counts.perfect = 128;     // varint: 80 01
    r.all = {.graded = 5, .hit = 4};
    r.measures = {{.measure = 10, .graded = 3, .missed = 1, .wrong = 0},
                  {.measure = 12, .graded = 3, .missed = 0, .wrong = 2}};
    std::string want;
    want += "TR";
    want += '\x01';
    want += "\x08\x07\x06\x05\x04\x03\x02\x01";
    want += '\x02';
    want += "ab";
    want += '\x01';     // Wait
    want += '\x02';     // Right
    want += "\x96\x01"; // 150
    want += '\x05';
    want += "\xD8\x04";
    want += '\x01';
    want += '\x01';
    want += "\xAC\x02";
    want += "\x80\x01";           // perfect
    want += std::string(5, '\0'); // good early late missed played
    want += std::string(4, '\0');
    want += std::string(6, '\0');               // left, right, unknown
    want += '\x05';                             // all.graded
    want += '\x04';                             // all.hit
    want += '\x02';                             // two bars
    want += std::string("\x0A\x03\x01\x00", 4); // bar 10: graded 3, missed 1, wrong 0
    want += std::string("\x02\x03\x00\x02", 4); // bar 12 as +2: graded 3, missed 0, wrong 2
    CHECK(serialize_run_record(r) == want);
}

TEST_CASE("a typical record is small") {
    RunRecord r = sample_record();
    for (int m = 5; m < 45; ++m) {
        r.measures.push_back({m, 8, m % 3, m % 2});
    }
    CHECK(serialize_run_record(r).size() < 300);
}

TEST_CASE("parse rejects what is not a record and leaves the output alone") {
    RunRecord keep = sample_record();
    RunRecord out = keep;
    CHECK(parse_run_record("", out) == RunParseStatus::Truncated);
    CHECK(parse_run_record("T", out) == RunParseStatus::Truncated);
    CHECK(parse_run_record("XRxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", out) ==
          RunParseStatus::BadMagic);
    std::string bytes = serialize_run_record(keep);
    bytes[2] = '\x02';
    CHECK(parse_run_record(bytes, out) == RunParseStatus::NewerVersion);
    bytes[2] = '\x00';
    CHECK(parse_run_record(bytes, out) == RunParseStatus::Corrupt);
    bytes = serialize_run_record(keep) + "x";
    CHECK(parse_run_record(bytes, out) == RunParseStatus::Corrupt); // trailing bytes
    CHECK(out == keep);
}

TEST_CASE("every strict prefix of a record is rejected") {
    std::string bytes = serialize_run_record(sample_record());
    RunRecord out;
    for (std::size_t n = 0; n < bytes.size(); ++n) {
        CAPTURE(n);
        CHECK(parse_run_record(std::string_view(bytes).substr(0, n), out) != RunParseStatus::Ok);
    }
}

TEST_CASE("bad enum values and overlong numbers are rejected") {
    std::string good = serialize_run_record(sample_record());
    RunRecord out;
    std::string bytes = good;
    const std::size_t mode_at = 3 + 8 + 1 + std::string("Für Elise").size();
    bytes[mode_at] = '\x07';
    CHECK(parse_run_record(bytes, out) == RunParseStatus::Corrupt);
    bytes = good;
    bytes[mode_at + 1] = '\x09';
    CHECK(parse_run_record(bytes, out) == RunParseStatus::Corrupt);

    // A varint of 11 continuation bytes where the speed goes.
    bytes = good.substr(0, mode_at + 2) + std::string(11, '\x80') + good.substr(mode_at + 3);
    CHECK(parse_run_record(bytes, out) != RunParseStatus::Ok);

    // A title length that runs past the end.
    bytes = good;
    bytes[11] = '\x7f';
    CHECK(parse_run_record(bytes, out) != RunParseStatus::Ok);

    // A claim of millions of bars with no bytes behind it does not allocate or loop.
    bytes = serialize_run_record(RunRecord{});
    bytes.back() = '\xff';
    bytes += "\xff\xff\xff\x7f";
    CHECK(parse_run_record(bytes, out) == RunParseStatus::Corrupt);
}

TEST_CASE("fuzz: random bytes never crash, and anything accepted is valid and stable") {
    std::mt19937 rng(0x70f1e5);
    const std::string seed_bytes = serialize_run_record(sample_record());
    int accepted = 0;
    for (int iter = 0; iter < 20'000; ++iter) {
        std::string bytes;
        switch (iter % 4) {
        case 0: { // pure noise
            bytes.resize(rng() % 120);
            for (char& c : bytes) {
                c = static_cast<char>(rng());
            }
            break;
        }
        case 1: { // noise behind a valid header
            bytes = "TR";
            bytes += '\x01';
            std::size_t n = rng() % 100;
            for (std::size_t i = 0; i < n; ++i) {
                bytes += static_cast<char>(rng());
            }
            break;
        }
        case 2: { // a valid record with a few bytes flipped
            bytes = seed_bytes;
            int flips = 1 + static_cast<int>(rng() % 4);
            for (int f = 0; f < flips; ++f) {
                bytes[rng() % bytes.size()] = static_cast<char>(rng());
            }
            break;
        }
        default: { // a valid record cut, grown or with a byte removed
            bytes = seed_bytes;
            switch (rng() % 3) {
            case 0:
                bytes.resize(rng() % bytes.size());
                break;
            case 1:
                bytes.insert(rng() % bytes.size(), 1, static_cast<char>(rng()));
                break;
            default:
                bytes.erase(rng() % bytes.size(), 1);
                break;
            }
        }
        }
        CAPTURE(iter);
        RunRecord r;
        RunParseStatus status = parse_run_record(bytes, r);
        if (status == RunParseStatus::Ok) {
            ++accepted;
            REQUIRE(r == sanitized(r));
            RunRecord again;
            REQUIRE(parse_run_record(serialize_run_record(r), again) == RunParseStatus::Ok);
            REQUIRE(again == r);
        }
    }
    CHECK(accepted > 0); // the flipped-byte cases keep some valid, so the check above ran
}
