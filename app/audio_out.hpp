// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "piano_synth.hpp"
#include "synth_sink.hpp"

#include "toonval/latency_stats.hpp"
#include "toonval/midi_event.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <string>

namespace toonval::app {

struct AudioOptions {
    std::string sound_font;   // --sound-font PATH; empty searches beside the executable
    bool latency_log = false; // --audio-latency-log
    int sample_frames = 128;  // requested device buffer; falls back to 256 on underruns
};

// The built-in piano on the default audio output: an SDL3 audio stream at 48 kHz, stereo float,
// whose callback renders PianoSynth. Main thread only, apart from the callback.
//
// Without the piano samples (toonval-piano.sf2) or an audio device, start() returns false and
// sink() is a silent sink, so callers never need to check.
class AudioOut {
  public:
    static constexpr int kSampleRate = 48000;
    static constexpr int kFallbackFrames = 256;

    AudioOut() = default;
    ~AudioOut();
    AudioOut(const AudioOut&) = delete;
    AudioOut& operator=(const AudioOut&) = delete;

    // Loads the samples and opens the default output. Prints one line on stdout saying what it
    // got (or on stderr why there is no sound).
    bool start(const AudioOptions& options);
    // Silences, closes the device and quits SDL's audio subsystem. Safe to call twice.
    void stop();
    bool running() const { return stream_ != nullptr; }

    SynthSink& sink();
    // The synth, or null when silent: for timed note-ons and volume.
    PianoSynth* synth() { return running() ? &synth_ : nullptr; }

    // Plays a MIDI note or pedal event, stamped with its arrival time. For --audio-latency-log
    // only: a connected piano makes its own sound.
    void play_midi(const MidiEvent& event);
    // All notes off when the window loses focus, the app goes to the background, or quits.
    void handle_event(const SDL_Event& event);
    // Call once a frame (or every few ms). Reopens at 256 frames after repeated underruns at 128,
    // and with --audio-latency-log prints the press-to-sound summary every 5 s.
    void update(std::int64_t now_ns);
    // Prints the latency summary now (if any notes were timed) and the callback counters.
    void report(std::int64_t now_ns);

    int device_frames() const { return device_frames_; }

  private:
    static void SDLCALL callback(void* userdata, SDL_AudioStream* stream, int additional_amount,
                                 int total_amount);
    bool open_device(int frames);
    void close_device();
    void print_device_line() const;
    // Estimated time from the callback writing a sample to it leaving the speaker: SDL's own
    // queue plus what the OS reports for the device. 0 parts are unknown.
    struct DeviceLatency {
        double sdl_queue_ms = 0.0;
        double os_ms = 0.0; // CoreAudio device + stream latency, safety offset and IO buffer
        double total_ms() const { return sdl_queue_ms + os_ms; }
    };
    DeviceLatency device_latency() const;

    PianoSynth synth_;
    SilentSynthSink silent_;
    AudioOptions options_;
    SDL_AudioStream* stream_ = nullptr;
    bool subsystem_ = false;
    int device_frames_ = 0;
    int device_rate_ = 0;
    bool fell_back_ = false;
    std::int64_t opened_ns_ = 0;
    std::uint64_t late_seen_ = 0;

    // Shared with the callback.
    std::atomic<std::uint64_t> callbacks_{0};
    std::atomic<std::uint64_t> late_callbacks_{0};
    std::atomic<std::int64_t> max_render_ns_{0};
    // Callback only (set before the device starts).
    std::int64_t period_ns_ = 0;
    std::int64_t late_ns_ = 0; // a gap longer than this between callbacks counts as an underrun
    std::int64_t last_callback_ns_ = 0;
    std::array<float, 2 * 1024> buffer_{};

    // --audio-latency-log
    LatencyStats latency_;
    std::int64_t next_report_ns_ = 0;
};

} // namespace toonval::app
