// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/computer_keyboard.hpp"

#include <algorithm>

namespace toonval {
namespace {

struct KeyDef {
    int scancode;
    int offset;
    std::string_view label;
};

constexpr KeyDef kPlaying[] = {
    {scancode::kA, 0, "A"},  {scancode::kW, 1, "W"},          {scancode::kS, 2, "S"},
    {scancode::kE, 3, "E"},  {scancode::kD, 4, "D"},          {scancode::kF, 5, "F"},
    {scancode::kT, 6, "T"},  {scancode::kG, 7, "G"},          {scancode::kY, 8, "Y"},
    {scancode::kH, 9, "H"},  {scancode::kU, 10, "U"},         {scancode::kJ, 11, "J"},
    {scancode::kK, 12, "K"}, {scancode::kO, 13, "O"},         {scancode::kL, 14, "L"},
    {scancode::kP, 15, "P"}, {scancode::kSemicolon, 16, ";"}, {scancode::kApostrophe, 17, "'"},
};

bool in_table(int scancode) { return scancode >= 0 && scancode < 256; }

// Floor and ceiling division for a positive divisor.
int floor_div(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
int ceil_div(int a, int b) { return -floor_div(-a, b); }

} // namespace

ComputerKeyboard::ComputerKeyboard(KeyRange keyboard) : keyboard_(keyboard) {
    const KeyboardLayout normalised(keyboard);
    keyboard_ = normalised.range();
    update_bounds();
    octave_shift_ = std::clamp(0, min_shift_, max_shift_);
}

void ComputerKeyboard::set_keyboard(KeyRange keyboard) {
    keyboard_ = KeyboardLayout(keyboard).range();
    update_bounds();
    octave_shift_ = std::clamp(octave_shift_, min_shift_, max_shift_);
}

// The shifts where the whole span, base up to base + 17, is on the keyboard. A keyboard too narrow
// for that has none: the shift stays 0 and notes off the keyboard are dropped.
void ComputerKeyboard::update_bounds() {
    min_shift_ = ceil_div(keyboard_.lowest - kTypingBaseNote, 12);
    max_shift_ = floor_div(keyboard_.highest - (kTypingNoteSpan - 1) - kTypingBaseNote, 12);
    if (min_shift_ > max_shift_) {
        min_shift_ = 0;
        max_shift_ = 0;
    }
}

int ComputerKeyboard::offset_of(int scancode) {
    for (const KeyDef& k : kPlaying) {
        if (k.scancode == scancode) {
            return k.offset;
        }
    }
    return -1;
}

bool ComputerKeyboard::is_typing_key(int scancode) {
    if (offset_of(scancode) >= 0) {
        return true;
    }
    switch (scancode) {
    case scancode::kZ:
    case scancode::kX:
    case scancode::kC:
    case scancode::kV:
    case scancode::kTab:
        return true;
    default:
        return false;
    }
}

std::optional<int> ComputerKeyboard::note_for(int scancode) const {
    const int offset = offset_of(scancode);
    if (offset < 0) {
        return std::nullopt;
    }
    const int note = base_note() + offset;
    if (!keyboard_.contains(note)) {
        return std::nullopt;
    }
    return note;
}

std::string_view ComputerKeyboard::label_for_note(int note) const {
    const int offset = note - base_note();
    for (const KeyDef& k : kPlaying) {
        if (k.offset == offset) {
            return keyboard_.contains(note) ? k.label : std::string_view{};
        }
    }
    return {};
}

void ComputerKeyboard::emit(MidiEventType type, int data1, int data2, std::int64_t time_ns,
                            std::vector<MidiEvent>& out) const {
    MidiEvent e;
    e.time_ns = time_ns;
    e.type = type;
    e.channel = 0;
    e.data1 = static_cast<std::uint8_t>(data1);
    e.data2 = static_cast<std::uint8_t>(data2);
    e.source = InputSource::ComputerKeyboard;
    out.push_back(e);
}

void ComputerKeyboard::press_note(Held& slot, int note, std::int64_t time_ns,
                                  std::vector<MidiEvent>& out) {
    slot.down = true;
    slot.note = static_cast<std::int16_t>(note);
    if (note < 0) {
        return;
    }
    ++held_count_;
    auto& count = shared_[static_cast<std::size_t>(note)];
    if (count++ == 0) {
        emit(MidiEventType::NoteOn, note, velocity(), time_ns, out);
    }
}

void ComputerKeyboard::release_note(int note, std::int64_t time_ns, std::vector<MidiEvent>& out) {
    if (note < 0) {
        return;
    }
    --held_count_;
    auto& count = shared_[static_cast<std::size_t>(note)];
    if (--count == 0) {
        emit(MidiEventType::NoteOff, note, 0, time_ns, out);
    }
}

void ComputerKeyboard::key(int code, bool down, bool repeat, std::int64_t time_ns,
                           std::vector<MidiEvent>& out) {
    if (!in_table(code) || !is_typing_key(code)) {
        return;
    }
    Held& slot = keys_[static_cast<std::size_t>(code)];
    if (!down) {
        if (!slot.down) {
            return; // a key-up for a press we never saw
        }
        slot.down = false;
        if (code == scancode::kTab) {
            sustain_down_ = false;
            emit(MidiEventType::ControlChange, kCcSustain, 0, time_ns, out);
            return;
        }
        const int note = slot.note;
        slot.note = -1;
        release_note(note, time_ns, out);
        return;
    }
    // Key-down. Auto-repeat, or a second down with no up between, is the same press.
    if (repeat || slot.down) {
        return;
    }
    const int offset = offset_of(code);
    if (offset >= 0) {
        const int note = base_note() + offset;
        press_note(slot, keyboard_.contains(note) ? note : -1, time_ns, out);
        return;
    }
    slot.down = true;
    switch (code) {
    case scancode::kZ:
        octave_shift_ = std::max(octave_shift_ - 1, min_shift_);
        break;
    case scancode::kX:
        octave_shift_ = std::min(octave_shift_ + 1, max_shift_);
        break;
    case scancode::kC:
        velocity_level_ = std::max(velocity_level_ - 1, 0);
        break;
    case scancode::kV:
        velocity_level_ =
            std::min(velocity_level_ + 1, static_cast<int>(kVelocityLevels.size()) - 1);
        break;
    case scancode::kTab:
        sustain_down_ = true;
        emit(MidiEventType::ControlChange, kCcSustain, 127, time_ns, out);
        break;
    default:
        break;
    }
}

void ComputerKeyboard::release_all(std::int64_t time_ns, std::vector<MidiEvent>& out) {
    for (std::size_t code = 0; code < keys_.size(); ++code) {
        Held& slot = keys_[code];
        if (!slot.down) {
            continue;
        }
        slot.down = false;
        const int note = slot.note;
        slot.note = -1;
        release_note(note, time_ns, out);
    }
    if (sustain_down_) {
        sustain_down_ = false;
        emit(MidiEventType::ControlChange, kCcSustain, 0, time_ns, out);
    }
}

} // namespace toonval
