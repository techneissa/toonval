// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/midi_event.hpp"

namespace toonval {

namespace {
constexpr std::uint8_t kStatusBit = 0x80;
constexpr std::uint8_t kNoteOffStatus = 0x80;
constexpr std::uint8_t kNoteOnStatus = 0x90;
constexpr std::uint8_t kControlChangeStatus = 0xB0;
constexpr std::size_t kChannelMessageSize = 3;
constexpr std::uint8_t kSysExStart = 0xF0;
constexpr std::uint8_t kFirstRealtime = 0xF8;

bool is_data(std::uint8_t b) { return (b & kStatusBit) == 0; }
bool is_realtime(std::uint8_t b) { return b >= kFirstRealtime; }

// Size of a channel message including its status byte.
std::size_t channel_message_size(std::uint8_t status) {
    const std::uint8_t kind = status & 0xF0;
    return (kind == 0xC0 || kind == 0xD0) ? 2 : 3;
}
} // namespace

MidiEvent decode_midi(const std::uint8_t* bytes, std::size_t size, std::int64_t time_ns) {
    MidiEvent ev;
    ev.time_ns = time_ns;
    if (bytes == nullptr || size != kChannelMessageSize) {
        return ev;
    }
    // Data bytes never have the high bit; a message that starts with one lost its status byte.
    const std::uint8_t status = bytes[0];
    if ((status & kStatusBit) == 0 || (bytes[1] & kStatusBit) != 0 ||
        (bytes[2] & kStatusBit) != 0) {
        return ev;
    }
    MidiEventType type = MidiEventType::Other;
    switch (status & 0xF0) {
    case kNoteOffStatus:
        type = MidiEventType::NoteOff;
        break;
    case kNoteOnStatus:
        type = bytes[2] == 0 ? MidiEventType::NoteOff : MidiEventType::NoteOn;
        break;
    case kControlChangeStatus:
        type = MidiEventType::ControlChange;
        break;
    default:
        return ev; // other channel messages and every 0xF0..0xFF system message
    }
    ev.type = type;
    ev.channel = status & 0x0F;
    ev.data1 = bytes[1];
    ev.data2 = bytes[2];
    return ev;
}

bool is_trainer_event(const MidiEvent& ev) {
    switch (ev.type) {
    case MidiEventType::NoteOn:
    case MidiEventType::NoteOff:
        return true;
    case MidiEventType::ControlChange:
        return ev.data1 == kCcSustain || ev.data1 == kCcAllSoundOff || ev.data1 == kCcAllNotesOff;
    case MidiEventType::Other:
        break;
    }
    return false;
}

bool MidiStreamSplitter::next(ChannelMessage& out) {
    // One byte at a time with all state in members, so where a chunk ends never matters.
    while (pos_ < size_) {
        const std::uint8_t b = bytes_[pos_++];
        if (is_realtime(b)) {
            continue; // clock, active sensing, ...: never part of a message, never cancels one
        }
        if (!is_data(b)) {
            // 0x80..0xEF starts a channel message. SysEx, its end (F7) and system common
            // (F1..F6) clear running status, so the data bytes after them are dropped.
            running_status_ = b < kSysExStart ? b : 0;
            got_ = 0;
            continue;
        }
        if (running_status_ == 0) {
            continue; // SysEx or system common data, or a data byte with no status
        }
        data_[got_++] = b;
        if (got_ == channel_message_size(running_status_) - 1) {
            out.bytes = {running_status_, data_[0], got_ == 2 ? data_[1] : std::uint8_t{0}};
            out.size = got_ + 1;
            got_ = 0; // running status stays for the next message
            return true;
        }
    }
    return false;
}

} // namespace toonval
