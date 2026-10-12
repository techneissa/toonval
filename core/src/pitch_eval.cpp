// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/pitch_eval.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace toonval {

namespace {

constexpr float kOnsetToleranceS = 0.020f;

float to_ms(std::int64_t samples, int sample_rate) {
    return 1000.0f * static_cast<float>(samples) / static_cast<float>(sample_rate);
}

// The detector, calibrated on the clip options' room noise alone (the noise-floor calibration
// step).
PitchDetector calibrated_detector(const SynthEvalOptions& options) {
    PitchDetector detector(options.detector);
    const int rate = options.detector.sample_rate;
    std::vector<float> noise(static_cast<size_t>(rate / 2), 0.0f);
    add_room_noise(noise.data(), static_cast<int>(noise.size()), options.clip.noise_rms,
                   options.seed ^ 0x5EEDull);
    detector.begin_noise_calibration();
    run_detector(detector, noise.data(), static_cast<int>(noise.size()), options.block);
    detector.end_noise_calibration();
    detector.reset();
    return detector;
}

std::uint64_t clip_seed(const SynthEvalOptions& o, int note, size_t velocity, int variant) {
    return o.seed * 1000003ull + static_cast<std::uint64_t>(note) * 7919ull +
           static_cast<std::uint64_t>(velocity) * 104729ull +
           static_cast<std::uint64_t>(variant) * 1299709ull;
}

ExpectedVerdict verdict_for(int played, int asked) {
    switch (played - asked) {
    case 0:
        return ExpectedVerdict::Expected;
    case -1:
        return ExpectedVerdict::NeighborBelow;
    case 1:
        return ExpectedVerdict::NeighborAbove;
    case -12:
        return ExpectedVerdict::OctaveBelow;
    case 12:
        return ExpectedVerdict::OctaveAbove;
    default:
        return ExpectedVerdict::NotSure;
    }
}

const PitchEvent* first_expected_event(const std::vector<PitchEvent>& events, std::int64_t onset,
                                       int sample_rate) {
    const auto tol = static_cast<std::int64_t>(kOnsetToleranceS * static_cast<float>(sample_rate));
    for (const auto& e : events) {
        if (e.type == PitchEventType::Expected && e.onset_sample >= onset - tol) {
            return &e;
        }
    }
    return nullptr;
}

} // namespace

NoteScore score_note(int note, std::int64_t onset, std::int64_t end, const PitchEvent* events,
                     int count, int sample_rate) {
    NoteScore score;
    const auto tol = static_cast<std::int64_t>(kOnsetToleranceS * static_cast<float>(sample_rate));
    bool not_sure = false;
    bool correct = false;
    for (int i = 0; i < count; ++i) {
        const PitchEvent& e = events[i];
        if (e.onset_sample < onset - tol || e.onset_sample >= end) {
            continue;
        }
        if (e.type == PitchEventType::NotSure) {
            not_sure = true;
        } else if (e.type == PitchEventType::NoteOn) {
            if (e.note == note) {
                if (!correct) {
                    correct = true;
                    score.latency_samples = e.sample - onset;
                }
            } else {
                score.reported_note = e.note;
                score.outcome = std::abs(e.note - note) % 12 == 0 ? NoteOutcome::OctaveError
                                                                  : NoteOutcome::Wrong;
                score.latency_samples = -1;
                return score;
            }
        }
    }
    if (correct) {
        score.outcome = NoteOutcome::Correct;
        score.reported_note = note;
    } else {
        score.outcome = not_sure ? NoteOutcome::NotSure : NoteOutcome::Missed;
    }
    return score;
}

int register_of(int note) {
    if (note < 36) {
        return 0;
    }
    return std::min(kRegisterCount - 1, (note - 36) / 12 + 1);
}

const char* register_name(int index) {
    static constexpr const char* kNames[kRegisterCount] = {"A0-B1", "C2-B2", "C3-B3", "C4-B4",
                                                           "C5-B5", "C6-B6", "C7-C8"};
    return index >= 0 && index < kRegisterCount ? kNames[index] : "?";
}

void EvalReport::add(int note, const NoteScore& score, int sample_rate) {
    for (RegisterStats* s : {&registers[static_cast<size_t>(register_of(note))], &total}) {
        ++s->notes;
        switch (score.outcome) {
        case NoteOutcome::Correct:
            ++s->correct;
            s->latency_ms.push_back(to_ms(score.latency_samples, sample_rate));
            break;
        case NoteOutcome::Wrong:
            ++s->wrong;
            break;
        case NoteOutcome::OctaveError:
            ++s->wrong;
            ++s->octave;
            break;
        case NoteOutcome::NotSure:
            ++s->not_sure;
            break;
        case NoteOutcome::Missed:
            ++s->missed;
            break;
        }
    }
}

float rate(int count, int notes) {
    return notes > 0 ? static_cast<float>(count) / static_cast<float>(notes) : 0.0f;
}

float percentile(std::vector<float> values, float p) {
    if (values.empty()) {
        return 0.0f;
    }
    std::sort(values.begin(), values.end());
    const float rank =
        std::ceil(std::clamp(p, 0.0f, 100.0f) / 100.0f * static_cast<float>(values.size()));
    const size_t index = static_cast<size_t>(std::max(1.0f, rank)) - 1;
    return values[std::min(index, values.size() - 1)];
}

std::vector<PitchEvent> run_detector(PitchDetector& detector, const float* samples, int count,
                                     int block) {
    std::vector<PitchEvent> events;
    block = std::max(1, block);
    const int capacity = (block / PitchDetector::kHop + 2) * PitchDetector::kMaxEventsPerHop;
    std::vector<PitchEvent> buffer(static_cast<size_t>(capacity));
    for (int i = 0; i < count; i += block) {
        const int n = std::min(block, count - i);
        const int got = detector.process(samples + i, n, buffer.data(), capacity);
        events.insert(events.end(), buffer.begin(), buffer.begin() + got);
    }
    return events;
}

EvalReport evaluate_synthetic(const SynthEvalOptions& options) {
    EvalReport report;
    PitchDetector detector = calibrated_detector(options);
    const int rate_hz = options.detector.sample_rate;
    for (int note = options.low_note; note <= options.high_note;
         note += std::max(1, options.note_step)) {
        for (size_t v = 0; v < options.velocities.size(); ++v) {
            for (int variant = 0; variant < options.variants; ++variant) {
                const SynthClip clip =
                    make_synth_clip(note, options.velocities[v],
                                    clip_seed(options, note, v, variant), options.clip, rate_hz);
                detector.reset();
                const auto events =
                    run_detector(detector, clip.samples.data(),
                                 static_cast<int>(clip.samples.size()), options.block);
                const NoteScore score =
                    score_note(note, clip.onset, static_cast<std::int64_t>(clip.samples.size()),
                               events.data(), static_cast<int>(events.size()), rate_hz);
                report.add(note, score, rate_hz);
                if (options.records != nullptr) {
                    options.records->push_back({note, options.velocities[v], variant,
                                                clip.interferer_note, score, events, clip.onset});
                }
            }
        }
    }
    return report;
}

EvalReport evaluate_melody_synthetic(const SynthEvalOptions& options, int melodies, int notes) {
    EvalReport report;
    PitchDetector detector = calibrated_detector(options);
    const int rate_hz = options.detector.sample_rate;
    std::uint64_t state = options.seed * 0x9E3779B97F4A7C15ull + 77;
    auto next = [&state](int n) {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<int>((state >> 33) % static_cast<std::uint64_t>(n));
    };
    const int lead = static_cast<int>(options.clip.lead_in_s * static_cast<float>(rate_hz));
    for (int m = 0; m < melodies; ++m) {
        std::vector<int> pitch(static_cast<size_t>(notes));
        std::vector<int> onset(static_cast<size_t>(notes));
        int note = options.low_note + next(options.high_note - options.low_note + 1);
        int t = lead;
        for (int i = 0; i < notes; ++i) {
            pitch[static_cast<size_t>(i)] = note;
            onset[static_cast<size_t>(i)] = t;
            const int min_ms = note < 48 ? 250 : 140;
            t += (min_ms + next(400 - min_ms + 1)) * rate_hz / 1000;
            int step = 0;
            while (step == 0) {
                step = next(15) - 7;
            }
            note = std::clamp(note + step, options.low_note, options.high_note);
        }
        const int length = t + rate_hz / 4;
        std::vector<float> audio(static_cast<size_t>(length), 0.0f);
        std::vector<float> one(static_cast<size_t>(length), 0.0f);
        const float velocity = options.velocities[static_cast<size_t>(
            next(static_cast<int>(std::max<size_t>(1, options.velocities.size()))))];
        for (int i = 0; i < notes; ++i) {
            std::fill(one.begin(), one.end(), 0.0f);
            PianoToneOptions tone;
            tone.note = pitch[static_cast<size_t>(i)];
            tone.velocity = velocity;
            tone.detune_cents = static_cast<float>(next(1001) - 500) / 100.0f;
            tone.seed = options.seed * 7919u + static_cast<unsigned>(m * 1000 + i);
            add_piano_tone(one.data(), length, onset[static_cast<size_t>(i)], tone, rate_hz);
            // The damper falls 40 ms after the next note starts.
            const int release =
                i + 1 < notes ? onset[static_cast<size_t>(i) + 1] + rate_hz / 25 : length;
            for (int k = 0; k < length; ++k) {
                const float fade = k < release ? 1.0f
                                               : std::exp(-static_cast<float>(k - release) /
                                                          (0.03f * static_cast<float>(rate_hz)));
                audio[static_cast<size_t>(k)] += one[static_cast<size_t>(k)] * fade;
            }
        }
        if (options.clip.interferer) {
            // A steady other tone under the whole melody, at interferer_db against a mid note.
            PianoToneOptions other;
            other.note = std::clamp(pitch[0] + 5, kPianoLowNote, kPianoHighNote);
            other.velocity = velocity;
            other.hammer_noise = 0.0f;
            std::vector<float> drone(static_cast<size_t>(length), 0.0f);
            add_piano_tone(drone.data(), length, 0, other, rate_hz);
            const float gain = std::pow(10.0f, options.clip.interferer_db / 20.0f);
            for (size_t k = 0; k < audio.size(); ++k) {
                audio[k] += gain * drone[k];
            }
        }
        add_room_noise(audio.data(), length, options.clip.noise_rms,
                       options.seed * 31u + static_cast<unsigned>(m));
        detector.reset();
        const auto events = run_detector(detector, audio.data(), length, options.block);
        for (int i = 0; i < notes; ++i) {
            // The next note's onset may be placed a little early; it belongs to that note.
            const std::int64_t end =
                i + 1 < notes ? onset[static_cast<size_t>(i) + 1] - rate_hz / 50 : length;
            const NoteScore score =
                score_note(pitch[static_cast<size_t>(i)], onset[static_cast<size_t>(i)], end,
                           events.data(), static_cast<int>(events.size()), rate_hz);
            report.add(pitch[static_cast<size_t>(i)], score, rate_hz);
            if (options.records != nullptr) {
                std::vector<PitchEvent> near;
                for (const auto& e : events) {
                    if ((e.sample >= onset[static_cast<size_t>(i)] && e.sample < end) ||
                        (e.onset_sample >= onset[static_cast<size_t>(i)] - rate_hz / 50 &&
                         e.onset_sample < end)) {
                        near.push_back(e);
                    }
                }
                options.records->push_back({pitch[static_cast<size_t>(i)], velocity, m, -1, score,
                                            near, onset[static_cast<size_t>(i)]});
            }
        }
    }
    return report;
}

ExpectedEvalReport evaluate_expected_synthetic(const SynthEvalOptions& options) {
    ExpectedEvalReport report;
    PitchDetector detector = calibrated_detector(options);
    const int rate_hz = options.detector.sample_rate;
    for (int note = options.low_note; note <= options.high_note;
         note += std::max(1, options.note_step)) {
        for (size_t v = 0; v < options.velocities.size(); ++v) {
            for (int variant = 0; variant < options.variants; ++variant) {
                const SynthClip clip =
                    make_synth_clip(note, options.velocities[v],
                                    clip_seed(options, note, v, variant), options.clip, rate_hz);
                for (int offset : {0, -1, 1, -12, 12}) {
                    const int asked = note + offset;
                    if (asked < kPianoLowNote || asked > kPianoHighNote) {
                        continue;
                    }
                    detector.reset();
                    detector.set_expected_notes(&asked, 1);
                    const auto events =
                        run_detector(detector, clip.samples.data(),
                                     static_cast<int>(clip.samples.size()), options.block);
                    const PitchEvent* e = first_expected_event(events, clip.onset, rate_hz);
                    const ExpectedVerdict want = verdict_for(note, asked);
                    VerdictStats& cat = offset == 0             ? report.expected
                                        : std::abs(offset) == 1 ? report.neighbor
                                                                : report.octave;
                    for (VerdictStats* s :
                         {&cat, &report.registers[static_cast<size_t>(register_of(note))]}) {
                        ++s->queries;
                        if (e == nullptr || e->verdict == ExpectedVerdict::NotSure) {
                            ++s->not_sure;
                        } else if (e->verdict == want) {
                            ++s->correct;
                            s->latency_ms.push_back(to_ms(e->sample - clip.onset, rate_hz));
                        } else {
                            ++s->wrong;
                            if (e->verdict == ExpectedVerdict::Expected) {
                                ++s->false_accept;
                            }
                        }
                    }
                }
            }
        }
    }
    detector.set_expected_notes(nullptr, 0);
    return report;
}

ChordEvalReport evaluate_chord_synthetic(const SynthEvalOptions& options, int chords) {
    ChordEvalReport report;
    PitchDetector detector = calibrated_detector(options);
    const int rate_hz = options.detector.sample_rate;
    SynthClipOptions clip_options = options.clip;
    clip_options.interferer = false;
    std::uint64_t state = options.seed * 0x9E3779B97F4A7C15ull + 5;
    auto next = [&state](int n) {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<int>((state >> 33) % static_cast<std::uint64_t>(n));
    };
    for (int c = 0; c < chords; ++c) {
        const int root = 36 + next(49); // C2..C6
        const bool minor = next(2) == 1;
        const int asked[3] = {root, root + (minor ? 3 : 4), root + 7};
        const int left_out = next(2) == 1 ? next(3) : -1; // half the chords miss one note
        int played[3];
        int played_count = 0;
        for (int i = 0; i < 3; ++i) {
            if (i != left_out) {
                played[played_count++] = asked[i];
            }
        }
        const float velocity = options.velocities[static_cast<size_t>(
            next(static_cast<int>(std::max<size_t>(1, options.velocities.size()))))];
        const SynthClip clip =
            make_synth_chord_clip(played, played_count, velocity,
                                  options.seed + static_cast<unsigned>(c), clip_options, rate_hz);
        detector.reset();
        detector.set_expected_notes(asked, 3);
        const auto events = run_detector(detector, clip.samples.data(),
                                         static_cast<int>(clip.samples.size()), options.block);
        const PitchEvent* e = first_expected_event(events, clip.onset, rate_hz);
        if (e != nullptr && e->chord_present != 0) {
            report.latency_ms.push_back(to_ms(e->sample - clip.onset, rate_hz));
        }
        for (int i = 0; i < 3; ++i) {
            const bool was_played = i != left_out;
            const bool present = e != nullptr && ((e->chord_present >> i) & 1u) != 0;
            const bool absent = e != nullptr && ((e->chord_absent >> i) & 1u) != 0;
            ++report.notes;
            if (!was_played) {
                ++report.missing_notes;
            }
            if (!present && !absent) {
                ++report.not_sure;
            } else if (present == was_played) {
                ++report.correct;
                if (!was_played) {
                    ++report.missing_caught;
                }
            } else {
                ++report.wrong;
                if (!was_played) {
                    ++report.false_present;
                }
            }
        }
    }
    detector.set_expected_notes(nullptr, 0);
    return report;
}

} // namespace toonval
