// SPDX-License-Identifier: GPL-3.0-or-later
#include "midi_output.hpp"

#include <libremidi/libremidi.hpp>

#include <SDL3/SDL_log.h>

#include <algorithm>

namespace toonval::app {

namespace {

constexpr std::int64_t kScanIntervalNs = 1'000'000'000;
constexpr std::uint8_t kStatusNoteOff = 0x80;
constexpr std::uint8_t kStatusNoteOn = 0x90;
constexpr std::uint8_t kStatusControl = 0xB0;

std::string label(const libremidi::output_port& port) {
    return port.display_name.empty() ? port.port_name : port.display_name;
}

} // namespace

MidiOutput::MidiOutput() {
    libremidi::observer_configuration conf;
    conf.track_hardware = true;
    conf.track_virtual = true;
    conf.notify_in_constructor = false;
    observer_ = std::make_unique<libremidi::observer>(conf);
}

MidiOutput::~MidiOutput() { all_off(); }

void MidiOutput::update(std::span<const std::string> open_input_names, std::int64_t now_ns,
                        bool force) {
    if (!force && now_ns < next_scan_ns_) {
        return;
    }
    next_scan_ns_ = now_ns + kScanIntervalNs;
    const auto ports = observer_->get_output_ports();

    if (out_ != nullptr) {
        const bool present = std::any_of(ports.begin(), ports.end(), [&](const auto& p) {
            return p.port == port_ && label(p) == name_;
        });
        if (present) {
            return;
        }
        SDL_Log("MIDI output removed: %s", name_.c_str());
        out_.reset(); // nobody to tell about the notes that were sounding
        sounding_ = {};
        pedal_.reset();
        used_.reset();
        name_.clear();
    }
    for (const auto& port : ports) {
        const std::string name = label(port);
        if (!is_piano_output(name, open_input_names) &&
            !is_piano_output(port.port_name, open_input_names)) {
            continue;
        }
        auto out = std::make_unique<libremidi::midi_out>(
            libremidi::output_configuration{}, libremidi::midi_out_configuration_for(*observer_));
        if (const auto err = out->open_port(port, "toonval out"); err != stdx::error{}) {
            SDL_Log("MIDI output failed to open: %s", name.c_str());
            continue;
        }
        out_ = std::move(out);
        name_ = name;
        port_ = port.port;
        SDL_Log("MIDI output opened, virtual presses play on: %s", name.c_str());
        return;
    }
}

void MidiOutput::send(std::uint8_t status, std::uint8_t data1, std::uint8_t data2) {
    if (out_ != nullptr) {
        // A failed send has nobody to report to; the next note will try again.
        (void)out_->send_message(status, data1, data2);
    }
}

void MidiOutput::note_on(int channel, int note, int velocity) {
    if (out_ == nullptr || channel < 0 || channel > 15 || note < 0 || note > 127) {
        return;
    }
    const auto ch = static_cast<std::size_t>(channel);
    sounding_[ch].set(static_cast<std::size_t>(note));
    used_.set(ch);
    send(static_cast<std::uint8_t>(kStatusNoteOn | channel), static_cast<std::uint8_t>(note),
         static_cast<std::uint8_t>(std::clamp(velocity, 1, 127)));
}

void MidiOutput::note_off(int channel, int note) {
    if (out_ == nullptr || channel < 0 || channel > 15 || note < 0 || note > 127) {
        return;
    }
    const auto ch = static_cast<std::size_t>(channel);
    if (!sounding_[ch].test(static_cast<std::size_t>(note))) {
        return; // never started here
    }
    sounding_[ch].reset(static_cast<std::size_t>(note));
    send(static_cast<std::uint8_t>(kStatusNoteOff | channel), static_cast<std::uint8_t>(note), 0);
}

void MidiOutput::sustain(int channel, bool down) {
    if (out_ == nullptr || channel < 0 || channel > 15) {
        return;
    }
    const auto ch = static_cast<std::size_t>(channel);
    pedal_.set(ch, down);
    used_.set(ch);
    send(static_cast<std::uint8_t>(kStatusControl | channel), kCcSustain, down ? 127 : 0);
}

void MidiOutput::all_off() {
    if (out_ == nullptr) {
        return;
    }
    for (std::size_t ch = 0; ch < 16; ++ch) {
        if (!used_.test(ch)) {
            continue;
        }
        for (std::size_t note = 0; note < 128; ++note) {
            if (sounding_[ch].test(note)) {
                send(static_cast<std::uint8_t>(kStatusNoteOff | ch),
                     static_cast<std::uint8_t>(note), 0);
            }
        }
        sounding_[ch].reset();
        if (pedal_.test(ch)) {
            send(static_cast<std::uint8_t>(kStatusControl | ch), kCcSustain, 0);
            pedal_.reset(ch);
        }
        send(static_cast<std::uint8_t>(kStatusControl | ch), kCcAllNotesOff, 0);
    }
    used_.reset();
}

void MidiOutput::play(const PlaybackMessage& m) {
    if (out_ == nullptr || m.channel > 15) {
        return;
    }
    const int ch = m.channel;
    switch (m.kind) {
    case PlaybackMessage::Kind::NoteOn:
        if (m.value == 0) {
            note_off(ch, m.data);
        } else {
            note_on(ch, m.data, m.value);
        }
        break;
    case PlaybackMessage::Kind::NoteOff:
        note_off(ch, m.data);
        break;
    case PlaybackMessage::Kind::ControlChange:
        if (m.data == kCcSustain) {
            sustain(ch, m.value >= 64);
            break;
        }
        used_.set(static_cast<std::size_t>(ch));
        send(static_cast<std::uint8_t>(kStatusControl | ch), static_cast<std::uint8_t>(m.data & 0x7F),
             static_cast<std::uint8_t>(m.value & 0x7F));
        if (m.data == kCcAllNotesOff || m.data == 120) {
            sounding_[static_cast<std::size_t>(ch)].reset();
        }
        break;
    case PlaybackMessage::Kind::ProgramChange:
        used_.set(static_cast<std::size_t>(ch));
        (void)out_->send_message(static_cast<unsigned char>(0xC0 | ch),
                                 static_cast<unsigned char>(m.data & 0x7F));
        break;
    case PlaybackMessage::Kind::PitchBend:
        used_.set(static_cast<std::size_t>(ch));
        send(static_cast<std::uint8_t>(0xE0 | ch), static_cast<std::uint8_t>(m.value & 0x7F),
             static_cast<std::uint8_t>((m.value >> 7) & 0x7F));
        break;
    }
}

int MidiOutput::sounding() const {
    int n = 0;
    for (const auto& ch : sounding_) {
        n += static_cast<int>(ch.count());
    }
    return n;
}

} // namespace toonval::app
