// SPDX-License-Identifier: GPL-3.0-or-later
#include "piano_synth.hpp"

#include "tsf_glue.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#endif

namespace toonval::app {
namespace {

// TinySoundFont's output gain. With it a single note of the grand peaks at about -7 dBFS at
// velocity 127 and -9 dBFS at 100, and a six-note chord at 100 near -4 dBFS, so the limiter
// only acts on big two-handed chords.
constexpr float kOutputGainDb = -6.0F;
constexpr float kVolumeRampSeconds = 0.005F;
constexpr float kLimiterReleaseSeconds = 0.15F;

std::int64_t steady_ns() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

// Decaying piano releases produce denormal floats, which are very slow on some CPUs. Flush them to
// zero on this thread. Reads the control register first and writes only when the bit is missing.
void enable_flush_to_zero() {
#if defined(__aarch64__)
    std::uint64_t fpcr = 0;
    asm volatile("mrs %0, fpcr" : "=r"(fpcr));
    constexpr std::uint64_t kFz = std::uint64_t{1} << 24;
    if ((fpcr & kFz) == 0) {
        asm volatile("msr fpcr, %0" : : "r"(fpcr | kFz));
    }
#elif defined(__x86_64__) || defined(__i386__)
    constexpr unsigned kFtzDaz = 0x8040; // flush-to-zero and denormals-are-zero
    if ((_mm_getcsr() & kFtzDaz) != kFtzDaz) {
        _mm_setcsr(_mm_getcsr() | kFtzDaz);
    }
#endif
}

std::uint8_t clamp_note(int note) { return static_cast<std::uint8_t>(std::clamp(note, 0, 127)); }

} // namespace

PianoSynth::PianoSynth() = default;

PianoSynth::~PianoSynth() {
    if (font_ != nullptr) {
        tsf_close(font_);
    }
}

bool PianoSynth::load_file(const std::string& path, int sample_rate) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error_ = "cannot open " + path;
        return false;
    }
    const std::vector<char> data((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
    if (!load_memory(data.data(), data.size(), sample_rate)) {
        error_ = path + ": " + error_;
        return false;
    }
    return true;
}

bool PianoSynth::load_memory(const void* data, std::size_t size, int sample_rate) {
    if (size == 0 || size > 0x7FFFFFFF) {
        error_ = "empty or oversized SoundFont";
        return false;
    }
    tsf* font = tsf_load_memory(data, static_cast<int>(size));
    if (font == nullptr) {
        error_ = "not a SoundFont 2 file";
        return false;
    }
    return finish_load(font, sample_rate);
}

bool PianoSynth::finish_load(tsf* font, int sample_rate) {
    // Bank 0 program 0, the acoustic grand; otherwise the first preset in the file.
    int preset = tsf_get_presetindex(font, 0, 0);
    if (preset < 0) {
        preset = 0;
    }
    // Everything that allocates happens here, before the audio thread starts: the voice pool and
    // channel 0, so note_on and render never allocate.
    if (tsf_get_presetcount(font) == 0 || tsf_set_max_voices(font, kMaxVoices) == 0 ||
        tsf_channel_set_presetindex(font, 0, preset) == 0) {
        tsf_close(font);
        error_ = "SoundFont has no presets, or out of memory";
        return false;
    }
    tsf_set_output(font, TSF_STEREO_INTERLEAVED, sample_rate, kOutputGainDb);
    tsf_channel_set_sustain(font, 0, 0);
    if (font_ != nullptr) {
        tsf_close(font_);
    }
    font_ = font;
    preset_index_ = preset;
    sample_rate_ = static_cast<float>(sample_rate);
    volume_step_ = 1.0F - std::exp(-1.0F / (kVolumeRampSeconds * sample_rate_));
    limiter_release_ = std::exp(-1.0F / (kLimiterReleaseSeconds * sample_rate_));
    error_.clear();
    return true;
}

void PianoSynth::push(const SynthCommand& c) { commands_.push(c); }

void PianoSynth::note_on(int note, int velocity) { note_on_at(note, velocity, steady_ns()); }

void PianoSynth::note_on_at(int note, int velocity, std::int64_t event_ns) {
    if (velocity <= 0) {
        note_off(note);
        return;
    }
    SynthCommand c;
    c.type = SynthCommand::Type::NoteOn;
    c.note = clamp_note(note);
    c.velocity = static_cast<std::uint8_t>(std::min(velocity, 127));
    c.event_ns = event_ns;
    push(c);
}

void PianoSynth::note_off(int note) {
    SynthCommand c;
    c.type = SynthCommand::Type::NoteOff;
    c.note = clamp_note(note);
    push(c);
}

void PianoSynth::sustain(bool down) {
    SynthCommand c;
    c.type = SynthCommand::Type::Sustain;
    c.velocity = down ? 1 : 0;
    push(c);
}

void PianoSynth::all_notes_off() {
    SynthCommand c;
    c.type = SynthCommand::Type::AllNotesOff;
    push(c);
}

void PianoSynth::set_volume(float volume) {
    SynthCommand c;
    c.type = SynthCommand::Type::Volume;
    c.value = std::clamp(volume, 0.0F, 1.0F);
    push(c);
}

void PianoSynth::apply(const SynthCommand& c) {
    switch (c.type) {
    case SynthCommand::Type::NoteOn: {
        const float velocity = static_cast<float>(c.velocity) / 127.0F;
        const int stolen = tsf_glue_make_room(font_, preset_index_, c.note, velocity);
        if (stolen > 0) {
            stolen_.fetch_add(static_cast<std::uint64_t>(stolen), std::memory_order_relaxed);
        }
        tsf_channel_note_on(font_, 0, c.note, velocity);
        if (c.event_ns != 0 && timed_count_ < static_cast<int>(std::size(timed_))) {
            timed_[timed_count_++] = c.event_ns;
        }
        break;
    }
    case SynthCommand::Type::NoteOff:
        tsf_channel_note_off(font_, 0, c.note);
        break;
    case SynthCommand::Type::Sustain:
        tsf_channel_set_sustain(font_, 0, c.velocity != 0 ? 1 : 0);
        break;
    case SynthCommand::Type::AllNotesOff:
        // Lift the pedal first, or the quick release would leave pedal-held voices behind.
        tsf_channel_set_sustain(font_, 0, 0);
        tsf_channel_sounds_off_all(font_, 0);
        break;
    case SynthCommand::Type::Volume:
        volume_target_ = c.value;
        break;
    }
}

void PianoSynth::render(float* out, int frames) {
    enable_flush_to_zero();
    const std::size_t samples = static_cast<std::size_t>(frames) * 2;
    if (font_ == nullptr) {
        SynthCommand c;
        while (commands_.pop(c)) {
        }
        std::memset(out, 0, samples * sizeof(float));
        return;
    }

    SynthCommand c;
    while (commands_.pop(c)) {
        apply(c);
    }
    tsf_render_float(font_, out, frames, 0);
    limit(out, frames);
    active_voices_.store(tsf_active_voice_count(font_), std::memory_order_relaxed);

    // A note started in this block sounds from its first sample (a piano attack is a few ms), so
    // the block's end is when its first sample was handed to the device.
    if (timed_count_ > 0) {
        const bool sounding = std::any_of(out, out + samples, [](float s) { return s != 0.0F; });
        if (sounding) {
            const std::int64_t now = steady_ns();
            for (int i = 0; i < timed_count_; ++i) {
                latencies_.push(now - timed_[i]);
            }
            timed_count_ = 0;
        }
    }
}

int PianoSynth::voices_for_key(int key) const {
    return font_ != nullptr ? tsf_glue_voices_for_key(font_, key) : 0;
}

// Master volume, then a peak limiter with instant attack and a 150 ms release: the follower never
// sits below the current peak, so the output never exceeds kCeiling.
void PianoSynth::limit(float* out, int frames) {
    for (int i = 0; i < frames; ++i) {
        volume_ += (volume_target_ - volume_) * volume_step_;
        float left = out[2 * i] * volume_;
        float right = out[2 * i + 1] * volume_;
        if (!std::isfinite(left) || !std::isfinite(right)) {
            left = 0.0F;
            right = 0.0F;
        }
        const float peak = std::max(std::fabs(left), std::fabs(right));
        limiter_env_ = std::max(peak, limiter_env_ * limiter_release_);
        const float gain = limiter_env_ > kCeiling ? kCeiling / limiter_env_ : 1.0F;
        // The clamp only catches float rounding in the division.
        out[2 * i] = std::clamp(left * gain, -kCeiling, kCeiling);
        out[2 * i + 1] = std::clamp(right * gain, -kCeiling, kCeiling);
    }
}

} // namespace toonval::app
