// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "synth_sink.hpp"
#include "toonval/playback.hpp"
#include "toonval/sound_sink.hpp"

#include <bitset>
#include <cstddef>

namespace toonval::app {

// The built-in piano as a core SoundSink, so taps and typing sound when no piano is connected.
// The built-in piano has one channel, so the channel is ignored.
class BuiltinPianoSink final : public toonval::SoundSink {
  public:
    explicit BuiltinPianoSink(app::SynthSink& synth) : synth_(synth) {}

    void note_on(int, int note, int velocity) override { synth_.note_on(note, velocity); }
    void note_off(int, int note) override { synth_.note_off(note); }
    void sustain(int, bool down) override { synth_.sustain(down); }
    void all_off() override { synth_.all_notes_off(); }

  private:
    app::SynthSink& synth_;
};

// The song's MIDI (SongController) on the built-in piano, when no piano is connected. One channel:
// the channel is ignored, program changes and bends too. It remembers the song notes it started,
// so All Notes Off (CC123, CC120) and all_off() stop exactly those, not the player's own.
class BuiltinSongSink {
  public:
    explicit BuiltinSongSink(app::SynthSink& synth) : synth_(synth) {}

    void play(const PlaybackMessage& m) {
        switch (m.kind) {
        case PlaybackMessage::Kind::NoteOn:
            if (m.value > 0) {
                synth_.note_on(m.data & 127, m.value & 127);
                sounding_.set(m.data & 127u);
                break;
            }
            [[fallthrough]];
        case PlaybackMessage::Kind::NoteOff:
            if (sounding_.test(m.data & 127u)) {
                synth_.note_off(m.data & 127);
                sounding_.reset(m.data & 127u);
            }
            break;
        case PlaybackMessage::Kind::ControlChange:
            if (m.data == 64) {
                pedal_ = m.value >= 64;
                synth_.sustain(pedal_);
            } else if (m.data == 123 || m.data == 120) {
                stop_notes();
            }
            break;
        case PlaybackMessage::Kind::ProgramChange:
        case PlaybackMessage::Kind::PitchBend:
            break;
        }
    }
    // Every song note off and the pedal up.
    void all_off() {
        stop_notes();
        if (pedal_) {
            synth_.sustain(false);
            pedal_ = false;
        }
    }
    std::size_t sounding() const { return sounding_.count(); }

  private:
    void stop_notes() {
        for (int n = 0; n < 128; ++n) {
            if (sounding_.test(static_cast<std::size_t>(n))) {
                synth_.note_off(n);
            }
        }
        sounding_.reset();
    }

    app::SynthSink& synth_;
    std::bitset<128> sounding_;
    bool pedal_ = false;
};

} // namespace toonval::app
