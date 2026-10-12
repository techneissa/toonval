// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace toonval::app {

// Where notes played without a MIDI keyboard (taps, the computer keyboard) go to make a sound.
// Called from the main thread only. Every call returns at once: it queues a small command for the
// audio thread and never waits for it.
class SynthSink {
  public:
    virtual ~SynthSink() = default;

    // note: MIDI note number 0 to 127. velocity: 1 to 127 (0 means note_off).
    virtual void note_on(int note, int velocity) = 0;
    virtual void note_off(int note) = 0;
    // The sustain pedal: while down, released notes keep sounding until it comes up.
    virtual void sustain(bool down) = 0;
    // Silences everything within a few milliseconds and lifts the pedal (focus loss, quit).
    virtual void all_notes_off() = 0;
};

// A sink that makes no sound, for when no piano samples are installed.
class SilentSynthSink final : public SynthSink {
  public:
    void note_on(int, int) override {}
    void note_off(int) override {}
    void sustain(bool) override {}
    void all_notes_off() override {}
};

} // namespace toonval::app
