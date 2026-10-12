// SPDX-License-Identifier: GPL-3.0-or-later
#include "pitch_dsp.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>

namespace toonval::pitch_dsp {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Vertex offset of the parabola through (-1, a), (0, b), (1, c), in [-0.5, 0.5].
float parabola_offset(float a, float b, float c) {
    const float denom = a - 2.0f * b + c;
    if (std::fabs(denom) < 1e-12f) {
        return 0.0f;
    }
    return std::clamp(0.5f * (a - c) / denom, -0.5f, 0.5f);
}

float parabola_value(float a, float b, float c, float offset) {
    return b - 0.25f * (a - c) * offset;
}

// True when `lag` is close to an integer multiple (2 or more) of `base`.
bool near_multiple(float lag, float base) {
    if (base <= 0.0f) {
        return false;
    }
    const float ratio = lag / base;
    const float nearest = std::round(ratio);
    return nearest >= 2.0f && std::fabs(ratio - nearest) < 0.04f * nearest;
}

} // namespace

int next_pow2(int n) {
    int p = 1;
    while (p < n) {
        p <<= 1;
    }
    return p;
}

RealFft::RealFft(int max_size) : max_size_(std::max(kMinFft, next_pow2(max_size))) {
    for (int n = kMinFft; n <= max_size_; n <<= 1) {
        forward_.push_back(kiss_fftr_alloc(n, 0, nullptr, nullptr));
        inverse_.push_back(kiss_fftr_alloc(n, 1, nullptr, nullptr));
    }
}

RealFft::~RealFft() {
    for (auto* cfg : forward_) {
        kiss_fftr_free(cfg);
    }
    for (auto* cfg : inverse_) {
        kiss_fftr_free(cfg);
    }
}

int RealFft::index_of(int n) const {
    int index = 0;
    for (int size = kMinFft; size < n; size <<= 1) {
        ++index;
    }
    return index;
}

void RealFft::forward(int n, const float* in, kiss_fft_cpx* out) const {
    kiss_fftr(forward_[static_cast<size_t>(index_of(n))], in, out);
}

void RealFft::inverse(int n, const kiss_fft_cpx* in, float* out) const {
    kiss_fftri(inverse_[static_cast<size_t>(index_of(n))], in, out);
}

FrameAnalyzer::FrameAnalyzer(int sample_rate, int max_frame)
    : sample_rate_(sample_rate), max_frame_(next_pow2(std::max(max_frame, kMinFft))),
      fft_(2 * max_frame_) {
    const auto n = static_cast<size_t>(fft_.max_size());
    time_.assign(n, 0.0f);
    spectrum_.assign(n / 2 + 1, kiss_fft_cpx{0.0f, 0.0f});
    acf_.assign(n, 0.0f);
    energy_prefix_.assign(static_cast<size_t>(max_frame_) + 1, 0.0);
    nsdf_.assign(static_cast<size_t>(max_frame_) / 2 + 2, 0.0f);
    cmndf_.assign(static_cast<size_t>(max_frame_) / 2 + 2, 1.0f);
    magnitude_.assign(n / 2 + 1, 0.0f);
}

void FrameAnalyzer::analyze(const float* x, int length, int max_lag) {
    length_ = std::clamp(length, 2, max_frame_);
    max_lag_ = std::clamp(max_lag, 1, length_ / 2);
    nfft_ = std::max(kMinFft, next_pow2(2 * length_));
    const auto len = static_cast<size_t>(length_);
    const auto nfft = static_cast<size_t>(nfft_);

    // Autocorrelation: |FFT|^2 then inverse, zero-padded so lags never wrap.
    std::copy(x, x + length_, time_.begin());
    std::fill(time_.begin() + static_cast<std::ptrdiff_t>(len),
              time_.begin() + static_cast<std::ptrdiff_t>(nfft), 0.0f);
    fft_.forward(nfft_, time_.data(), spectrum_.data());
    for (size_t k = 0; k <= nfft / 2; ++k) {
        const float re = spectrum_[k].r;
        const float im = spectrum_[k].i;
        spectrum_[k].r = re * re + im * im;
        spectrum_[k].i = 0.0f;
    }
    fft_.inverse(nfft_, spectrum_.data(), acf_.data());
    const float inv_n = 1.0f / static_cast<float>(nfft_);

    energy_prefix_[0] = 0.0;
    for (size_t j = 0; j < len; ++j) {
        energy_prefix_[j + 1] = energy_prefix_[j] + static_cast<double>(x[j]) * x[j];
    }
    const double total = energy_prefix_[len];
    rms_ = static_cast<float>(std::sqrt(total / static_cast<double>(len)));
    const double tiny = 1e-20 + total * 1e-9;

    double running = 0.0;
    nsdf_[0] = 1.0f;
    cmndf_[0] = 1.0f;
    for (int lag = 1; lag <= max_lag_; ++lag) {
        const auto t = static_cast<size_t>(lag);
        const double m = energy_prefix_[len - t] + (total - energy_prefix_[t]);
        const double r = static_cast<double>(acf_[t]) * inv_n;
        nsdf_[t] = m > tiny ? static_cast<float>(2.0 * r / m) : 0.0f;
        const double d = std::max(0.0, m - 2.0 * r);
        running += d;
        cmndf_[t] = running > tiny ? static_cast<float>(d * lag / running) : 1.0f;
    }

    // Hann-windowed spectrum. The window comes from a cosine recurrence, not a call per sample.
    const double step = 2.0 * kPi / static_cast<double>(std::max(1, length_ - 1));
    const double cs = std::cos(step);
    const double sn = std::sin(step);
    double c = 1.0;
    double s = 0.0;
    double window_sum = 0.0;
    for (size_t j = 0; j < len; ++j) {
        const double w = 0.5 - 0.5 * c;
        window_sum += w;
        time_[j] = static_cast<float>(x[j] * w);
        const double nc = c * cs - s * sn;
        s = s * cs + c * sn;
        c = nc;
    }
    fft_.forward(nfft_, time_.data(), spectrum_.data());
    const float scale = window_sum > 0.0 ? static_cast<float>(2.0 / window_sum) : 0.0f;
    for (size_t k = 0; k <= nfft / 2; ++k) {
        magnitude_[k] = std::hypot(spectrum_[k].r, spectrum_[k].i) * scale;
    }
}

float FrameAnalyzer::nsdf_at(float lag) const {
    if (lag < 0.0f || lag > static_cast<float>(max_lag_)) {
        return 0.0f;
    }
    const int i = static_cast<int>(lag);
    const float frac = lag - static_cast<float>(i);
    const float a = nsdf_[static_cast<size_t>(i)];
    const float b = i + 1 <= max_lag_ ? nsdf_[static_cast<size_t>(i) + 1] : a;
    return a + (b - a) * frac;
}

float FrameAnalyzer::peak(float hz, float tol_hz, float* peak_hz) const {
    const float bin = bin_hz();
    const int last = nfft_ / 2 - 1;
    int lo = static_cast<int>(std::ceil((hz - tol_hz) / bin));
    int hi = static_cast<int>(std::floor((hz + tol_hz) / bin));
    lo = std::max(lo, 1);
    hi = std::min(hi, last);
    if (lo > hi) {
        const int k = static_cast<int>(std::lround(hz / bin));
        if (k < 1 || k > last) {
            if (peak_hz != nullptr) {
                *peak_hz = hz;
            }
            return 0.0f;
        }
        lo = hi = k;
    }
    int best = lo;
    for (int k = lo + 1; k <= hi; ++k) {
        if (magnitude_[static_cast<size_t>(k)] > magnitude_[static_cast<size_t>(best)]) {
            best = k;
        }
    }
    const float mag = magnitude_[static_cast<size_t>(best)];
    if (peak_hz != nullptr) {
        const float eps = 1e-12f;
        const float a = std::log(magnitude_[static_cast<size_t>(best) - 1] + eps);
        const float b = std::log(mag + eps);
        const float c = std::log(magnitude_[static_cast<size_t>(best) + 1] + eps);
        *peak_hz = (static_cast<float>(best) + parabola_offset(a, b, c)) * bin;
    }
    return mag;
}

float FrameAnalyzer::peak_magnitude(float hz, float tol_hz) const {
    return peak(hz, tol_hz, nullptr);
}

float FrameAnalyzer::band_energy(float lo_hz, float hi_hz) const {
    const float bin = bin_hz();
    const int lo = std::max(1, static_cast<int>(std::ceil(lo_hz / bin)));
    const int hi = std::min(nfft_ / 2, static_cast<int>(std::floor(hi_hz / bin)));
    float sum = 0.0f;
    for (int k = lo; k <= hi; ++k) {
        const float m = magnitude_[static_cast<size_t>(k)];
        sum += m * m;
    }
    return sum;
}

int mpm_candidates(const FrameAnalyzer& a, float min_lag, float k, LagCandidate* out) {
    const float* n = a.nsdf();
    const int max_lag = a.max_lag();
    struct KeyMax {
        int index;
        float value;
    };
    std::array<KeyMax, 96> keys{};
    int key_count = 0;

    int t = 1;
    while (t <= max_lag && n[t] > 0.0f) { // skip the lobe around lag 0
        ++t;
    }
    bool in_region = false;
    int best = 0;
    for (; t <= max_lag; ++t) {
        if (n[t] > 0.0f) {
            if (!in_region || n[t] > n[best]) {
                best = t;
            }
            in_region = true;
        } else if (in_region) {
            if (static_cast<float>(best) >= min_lag && key_count < static_cast<int>(keys.size())) {
                keys[static_cast<size_t>(key_count++)] = {best, n[best]};
            }
            in_region = false;
        }
    }
    // A region still open at the end counts only if its peak is not at the edge.
    if (in_region && best < max_lag - 1 && static_cast<float>(best) >= min_lag &&
        key_count < static_cast<int>(keys.size())) {
        keys[static_cast<size_t>(key_count++)] = {best, n[best]};
    }
    if (key_count == 0) {
        return 0;
    }
    int highest = 0;
    for (int i = 1; i < key_count; ++i) {
        if (keys[static_cast<size_t>(i)].value > keys[static_cast<size_t>(highest)].value) {
            highest = i;
        }
    }
    const float threshold = k * keys[static_cast<size_t>(highest)].value;
    int winner = highest;
    for (int i = 0; i < key_count; ++i) {
        if (keys[static_cast<size_t>(i)].value >= threshold) {
            winner = i;
            break;
        }
    }
    auto refine = [&](int index) {
        const int i = keys[static_cast<size_t>(index)].index;
        const float y0 = n[i - 1];
        const float y1 = n[i];
        const float y2 = i + 1 <= max_lag ? n[i + 1] : y1;
        const float off = parabola_offset(y0, y1, y2);
        return LagCandidate{static_cast<float>(i) + off,
                            std::clamp(parabola_value(y0, y1, y2, off), 0.0f, 1.0f)};
    };
    out[0] = refine(winner);
    int count = 1;
    if (highest != winner) {
        const LagCandidate other = refine(highest);
        if (!near_multiple(other.lag, out[0].lag)) {
            out[count++] = other;
        }
    }
    for (int div = 2; div <= 6 && count < kMaxLagCandidates; ++div) {
        const float target = out[0].lag / static_cast<float>(div);
        for (int i = 0; i < key_count; ++i) {
            const auto& key = keys[static_cast<size_t>(i)];
            if (i != winner && i != highest && key.value >= 0.5f &&
                std::fabs(static_cast<float>(key.index) - target) <= 0.03f * target + 1.0f) {
                out[count++] = refine(i);
                break;
            }
        }
    }
    return count;
}

int yin_candidates(const FrameAnalyzer& a, float min_lag, float threshold, LagCandidate* out) {
    const float* d = a.cmndf();
    const int last = a.max_lag() - 1; // keep one lag of margin for the parabola
    const int first = std::max(2, static_cast<int>(std::ceil(min_lag)));
    if (first > last) {
        return 0;
    }
    int global = first;
    for (int t = first; t <= last; ++t) {
        if (d[t] < d[global]) {
            global = t;
        }
    }
    int winner = -1;
    for (int t = first; t <= last; ++t) {
        if (d[t] < threshold) {
            while (t + 1 <= last && d[t + 1] < d[t]) {
                ++t;
            }
            winner = t;
            break;
        }
    }
    // The global minimum on the edge of the range is not a dip.
    if (winner < 0) {
        if (global == first || global == last) {
            return 0;
        }
        winner = global;
    }
    auto refine = [&](int i) {
        const float y0 = d[i - 1];
        const float y1 = d[i];
        const float y2 = d[i + 1];
        const float off = parabola_offset(y0, y1, y2);
        return LagCandidate{static_cast<float>(i) + off,
                            std::clamp(1.0f - parabola_value(y0, y1, y2, off), 0.0f, 1.0f)};
    };
    out[0] = refine(winner);
    int count = 1;
    if (global != winner && global != first && global != last) {
        const LagCandidate other = refine(global);
        if (!near_multiple(other.lag, out[0].lag)) {
            out[count++] = other;
        }
    }
    for (int div = 2; div <= 6 && count < kMaxLagCandidates; ++div) {
        const float target = out[0].lag / static_cast<float>(div);
        const int lo = std::max(first + 1, static_cast<int>(target * 0.97f) - 1);
        const int hi = std::min(last - 1, static_cast<int>(target * 1.03f) + 1);
        int best = -1;
        for (int t = lo; t <= hi; ++t) {
            if (d[t] < d[t - 1] && d[t] <= d[t + 1] && (best < 0 || d[t] < d[best])) {
                best = t;
            }
        }
        if (best >= 0 && d[best] < 0.5f) {
            out[count++] = refine(best);
        }
    }
    return count;
}

float piano_inharmonicity(int note) {
    struct Anchor {
        int note;
        float b;
    };
    static constexpr std::array<Anchor, 8> kAnchors = {{{21, 3.0e-4f},
                                                        {33, 1.5e-4f},
                                                        {45, 1.0e-4f},
                                                        {57, 1.6e-4f},
                                                        {69, 3.5e-4f},
                                                        {81, 9.0e-4f},
                                                        {93, 3.0e-3f},
                                                        {108, 1.5e-2f}}};
    if (note <= kAnchors.front().note) {
        return kAnchors.front().b;
    }
    for (size_t i = 1; i < kAnchors.size(); ++i) {
        if (note <= kAnchors[i].note) {
            const auto& lo = kAnchors[i - 1];
            const auto& hi = kAnchors[i];
            const float t =
                static_cast<float>(note - lo.note) / static_cast<float>(hi.note - lo.note);
            return std::exp(std::log(lo.b) + t * (std::log(hi.b) - std::log(lo.b)));
        }
    }
    return kAnchors.back().b;
}

bool in_notch(float f, float notch_hz) {
    if (notch_hz <= 0.0f) {
        return false;
    }
    const float k = std::round(f / notch_hz);
    return k >= 1.0f && std::fabs(f - k * notch_hz) <= std::min(0.03f * f, 0.15f * notch_hz);
}

float partial_energy(const FrameAnalyzer& a, float f0, float b, int first, int step, int max_n,
                     float max_hz, float cap_hz, float notch_hz) {
    const float two_bins = 2.0f * a.bin_hz();
    float sum = 0.0f;
    for (int n = first; n <= max_n; n += step) {
        const float f = partial_hz(f0, n, b);
        if (f > max_hz) {
            break;
        }
        if (in_notch(f, notch_hz)) {
            continue;
        }
        const float tol = std::min(std::max(two_bins, 0.015f * f), cap_hz);
        const float m = a.peak_magnitude(f, tol);
        sum += m * m;
    }
    return sum;
}

OctaveEvidence octave_evidence(const FrameAnalyzer& a, float f0, float b_f0, float b_sub,
                               float min_f0_hz, float max_hz, float notch_hz) {
    OctaveEvidence e;
    const float lobe = 2.0f * a.resolution_hz(); // Hann main-lobe half-width
    const float tiny = 1e-14f;

    if (2.0f * f0 <= max_hz) {
        const float odd = partial_energy(a, f0, b_f0, 1, 2, 15, max_hz, 0.3f * f0, notch_hz);
        const float even = partial_energy(a, f0, b_f0, 2, 2, 16, max_hz, 0.3f * f0, notch_hz);
        e.odd_ratio = odd / (even + tiny);
        e.odd_resolvable = lobe <= 0.7f * f0;
        if (3.0f * f0 <= max_hz) {
            float off = 0.0f;
            for (int n = 1; n <= 14; ++n) {
                if (n % 3 != 0) {
                    off += partial_energy(a, f0, b_f0, n, 1, n, max_hz, 0.3f * f0, notch_hz);
                }
            }
            e.triple_ratio =
                off / (partial_energy(a, f0, b_f0, 3, 3, 15, max_hz, 0.3f * f0, notch_hz) + tiny);
        }
    }

    // The lower candidates' partials sit `sub` Hz from f0's. With four periods of f0 in the window
    // the Hann main lobe reaches exactly that far, so its edge (well under -20 dB) is all that
    // leaks into a search kept to 15% of `sub`.
    const float half = 0.5f * f0;
    if (half >= min_f0_hz) {
        const float sub = partial_energy(a, half, b_sub, 1, 2, 15, max_hz, 0.15f * half, notch_hz);
        const float own = partial_energy(a, f0, b_f0, 1, 1, 8, max_hz, 0.15f * half, notch_hz);
        e.half_ratio = sub / (own + tiny);
        e.half_resolvable = lobe <= half;
    }

    const float third = f0 / 3.0f;
    if (third >= min_f0_hz) {
        float sub = 0.0f;
        for (int m = 1; m <= 11; ++m) {
            if (m % 3 != 0) {
                sub += partial_energy(a, third, b_sub, m, 1, m, max_hz, 0.15f * third, notch_hz);
            }
        }
        const float own = partial_energy(a, f0, b_f0, 1, 1, 4, max_hz, 0.15f * third, notch_hz);
        e.third_ratio = sub / (own + tiny);
        e.third_resolvable = lobe <= third;
    }
    return e;
}

float harmonicity(const FrameAnalyzer& a, float f0, float b, float max_hz) {
    const float lobe = 2.0f * a.resolution_hz();
    if (lobe > 0.35f * f0) {
        return 1.0f; // partials are not resolved yet; no evidence either way
    }
    float on = 0.0f;
    float top = f0;
    for (int n = 1; n <= 16; ++n) {
        const float f = partial_hz(f0, n, b);
        if (f > max_hz) {
            break;
        }
        const float w = std::min(std::max(lobe, 0.02f * f), 0.35f * f0);
        on += a.band_energy(f - w, f + w);
        top = f;
    }
    const float total = a.band_energy(0.7f * f0, top + 0.5f * f0);
    return total > 0.0f ? std::min(1.0f, on / total) : 0.0f;
}

bool partials_complete(const FrameAnalyzer& a, float f0, float b, float max_hz, float notch_hz) {
    const float lobe = 2.0f * a.resolution_hz();
    if (lobe > 0.35f * f0) {
        const float low = a.band_energy(0.5f * f0, std::min(max_hz, 16.5f * f0));
        return low >= 0.25f * a.band_energy(0.5f * f0, max_hz);
    }
    std::array<float, 6> mag{};
    int count = 0;
    for (int n = 1; n <= 6; ++n) {
        const float f = partial_hz(f0, n, b);
        if (f > max_hz) {
            break;
        }
        if (in_notch(f, notch_hz)) {
            continue;
        }
        const float tol = std::min(std::max(2.0f * a.bin_hz(), 0.015f * f), 0.3f * f0);
        mag[static_cast<size_t>(count++)] = a.peak_magnitude(f, tol);
    }
    if (count == 0) {
        return notch_hz > 0.0f; // every partial sits in a notch: no evidence either way
    }
    float strongest = 0.0f;
    const int first = std::max(1, static_cast<int>(0.7f * f0 / a.bin_hz()));
    const int last = std::min(a.bins() - 1, static_cast<int>(max_hz / a.bin_hz()));
    for (int k = first; k <= last; ++k) {
        strongest = std::max(strongest, a.magnitude()[k]);
    }
    if (strongest <= 0.0f) {
        return false;
    }
    const float floor = 0.05f * strongest;
    if (count <= 2) {
        return mag[0] >= floor;
    }
    int missing = 0;
    for (int i = 0; i < count; ++i) {
        missing += mag[static_cast<size_t>(i)] < floor ? 1 : 0;
    }
    return missing <= 1;
}

float stretch_bias(const FrameAnalyzer& a, float f0, float b, float max_hz) {
    const float bin = a.bin_hz();
    const int first = std::max(1, static_cast<int>(0.5f * f0 / bin));
    const int last = std::min(a.bins() - 1, static_cast<int>(std::min(max_hz, 16.5f * f0) / bin));
    double m2 = 0.0;
    double m4 = 0.0;
    for (int k = first; k <= last; ++k) {
        const double e = static_cast<double>(a.magnitude()[k]) * a.magnitude()[k];
        const double n2 = std::pow(static_cast<double>(k) * bin / f0, 2.0);
        m2 += e * n2;
        m4 += e * n2 * n2;
    }
    return m2 > 0.0 ? static_cast<float>(0.5 * b * m4 / m2) : 0.0f;
}

float refine_f0(const FrameAnalyzer& a, float f0, float b, float max_hz, float search) {
    std::array<float, 4> estimate{};
    std::array<float, 4> weight{};
    int count = 0;
    float strongest = 0.0f;
    for (int n = 1; n <= 4; ++n) {
        const float f = partial_hz(f0, n, b);
        if (f > max_hz) {
            break;
        }
        const float tol = std::min(std::max(2.0f * a.bin_hz(), search * f), 0.3f * f0);
        float pk = f;
        const float m = a.peak(f, tol, &pk);
        const float nn = static_cast<float>(n);
        estimate[static_cast<size_t>(count)] = pk / (nn * std::sqrt(1.0f + b * nn * nn));
        // Higher partials carry more stretch-model error: weight them down.
        weight[static_cast<size_t>(count)] = m / nn;
        strongest = std::max(strongest, m / nn);
        ++count;
    }
    float sum = 0.0f;
    float wsum = 0.0f;
    for (int i = 0; i < count; ++i) {
        if (weight[static_cast<size_t>(i)] >= 0.2f * strongest && strongest > 0.0f) {
            sum += estimate[static_cast<size_t>(i)] * weight[static_cast<size_t>(i)];
            wsum += weight[static_cast<size_t>(i)];
        }
    }
    return wsum > 0.0f ? sum / wsum : f0;
}

} // namespace toonval::pitch_dsp
