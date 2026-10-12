// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/sound_sink.hpp"

#include <string>
#include <vector>

#include <doctest/doctest.h>

using namespace toonval;

namespace {

struct Recorder final : SoundSink {
    std::vector<int> log; // 1 on, 2 off, 3 pedal down, 4 pedal up, 5 all off; then the note
    void note_on(int ch, int note, int vel) override { log.insert(log.end(), {1, ch, note, vel}); }
    void note_off(int ch, int note) override { log.insert(log.end(), {2, ch, note}); }
    void sustain(int ch, bool down) override { log.insert(log.end(), {down ? 3 : 4, ch}); }
    void all_off() override { log.push_back(5); }
};

MidiEvent ev(MidiEventType type, int d1, int d2, InputSource source) {
    MidiEvent e;
    e.type = type;
    e.data1 = static_cast<std::uint8_t>(d1);
    e.data2 = static_cast<std::uint8_t>(d2);
    e.source = source;
    return e;
}

} // namespace

TEST_CASE("events default to the MIDI source and are not virtual") {
    const MidiEvent e;
    CHECK(e.source == InputSource::Midi);
    CHECK_FALSE(e.is_virtual());
    CHECK(MidiEvent{0, MidiEventType::NoteOn, 0, 60, 90, false, InputSource::Pointer}.is_virtual());
}

TEST_CASE("virtual note and pedal events go to the sink") {
    Recorder r;
    CHECK(forward_to_sink(r, ev(MidiEventType::NoteOn, 60, 90, InputSource::ComputerKeyboard)));
    CHECK(forward_to_sink(r, ev(MidiEventType::NoteOff, 60, 0, InputSource::Pointer)));
    CHECK(forward_to_sink(
        r, ev(MidiEventType::ControlChange, 64, 127, InputSource::ComputerKeyboard)));
    CHECK(
        forward_to_sink(r, ev(MidiEventType::ControlChange, 64, 0, InputSource::ComputerKeyboard)));
    const std::vector<int> expected{1, 0, 60, 90, 2, 0, 60, 3, 0, 4, 0};
    CHECK(r.log == expected);
}

TEST_CASE("a MIDI keyboard's own notes never go to the sink") {
    Recorder r;
    CHECK_FALSE(forward_to_sink(r, ev(MidiEventType::NoteOn, 60, 90, InputSource::Midi)));
    CHECK_FALSE(forward_to_sink(r, ev(MidiEventType::NoteOff, 60, 0, InputSource::Midi)));
    CHECK_FALSE(forward_to_sink(r, ev(MidiEventType::ControlChange, 64, 127, InputSource::Midi)));
    CHECK(r.log.empty());
}

TEST_CASE("other controllers, other events and synthetic events are not forwarded") {
    Recorder r;
    CHECK_FALSE(forward_to_sink(r, ev(MidiEventType::ControlChange, 1, 50, InputSource::Pointer)));
    CHECK_FALSE(forward_to_sink(r, ev(MidiEventType::Other, 0, 0, InputSource::Pointer)));
    MidiEvent synth = ev(MidiEventType::ControlChange, 123, 0, InputSource::Pointer);
    synth.synthetic = true;
    CHECK_FALSE(forward_to_sink(r, synth));
    CHECK(r.log.empty());
}

TEST_CASE("the silent sink accepts everything and does nothing") {
    SilentSoundSink s;
    s.note_on(0, 60, 90);
    s.note_off(0, 60);
    s.sustain(0, true);
    s.all_off();
    CHECK(forward_to_sink(s, ev(MidiEventType::NoteOn, 60, 90, InputSource::Pointer)));
}

TEST_CASE("an output named like a known keyboard is the piano") {
    const std::vector<std::string> none;
    CHECK(is_piano_output("PSR-E383", none));
    CHECK(is_piano_output("Yamaha PSR-E383 MIDI 1", none));
    CHECK_FALSE(is_piano_output("PSR-E38", none)); // not a word match
    CHECK_FALSE(is_piano_output("Digital Piano", none));
}

TEST_CASE("an output with the name of an open input is the same device") {
    const std::vector<std::string> inputs{"Some Stage Piano", "Other Keys"};
    CHECK(is_piano_output("Some Stage Piano", inputs));
    CHECK(is_piano_output("some stage piano", inputs));
    CHECK(is_piano_output("Other Keys", inputs));
    CHECK_FALSE(is_piano_output("Some Stage", inputs));
    CHECK_FALSE(is_piano_output("Speakers", inputs));
}

TEST_CASE("software and nameless outputs are never the piano") {
    const std::vector<std::string> inputs{"toonval virtual", "Midi Through Port-0", ""};
    CHECK_FALSE(is_piano_output("toonval virtual", inputs));
    CHECK_FALSE(is_piano_output("Midi Through Port-0", inputs));
    CHECK_FALSE(is_piano_output("", inputs));
    CHECK_FALSE(is_piano_output("Midi Through:Midi Through Port-0 14:0", {}));
}
