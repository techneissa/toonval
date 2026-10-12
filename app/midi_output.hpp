// SPDX-License-Identifier: GPL-3.0-or-later
// Sends the notes the player plays on the screen or the computer keyboard to the piano over MIDI.
#pragma once

#include "toonval/playback.hpp"
#include "toonval/sound_sink.hpp"

#include <array>
#include <bitset>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace libremidi {
class midi_out;
class observer;
} // namespace libremidi

namespace toonval::app {

// A SoundSink that writes MIDI to the output port of the piano. Which port: the one whose name is
// a known keyboard's or the name of an open MIDI input (is_piano_output). With no such port every
// call does nothing, so it can sit in front of a silent sink. Used from the frame thread only.
//
// Note-offs are never lost: it remembers every note it sent a note-on for. `note_off` ends one;
// `all_off` (lost focus, quit) sends a note-off for each still sounding, then All Notes Off (CC123)
// on every channel it used and the pedal up. A port that disappears or is replaced forgets them,
// since there is nobody to tell.
class MidiOutput final : public SoundSink {
  public:
    MidiOutput();
    ~MidiOutput() override;
    MidiOutput(const MidiOutput&) = delete;
    MidiOutput& operator=(const MidiOutput&) = delete;

    // Opens the piano's output port when there is one and closes it when it is gone. Lists the
    // ports at most once a second; `force` lists them now (the inputs changed).
    void update(std::span<const std::string> open_input_names, std::int64_t now_ns, bool force);

    bool connected() const { return out_ != nullptr; }
    const std::string& port_name() const { return name_; }
    // Notes it has sent a note-on for and not yet a note-off.
    int sounding() const;

    void note_on(int channel, int note, int velocity) override;
    void note_off(int channel, int note) override;
    void sustain(int channel, bool down) override;
    void all_off() override;

    // One message of the song (SongController). Its notes and pedal are remembered like the
    // player's, so all_off() stops them too; All Notes Off and All Sound Off forget a channel's.
    void play(const PlaybackMessage& message);

  private:
    void send(std::uint8_t status, std::uint8_t data1, std::uint8_t data2);

    std::unique_ptr<libremidi::observer> observer_;
    std::unique_ptr<libremidi::midi_out> out_;
    std::string name_;
    std::uint64_t port_ = 0;
    std::int64_t next_scan_ns_ = 0;
    std::array<std::bitset<128>, 16> sounding_;
    std::bitset<16> pedal_;
    std::bitset<16> used_; // channels written to since the last all_off
};

} // namespace toonval::app
