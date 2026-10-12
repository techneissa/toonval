// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/key_state.hpp"

namespace toonval {

namespace {
constexpr std::uint8_t kSustainDownThreshold = 64;

constexpr std::size_t index(int note) { return static_cast<std::size_t>(note); }
} // namespace

KeyTransition KeyState::apply(const MidiEvent& ev) {
    switch (ev.type) {
    case MidiEventType::NoteOn:
    case MidiEventType::NoteOff: {
        if (!is_valid_note(ev.data1)) {
            return {};
        }
        Key& key = keys_[ev.data1];
        // A note-on with velocity 0 is a note-off, whoever built the event.
        const bool press = ev.type == MidiEventType::NoteOn && ev.data2 != 0;
        if (press) {
            if (key.down) {
                return {};
            }
            key = {true, ev.data2, ev.time_ns};
            ++held_count_;
            return {KeyTransitionType::Pressed, ev.data1, ev.data2, 0};
        }
        if (!key.down) {
            return {};
        }
        key = {};
        --held_count_;
        return {KeyTransitionType::Released, ev.data1, ev.data2, 0};
    }
    case MidiEventType::ControlChange:
        switch (ev.data1) {
        case kCcSustain:
            sustain_down_ = ev.data2 >= kSustainDownThreshold;
            return {};
        case kCcAllSoundOff:
        case kCcAllNotesOff:
            return release_all();
        default:
            return {};
        }
    case MidiEventType::Other:
        break;
    }
    return {};
}

KeyTransition KeyState::clear() {
    sustain_down_ = false;
    return release_all();
}

KeyTransition KeyState::release_all() {
    const int released = held_count_;
    keys_.fill(Key{});
    held_count_ = 0;
    if (released == 0) {
        return {};
    }
    return {KeyTransitionType::AllReleased, 0, 0, static_cast<std::uint8_t>(released)};
}

bool KeyState::is_down(int note) const { return is_valid_note(note) && keys_[index(note)].down; }

std::vector<std::uint8_t> KeyState::held_notes() const {
    std::vector<std::uint8_t> notes;
    held_notes(notes);
    return notes;
}

void KeyState::held_notes(std::vector<std::uint8_t>& out) const {
    out.clear();
    out.reserve(static_cast<std::size_t>(held_count_));
    for (int note = kMidiNoteMin; note <= kMidiNoteMax; ++note) {
        if (keys_[index(note)].down) {
            out.push_back(static_cast<std::uint8_t>(note));
        }
    }
}

std::optional<std::int64_t> KeyState::press_time_ns(int note) const {
    if (!is_down(note)) {
        return std::nullopt;
    }
    return keys_[index(note)].press_time_ns;
}

std::optional<std::uint8_t> KeyState::press_velocity(int note) const {
    if (!is_down(note)) {
        return std::nullopt;
    }
    return keys_[index(note)].velocity;
}

} // namespace toonval
