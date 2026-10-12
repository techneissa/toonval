// SPDX-License-Identifier: GPL-3.0-or-later
// Expected-note mode: instead of "what note is this?", ask "is this the expected
// note, a neighbour, or an octave off?", which is a much easier question to answer correctly.
#include "pitch_dsp.hpp"
#include "toonval/pitch.hpp"
#include "toonval/pitch_detect.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace toonval {

namespace {

namespace dsp = pitch_dsp;

constexpr float kMaxPartialHz = 9000.0f;
constexpr float kSemitone = 1.0594630943592953f;

float hz_of(int note, float a4) { return a4 * std::exp2(static_cast<float>(note - kA4) / 12.0f); }

float period_of(int note, int sample_rate) {
    return static_cast<float>(sample_rate) / hz_of(note, 440.0f);
}

} // namespace

struct ExpectedNoteChecker::Impl {
    explicit Impl(int rate) : sample_rate(rate), analyzer(rate, kMaxFrame) {}
    int sample_rate;
    dsp::FrameAnalyzer analyzer;

    float max_hz() const {
        return std::min(0.45f * static_cast<float>(sample_rate), kMaxPartialHz);
    }

    // Energy of partials n >= first_n of `f0` (those a neighbour semitone's partials no longer
    // overlap); -1 when fewer than three such partials fit below max_hz.
    float neighbour_energy(float f0, float b, int first_n) const {
        float sum = 0.0f;
        int used = 0;
        for (int n = first_n; n <= 24; ++n) {
            const float f = dsp::partial_hz(f0, n, b);
            if (f > max_hz()) {
                break;
            }
            const float tol = std::max(2.0f * analyzer.bin_hz(), 0.012f * f);
            const float m = analyzer.peak_magnitude(f, tol);
            sum += m * m;
            ++used;
        }
        return used >= 3 ? sum : -1.0f;
    }
};

ExpectedNoteChecker::ExpectedNoteChecker(int sample_rate)
    : impl_(std::make_unique<Impl>(sample_rate)) {}
ExpectedNoteChecker::~ExpectedNoteChecker() = default;

int ExpectedNoteChecker::window_samples(int note, int sample_rate) {
    // Four periods let the spectrum separate the note from its octave below (two periods of it);
    // the lowest octave has nothing below it on the piano; three periods there separate odd
    // from even partials (the octave-above check) where the 85 ms frame allows.
    const float periods = note - 12 >= kPianoLowNote ? 4.0f : 3.0f;
    return std::clamp(static_cast<int>(std::ceil(periods * period_of(note, sample_rate))), 256,
                      kMaxFrame);
}

int ExpectedNoteChecker::chord_window_samples(const int* notes, int count, int sample_rate) {
    int lowest = 127;
    for (int i = 0; i < count; ++i) {
        lowest = std::min(lowest, notes[i]);
    }
    // Chord partials sit close together, so the spectrum needs far more periods than one note.
    return std::clamp(static_cast<int>(std::ceil(10.0f * period_of(lowest, sample_rate))), 1024,
                      kMaxFrame);
}

ExpectedNoteResult ExpectedNoteChecker::check(const float* frame, int length, int expected_note,
                                              float a4_hz) {
    ExpectedNoteResult result;
    Impl& d = *impl_;
    if (frame == nullptr || length < 64 || expected_note < 0 || expected_note > 127) {
        return result;
    }
    if (length > kMaxFrame) {
        frame += length - kMaxFrame;
        length = kMaxFrame;
    }
    const float fs = static_cast<float>(d.sample_rate);
    const float target_hz = hz_of(expected_note, a4_hz);
    const float period = fs / target_hz;
    // Search 2.5 semitones either side: stretched partials pull a treble period up to a semitone
    // short, and the neighbours must still land inside.
    const float span = std::exp2(2.5f / 12.0f);
    const int lo = std::max(2, static_cast<int>(std::floor(period / span)));
    const int hi = static_cast<int>(std::ceil(period * span));
    // Lags up to just past twice the period, for the octave below.
    d.analyzer.analyze(frame, length, static_cast<int>(std::ceil(2.1f * period)) + 2);
    if (hi + 1 > d.analyzer.max_lag()) {
        return result; // not enough audio yet for the neighbour below
    }

    // The NSDF maximum near the expected period: the time-domain pitch, constrained.
    const float* n = d.analyzer.nsdf();
    int best = lo;
    for (int t = lo + 1; t <= hi; ++t) {
        if (n[t] > n[best]) {
            best = t;
        }
    }
    const float low_hz = hz_of(kPianoLowNote, a4_hz) * 0.97f;
    const auto octave_below_evidence = [&](float f) {
        const int note = static_cast<int>(std::lround(note_from_hz(f, a4_hz)));
        return dsp::octave_evidence(d.analyzer, f, dsp::piano_inharmonicity(note),
                                    dsp::piano_inharmonicity(note - 12), low_hz, d.max_hz());
    };
    const bool interior = best > lo && best < hi;
    if (!interior || n[best] < 0.6f) {
        // No clear period near the note. If the octave below explains the spectrum and the
        // signal repeats at its period, say so.
        const auto ev = octave_below_evidence(target_hz);
        float below = 0.0f;
        for (int t = static_cast<int>(1.94f * period); t <= static_cast<int>(2.06f * period) + 1;
             ++t) {
            below = std::max(below, d.analyzer.nsdf_at(static_cast<float>(t)));
        }
        if (ev.half_resolvable && ev.half_ratio > 0.5f && below >= 0.8f) {
            result.verdict = ExpectedVerdict::OctaveBelow;
            result.heard_note = expected_note - 12;
            result.confidence = std::min(1.0f, ev.half_ratio);
        }
        return result;
    }
    const float y0 = n[best - 1];
    const float y1 = n[best];
    const float y2 = n[best + 1];
    const float denom = y0 - 2.0f * y1 + y2;
    const float off =
        std::fabs(denom) > 1e-12f ? std::clamp(0.5f * (y0 - y2) / denom, -0.5f, 0.5f) : 0.0f;
    const float clarity = std::min(1.0f, y1 - 0.25f * (y0 - y2) * off);
    float heard_hz = fs / (static_cast<float>(best) + off);
    // Stretched partials pull the period sharp; the resolved spectral peaks do not.
    if (2.0f * d.analyzer.resolution_hz() <= 0.35f * heard_hz) {
        const float b = dsp::piano_inharmonicity(expected_note);
        heard_hz = dsp::refine_f0(d.analyzer, heard_hz, b, d.max_hz(), 0.06f);
        heard_hz = dsp::refine_f0(d.analyzer, heard_hz, b, d.max_hz());
    } else {
        // Partials not resolved (the lowest notes): undo the average stretch instead.
        heard_hz /= 1.0f + dsp::stretch_bias(d.analyzer, heard_hz,
                                             dsp::piano_inharmonicity(expected_note), d.max_hz());
    }
    const float cents = 1200.0f * std::log2(heard_hz / target_hz);
    const int step = static_cast<int>(std::lround(cents / 100.0f));
    const float deviation = cents - 100.0f * static_cast<float>(step);
    result.cents = deviation;
    if (std::fabs(deviation) > kMaxCentsFromKey || clarity < 0.7f || std::abs(step) > 1) {
        return result;
    }

    // Octave checks on the heard pitch: odd partials missing means the octave above sounds; the
    // octave below's odd partials present means it sounds instead.
    const auto ev = octave_below_evidence(heard_hz);
    int heard = expected_note + step;
    if (heard - 12 >= kPianoLowNote && !ev.half_resolvable) {
        return result; // too short to rule the octave below out
    }
    if (!ev.odd_resolvable) {
        // The lowest notes: the window cannot resolve odd from even partials. In time, the
        // octave above would make the signal repeat at half the period too.
        const float lag = static_cast<float>(best) + off;
        float half_period = -1.0f;
        for (float t = 0.47f * lag; t <= 0.53f * lag; t += 1.0f) {
            half_period = std::max(half_period, d.analyzer.nsdf_at(t));
        }
        if (half_period >= 0.5f) {
            return result;
        }
    } else if (ev.odd_ratio < 0.02f) {
        heard += 12;
    } else if (ev.odd_ratio < 0.05f) {
        return result;
    }
    if (heard == expected_note + step) {
        if (ev.half_ratio > 0.35f) {
            heard -= 12;
        } else if (ev.half_ratio > 0.15f) {
            return result;
        }
    }

    // Harmonic energy at the stretched partials of the heard note against its two neighbours,
    // over the partials the window resolves from a semitone away. Skipped when only partials
    // above the 8th are resolved: those are too weak to compare.
    const float lobe = 2.0f * d.analyzer.resolution_hz();
    const int first_n = std::max(1, static_cast<int>(std::ceil(lobe / (0.045f * heard_hz))));
    if (heard == expected_note + step && first_n <= 8) {
        const float own = d.neighbour_energy(heard_hz, dsp::piano_inharmonicity(heard), first_n);
        const float below =
            d.neighbour_energy(heard_hz / kSemitone, dsp::piano_inharmonicity(heard - 1), first_n);
        const float above =
            d.neighbour_energy(heard_hz * kSemitone, dsp::piano_inharmonicity(heard + 1), first_n);
        if (own >= 0.0f && (below > own || above > own)) {
            return result;
        }
    }

    result.heard_note = heard;
    result.confidence = clarity;
    const int diff = heard - expected_note;
    switch (diff) {
    case 0:
        result.verdict = ExpectedVerdict::Expected;
        break;
    case -1:
        result.verdict = ExpectedVerdict::NeighborBelow;
        break;
    case 1:
        result.verdict = ExpectedVerdict::NeighborAbove;
        break;
    case -12:
        result.verdict = ExpectedVerdict::OctaveBelow;
        break;
    case 12:
        result.verdict = ExpectedVerdict::OctaveAbove;
        break;
    default:
        result.verdict = ExpectedVerdict::NotSure; // some other note: not a question we answer
        result.heard_note = -1;
        break;
    }
    return result;
}

ChordCheckResult ExpectedNoteChecker::check_chord(const float* frame, int length, const int* notes,
                                                  int count, float a4_hz) {
    ChordCheckResult result;
    Impl& d = *impl_;
    result.count = std::clamp(count, 0, kMaxChordNotes);
    for (int i = 0; i < result.count; ++i) {
        result.notes[i] = notes[i];
    }
    if (frame == nullptr || length < 256 || result.count == 0) {
        return result;
    }
    if (length > kMaxFrame) {
        frame += length - kMaxFrame;
        length = kMaxFrame;
    }
    d.analyzer.analyze(frame, length, 2);
    const float lobe = 2.0f * d.analyzer.resolution_hz();
    const float max_hz = d.max_hz();

    for (int i = 0; i < result.count; ++i) {
        const int note = result.notes[i];
        const float f0 = hz_of(note, a4_hz);
        const float b = dsp::piano_inharmonicity(note);
        float weight = 0.0f;
        float present = 0.0f;
        float absent = 0.0f;
        for (int n = 1; n <= 6; ++n) {
            const float f = dsp::partial_hz(f0, n, b);
            if (f > max_hz) {
                break;
            }
            // Use only partials no other chord note shares and the window resolves from a
            // semitone away.
            const float guard = std::max(0.03f * f, 2.0f * lobe);
            if (lobe > 0.045f * f) {
                continue;
            }
            bool shared = false;
            for (int j = 0; j < result.count && !shared; ++j) {
                if (j == i) {
                    continue;
                }
                const float g0 = hz_of(result.notes[j], a4_hz);
                const float gb = dsp::piano_inharmonicity(result.notes[j]);
                for (int m = 1; m <= 16; ++m) {
                    const float g = dsp::partial_hz(g0, m, gb);
                    if (g > f + guard) {
                        break;
                    }
                    if (std::fabs(g - f) <= guard) {
                        shared = true;
                        break;
                    }
                }
            }
            if (shared) {
                continue;
            }
            const float tol = std::max(2.0f * d.analyzer.bin_hz(), 0.012f * f);
            const float own = d.analyzer.peak_magnitude(f, tol);
            const float ref = std::min(d.analyzer.peak_magnitude(f / kSemitone, tol),
                                       d.analyzer.peak_magnitude(f * kSemitone, tol));
            const float w = 1.0f / static_cast<float>(n);
            weight += w;
            if (own > 4.0f * ref && own > 1e-5f) {
                present += w;
            } else if (own < 1.5f * ref || own <= 1e-5f) {
                absent += w;
            }
        }
        if (weight <= 0.0f) {
            result.states[i] = ChordNoteState::NotSure;
            continue;
        }
        const float score = present / weight;
        result.scores[i] = score;
        if (score >= 0.5f) {
            result.states[i] = ChordNoteState::Present;
        } else if (absent / weight >= 0.7f) {
            result.states[i] = ChordNoteState::Absent;
        } else {
            result.states[i] = ChordNoteState::NotSure;
        }
    }
    return result;
}

} // namespace toonval
