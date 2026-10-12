// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio_out.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#if defined(__APPLE__)
#include <CoreAudio/CoreAudio.h>
#endif

namespace toonval::app {
namespace {

constexpr std::int64_t kReportNs = 5'000'000'000;
// Underrun detection: SDL reports none, so the callback watches its own pace. A gap longer than
// SDL's whole queue (and at least this many periods) means the device ran dry. CoreAudio pulls in
// bursts (Bluetooth especially), so a gap of a few periods alone is normal there. Counted after a
// warm-up, since the first callbacks fill the queue in a burst.
constexpr std::int64_t kLatePeriods = 3;
constexpr std::int64_t kWarmupNs = 2'000'000'000;
constexpr std::uint64_t kLateLimit = 3;
constexpr const char* kSoundFontName = "toonval-piano.sf2";

std::int64_t steady_ns() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

double ms(std::int64_t ns) { return static_cast<double>(ns) / 1e6; }

// Where to look for the piano samples: --sound-font, then beside the executable (the build copies
// it there), then the install layout's share folder.
std::vector<std::string> sound_font_candidates(const std::string& option) {
    if (!option.empty()) {
        return {option};
    }
    std::vector<std::string> out;
    if (const char* base = SDL_GetBasePath(); base != nullptr) {
        const std::filesystem::path dir(base);
        out.push_back((dir / kSoundFontName).string());
        out.push_back((dir / ".." / "share" / "toonval" / kSoundFontName).string());
    }
    return out;
}

#if defined(__APPLE__)
// Output latency CoreAudio reports for the default output device: device and stream latency, the
// safety offset and one IO buffer, in frames at the device's nominal rate.
double coreaudio_output_ms() {
    AudioObjectPropertyAddress address{kAudioHardwarePropertyDefaultOutputDevice,
                                       kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain};
    AudioDeviceID device = kAudioObjectUnknown;
    UInt32 size = sizeof device;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size,
                                   &device) != noErr ||
        device == kAudioObjectUnknown) {
        return 0.0;
    }
    const auto get_u32 = [&](AudioObjectPropertySelector selector) -> UInt32 {
        AudioObjectPropertyAddress a{selector, kAudioObjectPropertyScopeOutput,
                                     kAudioObjectPropertyElementMain};
        UInt32 value = 0;
        UInt32 value_size = sizeof value;
        return AudioObjectGetPropertyData(device, &a, 0, nullptr, &value_size, &value) == noErr
                   ? value
                   : 0;
    };
    Float64 rate = 0.0;
    {
        AudioObjectPropertyAddress a{kAudioDevicePropertyNominalSampleRate,
                                     kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain};
        UInt32 rate_size = sizeof rate;
        if (AudioObjectGetPropertyData(device, &a, 0, nullptr, &rate_size, &rate) != noErr ||
            rate <= 0.0) {
            return 0.0;
        }
    }
    UInt32 frames = get_u32(kAudioDevicePropertyLatency) +
                    get_u32(kAudioDevicePropertySafetyOffset) +
                    get_u32(kAudioDevicePropertyBufferFrameSize);
    // The first output stream's own latency.
    AudioObjectPropertyAddress streams{kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput,
                                       kAudioObjectPropertyElementMain};
    UInt32 streams_size = 0;
    if (AudioObjectGetPropertyDataSize(device, &streams, 0, nullptr, &streams_size) == noErr &&
        streams_size >= sizeof(AudioStreamID)) {
        std::vector<AudioStreamID> ids(streams_size / sizeof(AudioStreamID));
        if (AudioObjectGetPropertyData(device, &streams, 0, nullptr, &streams_size, ids.data()) ==
            noErr) {
            AudioObjectPropertyAddress a{kAudioStreamPropertyLatency,
                                         kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain};
            UInt32 value = 0;
            UInt32 value_size = sizeof value;
            if (AudioObjectGetPropertyData(ids[0], &a, 0, nullptr, &value_size, &value) == noErr) {
                frames += value;
            }
        }
    }
    return 1000.0 * frames / rate;
}
#endif

} // namespace

AudioOut::~AudioOut() { stop(); }

bool AudioOut::start(const AudioOptions& options) {
    options_ = options;
    bool loaded = false;
    std::string why = "no toonval-piano.sf2 found (build with TOONVAL_PIANO_SF2_FILE or "
                      "TOONVAL_PIANO_SF2_URL, or pass --sound-font PATH)";
    for (const std::string& path : sound_font_candidates(options.sound_font)) {
        if (!options.sound_font.empty() || std::filesystem::exists(path)) {
            loaded = synth_.load_file(path, kSampleRate);
            if (!loaded) {
                why = synth_.error();
            }
            break;
        }
    }
    if (!loaded) {
        std::fprintf(stderr, "built-in piano off: %s\n", why.c_str());
        return false;
    }
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "built-in piano off: SDL audio failed: %s\n", SDL_GetError());
        return false;
    }
    subsystem_ = true;
    if (!open_device(std::max(options.sample_frames, 16))) {
        stop();
        return false;
    }
    print_device_line();
    return true;
}

bool AudioOut::open_device(int frames) {
    // A request, set before the device opens; the backend may give something else.
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, std::to_string(frames).c_str());
    const SDL_AudioSpec spec{SDL_AUDIO_F32, 2, kSampleRate};
    // The device can only be asked after it is open, so period_ns_ starts from the request and is
    // corrected below, before the stream is resumed and the callback first runs.
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &callback, this);
    if (stream_ == nullptr) {
        std::fprintf(stderr, "built-in piano off: no audio output: %s\n", SDL_GetError());
        return false;
    }
    SDL_AudioSpec device_spec{};
    int device_frames = 0;
    if (SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(stream_), &device_spec, &device_frames)) {
        device_frames_ = device_frames;
        device_rate_ = device_spec.freq;
    } else {
        device_frames_ = frames;
        device_rate_ = kSampleRate;
    }
    period_ns_ = device_rate_ > 0 ? std::int64_t{1'000'000'000} * device_frames_ / device_rate_ : 0;
    late_ns_ = std::max(kLatePeriods * period_ns_,
                        static_cast<std::int64_t>(device_latency().sdl_queue_ms * 1e6));
    last_callback_ns_ = 0;
    opened_ns_ = steady_ns();
    late_seen_ = late_callbacks_.load(std::memory_order_relaxed);
    if (!SDL_ResumeAudioStreamDevice(stream_)) {
        std::fprintf(stderr, "built-in piano off: cannot start audio: %s\n", SDL_GetError());
        close_device();
        return false;
    }
    return true;
}

void AudioOut::close_device() {
    if (stream_ != nullptr) {
        // Waits for a running callback to finish.
        SDL_DestroyAudioStream(stream_);
        stream_ = nullptr;
    }
}

void AudioOut::stop() {
    if (stream_ != nullptr) {
        // Let the quick release play out before the device goes, so quitting does not click.
        synth_.all_notes_off();
        SDL_Delay(20);
    }
    close_device();
    if (subsystem_) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        subsystem_ = false;
    }
}

SynthSink& AudioOut::sink() {
    if (running()) {
        return synth_;
    }
    return silent_;
}

void AudioOut::handle_event(const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_WINDOW_FOCUS_LOST:
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
    case SDL_EVENT_QUIT:
    case SDL_EVENT_TERMINATING:
        sink().all_notes_off();
        break;
    default:
        break;
    }
}

void AudioOut::play_midi(const MidiEvent& event) {
    if (!running()) {
        return;
    }
    switch (event.type) {
    case MidiEventType::NoteOn:
        synth_.note_on_at(event.data1, event.data2, event.time_ns);
        break;
    case MidiEventType::NoteOff:
        synth_.note_off(event.data1);
        break;
    case MidiEventType::ControlChange:
        if (event.data1 == kCcSustain) {
            synth_.sustain(event.data2 >= 64);
        } else if (event.data1 == kCcAllSoundOff || event.data1 == kCcAllNotesOff) {
            synth_.all_notes_off();
        }
        break;
    case MidiEventType::Other:
        break;
    }
}

void AudioOut::callback(void* userdata, SDL_AudioStream* stream, int additional_amount, int) {
    auto* self = static_cast<AudioOut*>(userdata);
    const std::int64_t start = steady_ns();
    if (self->last_callback_ns_ != 0 && self->late_ns_ > 0 &&
        start - self->last_callback_ns_ > self->late_ns_) {
        self->late_callbacks_.fetch_add(1, std::memory_order_relaxed);
    }
    self->last_callback_ns_ = start;

    constexpr int kFrameBytes = 2 * static_cast<int>(sizeof(float));
    const int chunk_frames = static_cast<int>(self->buffer_.size() / 2);
    int frames = additional_amount / kFrameBytes;
    while (frames > 0) {
        const int n = std::min(frames, chunk_frames);
        self->synth_.render(self->buffer_.data(), n);
        SDL_PutAudioStreamData(stream, self->buffer_.data(), n * kFrameBytes);
        frames -= n;
    }

    const std::int64_t took = steady_ns() - start;
    if (took > self->max_render_ns_.load(std::memory_order_relaxed)) {
        self->max_render_ns_.store(took, std::memory_order_relaxed);
    }
    self->callbacks_.fetch_add(1, std::memory_order_relaxed);
}

void AudioOut::update(std::int64_t now_ns) {
    if (!running()) {
        return;
    }
    // Underruns at the small buffer: reopen once at the fallback size.
    const std::uint64_t late = late_callbacks_.load(std::memory_order_relaxed);
    if (now_ns - opened_ns_ < kWarmupNs) {
        late_seen_ = late;
    } else if (!fell_back_ && device_frames_ < kFallbackFrames && late - late_seen_ >= kLateLimit) {
        std::printf("audio: %llu late callbacks at %d frames, reopening at %d\n",
                    static_cast<unsigned long long>(late - late_seen_), device_frames_,
                    kFallbackFrames);
        fell_back_ = true;
        close_device();
        if (!open_device(kFallbackFrames)) {
            return;
        }
        print_device_line();
    }

    std::int64_t ns = 0;
    while (synth_.take_latency(ns)) {
        latency_.add(ns);
    }
    if (!options_.latency_log) {
        return;
    }
    if (next_report_ns_ == 0) {
        next_report_ns_ = now_ns + kReportNs;
    }
    if (now_ns >= next_report_ns_) {
        next_report_ns_ = now_ns + kReportNs;
        report(now_ns);
    }
}

void AudioOut::report(std::int64_t now_ns) {
    if (!running()) {
        return;
    }
    std::int64_t ns = 0;
    while (synth_.take_latency(ns)) {
        latency_.add(ns);
    }
    const LatencySummary s = latency_.take_summary();
    const DeviceLatency device = device_latency();
    if (s.count > 0) {
        std::printf("audio latency press to first sample written: %zu notes, min %.2f ms, median "
                    "%.2f ms, max %.2f ms; + device %.1f ms (SDL queue %.1f, OS %.1f) = about "
                    "%.1f ms to the speaker (median)\n",
                    s.count, ms(s.min_ns), ms(s.median_ns), ms(s.max_ns), device.total_ms(),
                    device.sdl_queue_ms, device.os_ms, ms(s.median_ns) + device.total_ms());
    }
    std::printf("audio: %llu callbacks, %llu late, slowest render %.3f ms of a %.3f ms period, "
                "%llu commands dropped, %llu voices stolen\n",
                static_cast<unsigned long long>(callbacks_.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(late_callbacks_.load(std::memory_order_relaxed)),
                ms(max_render_ns_.load(std::memory_order_relaxed)), ms(period_ns_),
                static_cast<unsigned long long>(synth_.dropped_commands()),
                static_cast<unsigned long long>(synth_.stolen_voices()));
    (void)now_ns;
}

AudioOut::DeviceLatency AudioOut::device_latency() const {
    DeviceLatency out;
    if (device_rate_ <= 0) {
        return out;
    }
    const double period_ms = 1000.0 * device_frames_ / device_rate_;
#if defined(__APPLE__)
    // SDL 3.4's CoreAudio backend (SDL_coreaudio.m) queues 3 AudioQueue buffers, or
    // 2 * ceil(15 ms / period) when a period is under 15 ms, and keeps them all filled.
    const int buffers = period_ms < 15.0 ? 2 * static_cast<int>(std::ceil(15.0 / period_ms)) : 3;
    out.sdl_queue_ms = buffers * period_ms;
    out.os_ms = coreaudio_output_ms();
#else
    // SDL's ALSA backend asks for two periods of this size.
    out.sdl_queue_ms = 2 * period_ms;
#endif
    return out;
}

void AudioOut::print_device_line() const {
    const char* driver = SDL_GetCurrentAudioDriver();
    const char* name = SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(stream_));
    const DeviceLatency device = device_latency();
    std::printf("audio: %s on %s, device %d Hz, %d frames per buffer (asked %d, %.2f ms); "
                "estimated output latency %.1f ms (SDL queue %.1f, OS %.1f)\n",
                name != nullptr ? name : "default output", driver != nullptr ? driver : "?",
                device_rate_, device_frames_, fell_back_ ? kFallbackFrames : options_.sample_frames,
                device_rate_ > 0 ? 1000.0 * device_frames_ / device_rate_ : 0.0, device.total_ms(),
                device.sdl_queue_ms, device.os_ms);
}

} // namespace toonval::app
