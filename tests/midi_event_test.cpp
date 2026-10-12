// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/midi_event.hpp"

#include <array>

#include <doctest/doctest.h>

using namespace toonval;

namespace {
MidiEvent decode(std::initializer_list<std::uint8_t> bytes, std::int64_t t = 0) {
    return decode_midi(bytes.begin(), bytes.size(), t);
}
} // namespace

TEST_CASE("note-on decodes note, velocity, channel and the caller's timestamp") {
    MidiEvent ev = decode({0x93, 60, 100}, 123456789);
    CHECK(ev.type == MidiEventType::NoteOn);
    CHECK(ev.channel == 3);
    CHECK(ev.data1 == 60);
    CHECK(ev.data2 == 100);
    CHECK(ev.time_ns == 123456789);
}

TEST_CASE("note-off decodes note, release velocity and channel") {
    MidiEvent ev = decode({0x85, 72, 40});
    CHECK(ev.type == MidiEventType::NoteOff);
    CHECK(ev.channel == 5);
    CHECK(ev.data1 == 72);
    CHECK(ev.data2 == 40);
}

TEST_CASE("note-on with velocity 0 decodes as note-off") {
    MidiEvent ev = decode({0x90, 60, 0}, 42);
    CHECK(ev.type == MidiEventType::NoteOff);
    CHECK(ev.data1 == 60);
    CHECK(ev.data2 == 0);
    CHECK(ev.time_ns == 42);
}

TEST_CASE("control change decodes controller and value") {
    MidiEvent ev = decode({0xB2, 64, 127});
    CHECK(ev.type == MidiEventType::ControlChange);
    CHECK(ev.channel == 2);
    CHECK(ev.data1 == 64);
    CHECK(ev.data2 == 127);
}

TEST_CASE("empty input decodes as Other") {
    MidiEvent ev = decode_midi(nullptr, 0, 7);
    CHECK(ev.type == MidiEventType::Other);
    CHECK(ev.time_ns == 7);
}

TEST_CASE("truncated messages decode as Other, never as notes") {
    CHECK(decode({0x90}).type == MidiEventType::Other);
    CHECK(decode({0x90, 60}).type == MidiEventType::Other);
    CHECK(decode({0x80, 60}).type == MidiEventType::Other);
    CHECK(decode({0xB0, 64}).type == MidiEventType::Other);
}

TEST_CASE("input starting with a data byte decodes as Other") {
    CHECK(decode({60, 100}).type == MidiEventType::Other);
    CHECK(decode({0x3C, 0x64, 0x00}).type == MidiEventType::Other);
}

TEST_CASE("data bytes with the high bit set make the message Other") {
    CHECK(decode({0x90, 0x80 | 60, 100}).type == MidiEventType::Other);
    CHECK(decode({0x90, 60, 0x90}).type == MidiEventType::Other);
    CHECK(decode({0xB0, 64, 0xFF}).type == MidiEventType::Other);
}

TEST_CASE("over-long channel messages decode as Other") {
    CHECK(decode({0x90, 60, 100, 62, 100}).type == MidiEventType::Other);
}

TEST_CASE("SysEx decodes as Other even when its bytes look like a note") {
    CHECK(decode({0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7}).type == MidiEventType::Other);
    CHECK(decode({0xF0, 60, 100}).type == MidiEventType::Other);
}

TEST_CASE("system realtime and common messages decode as Other") {
    CHECK(decode({0xF8}).type == MidiEventType::Other);
    CHECK(decode({0xFE}).type == MidiEventType::Other);
    CHECK(decode({0xFA}).type == MidiEventType::Other);
    CHECK(decode({0xFF}).type == MidiEventType::Other);
    CHECK(decode({0xF2, 60, 100}).type == MidiEventType::Other);
    CHECK(decode({0xF8, 60, 100}).type == MidiEventType::Other);
}

TEST_CASE("channel messages the trainer ignores decode as Other") {
    CHECK(decode({0xE0, 0, 64}).type == MidiEventType::Other);  // pitch bend
    CHECK(decode({0xC0, 5}).type == MidiEventType::Other);      // program change
    CHECK(decode({0xA0, 60, 50}).type == MidiEventType::Other); // poly aftertouch
    CHECK(decode({0xD0, 50}).type == MidiEventType::Other);     // channel pressure
}

TEST_CASE("every status byte with every channel decodes without crashing") {
    for (int status = 0; status < 256; ++status) {
        for (std::size_t size = 0; size <= 4; ++size) {
            std::array<std::uint8_t, 4> bytes{static_cast<std::uint8_t>(status), 60, 100, 1};
            MidiEvent ev = decode_midi(bytes.data(), size, 1);
            MidiEventType expected = MidiEventType::Other;
            if (size == 3 && status >= 0x80) {
                switch (status & 0xF0) {
                case 0x80:
                    expected = MidiEventType::NoteOff;
                    break;
                case 0x90:
                    expected = MidiEventType::NoteOn; // velocity 100
                    break;
                case 0xB0:
                    expected = MidiEventType::ControlChange;
                    break;
                default:
                    break;
                }
            }
            CHECK(ev.type == expected);
            if (expected != MidiEventType::Other) {
                CHECK(ev.channel == (status & 0x0F));
                CHECK(ev.data1 == 60);
                CHECK(ev.data2 == 100);
            }
        }
    }
}

TEST_CASE("span overload decodes like the pointer overload") {
    std::array<std::uint8_t, 3> msg{0x90, 61, 90};
    MidiEvent ev = decode_midi(std::span<const std::uint8_t>(msg), 5);
    CHECK(ev.type == MidiEventType::NoteOn);
    CHECK(ev.data1 == 61);
}
