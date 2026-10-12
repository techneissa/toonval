// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "synth_sink.hpp"

#include "toonval/spsc_queue.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

struct tsf;

namespace toonval::app {

// One command from the main thread to the audio thread. Plain data, copied through a queue.
struct SynthCommand {
    enum class Type : std::uint8_t { NoteOn, NoteOff, Sustain, AllNotesOff, Volume };
    Type type = Type::NoteOn;
    std::uint8_t note = 0;
    std::uint8_t velocity = 0; // NoteOn; Sustain uses 1 for down
    float value = 0.0F;        // Volume
    std::int64_t event_ns = 0; // NoteOn: when the press happened (steady clock), 0 = not timed
};

// The built-in piano: TinySoundFont playing the acoustic grand from toonval-piano.sf2, behind the
// SynthSink interface. No SDL in here, so tests render it offline without an audio device.
//
// Threads: load, the SynthSink calls, set_volume and take_latency run on the main thread; render
// runs on the audio thread (or the test thread). They share only the two lock-free queues and a
// few atomics. render never allocates, locks, logs or waits.
//
// Without a loaded SoundFont every call is accepted and render writes silence.
class PianoSynth final : public SynthSink {
  public:
    static constexpr int kMaxVoices = 64;
    // The soft limiter holds peaks at this level (-1 dBFS), so chords never clip.
    static constexpr float kCeiling = 0.891F;

    PianoSynth();
    ~PianoSynth() override;
    PianoSynth(const PianoSynth&) = delete;
    PianoSynth& operator=(const PianoSynth&) = delete;

    // Main thread, before audio starts. Loads a SoundFont and sets the output rate (stereo
    // interleaved float). Returns false, and keeps the silent state, when the data is not a
    // usable SoundFont; error() then says why.
    bool load_file(const std::string& path, int sample_rate);
    bool load_memory(const void* data, std::size_t size, int sample_rate);
    bool loaded() const { return font_ != nullptr; }
    const std::string& error() const { return error_; }

    // SynthSink, main thread. note_on stamps the press with the steady clock.
    void note_on(int note, int velocity) override;
    void note_off(int note) override;
    void sustain(bool down) override;
    void all_notes_off() override;

    // note_on with the time the press happened (steady clock ns, as midi_clock_ns), for latency.
    void note_on_at(int note, int velocity, std::int64_t event_ns);
    // Master volume, 0 to 1 (linear gain, ramped over a few ms).
    void set_volume(float volume);

    // Audio thread. Applies queued commands, then writes `frames` stereo frames (interleaved).
    void render(float* out, int frames);

    // Main thread: one press-to-first-sample latency in ns, measured by render from a note-on's
    // event time to the end of the block that started its sound. False when none are waiting.
    bool take_latency(std::int64_t& ns) { return latencies_.pop(ns); }

    // Commands dropped because the queue was full (main thread pushing faster than audio drains).
    std::uint64_t dropped_commands() const { return commands_.dropped(); }
    // Voices sounding, as of the last render. Any thread.
    int active_voices() const { return active_voices_.load(std::memory_order_relaxed); }
    // Voices cut to make room for new notes, in total. Any thread.
    std::uint64_t stolen_voices() const { return stolen_.load(std::memory_order_relaxed); }
    // Voices sounding for one key. Audio thread (or tests) only.
    int voices_for_key(int key) const;

  private:
    bool finish_load(tsf* font, int sample_rate);
    void push(const SynthCommand& c);
    void apply(const SynthCommand& c);
    void limit(float* out, int frames);

    tsf* font_ = nullptr;
    int preset_index_ = 0;
    float sample_rate_ = 48000.0F;
    std::string error_;

    SpscQueue<SynthCommand, 512> commands_;  // main -> audio
    SpscQueue<std::int64_t, 256> latencies_; // audio -> main
    std::atomic<int> active_voices_{0};
    std::atomic<std::uint64_t> stolen_{0};

    // Audio thread only.
    float volume_ = 1.0F;
    float volume_target_ = 1.0F;
    float volume_step_ = 0.0F; // per-sample ramp coefficient
    float limiter_env_ = 0.0F; // peak follower
    float limiter_release_ = 0.0F;
    std::int64_t timed_[16] = {}; // event times of the note-ons applied in this block
    int timed_count_ = 0;
};

} // namespace toonval::app
