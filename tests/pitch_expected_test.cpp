// SPDX-License-Identifier: GPL-3.0-or-later
#include "pitch_tones.hpp"
#include "toonval/pitch_detect.hpp"
#include "toonval/pitch_eval.hpp"
#include "toonval/pitch_synth.hpp"

#include <algorithm>
#include <vector>

#include <doctest/doctest.h>

using namespace toonval;

namespace {

using namespace pitch_test;

SynthClip clip(int note, std::uint64_t seed = 5) {
    SynthClipOptions o;
    o.note_s = 0.2f;
    return make_synth_clip(note, 0.7f, seed, o, kRate);
}

// The first Expected event for the onset, run through the streaming detector. A detector that
// emits none fails the test: "no answer" is not a verdict.
PitchEvent ask_samples(const std::vector<float>& samples, std::int64_t onset,
                       const std::vector<int>& expected) {
    PitchDetector d;
    d.set_expected_notes(expected.data(), static_cast<int>(expected.size()));
    const auto events = run_detector(d, samples.data(), static_cast<int>(samples.size()));
    for (const auto& e : events) {
        if (e.type == PitchEventType::Expected && e.onset_sample >= onset - kRate / 50) {
            return e;
        }
    }
    FAIL("the detector emitted no Expected event for the onset");
    return {};
}

PitchEvent ask(const SynthClip& c, const std::vector<int>& expected) {
    return ask_samples(c.samples, c.onset, expected);
}

// An exact harmonic tone `cents` from the key, starting 4800 samples in.
std::vector<float> key_tone(int note, double cents) {
    return tone(grid_hz(note, cents), rich(0.3), kRate / 2, 4800);
}

// The frame a direct check of `expected` sees: three windows of an exact tone, at most the
// longest frame the checker takes.
int long_frame(int expected) {
    return std::min(ExpectedNoteChecker::kMaxFrame,
                    3 * ExpectedNoteChecker::window_samples(expected, kRate));
}

void check_not_sure(const ExpectedNoteResult& r) {
    CHECK(r.verdict == ExpectedVerdict::NotSure);
    CHECK(r.heard_note == -1);
    CHECK(r.confidence == 0.0f);
}

} // namespace

TEST_CASE("expected note: the played note is confirmed as expected") {
    for (int note : {33, 48, 60, 72, 96}) {
        CAPTURE(note);
        const auto e = ask(clip(note), {note});
        CHECK(e.type == PitchEventType::Expected);
        CHECK(e.verdict == ExpectedVerdict::Expected);
        CHECK(e.note == note);
    }
}

TEST_CASE("expected note: a semitone off is a neighbour, named from the expected note") {
    // Asked for C4, played B3: the player is one key below.
    CHECK(ask(clip(59), {60}).verdict == ExpectedVerdict::NeighborBelow);
    CHECK(ask(clip(61), {60}).verdict == ExpectedVerdict::NeighborAbove);
    CHECK(ask(clip(83), {84}).verdict == ExpectedVerdict::NeighborBelow);
}

TEST_CASE("expected note: an octave off is reported as an octave error, not as the note") {
    CHECK(ask(clip(72), {60}).verdict == ExpectedVerdict::OctaveAbove);
    CHECK(ask(clip(48), {60}).verdict == ExpectedVerdict::OctaveBelow);
}

TEST_CASE("expected note: some other note is never confirmed as the expected one") {
    const auto e = ask(clip(64), {60}); // a major third off: not a question this mode answers
    CHECK(e.verdict != ExpectedVerdict::Expected);
}

TEST_CASE("expected note: bass needs a longer window than treble") {
    CHECK(ExpectedNoteChecker::window_samples(21, kRate) >
          ExpectedNoteChecker::window_samples(48, kRate));
    CHECK(ExpectedNoteChecker::window_samples(48, kRate) >
          ExpectedNoteChecker::window_samples(72, kRate));
    CHECK(ExpectedNoteChecker::window_samples(60, kRate) <= kRate * 20 / 1000);
}

TEST_CASE("expected chord: a complete triad is all present; a left-out note is absent") {
    SynthClipOptions o;
    o.interferer = false;
    o.note_s = 0.25f;
    const int chord[] = {60, 64, 67};
    const auto full = make_synth_chord_clip(chord, 3, 0.7f, 3, o, kRate);
    const auto e = ask(full, {60, 64, 67});
    CHECK(e.type == PitchEventType::Expected);
    CHECK(e.chord_present == 0b111);
    CHECK(e.chord_absent == 0);
    CHECK(e.verdict == ExpectedVerdict::Expected);

    const int partial[] = {60, 67}; // E4 left out
    const auto missing = make_synth_chord_clip(partial, 2, 0.7f, 3, o, kRate);
    const auto m = ask(missing, {60, 64, 67});
    CHECK((m.chord_present & 0b010) == 0); // never claims the missing E4
    CHECK(m.verdict != ExpectedVerdict::Expected);
}

TEST_CASE("expected note: 30 cents from the key is the hard limit, in the stream too") {
    // The earliest frames of the stream read a note a cent or two off, so the limit is checked
    // with the stretch-matched tone and, on the flat side, at 29 and 45 cents here and at 31 and
    // 32 in the direct checker test below.
    for (int note : {48, 60, 69, 72}) {
        for (double sign : {1.0, -1.0}) {
            CAPTURE(note);
            CAPTURE(sign);
            const auto near =
                ask_samples(stretched_tone(note, sign * 29.0, kRate / 2, 4800), 4800, {note});
            CHECK(near.type == PitchEventType::Expected);
            CHECK(near.verdict == ExpectedVerdict::Expected);
            CHECK(near.note == note);
            std::vector<double> far_cents = {45.0};
            if (sign > 0.0) {
                far_cents = {31.0, 32.0, 45.0};
            }
            for (double cents : far_cents) {
                CAPTURE(cents);
                const auto far =
                    ask_samples(stretched_tone(note, sign * cents, kRate / 2, 4800), 4800, {note});
                CHECK(far.type == PitchEventType::Expected);
                CHECK(far.verdict == ExpectedVerdict::NotSure);
                CHECK(far.note == -1);
            }
        }
    }
}

TEST_CASE("expected note: the limit is judged against the set tuning") {
    PitchDetector d;
    d.set_a4_hz(442.0f);
    const int expected[] = {60};
    d.set_expected_notes(expected, 1);
    const auto verdict_of = [&](const std::vector<float>& audio) {
        d.reset();
        for (const auto& e : run_detector(d, audio.data(), static_cast<int>(audio.size()))) {
            if (e.type == PitchEventType::Expected) {
                return e.verdict;
            }
        }
        FAIL("no Expected event");
        return ExpectedVerdict::NotSure;
    };
    CHECK(verdict_of(stretched_tone(60, 29.0, kRate / 2, 4800, 442.0)) ==
          ExpectedVerdict::Expected);
    CHECK(verdict_of(stretched_tone(60, 32.0, kRate / 2, 4800, 442.0)) == ExpectedVerdict::NotSure);
    // 442 Hz tuning, 440 Hz grid: a note on the 440 grid is 8 cents flat of the 442 one.
    CHECK(verdict_of(stretched_tone(60, 0.0, kRate / 2, 4800, 440.0)) == ExpectedVerdict::Expected);
}

TEST_CASE("expected note: a wrong, unclear or missing answer is an explicit not sure") {
    // 45 cents off, and unpitched noise.
    CHECK(ask_samples(key_tone(60, 45.0), 4800, {60}).verdict == ExpectedVerdict::NotSure);
    const auto burst = ask_samples(noise(kRate / 2, 0.3f, 9, 4800), 4800, {60});
    CHECK(burst.type == PitchEventType::Expected);
    CHECK(burst.verdict == ExpectedVerdict::NotSure);
    CHECK(burst.note == -1);
    // A0 needs 4096 samples; the tone fades after 2300, the gate closes first, and the answer is
    // "not sure" rather than silence.
    std::vector<float> fading = tone(grid_hz(21), rich(0.3), kRate / 2, 4800);
    fade_out(fading, 4800 + 2300, 600);
    const auto gone = ask_samples(fading, 4800, {21});
    CHECK(gone.type == PitchEventType::Expected);
    CHECK(gone.verdict == ExpectedVerdict::NotSure);
    CHECK(gone.note == -1);
    CHECK(gone.sample - gone.onset_sample < ExpectedNoteChecker::window_samples(21, kRate));
}

TEST_CASE("expected chord: an unreadable chord is not sure, with nothing claimed") {
    const auto e = ask_samples(noise(kRate / 2, 0.3f, 9, 4800), 4800, {60, 64, 67});
    CHECK(e.type == PitchEventType::Expected);
    CHECK(e.verdict == ExpectedVerdict::NotSure);
    CHECK(e.chord_present == 0);
}

TEST_CASE("expected note: a missing frame or note is not sure") {
    ExpectedNoteChecker checker(kRate);
    const auto audio = tone(grid_hz(60), rich(0.3), 8192);
    check_not_sure(checker.check(nullptr, 2000, 60));
    check_not_sure(checker.check(audio.data(), 63, 60));
    check_not_sure(checker.check(audio.data(), 0, 60));
    check_not_sure(checker.check(audio.data(), 2000, -1));
    check_not_sure(checker.check(audio.data(), 2000, 128));
}

TEST_CASE("expected note: a frame too short to hold the note's periods is not sure") {
    ExpectedNoteChecker checker(kRate);
    const auto audio = tone(grid_hz(60), rich(0.3), 8192);
    check_not_sure(checker.check(audio.data(), 300, 60));
    check_not_sure(checker.check(audio.data(), 400, 60));
}

TEST_CASE("expected note: a frame too short to rule out the octave below is not sure") {
    ExpectedNoteChecker checker(kRate);
    const auto audio = tone(grid_hz(60), rich(0.3), 8192);
    // Long enough to find the period (C4 is 183 samples) but under the four periods the
    // spectrum needs to tell the note from its octave below.
    const auto r = checker.check(audio.data(), 600, 60);
    check_not_sure(r);
    CHECK(checker.check(audio.data(), long_frame(60), 60).verdict == ExpectedVerdict::Expected);
}

TEST_CASE("expected note: noise with no period is not sure") {
    ExpectedNoteChecker checker(kRate);
    const auto hiss = noise(8192, 0.5f, 5);
    check_not_sure(checker.check(hiss.data(), long_frame(60), 60));
}

TEST_CASE("expected note: more than 30 cents off is not sure and reports the cents") {
    ExpectedNoteChecker checker(kRate);
    for (double sign : {1.0, -1.0}) {
        for (double cents : {31.0, 32.0, 45.0}) {
            CAPTURE(sign);
            CAPTURE(cents);
            const auto audio = stretched_tone(60, sign * cents, 8192, 0);
            const auto r = checker.check(audio.data(), long_frame(60), 60);
            check_not_sure(r);
            CHECK(r.cents == doctest::Approx(sign * cents).epsilon(0.05));
        }
    }
    const auto ok = stretched_tone(60, 29.0, 8192, 0);
    const auto r = checker.check(ok.data(), long_frame(60), 60);
    CHECK(r.verdict == ExpectedVerdict::Expected);
    CHECK(r.cents == doctest::Approx(29.0).epsilon(0.05));
}

TEST_CASE("expected note: two keys away, or two keys sounding at once, is not sure") {
    ExpectedNoteChecker checker(kRate);
    const auto two_up = tone(grid_hz(62), rich(0.3), 8192);
    check_not_sure(checker.check(two_up.data(), long_frame(60), 60));
    // C4 and C#4 together: neither can be told from the other.
    auto cluster = tone(grid_hz(60), rich(0.3), 8192);
    const auto upper = tone(grid_hz(61), rich(0.3), 8192);
    for (size_t i = 0; i < cluster.size(); ++i) {
        cluster[i] += upper[i];
    }
    check_not_sure(checker.check(cluster.data(), long_frame(60), 60));
}

TEST_CASE("expected note: a period that is not clear enough is not sure") {
    // C4 with the octave below's odd partials added: the period is smeared past clarity 0.7.
    ExpectedNoteChecker checker(kRate);
    auto audio = tone(grid_hz(60), rich(0.3), 8192);
    add_sub_octave(audio, grid_hz(60), 0.5, 0);
    check_not_sure(checker.check(audio.data(), long_frame(60), 60));
}

TEST_CASE("expected note: the octave below named when it explains the spectrum") {
    ExpectedNoteChecker checker(kRate);
    auto audio = tone(grid_hz(60), rich(0.3), 8192);
    add_sub_octave(audio, grid_hz(60), 1.0, 0);
    const auto r = checker.check(audio.data(), long_frame(60), 60);
    CHECK(r.verdict == ExpectedVerdict::OctaveBelow);
    CHECK(r.heard_note == 48);
}

TEST_CASE("expected note: the lowest octave cannot tell the octave above, so it is not sure") {
    // A0 asked, A1 played: three periods of A0 cannot separate odd from even partials, but the
    // signal repeating at half the period says the octave above is sounding.
    ExpectedNoteChecker checker(kRate);
    const auto audio = tone(grid_hz(33), rich(0.3), 8192);
    check_not_sure(checker.check(audio.data(), long_frame(21), 21));
}

TEST_CASE("expected note: odd partials between the octave above and the note are not sure") {
    ExpectedNoteChecker checker(kRate);
    for (int note : {60, 72}) {
        CAPTURE(note);
        const auto audio = tone(grid_hz(note), rich(0.3, 0.08), 8192);
        check_not_sure(checker.check(audio.data(), long_frame(note), note));
        // Well below that the octave above is named; at full level it is the note.
        const auto up = tone(grid_hz(note), rich(0.3, 0.02), 8192);
        CHECK(checker.check(up.data(), long_frame(note), note).verdict ==
              ExpectedVerdict::OctaveAbove);
        const auto plain = tone(grid_hz(note), rich(0.3), 8192);
        CHECK(checker.check(plain.data(), long_frame(note), note).verdict ==
              ExpectedVerdict::Expected);
    }
}

TEST_CASE("expected note: a partly present octave below is not sure") {
    ExpectedNoteChecker checker(kRate);
    auto audio = tone(grid_hz(60), rich(0.3), 8192);
    add_sub_octave(audio, grid_hz(60), 0.45, 0);
    check_not_sure(checker.check(audio.data(), long_frame(60), 60));
    auto faint = tone(grid_hz(60), rich(0.3), 8192);
    add_sub_octave(faint, grid_hz(60), 0.2, 0);
    CHECK(checker.check(faint.data(), long_frame(60), 60).verdict == ExpectedVerdict::Expected);
}

TEST_CASE("expected note: only the last 4096 samples of a longer frame are used") {
    ExpectedNoteChecker checker(kRate);
    std::vector<float> audio = noise(6000, 0.5f, 3);
    const auto steady = tone(grid_hz(60), rich(0.3), 4096);
    audio.insert(audio.end(), steady.begin(), steady.end());
    const auto r = checker.check(audio.data(), static_cast<int>(audio.size()), 60);
    CHECK(r.verdict == ExpectedVerdict::Expected);
}

TEST_CASE("expected chord: the window follows the lowest note and stays in range") {
    const int triad[] = {60, 64, 67};
    const int shuffled[] = {67, 60, 64};
    const int w = ExpectedNoteChecker::chord_window_samples(triad, 3, kRate);
    // Ten periods of C4.
    CHECK(w == static_cast<int>(std::ceil(10.0 * kRate / grid_hz(60))));
    CHECK(ExpectedNoteChecker::chord_window_samples(shuffled, 3, kRate) == w);
    CHECK(w > ExpectedNoteChecker::window_samples(60, kRate));
    const int bass[] = {33, 40};
    CHECK(ExpectedNoteChecker::chord_window_samples(bass, 2, kRate) ==
          ExpectedNoteChecker::kMaxFrame);
    const int treble[] = {96, 100};
    CHECK(ExpectedNoteChecker::chord_window_samples(treble, 2, kRate) == 1024);
    const int low[] = {48, 52, 55};
    CHECK(ExpectedNoteChecker::chord_window_samples(low, 3, kRate) > w);
}

TEST_CASE("expected chord: check_chord marks present notes present and a left-out note absent") {
    SynthClipOptions o;
    o.interferer = false;
    o.note_s = 0.25f;
    const int triad[] = {60, 64, 67};
    ExpectedNoteChecker checker(kRate);
    // The window is the earliest a verdict is asked for; the stream checks again on longer frames.
    const int window = ExpectedNoteChecker::kMaxFrame;

    const auto full = make_synth_chord_clip(triad, 3, 0.7f, 3, o, kRate);
    const auto all = checker.check_chord(full.samples.data() + full.onset, window, triad, 3);
    REQUIRE(all.count == 3);
    for (int i = 0; i < 3; ++i) {
        CAPTURE(i);
        CHECK(all.notes[i] == triad[i]);
        CHECK(all.states[i] == ChordNoteState::Present);
        CHECK(all.scores[i] >= 0.5f);
    }

    const int played[] = {60, 67};
    const auto partial = make_synth_chord_clip(played, 2, 0.7f, 3, o, kRate);
    const auto some = checker.check_chord(partial.samples.data() + partial.onset, window, triad, 3);
    REQUIRE(some.count == 3);
    CHECK(some.states[0] == ChordNoteState::Present);
    CHECK(some.states[1] != ChordNoteState::Present); // E4 was not played
    CHECK(some.states[2] == ChordNoteState::Present);
    CHECK(some.scores[1] < some.scores[0]);
}

TEST_CASE(
    "expected chord: check_chord with no frame, too short a frame or no notes claims nothing") {
    ExpectedNoteChecker checker(kRate);
    const int triad[] = {60, 64, 67};
    const auto audio = tone(grid_hz(60), rich(0.3), 8192);
    for (const auto& r : {checker.check_chord(nullptr, 2000, triad, 3),
                          checker.check_chord(audio.data(), 255, triad, 3)}) {
        REQUIRE(r.count == 3);
        for (int i = 0; i < 3; ++i) {
            CHECK(r.states[i] == ChordNoteState::NotSure);
            CHECK(r.scores[i] == 0.0f);
        }
    }
    CHECK(checker.check_chord(audio.data(), 2000, triad, 0).count == 0);
    CHECK(checker.check_chord(audio.data(), 2000, triad, -3).count == 0);
    // More notes than a chord holds: the first kMaxChordNotes are kept.
    const int many[] = {48, 50, 52, 53, 55, 57, 59, 60, 62, 64};
    const auto big = checker.check_chord(audio.data(), 2000, many, 10);
    CHECK(big.count == kMaxChordNotes);
    CHECK(big.notes[kMaxChordNotes - 1] == 60);
}
