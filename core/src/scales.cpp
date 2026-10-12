// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/scales.hpp"

#include "toonval/pitch.hpp"

#include <cstdlib>

namespace toonval {
namespace {

constexpr std::array<int, 7> kMajor{0, 2, 4, 5, 7, 9, 11};
constexpr std::array<int, 7> kNaturalMinor{0, 2, 3, 5, 7, 8, 10};
constexpr std::array<int, 7> kHarmonicMinor{0, 2, 3, 5, 7, 8, 11};
constexpr std::array<int, 7> kMelodicMinor{0, 2, 3, 5, 7, 9, 11};
constexpr std::array<int, 7> kDorian{0, 2, 3, 5, 7, 9, 10};
constexpr std::array<int, 7> kPhrygian{0, 1, 3, 5, 7, 8, 10};
constexpr std::array<int, 7> kLydian{0, 2, 4, 6, 7, 9, 11};
constexpr std::array<int, 7> kMixolydian{0, 2, 4, 5, 7, 9, 10};
constexpr std::array<int, 7> kLocrian{0, 1, 3, 5, 6, 8, 10};
constexpr std::array<int, 5> kMajorPentatonic{0, 2, 4, 7, 9};
constexpr std::array<int, 5> kMinorPentatonic{0, 3, 5, 7, 10};
constexpr std::array<int, 6> kBlues{0, 3, 5, 6, 7, 10};
constexpr std::array<int, 12> kChromatic{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
constexpr std::array<int, 6> kWholeTone{0, 2, 4, 6, 8, 10};

// How many letters above the tonic's letter each degree is written on. A seven-note scale takes
// 0..6; the others keep the letter of the degree they come from, with the flat fifth of the blues
// written as a flat fifth (C Eb F Gb G Bb) and the whole-tone scale as 1 2 3 #4 #5 b7.
constexpr std::array<int, 7> kSevenLetters{0, 1, 2, 3, 4, 5, 6};
constexpr std::array<int, 5> kMajorPentatonicLetters{0, 1, 2, 4, 5};
constexpr std::array<int, 5> kMinorPentatonicLetters{0, 2, 3, 4, 6};
constexpr std::array<int, 6> kBluesLetters{0, 2, 3, 4, 4, 6};
constexpr std::array<int, 6> kWholeToneLetters{0, 1, 2, 3, 4, 6};

constexpr std::array<char, 7> kLetters{'C', 'D', 'E', 'F', 'G', 'A', 'B'};
constexpr std::array<int, 7> kLetterPc{0, 2, 4, 5, 7, 9, 11};

int letter_index(char letter) {
    for (std::size_t i = 0; i < kLetters.size(); ++i) {
        if (kLetters[i] == letter) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::span<const int> letter_steps(ScaleType type) {
    switch (type) {
    case ScaleType::MajorPentatonic:
        return kMajorPentatonicLetters;
    case ScaleType::MinorPentatonic:
        return kMinorPentatonicLetters;
    case ScaleType::Blues:
        return kBluesLetters;
    case ScaleType::WholeTone:
        return kWholeToneLetters;
    case ScaleType::Chromatic:
        return {};
    default:
        return kSevenLetters;
    }
}

// Letter and accidental of each pitch class in a chromatic scale written with sharps or flats.
struct PcName {
    char letter;
    int accidental;
};
constexpr std::array<PcName, 12> kSharpNames{{{'C', 0},
                                              {'C', 1},
                                              {'D', 0},
                                              {'D', 1},
                                              {'E', 0},
                                              {'F', 0},
                                              {'F', 1},
                                              {'G', 0},
                                              {'G', 1},
                                              {'A', 0},
                                              {'A', 1},
                                              {'B', 0}}};
constexpr std::array<PcName, 12> kFlatNames{{{'C', 0},
                                             {'D', -1},
                                             {'D', 0},
                                             {'E', -1},
                                             {'E', 0},
                                             {'F', 0},
                                             {'G', -1},
                                             {'G', 0},
                                             {'A', -1},
                                             {'A', 0},
                                             {'B', -1},
                                             {'B', 0}}};

bool tonic_ok(Tonic t) {
    return letter_index(t.letter) >= 0 && t.accidental >= -2 && t.accidental <= 2;
}

int accidental_cost(const std::vector<SpelledNote>& notes) {
    int cost = 0;
    for (const SpelledNote& n : notes) {
        cost += std::abs(n.accidental);
    }
    return cost;
}

} // namespace

const char* scale_type_name(ScaleType type) {
    switch (type) {
    case ScaleType::Major:
        return "Major";
    case ScaleType::NaturalMinor:
        return "Natural minor";
    case ScaleType::HarmonicMinor:
        return "Harmonic minor";
    case ScaleType::MelodicMinor:
        return "Melodic minor";
    case ScaleType::Dorian:
        return "Dorian";
    case ScaleType::Phrygian:
        return "Phrygian";
    case ScaleType::Lydian:
        return "Lydian";
    case ScaleType::Mixolydian:
        return "Mixolydian";
    case ScaleType::Locrian:
        return "Locrian";
    case ScaleType::MajorPentatonic:
        return "Major pentatonic";
    case ScaleType::MinorPentatonic:
        return "Minor pentatonic";
    case ScaleType::Blues:
        return "Blues";
    case ScaleType::Chromatic:
        return "Chromatic";
    case ScaleType::WholeTone:
        return "Whole tone";
    }
    return "";
}

std::span<const int> scale_intervals(ScaleType type) {
    switch (type) {
    case ScaleType::Major:
        return kMajor;
    case ScaleType::NaturalMinor:
        return kNaturalMinor;
    case ScaleType::HarmonicMinor:
        return kHarmonicMinor;
    case ScaleType::MelodicMinor:
        return kMelodicMinor;
    case ScaleType::Dorian:
        return kDorian;
    case ScaleType::Phrygian:
        return kPhrygian;
    case ScaleType::Lydian:
        return kLydian;
    case ScaleType::Mixolydian:
        return kMixolydian;
    case ScaleType::Locrian:
        return kLocrian;
    case ScaleType::MajorPentatonic:
        return kMajorPentatonic;
    case ScaleType::MinorPentatonic:
        return kMinorPentatonic;
    case ScaleType::Blues:
        return kBlues;
    case ScaleType::Chromatic:
        return kChromatic;
    case ScaleType::WholeTone:
        return kWholeTone;
    }
    return kMajor;
}

std::span<const int> scale_intervals_descending(ScaleType type) {
    return type == ScaleType::MelodicMinor ? std::span<const int>(kNaturalMinor)
                                           : scale_intervals(type);
}

std::vector<int> scale_pitch_classes(int root_pc, ScaleType type) {
    const int root = pitch_class(root_pc);
    std::vector<int> pcs;
    for (int step : scale_intervals(type)) {
        pcs.push_back((root + step) % 12);
    }
    return pcs;
}

unsigned scale_pitch_class_mask(int root_pc, ScaleType type) {
    unsigned mask = 0;
    for (int pc : scale_pitch_classes(root_pc, type)) {
        mask |= 1u << pc;
    }
    return mask;
}

bool is_in_scale(int note, int root_pc, ScaleType type) {
    return is_valid_note(note) &&
           (scale_pitch_class_mask(root_pc, type) >> pitch_class(note) & 1u) != 0;
}

bool is_root(int note, int root_pc) {
    return is_valid_note(note) && pitch_class(note) == pitch_class(root_pc);
}

int Tonic::pitch_class() const {
    const int index = letter_index(letter);
    if (index < 0) {
        return -1;
    }
    return ((kLetterPc[static_cast<std::size_t>(index)] + accidental) % 12 + 12) % 12;
}

std::vector<SpelledNote> spell_scale(Tonic tonic, ScaleType type) {
    if (!tonic_ok(tonic)) {
        return {};
    }
    const int tonic_pc = tonic.pitch_class();
    const std::span<const int> steps = scale_intervals(type);
    std::vector<SpelledNote> out;
    if (type == ScaleType::Chromatic) {
        const auto& table = tonic.accidental < 0 ? kFlatNames : kSharpNames;
        out.push_back({tonic.letter, tonic.accidental, tonic_pc});
        for (std::size_t i = 1; i < steps.size(); ++i) {
            const int pc = (tonic_pc + steps[i]) % 12;
            const PcName& n = table[static_cast<std::size_t>(pc)];
            out.push_back({n.letter, n.accidental, pc});
        }
        return out;
    }
    const std::span<const int> letters = letter_steps(type);
    const int base = letter_index(tonic.letter);
    for (std::size_t i = 0; i < steps.size(); ++i) {
        const auto letter = static_cast<std::size_t>((base + letters[i]) % 7);
        const int pc = (tonic_pc + steps[i]) % 12;
        int accidental = (pc - kLetterPc[letter] + 12) % 12;
        if (accidental > 6) {
            accidental -= 12;
        }
        if (std::abs(accidental) > 2) {
            return {};
        }
        out.push_back({kLetters[letter], accidental, pc});
    }
    return out;
}

Tonic default_tonic(int root_pc, ScaleType type, bool prefer_flats) {
    const int pc = pitch_class(root_pc);
    for (std::size_t i = 0; i < kLetterPc.size(); ++i) {
        if (kLetterPc[i] == pc) {
            return {kLetters[i], 0};
        }
    }
    const PcName& sharp = kSharpNames[static_cast<std::size_t>(pc)];
    const PcName& flat = kFlatNames[static_cast<std::size_t>(pc)];
    const Tonic sharp_tonic{sharp.letter, sharp.accidental};
    const Tonic flat_tonic{flat.letter, flat.accidental};
    const std::vector<SpelledNote> with_sharps = spell_scale(sharp_tonic, type);
    const std::vector<SpelledNote> with_flats = spell_scale(flat_tonic, type);
    if (with_sharps.empty()) {
        return flat_tonic;
    }
    if (with_flats.empty()) {
        return sharp_tonic;
    }
    const int sharp_cost = accidental_cost(with_sharps);
    const int flat_cost = accidental_cost(with_flats);
    if (sharp_cost != flat_cost) {
        return sharp_cost < flat_cost ? sharp_tonic : flat_tonic;
    }
    return prefer_flats ? flat_tonic : sharp_tonic;
}

std::string spelled_name(const SpelledNote& note) {
    std::string name(1, note.letter);
    for (int i = 0; i < std::abs(note.accidental); ++i) {
        name += note.accidental > 0 ? '#' : 'b';
    }
    return name;
}

std::string spelled_name(int midi_note, const SpelledNote& note, int octave_offset) {
    if (!is_valid_note(midi_note)) {
        return {};
    }
    // The octave number goes with the letter: B#3 sounds as C4 and Cb4 as B3.
    return spelled_name(note) + std::to_string(octave(midi_note - note.accidental) + octave_offset);
}

std::vector<std::string> scale_note_names(Tonic tonic, ScaleType type) {
    std::vector<std::string> names;
    for (const SpelledNote& n : spell_scale(tonic, type)) {
        names.push_back(spelled_name(n));
    }
    return names;
}

std::vector<std::string> scale_note_names_with_octave(Tonic tonic, ScaleType type, int root_note,
                                                      int octave_offset) {
    std::vector<std::string> names;
    if (!is_valid_note(root_note) || pitch_class(root_note) != tonic.pitch_class()) {
        return names;
    }
    const std::vector<SpelledNote> spelled = spell_scale(tonic, type);
    const std::span<const int> steps = scale_intervals(type);
    for (std::size_t i = 0; i < spelled.size(); ++i) {
        const int note = root_note + steps[i];
        if (!is_valid_note(note)) {
            break;
        }
        names.push_back(spelled_name(note, spelled[i], octave_offset));
    }
    return names;
}

} // namespace toonval
