// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/pitch_eval.hpp"
#include "toonval/pitch_synth.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <doctest/doctest.h>

using namespace toonval;

namespace {

constexpr int kRate = 48000;

PitchEvent on(int note, std::int64_t onset, std::int64_t at) {
    PitchEvent e;
    e.type = PitchEventType::NoteOn;
    e.note = note;
    e.onset_sample = onset;
    e.sample = at;
    return e;
}

} // namespace

TEST_CASE("pitch scoring: the first matching note-on is correct, with its latency") {
    const PitchEvent ev[] = {on(60, 1000, 2000)};
    const auto s = score_note(60, 1000, 20000, ev, 1, kRate);
    CHECK(s.outcome == NoteOutcome::Correct);
    CHECK(s.latency_samples == 1000);
}

TEST_CASE("pitch scoring: a wrong note-on after a correct one still makes the note wrong") {
    const PitchEvent ev[] = {on(60, 1000, 2000), on(62, 5000, 6000)};
    CHECK(score_note(60, 1000, 20000, ev, 2, kRate).outcome == NoteOutcome::Wrong);
}

TEST_CASE("pitch scoring: octaves off count as octave errors, other notes as wrong") {
    const PitchEvent up[] = {on(72, 1000, 2000)};
    const PitchEvent down2[] = {on(36, 1000, 2000)};
    const PitchEvent fifth[] = {on(67, 1000, 2000)};
    CHECK(score_note(60, 1000, 20000, up, 1, kRate).outcome == NoteOutcome::OctaveError);
    CHECK(score_note(60, 1000, 20000, down2, 1, kRate).outcome == NoteOutcome::OctaveError);
    CHECK(score_note(60, 1000, 20000, fifth, 1, kRate).outcome == NoteOutcome::Wrong);
}

TEST_CASE("pitch scoring: not sure, missed, and events of other notes") {
    PitchEvent unsure;
    unsure.type = PitchEventType::NotSure;
    unsure.onset_sample = 1000;
    CHECK(score_note(60, 1000, 20000, &unsure, 1, kRate).outcome == NoteOutcome::NotSure);
    CHECK(score_note(60, 1000, 20000, nullptr, 0, kRate).outcome == NoteOutcome::Missed);
    // An onset more than 20 ms early belongs to something before; one at `end` to the next note.
    const PitchEvent other[] = {on(62, 1000 - kRate / 40, 2000), on(64, 20000, 21000)};
    CHECK(score_note(60, 1000, 20000, other, 2, kRate).outcome == NoteOutcome::Missed);
}

TEST_CASE("pitch scoring: registers by octave, A0-B1 first and C7-C8 last") {
    CHECK(register_of(21) == 0);
    CHECK(register_of(35) == 0);
    CHECK(register_of(36) == 1);
    CHECK(register_of(60) == 3);
    CHECK(register_of(108) == kRegisterCount - 1);
    CHECK(percentile({3.0f, 1.0f, 2.0f}, 100) == 3.0f);
    CHECK(percentile({3.0f, 1.0f, 2.0f}, 50) == 2.0f);
}

TEST_CASE("synthetic piano: same seed, same samples; the onset is silent before it") {
    SynthClipOptions o;
    o.noise_rms = 0.0f;
    o.interferer = false;
    const auto a = make_synth_clip(60, 0.7f, 42, o, kRate);
    const auto b = make_synth_clip(60, 0.7f, 42, o, kRate);
    REQUIRE(a.samples.size() == b.samples.size());
    CHECK(a.samples == b.samples);
    CHECK(a.samples[static_cast<size_t>(a.onset) - 1] == 0.0f);
    float peak = 0.0f;
    for (float v : a.samples) {
        peak = std::max(peak, std::fabs(v));
    }
    CHECK(peak > 0.05f);
    CHECK(peak < 1.0f);
    const auto c = make_synth_clip(60, 0.7f, 43, o, kRate);
    CHECK(c.samples != a.samples);
}

TEST_CASE("synthetic piano: partials are stretched by the inharmonicity") {
    // B rises from the middle of the keyboard to the top, as on a real piano.
    CHECK(synth_inharmonicity(108) > 10.0f * synth_inharmonicity(60));
    CHECK(synth_inharmonicity(21) > synth_inharmonicity(48));
}

TEST_CASE("synthetic piano: loudness runs from -36 dBFS to -6 dBFS with velocity") {
    CHECK(piano_tone_amplitude(1.0f) == doctest::Approx(0.5f).epsilon(0.01));
    CHECK(piano_tone_amplitude(0.0f) == doctest::Approx(0.01585f).epsilon(0.01));
    float last = 0.0f;
    for (float v = 0.0f; v <= 1.0f; v += 0.1f) {
        CHECK(piano_tone_amplitude(v) > last);
        last = piano_tone_amplitude(v);
    }
    // Out-of-range velocities are the ends.
    CHECK(piano_tone_amplitude(-3.0f) == piano_tone_amplitude(0.0f));
    CHECK(piano_tone_amplitude(7.0f) == piano_tone_amplitude(1.0f));
}

TEST_CASE("synthetic piano: add_piano_tone starts at the given sample and adds to what is there") {
    PianoToneOptions o;
    o.note = 69;
    o.velocity = 0.8f;
    std::vector<float> out(kRate / 2, 0.0f);
    add_piano_tone(out.data(), static_cast<int>(out.size()), 4800, o, kRate);
    for (int i = 0; i < 4800; ++i) {
        REQUIRE(out[static_cast<size_t>(i)] == 0.0f);
    }
    float peak = 0.0f;
    for (float v : out) {
        peak = std::max(peak, std::fabs(v));
    }
    CHECK(peak > 0.1f * piano_tone_amplitude(o.velocity));
    CHECK(peak < 1.5f * piano_tone_amplitude(o.velocity));

    // A second tone adds on top: the sum of two identical tones is twice one.
    std::vector<float> twice = out;
    add_piano_tone(twice.data(), static_cast<int>(twice.size()), 4800, o, kRate);
    for (size_t i = 0; i < out.size(); ++i) {
        REQUIRE(twice[i] == doctest::Approx(2.0f * out[i]).epsilon(1e-4).scale(1e-4));
    }

    // A start outside the buffer, a negative one and a null buffer do nothing.
    std::vector<float> none(1000, 0.0f);
    add_piano_tone(none.data(), 1000, 1000, o, kRate);
    add_piano_tone(none.data(), 1000, -1, o, kRate);
    add_piano_tone(nullptr, 1000, 0, o, kRate);
    for (float v : none) {
        CHECK(v == 0.0f);
    }
}

TEST_CASE("synthetic piano: the tone's pitch follows the note, the detune and A4") {
    for (float detune : {0.0f, 20.0f, -20.0f}) {
        for (float a4 : {440.0f, 442.0f}) {
            PianoToneOptions o;
            o.note = 69;
            o.detune_cents = detune;
            o.a4_hz = a4;
            o.hammer_noise = 0.0f;
            std::vector<float> out(kRate / 2, 0.0f);
            add_piano_tone(out.data(), static_cast<int>(out.size()), 4800, o, kRate);
            PitchDetectorConfig cfg;
            cfg.a4_hz = a4;
            PitchDetector d(cfg);
            const auto events = run_detector(d, out.data(), static_cast<int>(out.size()));
            const PitchEvent* on = nullptr;
            for (const auto& e : events) {
                if (e.type == PitchEventType::NoteOn) {
                    on = &e;
                    break;
                }
            }
            CAPTURE(detune);
            CAPTURE(a4);
            REQUIRE(on != nullptr);
            CHECK(on->note == 69);
            const double want = a4 * std::exp2(detune / 1200.0);
            CHECK(on->frequency_hz == doctest::Approx(want).epsilon(0.004));
        }
    }
}

TEST_CASE("synthetic piano: same seed, same tone; another seed, another tone") {
    PianoToneOptions o;
    o.note = 48;
    std::vector<float> a(kRate / 4, 0.0f);
    std::vector<float> b(kRate / 4, 0.0f);
    std::vector<float> c(kRate / 4, 0.0f);
    add_piano_tone(a.data(), static_cast<int>(a.size()), 100, o, kRate);
    add_piano_tone(b.data(), static_cast<int>(b.size()), 100, o, kRate);
    o.seed = 2;
    add_piano_tone(c.data(), static_cast<int>(c.size()), 100, o, kRate);
    CHECK(a == b);
    CHECK(a != c);
}

TEST_CASE(
    "synthetic piano: add_room_noise adds noise of the asked RMS, the same for the same seed") {
    const auto rms_of = [](const std::vector<float>& v) {
        double sum = 0.0;
        for (float x : v) {
            sum += static_cast<double>(x) * x;
        }
        return std::sqrt(sum / static_cast<double>(v.size()));
    };
    std::vector<float> a(kRate, 0.0f);
    std::vector<float> b(kRate, 0.0f);
    std::vector<float> c(kRate, 0.0f);
    add_room_noise(a.data(), kRate, 1e-3f, 7);
    add_room_noise(b.data(), kRate, 1e-3f, 7);
    add_room_noise(c.data(), kRate, 1e-3f, 8);
    CHECK(rms_of(a) == doctest::Approx(1e-3).epsilon(0.01));
    CHECK(a == b);
    CHECK(a != c);
    // It adds to what is there.
    std::vector<float> on_top(kRate, 0.25f);
    add_room_noise(on_top.data(), kRate, 1e-3f, 7);
    for (size_t i = 0; i < on_top.size(); ++i) {
        REQUIRE(on_top[i] == doctest::Approx(0.25f + a[i]).epsilon(1e-5));
    }
    // No noise to add, or nowhere to put it.
    std::vector<float> quiet(100, 0.0f);
    add_room_noise(quiet.data(), 100, 0.0f, 1);
    add_room_noise(quiet.data(), 0, 1e-3f, 1);
    add_room_noise(nullptr, 100, 1e-3f, 1);
    for (float v : quiet) {
        CHECK(v == 0.0f);
    }
}

TEST_CASE("pitch scoring: register_name labels the seven registers and nothing else") {
    CHECK(std::string(register_name(0)) == "A0-B1");
    CHECK(std::string(register_name(1)) == "C2-B2");
    CHECK(std::string(register_name(kRegisterCount - 1)) == "C7-C8");
    for (int i = 0; i < kRegisterCount; ++i) {
        for (int j = i + 1; j < kRegisterCount; ++j) {
            CHECK(std::string(register_name(i)) != std::string(register_name(j)));
        }
    }
    CHECK(std::string(register_name(-1)) == "?");
    CHECK(std::string(register_name(kRegisterCount)) == "?");
}

namespace {

// A small, fast slice of the synthetic set: every octave of the keyboard, one velocity.
SynthEvalOptions small_set() {
    SynthEvalOptions o;
    o.low_note = 24;
    o.note_step = 12;
    o.velocities = {0.7f};
    o.variants = 1;
    o.seed = 3;
    o.clip.note_s = 0.2f;
    return o;
}

} // namespace

TEST_CASE("pitch evaluation: expected-note mode never gives a wrong verdict on a small set") {
    const ExpectedEvalReport r = evaluate_expected_synthetic(small_set());
    for (const VerdictStats* s : {&r.expected, &r.neighbor, &r.octave}) {
        CHECK(s->queries > 0);
        CHECK(s->wrong == 0);
        CHECK(s->false_accept == 0);
        CHECK(s->correct + s->wrong + s->not_sure == s->queries);
    }
    // Not a checker that stays safe by never answering.
    CHECK(rate(r.expected.correct, r.expected.queries) >= 0.75f);
    int by_register = 0;
    for (const auto& reg : r.registers) {
        by_register += reg.queries;
    }
    CHECK(by_register == r.expected.queries + r.neighbor.queries + r.octave.queries);
}

TEST_CASE("pitch evaluation: expected-chord mode never gives a wrong verdict on a small set") {
    SynthEvalOptions o = small_set();
    o.clip.note_s = 0.25f;
    const ChordEvalReport r = evaluate_chord_synthetic(o, 12);
    CHECK(r.notes == 36);
    CHECK(r.wrong == 0);
    CHECK(r.false_present == 0);
    CHECK(r.correct + r.wrong + r.not_sure == r.notes);
    CHECK(r.missing_caught <= r.missing_notes);
    CHECK(r.correct >= r.notes / 2);
}

TEST_CASE("pitch evaluation: a tiny melody run completes and its counts add up") {
    // Accuracy is not gated here: low-register legato is still being worked on.
    SynthEvalOptions o = small_set();
    const EvalReport r = evaluate_melody_synthetic(o, 2, 4);
    CHECK(r.total.notes == 8);
    CHECK(r.total.correct + r.total.wrong + r.total.not_sure + r.total.missed == r.total.notes);
    CHECK(r.total.octave <= r.total.wrong);
    CHECK(r.total.latency_ms.size() == static_cast<size_t>(r.total.correct));
    int notes = 0;
    for (const auto& reg : r.registers) {
        notes += reg.notes;
        CHECK(reg.correct + reg.wrong + reg.not_sure + reg.missed == reg.notes);
    }
    CHECK(notes == r.total.notes);
}
