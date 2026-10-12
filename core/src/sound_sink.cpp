// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/sound_sink.hpp"

#include "toonval/keyboard.hpp"

#include <algorithm>
#include <cctype>

namespace toonval {

bool forward_to_sink(SoundSink& sink, const MidiEvent& ev) {
    if (!ev.is_virtual() || ev.synthetic) {
        return false;
    }
    switch (ev.type) {
    case MidiEventType::NoteOn:
        sink.note_on(ev.channel, ev.data1, ev.data2);
        return true;
    case MidiEventType::NoteOff:
        sink.note_off(ev.channel, ev.data1);
        return true;
    case MidiEventType::ControlChange:
        if (ev.data1 == kCcSustain) {
            sink.sustain(ev.channel, ev.data2 >= 64);
            return true;
        }
        return false;
    case MidiEventType::Other:
        break;
    }
    return false;
}

namespace {

bool equal_ignoring_case(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

bool contains_ignoring_case(std::string_view text, std::string_view part) {
    return std::search(text.begin(), text.end(), part.begin(), part.end(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           }) != text.end();
}

} // namespace

bool is_piano_output(std::string_view output_name, std::span<const std::string> open_input_names) {
    if (output_name.empty() || output_name.starts_with("toonval") ||
        contains_ignoring_case(output_name, "Midi Through")) {
        return false;
    }
    if (find_profile(output_name) != nullptr) {
        return true;
    }
    return std::any_of(open_input_names.begin(), open_input_names.end(),
                       [&](const std::string& in) { return equal_ignoring_case(in, output_name); });
}

} // namespace toonval
