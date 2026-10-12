// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/pitch.hpp"

#include <array>
#include <cmath>

namespace toonval {

double frequency_hz(int note, double a4_hz) { return a4_hz * std::exp2((note - kA4) / 12.0); }

double note_from_frequency(double hz, double a4_hz) {
    if (!std::isfinite(hz) || !std::isfinite(a4_hz) || hz <= 0.0 || a4_hz <= 0.0) {
        return -1.0;
    }
    return kA4 + 12.0 * std::log2(hz / a4_hz);
}

std::string note_name(int note, bool prefer_flats, int octave_offset) {
    if (!is_valid_note(note)) {
        return {};
    }
    static constexpr std::array<const char*, 12> kSharps = {"C",  "C#", "D",  "D#", "E",  "F",
                                                            "F#", "G",  "G#", "A",  "A#", "B"};
    static constexpr std::array<const char*, 12> kFlats = {"C",  "Db", "D",  "Eb", "E",  "F",
                                                           "Gb", "G",  "Ab", "A",  "Bb", "B"};
    const auto& names = prefer_flats ? kFlats : kSharps;
    return std::string(names[static_cast<size_t>(pitch_class(note))]) +
           std::to_string(octave(note) + octave_offset);
}

} // namespace toonval
