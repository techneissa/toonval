// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/pitch_detect.hpp"

#include "pitch_dsp.hpp"
#include "toonval/pitch.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace toonval {

namespace {

namespace dsp = pitch_dsp;

constexpr int kHistory = 8192;      // power of two; the longest frame plus the onset look-back
constexpr int kMaxFrame = 4096;     // longest analysis frame (85 ms at 48 kHz)
constexpr int kMinFrame = 192;      // shortest frame worth analysing
constexpr int kOnsetWindow = 512;   // spectral-flux frame
constexpr int kOnsetLookBack = 640; // samples searched to place the onset precisely
constexpr int kOnsetBlock = 32;
constexpr int kGateHops = 4;          // gate level: RMS over the last 512 samples
constexpr int kTrackEvery = 4;        // hops between pitch checks while a note sounds
constexpr int kTrackMisses = 3;       // failed checks in a row before the note counts as gone
constexpr int kDeadlineHops = 8;      // hops past the longest frame before giving up with NotSure
constexpr float kCombSeconds = 0.25f; // how long after an onset the last note is filtered out
constexpr float kRestrikeResidual = 0.3f; // comb residual below this: the same note struck again
constexpr int kTuningNotes = 8;
constexpr int kTuningMinNotes = 5;
constexpr float kMaxPartialHz = 9000.0f;

float hz_of(int note, float a4) { return a4 * std::exp2(static_cast<float>(note - kA4) / 12.0f); }

} // namespace

float note_from_hz(float hz, float a4_hz) {
    if (!(hz > 0.0f) || !(a4_hz > 0.0f)) {
        return -1.0f;
    }
    return static_cast<float>(kA4) + 12.0f * std::log2(hz / a4_hz);
}

int pitch_window_samples(int note, int sample_rate, float octave_check_periods) {
    const float period = static_cast<float>(sample_rate) / hz_of(note, 440.0f);
    const float periods = note - 12 >= kPianoLowNote ? octave_check_periods : 2.2f;
    return std::clamp(static_cast<int>(std::ceil(periods * period)), kMinFrame, kMaxFrame);
}

struct PitchDetector::Impl {
    enum class State : std::uint8_t { Idle, Acquiring, Sounding, Done };

    PitchDetectorConfig cfg;
    int states;    // notes low..high plus one unvoiced state
    float min_lag; // shortest period searched (highest note, a bit sharp)
    int max_lag;   // longest period searched (lowest note, a bit flat)
    float max_hz;  // partials above this are ignored

    std::vector<float> history = std::vector<float>(kHistory, 0.0f);
    std::int64_t written = 0;
    int since_hop = 0;

    dsp::FrameAnalyzer analyzer;
    dsp::RealFft onset_fft{kOnsetWindow};
    std::vector<float> onset_hann = std::vector<float>(kOnsetWindow);
    std::vector<float> onset_time = std::vector<float>(kOnsetWindow);
    std::vector<kiss_fft_cpx> onset_spec = std::vector<kiss_fft_cpx>(kOnsetWindow / 2 + 1);
    std::vector<float> onset_prev = std::vector<float>(kOnsetWindow / 2 + 1, 0.0f);
    std::vector<float> frame = std::vector<float>(kMaxFrame);
    // The frame with the previous note combed out: y[n] = x[n] - x[n - its period].
    dsp::FrameAnalyzer comb_analyzer;
    std::vector<float> comb = std::vector<float>(kMaxFrame);
    ExpectedNoteChecker checker;

    // Gate.
    float noise_floor = 0.0f;
    bool calibrating = false;
    double calib_sum = 0.0;
    std::int64_t calib_hops = 0;
    bool gate_open = false;
    int quiet_hops = 0;
    std::array<double, kGateHops> hop_energy{};
    int hop_index = 0;

    // Onset.
    float flux_avg = 0.0f;
    float energy_avg = 0.0f;
    int spike_hops = 0;                    // hops left for a flux spike's energy rise to arrive
    float spike_energy = 0.0f;             // the loudest recent hop when the spike came
    std::array<float, 16> recent_energy{}; // 2048 samples: longer than the lowest period
    int recent_index = 0;
    std::int64_t last_onset = std::numeric_limits<std::int64_t>::min() / 2;

    // Note state.
    State state = State::Idle;
    std::int64_t onset = 0;
    std::vector<float> delta;
    std::vector<float> obs;
    std::vector<float> next;
    int stable_note = -1;
    int stable_count = 0;
    float stable_conf = 0.0f;
    int current_note = -1;
    float current_period = 0.0f; // waveform period of the sounding note, in samples
    int prev_note = -1;          // the note sounding when the current onset came
    float prev_period = 0.0f;
    int track_counter = 0;
    int track_misses = 0;

    // Tuning.
    float a4;
    float a4_base;
    std::array<float, kTuningNotes> tuning{};
    int tuning_count = 0;
    int tuning_next = 0;

    // Expected-note mode.
    std::array<int, kMaxChordNotes> expected{};
    int expected_count = 0;
    ExpectedVerdict last_verdict = ExpectedVerdict::NotSure;
    std::uint16_t last_present = 0;
    std::uint16_t last_absent = 0;
    int verdict_same = 0;

    // Output.
    PitchEvent* out = nullptr;
    int capacity = 0;
    int emitted = 0;
    std::int64_t dropped = 0;

    explicit Impl(const PitchDetectorConfig& c)
        : cfg(c), analyzer(c.sample_rate, kMaxFrame), comb_analyzer(c.sample_rate, kMaxFrame),
          checker(c.sample_rate), a4(c.a4_hz), a4_base(c.a4_hz) {
        cfg.low_note = std::clamp(cfg.low_note, 0, 127);
        cfg.high_note = std::clamp(cfg.high_note, cfg.low_note, 127);
        cfg.confirm_frames = std::max(1, cfg.confirm_frames);
        states = cfg.high_note - cfg.low_note + 2;
        delta.assign(static_cast<size_t>(states), 0.0f);
        obs.assign(static_cast<size_t>(states), 0.0f);
        next.assign(static_cast<size_t>(states), 0.0f);
        const float fs = static_cast<float>(cfg.sample_rate);
        min_lag = fs / (hz_of(cfg.high_note, 440.0f) * 1.06f);
        max_lag = std::min(kMaxFrame / 2,
                           static_cast<int>(std::ceil(fs / (hz_of(cfg.low_note, 440.0f) * 0.94f))));
        max_hz = std::min(0.45f * fs, kMaxPartialHz);
        for (int i = 0; i < kOnsetWindow; ++i) {
            onset_hann[static_cast<size_t>(i)] = static_cast<float>(
                0.5 - 0.5 * std::cos(6.283185307179586 * i / (kOnsetWindow - 1)));
        }
    }

    float sample_at(std::int64_t index) const {
        return history[static_cast<size_t>(index & (kHistory - 1))];
    }

    // Copies history[end - length, end) into `dst`.
    void copy_recent(float* dst, std::int64_t end, int length) const {
        for (int i = 0; i < length; ++i) {
            dst[i] = sample_at(end - length + i);
        }
    }

    void emit(const PitchEvent& e) {
        if (emitted < capacity) {
            out[emitted++] = e;
        } else {
            ++dropped;
        }
    }

    float gate_threshold() const {
        return std::max(cfg.gate_min_rms, noise_floor * cfg.gate_ratio);
    }

    // RMS over the last kGateHops hops. One hop is too short: a bass partial's level in 128
    // samples swings with its phase, which would flap the gate open and shut.
    float gate_rms() {
        double sum = 0.0;
        for (int i = 0; i < kHop; ++i) {
            const float v = sample_at(written - kHop + i);
            sum += static_cast<double>(v) * v;
        }
        hop_energy[static_cast<size_t>(hop_index)] = sum;
        hop_index = (hop_index + 1) % kGateHops;
        double total = 0.0;
        for (double e : hop_energy) {
            total += e;
        }
        return static_cast<float>(std::sqrt(total / (kGateHops * kHop)));
    }

    // Mean positive change of log magnitude over the bins from 140 Hz to 6 kHz, against the
    // previous hop, and the energy in those bins. Magnitudes are floored near the gate level so
    // room noise adds little flux. The lowest bins are left out: a 512-sample frame cannot
    // resolve bass partials, and their level swings with phase from hop to hop.
    float spectral_flux(float* energy) {
        copy_recent(onset_time.data(), written, kOnsetWindow);
        for (int i = 0; i < kOnsetWindow; ++i) {
            onset_time[static_cast<size_t>(i)] *= onset_hann[static_cast<size_t>(i)];
        }
        onset_fft.forward(kOnsetWindow, onset_time.data(), onset_spec.data());
        const float scale = 4.0f / static_cast<float>(kOnsetWindow); // sine amplitude, Hann
        const float floor = 0.5f * gate_threshold();
        const int top =
            std::min(kOnsetWindow / 2, static_cast<int>(6000LL * kOnsetWindow / cfg.sample_rate));
        float flux = 0.0f;
        float sum = 0.0f;
        constexpr int first = 2;
        for (int k = first; k <= top; ++k) {
            const auto& c = onset_spec[static_cast<size_t>(k)];
            const float linear = std::hypot(c.r, c.i) * scale;
            sum += linear * linear;
            const float mag = std::log(linear + floor);
            const float diff = mag - onset_prev[static_cast<size_t>(k)];
            flux += std::max(0.0f, diff);
            onset_prev[static_cast<size_t>(k)] = mag;
        }
        *energy = sum;
        return flux / static_cast<float>(top - first + 1);
    }

    // Places the onset within the last kOnsetLookBack samples: the first 32-sample block whose
    // energy rises well above the blocks before the attack.
    std::int64_t locate_onset() const {
        constexpr int blocks = kOnsetLookBack / kOnsetBlock;
        std::array<float, blocks> e{};
        const std::int64_t start = written - kOnsetLookBack;
        for (int b = 0; b < blocks; ++b) {
            float sum = 0.0f;
            for (int i = 0; i < kOnsetBlock; ++i) {
                const float v = sample_at(start + b * kOnsetBlock + i);
                sum += v * v;
            }
            e[static_cast<size_t>(b)] = sum;
        }
        const float base = (e[0] + e[1] + e[2]) / 3.0f;
        const float peak = *std::max_element(e.begin(), e.end());
        if (peak < 2.0f * base) {
            return written - 256;
        }
        const float threshold = base + 0.2f * (peak - base);
        for (int b = 1; b < blocks; ++b) {
            if (e[static_cast<size_t>(b)] > threshold && e[static_cast<size_t>(b)] > 2.0f * base) {
                return start + b * kOnsetBlock;
            }
        }
        return written - 256;
    }

    void reset_note_search() {
        std::fill(delta.begin(), delta.end(), 0.0f);
        stable_note = -1;
        stable_count = 0;
        stable_conf = 0.0f;
        verdict_same = 0;
        last_verdict = ExpectedVerdict::NotSure;
        last_present = last_absent = 0;
    }

    void end_current_note() {
        if (state == State::Sounding && current_note >= 0) {
            PitchEvent e;
            e.type = PitchEventType::NoteOff;
            e.note = current_note;
            e.onset_sample = onset;
            e.sample = written;
            emit(e);
        }
        current_note = -1;
    }

    void not_sure() {
        PitchEvent e;
        e.type = expected_count > 0 ? PitchEventType::Expected : PitchEventType::NotSure;
        e.verdict = ExpectedVerdict::NotSure;
        e.onset_sample = onset;
        e.sample = written;
        if (expected_count > 1) {
            e.chord_present = last_present;
            e.chord_absent = last_absent;
        }
        emit(e);
        state = State::Done;
    }

    // `refractory`: no further flux onset for 30 ms (the same attack keeps raising the flux).
    // A gate opening sets none, so a real attack right after noise crept over the gate counts.
    void start_note(std::int64_t at, bool refractory) {
        const bool ringing = state == State::Sounding && current_note >= 0;
        prev_note = ringing ? current_note : -1;
        prev_period = ringing ? current_period : 0.0f;
        end_current_note();
        onset = at;
        if (refractory) {
            last_onset = written;
        }
        state = State::Acquiring;
        reset_note_search();
    }

    void hop() {
        const float rms = gate_rms();
        if (calibrating) {
            calib_sum += rms;
            ++calib_hops;
            return;
        }
        const float threshold = gate_threshold();
        bool opened = false;
        if (!gate_open && rms > threshold) {
            gate_open = true;
            opened = true;
            quiet_hops = 0;
        } else if (gate_open) {
            quiet_hops = rms < 0.6f * threshold ? quiet_hops + 1 : 0;
            if (quiet_hops >= 4) {
                gate_open = false;
            }
        }

        float energy = 0.0f;
        const float flux = spectral_flux(&energy);
        if (!gate_open) {
            if (state == State::Acquiring && written - onset >= 2048) {
                not_sure();
            }
            end_current_note();
            state = State::Idle;
            flux_avg = 0.0f;
            energy_avg = energy;
            recent_energy[static_cast<size_t>(recent_index)] = energy;
            recent_index = (recent_index + 1) % static_cast<int>(recent_energy.size());
            return;
        }
        // A note onset is new spectral content (flux well above its recent level) arriving with
        // more energy. Small flux alone also comes from a fading note and noise; a large jump
        // counts on its own (legato into a note no louder than the last).
        // A bass note's period is longer than the 512-sample frame, so the frame's level swings
        // with the phase. The rise is measured against the loudest hop of the last 2048 samples
        // (over a full period of A0), and it may trail the flux spike by up to two hops.
        const bool refractory = written - last_onset < cfg.sample_rate * 3 / 100;
        const bool spike = flux > 0.06f && flux > 3.0f * flux_avg;
        if (spike && spike_hops == 0) {
            spike_hops = 3;
            spike_energy = *std::max_element(recent_energy.begin(), recent_energy.end());
        }
        const bool flux_onset = !refractory && ((spike && flux > 0.3f) ||
                                                (spike_hops > 0 && energy > 1.2f * spike_energy));
        spike_hops = flux_onset ? 0 : std::max(0, spike_hops - 1);
        if (opened || flux_onset) {
            start_note(locate_onset(), flux_onset);
        } else {
            flux_avg = 0.9f * flux_avg + 0.1f * flux;
        }
        energy_avg = 0.8f * energy_avg + 0.2f * energy;
        recent_energy[static_cast<size_t>(recent_index)] = energy;
        recent_index = (recent_index + 1) % static_cast<int>(recent_energy.size());

        switch (state) {
        case State::Acquiring:
            if (expected_count > 0) {
                expected_step();
            } else {
                acquire_step();
            }
            break;
        case State::Sounding:
            track_step();
            break;
        case State::Idle:
        case State::Done:
            break;
        }
    }

    int frame_length() const {
        return static_cast<int>(std::min<std::int64_t>(written - onset, kMaxFrame));
    }

    bool past_deadline() const { return written - onset >= kMaxFrame + kDeadlineHops * kHop; }

    struct Candidate {
        int note = -1;
        float hz = 0.0f;
        float confidence = 0.0f;
        float period = 0.0f; // waveform period in samples
        bool ready = false;  // enough periods in the window to rule out the octave below
    };

    // Turns a period estimate into a note, correcting octave errors from the spectrum and
    // scaling its confidence by how clean and in-tune the evidence is. `clean` is the frame the
    // period came from: the raw frame, or the combed one with the previous note (fundamental
    // `notch_hz`) filtered out. Octave tests and pitch refinement read the raw spectrum, where no
    // partial is notched; harmonicity, completeness and sub-periods read the clean frame.
    Candidate judge(const dsp::LagCandidate& lag, int length, const dsp::FrameAnalyzer& clean,
                    float notch_hz) const {
        Candidate c;
        const float fs = static_cast<float>(cfg.sample_rate);
        float hz = fs / lag.lag;
        float period = lag.lag;
        float confidence = lag.score;
        const float low_hz = hz_of(cfg.low_note, a4) * 0.97f;
        const float high_hz = hz_of(cfg.high_note, a4) * 1.03f;
        // Stretched partials pull the period sharp (up to a semitone in the top octave). Once the
        // spectrum resolves the partials, move to their peaks: a semitone-wide search, then a
        // fine one.
        const auto refine = [&](float f) {
            const float b =
                dsp::piano_inharmonicity(static_cast<int>(std::lround(note_from_hz(f, a4))));
            if (2.0f * clean.resolution_hz() > 0.35f * f) {
                // Partials not resolved (the lowest notes): undo the average stretch instead.
                return f / (1.0f + dsp::stretch_bias(clean, f, b, max_hz));
            }
            f = dsp::refine_f0(clean, f, b, max_hz, 0.06f);
            return dsp::refine_f0(clean, f, b, max_hz);
        };
        hz = refine(hz);
        for (int pass = 0; pass < 3; ++pass) {
            const int n = static_cast<int>(std::lround(note_from_hz(hz, a4)));
            const auto ev =
                dsp::octave_evidence(clean, hz, dsp::piano_inharmonicity(n),
                                     dsp::piano_inharmonicity(n - 12), low_hz, max_hz, notch_hz);
            if (ev.odd_resolvable && ev.odd_ratio < 0.02f && 2.0f * hz <= high_hz) {
                hz = refine(hz * 2.0f);
                period /= 2.0f;
                continue;
            }
            if (ev.odd_resolvable && ev.triple_ratio < 0.02f && 3.0f * hz <= high_hz) {
                hz = refine(hz * 3.0f);
                period /= 3.0f;
                continue;
            }
            if (ev.half_resolvable && ev.half_ratio > 0.35f) {
                hz = refine(hz * 0.5f);
                period *= 2.0f;
                continue;
            }
            if (ev.third_resolvable && ev.third_ratio > 0.6f) {
                hz = refine(hz / 3.0f);
                period *= 3.0f;
                continue;
            }
            if (ev.odd_resolvable && (ev.odd_ratio < 0.05f || ev.triple_ratio < 0.05f)) {
                confidence *= 0.5f; // between "octave above" and "this note"
            }
            if (ev.half_resolvable && ev.half_ratio > 0.15f) {
                confidence *= 0.5f;
            }
            break;
        }
        if (hz < low_hz || hz > high_hz) {
            return c;
        }
        const float exact = note_from_hz(hz, a4);
        const int note = static_cast<int>(std::lround(exact));
        const float cents = std::fabs(exact - static_cast<float>(note)) * 100.0f;
        if (cents > kMaxCentsFromKey) {
            return c; // between keys: not a note, so the detector ends up "not sure"
        }
        const float b = dsp::piano_inharmonicity(note);
        const float harm = dsp::harmonicity(clean, hz, b, max_hz);
        confidence *= std::clamp((harm - 0.5f) / 0.35f, 0.0f, 1.0f);
        if (!dsp::partials_complete(clean, hz, b, max_hz, notch_hz)) {
            confidence = 0.0f; // most of its partials are missing: two notes' common subharmonic
        }
        // Two notes in a simple ratio (a new note over the last one still ringing) repeat
        // together at a common long period, but each also repeats at its own shorter one. A
        // single piano note scores near zero or below at a third of its period or less, so a
        // strong sub-period there means this pitch is not a note. (Half the period is left to
        // the odd/even octave test: a note with strong even partials repeats there too.)
        for (int k = 3; k <= 6 && confidence > 0.0f; ++k) {
            const float sub = period / static_cast<float>(k);
            if (sub < min_lag) {
                break;
            }
            float best = -1.0f;
            for (float t = sub * 0.98f; t <= sub * 1.02f + 1.0f; t += 1.0f) {
                best = std::max(best, clean.nsdf_at(t));
            }
            if (best >= 0.45f) {
                confidence = 0.0f;
            }
        }
        c.note = note;
        c.hz = hz;
        c.confidence = std::clamp(confidence, 0.0f, 1.0f);
        c.period = period;
        c.ready = length >= pitch_window_samples(note, cfg.sample_rate, cfg.octave_check_periods);
        return c;
    }

    void acquire_step() {
        const int length = frame_length();
        if (length < kMinFrame) {
            return;
        }
        copy_recent(frame.data(), written, length);
        const int lag_limit = std::min(length / 2, max_lag);
        analyzer.analyze(frame.data(), length, lag_limit);

        // While the previous note still rings, its partials blur the new note's period and pull
        // in common subharmonics. Comb it out (the audio before the onset cancels it from the
        // first sample) and search the result too. Candidates come from both frames, each judged
        // on its own; the old note counts from the raw frame only if it was struck again (the
        // comb then leaves little).
        bool combed = false;
        bool restruck = false;
        const bool ringing =
            prev_period > 0.0f && static_cast<float>(written - onset) <
                                      kCombSeconds * static_cast<float>(cfg.sample_rate);
        if (ringing) {
            const std::int64_t start = written - length;
            const auto whole = static_cast<std::int64_t>(prev_period);
            const float frac = prev_period - static_cast<float>(whole);
            for (int j = 0; j < length; ++j) {
                const std::int64_t at = start + j - whole;
                const float delayed = sample_at(at) + (sample_at(at - 1) - sample_at(at)) * frac;
                comb[static_cast<size_t>(j)] = frame[static_cast<size_t>(j)] - delayed;
            }
            comb_analyzer.analyze(comb.data(), length, lag_limit);
            const float raw = analyzer.rms();
            const float residual =
                raw > 0.0f ? (comb_analyzer.rms() * comb_analyzer.rms()) / (raw * raw) : 0.0f;
            restruck = residual < kRestrikeResidual;
            combed = !restruck;
        }

        std::array<Candidate, 2 * dsp::kMaxLagCandidates> cands{};
        int found = 0;
        const auto collect = [&](const dsp::FrameAnalyzer& source, float notch_hz) {
            std::array<dsp::LagCandidate, dsp::kMaxLagCandidates> lags{};
            const int n = cfg.method == PitchMethod::Mpm
                              ? dsp::mpm_candidates(source, min_lag, 0.9f, lags.data())
                              : dsp::yin_candidates(source, min_lag, 0.15f, lags.data());
            for (int i = 0; i < n; ++i) {
                Candidate c = judge(lags[static_cast<size_t>(i)], length, source, notch_hz);
                const bool old_note = prev_note >= 0 && c.note == prev_note;
                if (old_note && ringing && !restruck) {
                    c.confidence = 0.0f; // the old note still ringing, not a new strike
                }
                cands[static_cast<size_t>(found++)] = c;
            }
        };
        collect(analyzer, 0.0f);
        if (combed) {
            collect(comb_analyzer, static_cast<float>(cfg.sample_rate) / prev_period);
        }

        // Observation log-likelihoods: candidates get their confidence, other notes a small
        // floor, and the unvoiced state whatever confidence is left.
        const float floor_log = std::log(1e-3f);
        std::fill(obs.begin(), obs.end(), floor_log);
        float voiced = 0.0f;
        for (int i = 0; i < found; ++i) {
            const auto& c = cands[static_cast<size_t>(i)];
            if (c.note < cfg.low_note || c.note > cfg.high_note || c.confidence <= 0.0f) {
                continue;
            }
            auto& o = obs[static_cast<size_t>(c.note - cfg.low_note)];
            o = std::max(o, std::log(std::max(c.confidence, 1e-3f)));
            voiced = std::max(voiced, c.confidence);
        }
        obs.back() = std::log(std::max(1.0f - voiced, 0.02f));

        // One step of the max-product (Viterbi) recursion with "stay" much likelier than "jump",
        // so a single odd frame cannot move the path.
        const float stay = std::log(0.95f);
        const float jump = std::log(0.05f / static_cast<float>(states));
        const float best_prev = *std::max_element(delta.begin(), delta.end());
        for (size_t s = 0; s < delta.size(); ++s) {
            next[s] = std::max(delta[s] + stay, best_prev + jump) + obs[s];
        }
        const auto top_it = std::max_element(next.begin(), next.end());
        const float top = *top_it;
        const int best = static_cast<int>(top_it - next.begin());
        for (size_t s = 0; s < next.size(); ++s) {
            delta[s] = next[s] - top;
        }

        const Candidate* support = nullptr;
        if (best < states - 1) {
            const int note = cfg.low_note + best;
            for (int i = 0; i < found; ++i) {
                const auto& c = cands[static_cast<size_t>(i)];
                if (c.note == note && (support == nullptr || c.confidence > support->confidence)) {
                    support = &c;
                }
            }
        }
        if (support != nullptr && support->ready && support->confidence >= cfg.min_confidence) {
            if (support->note == stable_note) {
                ++stable_count;
                stable_conf += support->confidence;
            } else {
                stable_note = support->note;
                stable_count = 1;
                stable_conf = support->confidence;
            }
        } else {
            stable_count = 0;
            stable_note = -1;
            stable_conf = 0.0f;
        }

        if (stable_count >= cfg.confirm_frames) {
            const float refined = support->hz;
            PitchEvent e;
            e.type = PitchEventType::NoteOn;
            e.note = stable_note;
            e.frequency_hz = refined;
            e.confidence = stable_conf / static_cast<float>(stable_count);
            e.onset_sample = onset;
            e.sample = written;
            emit(e);
            current_note = stable_note;
            current_period = support->period;
            state = State::Sounding;
            track_counter = 0;
            track_misses = 0;
            if (cfg.learn_tuning && e.confidence >= 0.9f) {
                learn_tuning(stable_note, refined);
            }
            return;
        }
        if (past_deadline()) {
            not_sure();
        }
    }

    // While a note sounds, check now and then that its period is still there.
    void track_step() {
        if (++track_counter < kTrackEvery) {
            return;
        }
        track_counter = 0;
        const int length = std::clamp(
            pitch_window_samples(current_note, cfg.sample_rate, cfg.octave_check_periods), 512,
            kMaxFrame);
        const float period = static_cast<float>(cfg.sample_rate) / hz_of(current_note, a4);
        const int lag_max = std::min(length / 2, static_cast<int>(std::ceil(period * 1.04f)) + 2);
        if (static_cast<float>(lag_max) < period * 1.03f) {
            return;
        }
        copy_recent(frame.data(), written, length);
        analyzer.analyze(frame.data(), length, lag_max);
        float best = -1.0f;
        for (int t = static_cast<int>(period * 0.97f); t <= static_cast<int>(period * 1.03f) + 1;
             ++t) {
            best = std::max(best, analyzer.nsdf_at(static_cast<float>(t)));
        }
        track_misses = best < 0.4f ? track_misses + 1 : 0;
        if (track_misses >= kTrackMisses) {
            end_current_note();
            state = State::Done;
        }
    }

    void expected_step() {
        const std::int64_t elapsed = written - onset;
        const int needed = expected_count == 1
                               ? ExpectedNoteChecker::window_samples(expected[0], cfg.sample_rate)
                               : ExpectedNoteChecker::chord_window_samples(
                                     expected.data(), expected_count, cfg.sample_rate);
        if (elapsed < needed) {
            return;
        }
        const int length = frame_length();
        copy_recent(frame.data(), written, length);
        PitchEvent e;
        e.type = PitchEventType::Expected;
        e.onset_sample = onset;
        e.sample = written;
        bool decided = false;
        if (expected_count == 1) {
            const auto r = checker.check(frame.data(), length, expected[0], a4);
            if (r.verdict != ExpectedVerdict::NotSure && r.verdict == last_verdict) {
                ++verdict_same;
            } else {
                verdict_same = r.verdict != ExpectedVerdict::NotSure ? 1 : 0;
            }
            last_verdict = r.verdict;
            if (verdict_same >= 2) {
                e.verdict = r.verdict;
                e.note = r.heard_note;
                e.confidence = r.confidence;
                decided = true;
            }
        } else {
            const auto r =
                checker.check_chord(frame.data(), length, expected.data(), expected_count, a4);
            std::uint16_t present = 0;
            std::uint16_t absent = 0;
            bool unsure = false;
            float conf = 1.0f;
            for (int i = 0; i < r.count; ++i) {
                if (r.states[i] == ChordNoteState::Present) {
                    present = static_cast<std::uint16_t>(present | (1u << i));
                } else if (r.states[i] == ChordNoteState::Absent) {
                    absent = static_cast<std::uint16_t>(absent | (1u << i));
                } else {
                    unsure = true;
                }
                conf = std::min(conf, r.states[i] == ChordNoteState::Absent ? 1.0f - r.scores[i]
                                                                            : r.scores[i]);
            }
            if (!unsure && present == last_present && absent == last_absent) {
                ++verdict_same;
            } else {
                verdict_same = unsure ? 0 : 1;
            }
            last_present = present;
            last_absent = absent;
            if (verdict_same >= 2) {
                e.verdict = absent == 0 ? ExpectedVerdict::Expected : ExpectedVerdict::NotSure;
                e.chord_present = present;
                e.chord_absent = absent;
                e.confidence = conf;
                decided = true;
            }
        }
        if (decided) {
            emit(e);
            state = State::Done;
            return;
        }
        if (elapsed >= std::max<std::int64_t>(needed, kMaxFrame) + kDeadlineHops * kHop) {
            not_sure();
        }
    }

    void learn_tuning(int note, float hz) {
        const float implied = hz / std::exp2(static_cast<float>(note - kA4) / 12.0f);
        tuning[static_cast<size_t>(tuning_next)] = implied;
        tuning_next = (tuning_next + 1) % kTuningNotes;
        tuning_count = std::min(tuning_count + 1, kTuningNotes);
        if (tuning_count < kTuningMinNotes) {
            return;
        }
        std::array<float, kTuningNotes> sorted = tuning;
        std::sort(sorted.begin(), sorted.begin() + tuning_count);
        const float median = tuning_count % 2 == 1
                                 ? sorted[static_cast<size_t>(tuning_count / 2)]
                                 : 0.5f * (sorted[static_cast<size_t>(tuning_count / 2 - 1)] +
                                           sorted[static_cast<size_t>(tuning_count / 2)]);
        const float limit = std::exp2(50.0f / 1200.0f);
        a4 = std::clamp(median, a4_base / limit, a4_base * limit);
    }
};

PitchDetector::PitchDetector(const PitchDetectorConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}
PitchDetector::~PitchDetector() = default;
PitchDetector::PitchDetector(PitchDetector&&) noexcept = default;
PitchDetector& PitchDetector::operator=(PitchDetector&&) noexcept = default;

int PitchDetector::process(const float* samples, int count, PitchEvent* out, int capacity) {
    if (samples == nullptr || count <= 0) {
        return 0;
    }
    Impl& d = *impl_;
    d.out = out;
    d.capacity = out != nullptr ? std::max(0, capacity) : 0;
    d.emitted = 0;
    for (int i = 0; i < count; ++i) {
        const float v = samples[i];
        d.history[static_cast<size_t>(d.written & (kHistory - 1))] = std::isfinite(v) ? v : 0.0f;
        ++d.written;
        if (++d.since_hop == kHop) {
            d.since_hop = 0;
            d.hop();
        }
    }
    d.out = nullptr;
    return d.emitted;
}

void PitchDetector::reset() {
    Impl& d = *impl_;
    std::fill(d.history.begin(), d.history.end(), 0.0f);
    std::fill(d.onset_prev.begin(), d.onset_prev.end(), 0.0f);
    d.written = 0;
    d.since_hop = 0;
    d.gate_open = false;
    d.quiet_hops = 0;
    d.hop_energy.fill(0.0);
    d.hop_index = 0;
    d.flux_avg = 0.0f;
    d.energy_avg = 0.0f;
    d.spike_hops = 0;
    d.recent_energy.fill(0.0f);
    d.recent_index = 0;
    d.last_onset = std::numeric_limits<std::int64_t>::min() / 2;
    d.state = Impl::State::Idle;
    d.current_note = -1;
    d.reset_note_search();
}

std::int64_t PitchDetector::samples_processed() const { return impl_->written; }
std::int64_t PitchDetector::dropped_events() const { return impl_->dropped; }

void PitchDetector::begin_noise_calibration() {
    impl_->calibrating = true;
    impl_->calib_sum = 0.0;
    impl_->calib_hops = 0;
}

void PitchDetector::end_noise_calibration() {
    Impl& d = *impl_;
    d.calibrating = false;
    if (d.calib_hops > 0) {
        d.noise_floor = static_cast<float>(d.calib_sum / static_cast<double>(d.calib_hops));
    }
}

float PitchDetector::noise_floor_rms() const { return impl_->noise_floor; }
void PitchDetector::set_noise_floor_rms(float rms) { impl_->noise_floor = std::max(0.0f, rms); }

void PitchDetector::set_a4_hz(float a4_hz) {
    if (a4_hz > 0.0f && std::isfinite(a4_hz)) {
        impl_->a4 = a4_hz;
        impl_->a4_base = a4_hz;
        impl_->tuning_count = 0;
        impl_->tuning_next = 0;
    }
}

float PitchDetector::a4_hz() const { return impl_->a4; }
void PitchDetector::set_method(PitchMethod method) { impl_->cfg.method = method; }

void PitchDetector::set_expected_notes(const int* notes, int count) {
    Impl& d = *impl_;
    d.expected_count = 0;
    for (int i = 0; i < count && d.expected_count < kMaxChordNotes; ++i) {
        if (notes[i] >= d.cfg.low_note && notes[i] <= d.cfg.high_note) {
            d.expected[static_cast<size_t>(d.expected_count++)] = notes[i];
        }
    }
}

} // namespace toonval
