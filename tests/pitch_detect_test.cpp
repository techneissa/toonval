// SPDX-License-Identifier: GPL-3.0-or-later
#include "pitch_tones.hpp"
#include "toonval/pitch.hpp"
#include "toonval/pitch_detect.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

#include <doctest/doctest.h>

using namespace toonval;

namespace {

using namespace pitch_test;

std::vector<PitchEvent> run(PitchDetector& d, const std::vector<float>& audio, int block = 256) {
    std::vector<PitchEvent> events;
    PitchEvent buf[16];
    for (size_t i = 0; i < audio.size(); i += static_cast<size_t>(block)) {
        const int n =
            static_cast<int>(std::min<size_t>(static_cast<size_t>(block), audio.size() - i));
        const int got = d.process(audio.data() + i, n, buf, 16);
        events.insert(events.end(), buf, buf + got);
    }
    return events;
}

int count_type(const std::vector<PitchEvent>& ev, PitchEventType type) {
    int n = 0;
    for (const auto& e : ev) {
        n += e.type == type ? 1 : 0;
    }
    return n;
}

const PitchEvent* first_of(const std::vector<PitchEvent>& ev, PitchEventType type) {
    for (const auto& e : ev) {
        if (e.type == type) {
            return &e;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("pitch detector: a steady A4 is reported once as note 69, by both methods") {
    for (auto method : {PitchMethod::Mpm, PitchMethod::Yin}) {
        CAPTURE(static_cast<int>(method));
        PitchDetectorConfig cfg;
        cfg.method = method;
        PitchDetector d(cfg);
        const auto audio = tone(440.0, rich(0.3), kRate / 2, 4800);
        const auto ev = run(d, audio);
        REQUIRE(count_type(ev, PitchEventType::NoteOn) == 1);
        const auto* on = first_of(ev, PitchEventType::NoteOn);
        CHECK(on->note == 69);
        CHECK(on->frequency_hz == doctest::Approx(440.0).epsilon(0.01));
        CHECK(on->confidence >= 0.8f);
        CHECK(on->onset_sample >= 4800 - 256);
        CHECK(on->onset_sample <= 4800 + 256);
        CHECK(count_type(ev, PitchEventType::NotSure) == 0);
    }
}

TEST_CASE("pitch detector: silence and noise under the gate produce no events") {
    PitchDetector d;
    std::vector<float> audio(kRate, 0.0f);
    CHECK(run(d, audio).empty());
    std::uint32_t state = 12345;
    for (auto& v : audio) {
        state = state * 1664525u + 1013904223u;
        v = (static_cast<float>(state >> 8) / 16777216.0f - 0.5f) * 2e-4f; // about -80 dBFS
    }
    CHECK(run(d, audio).empty());
}

TEST_CASE("pitch detector: a bass note with a weak fundamental is not reported an octave up") {
    // A1 (55 Hz) shaped like a piano bass string: the fundamental is far weaker than partials 2-5.
    PitchDetector d;
    const auto audio =
        tone(55.0, {0.02, 0.10, 0.15, 0.15, 0.10, 0.08, 0.05, 0.04, 0.03, 0.02}, kRate, 4800);
    const auto ev = run(d, audio);
    REQUIRE(count_type(ev, PitchEventType::NoteOn) == 1);
    CHECK(first_of(ev, PitchEventType::NoteOn)->note == 33);
}

TEST_CASE("pitch detector: a strong second partial does not make the note an octave higher") {
    PitchDetector d;
    const auto audio = tone(440.0, {0.08, 0.3, 0.05, 0.05, 0.04, 0.03, 0.02}, kRate / 2, 4800);
    const auto ev = run(d, audio);
    REQUIRE(count_type(ev, PitchEventType::NoteOn) == 1);
    CHECK(first_of(ev, PitchEventType::NoteOn)->note == 69);
}

TEST_CASE("pitch detector: a click with no stable pitch is not reported as a note") {
    PitchDetector d;
    std::vector<float> audio(kRate / 2, 0.0f);
    std::uint32_t state = 99;
    for (int i = 4800; i < 4800 + 200; ++i) {
        state = state * 1664525u + 1013904223u;
        const float decay = std::exp(-static_cast<float>(i - 4800) / 40.0f);
        audio[static_cast<size_t>(i)] =
            (static_cast<float>(state >> 8) / 16777216.0f - 0.5f) * 0.8f * decay;
    }
    const auto ev = run(d, audio);
    CHECK(count_type(ev, PitchEventType::NoteOn) == 0);
}

TEST_CASE("pitch detector: a new onset ends the previous note before the next one starts") {
    PitchDetector d;
    auto audio = tone(440.0, rich(0.3), kRate / 2, 2400);
    const auto second = tone(523.2511, rich(0.3), kRate / 2);
    audio.insert(audio.end(), second.begin(), second.end());
    std::vector<float> silence(kRate / 4, 0.0f);
    audio.insert(audio.end(), silence.begin(), silence.end());
    const auto ev = run(d, audio);
    REQUIRE(ev.size() == 4);
    CHECK(ev[0].type == PitchEventType::NoteOn);
    CHECK(ev[0].note == 69);
    CHECK(ev[1].type == PitchEventType::NoteOff);
    CHECK(ev[1].note == 69);
    CHECK(ev[2].type == PitchEventType::NoteOn);
    CHECK(ev[2].note == 72);
    CHECK(ev[2].onset_sample >= kRate / 2 - 256);
    CHECK(ev[3].type == PitchEventType::NoteOff); // the silence at the end
    CHECK(ev[3].note == 72);
}

TEST_CASE("pitch detector: a pitch halfway between two keys is not sure, not a guess") {
    PitchDetector d;
    const double quarter_tone = 440.0 * std::exp2(0.5 / 12.0);
    const auto ev = run(d, tone(quarter_tone, rich(0.3), kRate / 2, 4800));
    CHECK(count_type(ev, PitchEventType::NoteOn) == 0);
    CHECK(count_type(ev, PitchEventType::NotSure) == 1);
}

TEST_CASE("pitch detector: the A4 reference moves the note grid") {
    PitchDetectorConfig cfg;
    cfg.a4_hz = 415.0f; // baroque pitch: 440 Hz is now A#4
    PitchDetector d(cfg);
    const auto ev = run(d, tone(440.0, rich(0.3), kRate / 2, 4800));
    REQUIRE(count_type(ev, PitchEventType::NoteOn) == 1);
    CHECK(first_of(ev, PitchEventType::NoteOn)->note == 70);
}

TEST_CASE("pitch detector: learns the tuning from a few confident notes") {
    PitchDetectorConfig cfg;
    cfg.learn_tuning = true;
    PitchDetector d(cfg);
    const double sharp = std::exp2(20.0 / 1200.0); // the whole instrument 20 cents sharp
    std::vector<float> audio;
    for (int note : {60, 64, 67, 72, 69, 65}) {
        const auto t =
            tone(440.0 * std::exp2((note - 69) / 12.0) * sharp, rich(0.3), kRate / 3, 1200);
        audio.insert(audio.end(), t.begin(), t.end());
    }
    const auto ev = run(d, audio);
    CHECK(count_type(ev, PitchEventType::NoteOn) == 6);
    const double learned_cents = 1200.0 * std::log2(d.a4_hz() / 440.0);
    CHECK(learned_cents == doctest::Approx(20.0).epsilon(0.15));
}

TEST_CASE("pitch detector: the calibrated noise floor raises the gate") {
    PitchDetector d;
    std::vector<float> noise(kRate, 0.0f);
    std::uint32_t state = 7;
    for (auto& v : noise) {
        state = state * 1664525u + 1013904223u;
        v = (static_cast<float>(state >> 8) / 16777216.0f - 0.5f) * 0.02f; // RMS about 0.0058
    }
    d.begin_noise_calibration();
    CHECK(run(d, noise).empty());
    d.end_noise_calibration();
    CHECK(d.noise_floor_rms() == doctest::Approx(0.0058).epsilon(0.1));
    // A tone at about the noise level stays under the gate (floor x 4).
    auto quiet = tone(440.0, {0.006}, kRate / 2, 0);
    for (size_t i = 0; i < quiet.size(); ++i) {
        quiet[i] += noise[i];
    }
    CHECK(count_type(run(d, quiet), PitchEventType::NoteOn) == 0);
}

TEST_CASE("pitch detector: events that do not fit are counted, not written past the buffer") {
    PitchDetector d;
    auto audio = tone(440.0, rich(0.3), kRate / 2, 2400);
    PitchEvent none[1];
    int total = 0;
    for (size_t i = 0; i + 256 <= audio.size(); i += 256) {
        total += d.process(audio.data() + i, 256, none, 0);
    }
    CHECK(total == 0);
    CHECK(d.dropped_events() == 1); // the note-on
    PitchEvent one[1];
    std::vector<float> silence(kRate / 4, 0.0f);
    int got = 0;
    for (size_t i = 0; i + 256 <= silence.size(); i += 256) {
        got += d.process(silence.data() + i, 256, one, 1);
    }
    CHECK(got == 1);
    CHECK(one[0].type == PitchEventType::NoteOff);
}

TEST_CASE("pitch detector: bass notes need a longer window than treble notes") {
    const int a0 = pitch_window_samples(21, kRate);
    const int c2 = pitch_window_samples(36, kRate);
    const int c4 = pitch_window_samples(60, kRate);
    const int c6 = pitch_window_samples(84, kRate);
    CHECK(a0 > c2);
    CHECK(c2 > c4);
    CHECK(c4 > c6);
    CHECK(a0 >= 2 * kRate / 55);    // at least two periods of A0
    CHECK(c4 <= kRate * 20 / 1000); // middle C leaves room in the 40 ms budget
}

namespace {

// The note-on and not-sure events of a run, for the tests of what must never be a note.
struct Outcome {
    int note_ons = 0;
    int not_sures = 0;
    const PitchEvent* not_sure = nullptr;
};

Outcome outcome_of(const std::vector<PitchEvent>& ev) {
    Outcome o;
    o.note_ons = count_type(ev, PitchEventType::NoteOn);
    o.not_sures = count_type(ev, PitchEventType::NotSure);
    o.not_sure = first_of(ev, PitchEventType::NotSure);
    return o;
}

// A note that ends as "not sure": exactly one NotSure event with no note and no frequency, and
// no NoteOn at all.
void check_only_not_sure(const std::vector<PitchEvent>& ev, std::int64_t onset) {
    const Outcome o = outcome_of(ev);
    CHECK(o.note_ons == 0);
    REQUIRE(o.not_sures == 1);
    CHECK(o.not_sure->note == -1);
    CHECK(o.not_sure->frequency_hz == 0.0f);
    CHECK(o.not_sure->onset_sample >= onset - 256);
    CHECK(o.not_sure->onset_sample <= onset + 256);
    CHECK(o.not_sure->sample > o.not_sure->onset_sample);
}

} // namespace

TEST_CASE("pitch detector: 30 cents from a key is the hard limit") {
    static_assert(kMaxCentsFromKey == 30.0f);
    for (int note : {60, 69}) {
        for (double sign : {1.0, -1.0}) {
            CAPTURE(note);
            CAPTURE(sign);
            {
                PitchDetector d;
                const auto ev =
                    run(d, tone(grid_hz(note, sign * 29.0), rich(0.3), kRate / 2, 4800));
                REQUIRE(count_type(ev, PitchEventType::NoteOn) == 1);
                CHECK(first_of(ev, PitchEventType::NoteOn)->note == note);
                CHECK(count_type(ev, PitchEventType::NotSure) == 0);
            }
            for (double cents : {31.0, 32.0, 45.0}) {
                CAPTURE(cents);
                PitchDetector d;
                const auto ev =
                    run(d, tone(grid_hz(note, sign * cents), rich(0.3), kRate / 2, 4800));
                check_only_not_sure(ev, 4800);
            }
        }
    }
}

TEST_CASE("pitch detector: the 30 cent limit follows the tuning, set and learned") {
    // Notes on a 442 Hz grid: 29 cents off that grid is a note, 31 is not, and neither is the
    // same pitch judged against 440.
    PitchDetector d;
    d.set_a4_hz(442.0f);
    CHECK(d.a4_hz() == 442.0f);
    const auto in_tune = run(d, tone(grid_hz(60, 0.0, 442.0), rich(0.3), kRate / 2, 4800));
    REQUIRE(count_type(in_tune, PitchEventType::NoteOn) == 1);
    CHECK(first_of(in_tune, PitchEventType::NoteOn)->note == 60);
    d.reset();
    const auto off29 = run(d, tone(grid_hz(60, 29.0, 442.0), rich(0.3), kRate / 2, 4800));
    REQUIRE(count_type(off29, PitchEventType::NoteOn) == 1);
    CHECK(first_of(off29, PitchEventType::NoteOn)->note == 60);
    d.reset();
    check_only_not_sure(run(d, tone(grid_hz(60, 31.0, 442.0), rich(0.3), kRate / 2, 4800)), 4800);

    // A pitch 45 cents sharp of 440 is not a note, but is one once the tuning is learned from an
    // instrument that is 25 cents sharp throughout.
    PitchDetectorConfig cfg;
    cfg.learn_tuning = true;
    PitchDetector learner(cfg);
    std::vector<float> audio;
    for (int note : {60, 64, 67, 72, 69, 65, 62, 71}) {
        const auto t = tone(grid_hz(note, 25.0), rich(0.3), kRate / 3, 1200);
        audio.insert(audio.end(), t.begin(), t.end());
    }
    const auto ev = run(learner, audio);
    // Every key is heard (the abrupt cut between two clips may repeat a note, never name another).
    int heard[128] = {};
    for (const auto& e : ev) {
        if (e.type == PitchEventType::NoteOn) {
            ++heard[e.note];
        }
    }
    for (int note : {60, 64, 67, 72, 69, 65, 62, 71}) {
        CAPTURE(note);
        CHECK(heard[note] >= 1);
        heard[note] = 0;
    }
    for (int note = 0; note < 128; ++note) {
        CHECK(heard[note] == 0);
    }
    learner.reset(); // keeps what was learned
    const auto sharp = run(learner, tone(grid_hz(66, 45.0), rich(0.3), kRate / 2, 4800));
    // 45 cents above 440 is only 20 cents above the learned tuning (about +25).
    REQUIRE(count_type(sharp, PitchEventType::NoteOn) == 1);
    CHECK(first_of(sharp, PitchEventType::NoteOn)->note == 66);
    PitchDetector fresh;
    check_only_not_sure(run(fresh, tone(grid_hz(66, 45.0), rich(0.3), kRate / 2, 4800)), 4800);
}

TEST_CASE("pitch detector: a learned tuning is limited to 50 cents from the set one") {
    PitchDetectorConfig cfg;
    cfg.learn_tuning = true;
    PitchDetector d(cfg);
    // The instrument drifts 25, then 50, then 75 cents sharp. Each stage is within 30 cents of
    // what was learned before it, so every note is heard, but the tuning stops at 50 cents.
    int heard = 0;
    for (double sharp : {25.0, 50.0, 75.0}) {
        std::vector<float> audio;
        for (int note : {60, 64, 67, 72, 69, 65}) {
            const auto t = tone(grid_hz(note, sharp), rich(0.3), kRate / 3, 1200);
            audio.insert(audio.end(), t.begin(), t.end());
        }
        heard += count_type(run(d, audio), PitchEventType::NoteOn);
        const double learned_cents = 1200.0 * std::log2(d.a4_hz() / 440.0);
        CAPTURE(sharp);
        CHECK(learned_cents <= 50.01);
    }
    CHECK(heard >= 12);
    CHECK(1200.0 * std::log2(d.a4_hz() / 440.0) == doctest::Approx(50.0).epsilon(0.01));
    // Setting the tuning again forgets what was learned.
    d.set_a4_hz(440.0f);
    CHECK(d.a4_hz() == 440.0f);
}

TEST_CASE("pitch detector: an octave the check cannot decide is not sure, never a guess") {
    // Odd partials at about a tenth of their natural level: too many to be the octave above,
    // too few to be the note itself.
    for (int note : {60, 69, 72}) {
        for (double odd : {0.08, 0.1}) {
            CAPTURE(note);
            CAPTURE(odd);
            PitchDetector d;
            check_only_not_sure(run(d, tone(grid_hz(note), rich(0.3, odd), kRate / 2, 4800)), 4800);
        }
    }
    // The same tone with its odd partials back at full level is just the note.
    PitchDetector d;
    const auto ev = run(d, tone(grid_hz(60), rich(0.3), kRate / 2, 4800));
    CHECK(first_of(ev, PitchEventType::NoteOn)->note == 60);
}

TEST_CASE("pitch detector: unpitched noise, short or long, ends as an explicit not sure") {
    PitchDetector long_burst;
    check_only_not_sure(run(long_burst, noise(kRate / 2, 0.3f, 5, 4800)), 4800);
    // A burst that ends before the deadline: the gate closing is what decides.
    PitchDetector short_burst;
    check_only_not_sure(run(short_burst, noise(kRate / 2, 0.3f, 5, 4800, 3000)), 4800);
}

TEST_CASE("pitch detector: null input does nothing and returns 0") {
    PitchDetector d;
    PitchEvent events[4];
    CHECK(d.process(nullptr, 256, events, 4) == 0);
    CHECK(d.process(nullptr, 256, nullptr, 0) == 0);
    CHECK(d.process(nullptr, 1 << 20, events, 4) == 0);
    const float one = 0.1f;
    CHECK(d.process(&one, 0, events, 4) == 0);
    CHECK(d.process(&one, -5, events, 4) == 0);
    CHECK(d.samples_processed() == 0);
    CHECK(d.dropped_events() == 0);
    // It still works afterwards.
    const auto ev = run(d, tone(440.0, rich(0.3), kRate / 2, 4800));
    CHECK(count_type(ev, PitchEventType::NoteOn) == 1);
}

TEST_CASE("pitch detector: note_from_hz is a fractional MIDI note, -1 for nonsense") {
    CHECK(note_from_hz(440.0f, 440.0f) == doctest::Approx(69.0f));
    CHECK(note_from_hz(880.0f, 440.0f) == doctest::Approx(81.0f));
    CHECK(note_from_hz(261.6256f, 440.0f) == doctest::Approx(60.0f).epsilon(1e-4));
    CHECK(note_from_hz(442.0f, 442.0f) == doctest::Approx(69.0f));
    // 50 cents sharp of A4 is half way to A#4.
    CHECK(note_from_hz(440.0f * std::exp2(0.5f / 12.0f), 440.0f) == doctest::Approx(69.5f));
    CHECK(note_from_hz(0.0f, 440.0f) == -1.0f);
    CHECK(note_from_hz(-10.0f, 440.0f) == -1.0f);
    CHECK(note_from_hz(440.0f, 0.0f) == -1.0f);
    CHECK(note_from_hz(440.0f, -440.0f) == -1.0f);
    CHECK(note_from_hz(std::nanf(""), 440.0f) == -1.0f);
    CHECK(note_from_hz(440.0f, std::nanf("")) == -1.0f);
}

TEST_CASE("pitch detector: samples_processed counts what was fed") {
    PitchDetector d;
    CHECK(d.samples_processed() == 0);
    const std::vector<float> silence(1000, 0.0f);
    d.process(silence.data(), 1000, nullptr, 0);
    CHECK(d.samples_processed() == 1000);
    d.process(silence.data(), 24, nullptr, 0);
    CHECK(d.samples_processed() == 1024);
}

TEST_CASE("pitch detector: reset forgets the audio and note, keeps the calibration") {
    PitchDetector d;
    d.set_noise_floor_rms(0.0005f);
    d.set_a4_hz(442.0f);
    const auto audio = tone(grid_hz(69, 0.0, 442.0), rich(0.3), kRate / 2, 4800);
    const auto first = run(d, audio);
    REQUIRE(count_type(first, PitchEventType::NoteOn) == 1);
    CHECK(d.samples_processed() == static_cast<std::int64_t>(audio.size()));

    d.reset(); // mid-note: forgotten without a note-off
    CHECK(d.samples_processed() == 0);
    CHECK(d.noise_floor_rms() == doctest::Approx(0.0005f));
    CHECK(d.a4_hz() == 442.0f);
    const auto second = run(d, audio);
    CHECK(count_type(second, PitchEventType::NoteOff) == 0);
    REQUIRE(count_type(second, PitchEventType::NoteOn) == 1);
    CHECK(first_of(second, PitchEventType::NoteOn)->note == 69);
    CHECK(first_of(second, PitchEventType::NoteOn)->onset_sample ==
          first_of(first, PitchEventType::NoteOn)->onset_sample);
}

TEST_CASE("pitch detector: set_noise_floor_rms moves the gate; a negative floor is 0") {
    // A quiet A4 (RMS about 0.007) that the default gate lets through.
    const auto quiet = tone(440.0, rich(0.01), kRate / 2, 4800);
    PitchDetector open;
    CHECK(count_type(run(open, quiet), PitchEventType::NoteOn) == 1);

    PitchDetector gated;
    gated.set_noise_floor_rms(0.01f); // gate at four times that
    CHECK(gated.noise_floor_rms() == doctest::Approx(0.01f));
    CHECK(run(gated, quiet).empty());

    gated.set_noise_floor_rms(-1.0f);
    CHECK(gated.noise_floor_rms() == 0.0f);
    CHECK(count_type(run(gated, quiet), PitchEventType::NoteOn) == 1);
}

TEST_CASE("pitch detector: set_a4_hz moves the grid and ignores nonsense") {
    PitchDetector d;
    d.set_a4_hz(442.0f);
    CHECK(d.a4_hz() == 442.0f);
    const auto ev = run(d, tone(442.0, rich(0.3), kRate / 2, 4800));
    REQUIRE(count_type(ev, PitchEventType::NoteOn) == 1);
    CHECK(first_of(ev, PitchEventType::NoteOn)->note == 69);
    CHECK(first_of(ev, PitchEventType::NoteOn)->frequency_hz ==
          doctest::Approx(442.0).epsilon(0.005));
    d.reset();
    // 440 Hz is 8 cents flat of that grid: still A4.
    const auto flat = run(d, tone(440.0, rich(0.3), kRate / 2, 4800));
    REQUIRE(count_type(flat, PitchEventType::NoteOn) == 1);
    CHECK(first_of(flat, PitchEventType::NoteOn)->note == 69);

    for (float bad : {0.0f, -440.0f, std::nanf(""), INFINITY}) {
        d.set_a4_hz(bad);
        CHECK(d.a4_hz() == 442.0f);
    }
}

TEST_CASE("pitch detector: set_method switches between YIN and McLeod on a live detector") {
    PitchDetector d; // McLeod by default
    const auto audio = tone(440.0, rich(0.3), kRate / 2, 4800);
    for (auto method : {PitchMethod::Yin, PitchMethod::Mpm, PitchMethod::Yin}) {
        CAPTURE(static_cast<int>(method));
        d.set_method(method);
        d.reset();
        const auto ev = run(d, audio);
        REQUIRE(count_type(ev, PitchEventType::NoteOn) == 1);
        CHECK(first_of(ev, PitchEventType::NoteOn)->note == 69);
    }
}

TEST_CASE("pitch detector: a moved-to detector keeps working, mid-note and when assigned") {
    const auto audio = tone(440.0, rich(0.3), kRate / 2, 4800);
    const size_t split = 5120; // 320 samples after the onset, before the note is confirmed
    const std::vector<float> head(audio.begin(), audio.begin() + static_cast<long>(split));
    const std::vector<float> tail(audio.begin() + static_cast<long>(split), audio.end());

    PitchDetector original;
    original.set_a4_hz(440.0f);
    const auto before = run(original, head);
    CHECK(before.empty());
    PitchDetector moved(std::move(original));
    CHECK(moved.samples_processed() == static_cast<std::int64_t>(split));
    const auto after = run(moved, tail);
    REQUIRE(count_type(after, PitchEventType::NoteOn) == 1);
    CHECK(first_of(after, PitchEventType::NoteOn)->note == 69);
    CHECK(first_of(after, PitchEventType::NoteOn)->onset_sample >= 4800 - 256);

    PitchDetector target;
    target = std::move(moved);
    CHECK(target.samples_processed() == static_cast<std::int64_t>(audio.size()));
    target.reset();
    const auto again = run(target, tone(523.2511, rich(0.3), kRate / 2, 4800));
    REQUIRE(count_type(again, PitchEventType::NoteOn) == 1);
    CHECK(first_of(again, PitchEventType::NoteOn)->note == 72);
}
