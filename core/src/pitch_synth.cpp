// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/pitch_synth.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>

namespace toonval {

namespace {

constexpr double kPi = 3.14159265358979323846;

// splitmix64: small, fast, and the same everywhere.
class Rng {
  public:
    explicit Rng(std::uint64_t seed) : state_(seed) {}
    std::uint64_t next() {
        std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    // Uniform in [0, 1).
    double uniform() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }
    double uniform(double lo, double hi) { return lo + (hi - lo) * uniform(); }

  private:
    std::uint64_t state_;
};

double note_hz(int note, double a4) { return a4 * std::exp2((note - 69) / 12.0); }

// Adds a decaying sinusoid: amplitude `amp` at `start`, multiplied by `env` during the attack.
void add_partial(float* out, int length, int start, double hz, double tau_s, double amp,
                 double phase, int rise, int sample_rate) {
    const double fs = static_cast<double>(sample_rate);
    const std::complex<double> step =
        std::polar(std::exp(-1.0 / (tau_s * fs)), 2.0 * kPi * hz / fs);
    std::complex<double> z = std::polar(amp, phase);
    for (int i = start; i < length; ++i) {
        const int k = i - start;
        const double env = k < rise ? 0.5 - 0.5 * std::cos(kPi * k / rise) : 1.0;
        out[i] += static_cast<float>(env * z.imag());
        z *= step;
        if (std::abs(z.imag()) < 1e-9 && std::abs(z.real()) < 1e-9) {
            break; // decayed below anything audible
        }
    }
}

} // namespace

float piano_tone_amplitude(float velocity) {
    const float v = std::clamp(velocity, 0.0f, 1.0f);
    return 0.5f * std::pow(10.0f, (v - 1.0f) * 1.5f);
}

float synth_inharmonicity(int note) {
    struct Anchor {
        int note;
        double b;
    };
    static constexpr std::array<Anchor, 8> kAnchors = {{{21, 4.0e-4},
                                                        {36, 1.3e-4},
                                                        {48, 1.2e-4},
                                                        {60, 3.0e-4},
                                                        {72, 6.0e-4},
                                                        {84, 1.6e-3},
                                                        {96, 5.0e-3},
                                                        {108, 1.8e-2}}};
    if (note <= kAnchors.front().note) {
        return static_cast<float>(kAnchors.front().b);
    }
    for (size_t i = 1; i < kAnchors.size(); ++i) {
        if (note <= kAnchors[i].note) {
            const auto& lo = kAnchors[i - 1];
            const auto& hi = kAnchors[i];
            const double t = static_cast<double>(note - lo.note) / (hi.note - lo.note);
            return static_cast<float>(
                std::exp(std::log(lo.b) + t * (std::log(hi.b) - std::log(lo.b))));
        }
    }
    return static_cast<float>(kAnchors.back().b);
}

void add_piano_tone(float* out, int length, int start, const PianoToneOptions& o, int sample_rate) {
    if (out == nullptr || start >= length || start < 0) {
        return;
    }
    Rng rng(o.seed * 0x2545F4914F6CDD1Dull + 17);
    const double fs = static_cast<double>(sample_rate);
    const double v = std::clamp(static_cast<double>(o.velocity), 0.0, 1.0);
    const double f0 = note_hz(o.note, o.a4_hz) * std::exp2(o.detune_cents / 1200.0);
    const double b = synth_inharmonicity(o.note) * std::max(0.0f, o.inharmonicity_scale);
    const double top_hz = std::min(0.45 * fs, 12000.0);
    const double alpha = 1.9 - 0.9 * v;            // louder is brighter
    const double strike = rng.uniform(0.11, 0.14); // hammer position: partials near 1/strike vanish
    const double bass = std::clamp((o.note - 21) / 24.0, 0.0, 1.0);

    std::array<double, 64> weight{};
    std::array<double, 64> freq{};
    int partials = 0;
    double total = 0.0;
    for (int n = 1; n <= 64; ++n) {
        const double f = f0 * n * std::sqrt(1.0 + b * n * n);
        if (f > top_hz) {
            break;
        }
        double a = std::pow(static_cast<double>(n), -alpha);
        a *= std::fabs(std::sin(kPi * n * strike)) + 0.05;
        if (n == 1) {
            a *= 0.12 + 0.88 * bass; // bass strings have a weak fundamental
        } else if (n == 2) {
            a *= 0.4 + 0.6 * bass;
        }
        a *= rng.uniform(0.7, 1.3);
        weight[static_cast<size_t>(partials)] = a;
        freq[static_cast<size_t>(partials)] = f;
        total += a;
        ++partials;
    }
    if (partials == 0) {
        return;
    }
    const double amp = piano_tone_amplitude(o.velocity) / total;
    const double register_t = (o.note - 21) / 87.0;
    const double tau1 = 2.5 * std::exp2(-(o.note - 21) / 26.0);
    const int rise = static_cast<int>(fs * (1.0 + 3.0 * (1.0 - register_t)) / 1000.0);
    const bool two_strings = o.note >= 30;
    const double detune_a = two_strings ? rng.uniform(-1.2, 1.2) : 0.0;
    const double detune_b = two_strings ? rng.uniform(-1.2, 1.2) : 0.0;
    for (int i = 0; i < partials; ++i) {
        const auto k = static_cast<size_t>(i);
        const double tau = tau1 / (1.0 + 0.08 * i * (1.0 + (o.note - 21) / 40.0));
        // Two strings (or two polarisations of one): a fast "prompt" decay and a slow
        // "aftersound", slightly detuned so they beat. One hammer strikes both, so they start
        // in phase.
        const double phase = rng.uniform(0.0, 2.0 * kPi);
        add_partial(out, length, start, freq[k] * std::exp2(detune_a / 1200.0), tau,
                    0.6 * amp * weight[k], phase, rise, sample_rate);
        add_partial(out, length, start, freq[k] * std::exp2(detune_b / 1200.0), 3.0 * tau,
                    0.4 * amp * weight[k], phase, rise, sample_rate);
    }

    // Hammer noise: low-passed white noise, a few milliseconds long, louder when struck harder.
    const double level = o.hammer_noise * piano_tone_amplitude(o.velocity) * 0.25 * v;
    const double cutoff = 1500.0 + 3000.0 * v;
    const double coeff = 1.0 - std::exp(-2.0 * kPi * cutoff / fs);
    const int noise_len = std::min(length - start, static_cast<int>(0.04 * fs));
    double lp = 0.0;
    for (int k = 0; k < noise_len; ++k) {
        lp += coeff * (rng.uniform(-1.0, 1.0) - lp);
        const double env = std::exp(-k / (0.004 * fs)) * std::min(1.0, k / (0.0003 * fs));
        out[start + k] += static_cast<float>(level * env * lp * 3.0);
    }
}

void add_room_noise(float* out, int length, float rms, std::uint64_t seed) {
    if (out == nullptr || length <= 0 || rms <= 0.0f) {
        return;
    }
    // Paul Kellet's economy pink filter. Run twice with the same seed: once to measure, once to
    // add at the requested RMS.
    double scale = 0.0;
    for (int pass = 0; pass < 2; ++pass) {
        Rng rng(seed ^ 0xA5A5A5A5DEADBEEFull);
        double b0 = 0.0;
        double b1 = 0.0;
        double b2 = 0.0;
        double sum = 0.0;
        for (int i = 0; i < length; ++i) {
            const double white = rng.uniform(-1.0, 1.0);
            b0 = 0.99765 * b0 + white * 0.0990460;
            b1 = 0.96300 * b1 + white * 0.2965164;
            b2 = 0.57000 * b2 + white * 1.0526913;
            const double pink = b0 + b1 + b2 + white * 0.1848;
            if (pass == 0) {
                sum += pink * pink;
            } else {
                out[i] += static_cast<float>(pink * scale);
            }
        }
        if (pass == 0) {
            const double measured = std::sqrt(sum / length);
            scale = measured > 0.0 ? rms / measured : 0.0;
        }
    }
}

SynthClip make_synth_clip(int note, float velocity, std::uint64_t seed,
                          const SynthClipOptions& options, int sample_rate) {
    SynthClip clip;
    Rng rng(seed * 0x9E3779B97F4A7C15ull + static_cast<std::uint64_t>(note) * 7919u + 1);
    const int length = static_cast<int>(
        std::lround(static_cast<double>(options.lead_in_s + options.note_s) * sample_rate));
    clip.samples.assign(static_cast<size_t>(length), 0.0f);
    clip.onset = std::lround(static_cast<double>(options.lead_in_s) * sample_rate);
    clip.note = note;
    clip.detune_cents =
        static_cast<float>(rng.uniform(-options.max_detune_cents, options.max_detune_cents));
    const float b_scale =
        static_cast<float>(1.0 + options.inharmonicity_spread * rng.uniform(-1.0, 1.0));

    const std::uint64_t noise_seed = rng.next();
    PianoToneOptions tone;
    tone.note = note;
    tone.velocity = velocity;
    tone.detune_cents = clip.detune_cents;
    tone.inharmonicity_scale = b_scale;
    tone.seed = rng.next();
    add_piano_tone(clip.samples.data(), length, static_cast<int>(clip.onset), tone, sample_rate);

    if (options.interferer) {
        int other = note;
        while (other == note) {
            other = note + static_cast<int>(rng.uniform(-19.0, 20.0));
        }
        other = std::clamp(other, 21, 108);
        if (other == note) {
            other = note <= 101 ? note + 7 : note - 7;
        }
        clip.interferer_note = other;
        // A steady tone (another instrument, a TV): four partials, very slow decay, at
        // interferer_db against the note's RMS over its first 100 ms.
        const int window = std::min(length - static_cast<int>(clip.onset), sample_rate / 10);
        double sum = 0.0;
        for (int i = 0; i < window; ++i) {
            const double x = clip.samples[static_cast<size_t>(clip.onset + i)];
            sum += x * x;
        }
        const double note_rms = window > 0 ? std::sqrt(sum / window) : 0.0;
        const double weights[4] = {0.48, 0.24, 0.16, 0.12};
        const double unit_rms =
            std::sqrt((0.48 * 0.48 + 0.24 * 0.24 + 0.16 * 0.16 + 0.12 * 0.12) / 2.0);
        const double level = note_rms * std::pow(10.0, options.interferer_db / 20.0) / unit_rms;
        const double hz = note_hz(other, 440.0) * std::exp2(rng.uniform(-8.0, 8.0) / 1200.0);
        for (int n = 1; n <= 4; ++n) {
            if (hz * n < 0.45 * sample_rate) {
                add_partial(clip.samples.data(), length, 0, hz * n, 4.0, level * weights[n - 1],
                            rng.uniform(0.0, 2.0 * kPi), sample_rate / 200, sample_rate);
            }
        }
    }
    add_room_noise(clip.samples.data(), length, options.noise_rms, noise_seed);
    return clip;
}

SynthClip make_synth_chord_clip(const int* notes, int count, float velocity, std::uint64_t seed,
                                const SynthClipOptions& options, int sample_rate) {
    SynthClip clip;
    Rng rng(seed * 0x9E3779B97F4A7C15ull + 99);
    const int length = static_cast<int>(
        std::lround(static_cast<double>(options.lead_in_s + options.note_s) * sample_rate));
    clip.samples.assign(static_cast<size_t>(length), 0.0f);
    clip.onset = std::lround(static_cast<double>(options.lead_in_s) * sample_rate);
    clip.note = count > 0 ? notes[0] : -1;
    add_room_noise(clip.samples.data(), length, options.noise_rms, rng.next());
    for (int i = 0; i < count; ++i) {
        PianoToneOptions tone;
        tone.note = notes[i];
        tone.velocity = velocity;
        tone.detune_cents =
            static_cast<float>(rng.uniform(-options.max_detune_cents, options.max_detune_cents));
        tone.inharmonicity_scale =
            static_cast<float>(1.0 + options.inharmonicity_spread * rng.uniform(-1.0, 1.0));
        tone.seed = rng.next();
        add_piano_tone(clip.samples.data(), length, static_cast<int>(clip.onset), tone,
                       sample_rate);
    }
    return clip;
}

} // namespace toonval
