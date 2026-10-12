// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/look.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "toonval/pitch.hpp"

namespace toonval {
namespace {

constexpr float kBlackKeyDarken = 0.35f;
// High contrast darkens less, so a black-key note can still reach `kHighContrastMinimum`.
constexpr float kBlackKeyDarkenHighContrast = 0.2f;

float clamp_or(float v, float lo, float hi, float fallback) {
    if (std::isnan(v)) {
        return fallback;
    }
    return std::clamp(v, lo, hi);
}

template <class E> E enum_or(E v, E last, E fallback) {
    return static_cast<std::uint8_t>(v) <= static_cast<std::uint8_t>(last) ? v : fallback;
}

// Soft-to-hard ramp: blue, yellow, red.
RGBA velocity_color(std::uint8_t velocity) {
    constexpr RGBA kSoft{76, 110, 245, 255};
    constexpr RGBA kMedium{252, 196, 25, 255};
    constexpr RGBA kHard{255, 80, 80, 255};
    const float t = static_cast<float>(std::min<int>(velocity, 127)) / 127.0f;
    return t < 0.5f ? lerp(kSoft, kMedium, t * 2.0f) : lerp(kMedium, kHard, (t - 0.5f) * 2.0f);
}

// A hue per octave, 32 degrees apart so the eleven MIDI octaves (-1..9) never share one.
RGBA octave_color(int note) {
    const int index = std::clamp(octave(note) + 1, 0, 10);
    return from_oklch(0.78, 0.13, 250.0 + 32.0 * index);
}

RGBA scheme_color(const LookSettings& look, int note, std::uint8_t velocity, Hand hand) {
    switch (look.color_scheme) {
    case ColorScheme::ByPitchClass:
        return look.pitch_colors[static_cast<std::size_t>(pitch_class(note))];
    case ColorScheme::ByOctave:
        return octave_color(note);
    case ColorScheme::ByVelocity:
        return velocity_color(velocity);
    case ColorScheme::Single:
        return look.single_color;
    case ColorScheme::ByHand:
        break;
    }
    switch (hand) {
    case Hand::Left:
        return look.left_color;
    case Hand::Right:
        return look.right_color;
    case Hand::Unknown:
        break;
    }
    return look.unknown_color;
}

constexpr std::array<std::string_view, 12> kLetterSharp{"C",  "C#", "D",  "D#", "E",  "F",
                                                        "F#", "G",  "G#", "A",  "A#", "B"};
constexpr std::array<std::string_view, 12> kLetterFlat{"C",  "Db", "D",  "Eb", "E",  "F",
                                                       "Gb", "G",  "Ab", "A",  "Bb", "B"};
constexpr std::array<std::string_view, 12> kSolfegeSharp{"Do",  "Do#", "Re",   "Re#", "Mi",  "Fa",
                                                         "Fa#", "Sol", "Sol#", "La",  "La#", "Si"};
constexpr std::array<std::string_view, 12> kSolfegeFlat{"Do",   "Reb", "Re",  "Mib", "Mi",  "Fa",
                                                        "Solb", "Sol", "Lab", "La",  "Sib", "Si"};

} // namespace

std::array<RGBA, 12> default_pitch_colors() {
    // Hues 30 degrees apart starting at red, with lightness alternating so neighbors differ in
    // brightness as well as hue. Every color keeps dark or white label text readable.
    std::array<RGBA, 12> wheel{};
    for (std::size_t i = 0; i < wheel.size(); ++i) {
        const double lightness = i % 2 == 0 ? 0.70 : 0.82;
        wheel[i] = from_oklch(lightness, 0.15, 25.0 + 30.0 * static_cast<double>(i));
    }
    return wheel;
}

BackgroundStyle LookSettings::effective_background() const {
    return reduced_effects && background == BackgroundStyle::Motion ? BackgroundStyle::Gradient
                                                                    : background;
}

bool LookSettings::effective_sparks() const { return sparks && !reduced_effects; }

LookSettings LookSettings::clamped() const {
    const LookSettings d;
    LookSettings s = *this;
    s.note_style = enum_or(s.note_style, NoteStyle::Neon, d.note_style);
    s.style_intensity =
        clamp_or(s.style_intensity, kMinStyleIntensity, kMaxStyleIntensity, d.style_intensity);
    s.color_scheme = enum_or(s.color_scheme, ColorScheme::Single, d.color_scheme);
    s.name_mode = enum_or(s.name_mode, NameMode::Solfege, d.name_mode);
    s.background = enum_or(s.background, BackgroundStyle::Motion, d.background);
    s.octave_numbering = enum_or(s.octave_numbering, OctaveNumbering::Yamaha, d.octave_numbering);
    s.key_size = enum_or(s.key_size, KeySize::ExtraLarge, d.key_size);
    s.chord_label_size =
        clamp_or(s.chord_label_size, kMinChordLabelSize, kMaxChordLabelSize, d.chord_label_size);
    s.visible_seconds =
        clamp_or(s.visible_seconds, kMinVisibleSeconds, kMaxVisibleSeconds, d.visible_seconds);
    s.keyboard_height =
        clamp_or(s.keyboard_height, kMinKeyboardHeight, kMaxKeyboardHeight, d.keyboard_height);
    s.grade_position = enum_or(s.grade_position, GradePosition::Top, d.grade_position);
    s.grade_opacity = std::clamp(s.grade_opacity, kMinGradeOpacity, kMaxGradeOpacity);
    return s;
}

void set_grade_words(LookSettings& look, bool on, int restore) {
    if (!on) {
        look.grade_words = false;
        return;
    }
    look.grade_words = true;
    if (look.grade_opacity <= kMinGradeOpacity) {
        look.grade_opacity = std::clamp(restore, kMinGradeOpacity + 1, kMaxGradeOpacity);
    }
}

void set_grade_opacity(LookSettings& look, int percent, int restore) {
    percent = std::clamp(percent, kMinGradeOpacity, kMaxGradeOpacity);
    if (percent > kMinGradeOpacity) {
        look.grade_words = true;
        look.grade_opacity = percent;
    } else {
        look.grade_words = false;
        look.grade_opacity = std::clamp(restore, kMinGradeOpacity + 1, kMaxGradeOpacity);
    }
}

bool LookSettings::is_valid() const { return clamped() == *this; }

LookSettings apply_preset(const LookSettings& current, PresetId id) {
    LookSettings out = preset(id);
    out.octave_numbering = current.octave_numbering;
    out.key_size = current.key_size;
    out.show_typing_keys = current.show_typing_keys;
    // Grade words are the player's choice, like the key size: a built-in look leaves them alone.
    out.grade_words = current.grade_words;
    out.grade_position = current.grade_position;
    out.grade_opacity = current.grade_opacity;
    return out;
}

bool is_preset(const LookSettings& look, PresetId id) { return apply_preset(look, id) == look; }

LookSettings preset(PresetId id) {
    LookSettings s;
    switch (id) {
    case PresetId::Default:
        break;
    case PresetId::Classic:
        s.note_style = NoteStyle::Filled;
        s.style_intensity = 1.0f;
        s.name_mode = NameMode::Off;
        s.held_glow = false;
        s.sparks = false;
        s.trail_fade = false;
        s.background = BackgroundStyle::Plain;
        break;
    case PresetId::NeonNight:
        s.note_style = NoteStyle::Neon;
        s.style_intensity = 0.85f;
        s.color_scheme = ColorScheme::ByPitchClass;
        s.name_mode = NameMode::Off;
        s.background = BackgroundStyle::Motion;
        s.visible_seconds = 4.5f;
        break;
    case PresetId::Glass:
        s.note_style = NoteStyle::Glassy;
        s.style_intensity = 0.8f;
        s.left_color = {94, 234, 212, 255};
        s.right_color = {130, 170, 255, 255};
        s.sparks = false;
        break;
    case PresetId::FrostedPastel:
        s.note_style = NoteStyle::Frosted;
        s.style_intensity = 0.7f;
        s.color_scheme = ColorScheme::ByPitchClass;
        for (std::size_t i = 0; i < s.pitch_colors.size(); ++i) {
            s.pitch_colors[i] = from_oklch(0.86, 0.07, 25.0 + 30.0 * static_cast<double>(i));
        }
        s.name_mode = NameMode::Solfege;
        s.sparks = false;
        s.grade_opacity = 80;
        break;
    case PresetId::HighContrast:
        s.note_style = NoteStyle::Filled;
        s.style_intensity = 1.0f;
        s.left_color = {255, 200, 60, 255};
        s.right_color = {90, 210, 255, 255};
        s.high_contrast = true;
        s.names_show_octave = true;
        s.held_glow = false;
        s.sparks = false;
        s.trail_fade = false;
        s.background = BackgroundStyle::Plain;
        s.chord_label_size = 1.5f;
        break;
    case PresetId::MinimalReduced:
        s.note_style = NoteStyle::Filled;
        s.style_intensity = 0.0f;
        s.name_mode = NameMode::Off;
        s.key_flash = false;
        s.held_glow = false;
        s.sparks = false;
        s.trail_fade = false;
        s.background = BackgroundStyle::Plain;
        s.reduced_effects = true;
        s.visible_seconds = 5.0f;
        s.grade_opacity = 60;
        break;
    }
    return s;
}

std::string_view preset_name(PresetId id) {
    switch (id) {
    case PresetId::Default:
        return "Default";
    case PresetId::Classic:
        return "Classic";
    case PresetId::NeonNight:
        return "Neon Night";
    case PresetId::Glass:
        return "Glass";
    case PresetId::FrostedPastel:
        return "Frosted Pastel";
    case PresetId::HighContrast:
        return "High Contrast";
    case PresetId::MinimalReduced:
        return "Minimal / Reduced";
    }
    return "";
}

RGBA note_color(const LookSettings& look, int note, std::uint8_t velocity, Hand hand) {
    RGBA c = scheme_color(look, note, velocity, hand);
    const bool black = is_black_key(note);
    if (look.high_contrast) {
        c = ensure_contrast(c, kBackgroundGradientTop, kHighContrastTarget);
    }
    if (black) {
        c = darken(c, look.high_contrast ? kBlackKeyDarkenHighContrast : kBlackKeyDarken);
        if (look.high_contrast) {
            c = ensure_contrast(c, kBackgroundGradientTop, kHighContrastMinimum);
        }
    }
    return c;
}

RGBA label_color_for(RGBA block) {
    constexpr RGBA kBlack{0, 0, 0, 255};
    constexpr RGBA kWhite{255, 255, 255, 255};
    return contrast_ratio(kBlack, block) >= contrast_ratio(kWhite, block) ? kBlack : kWhite;
}

RGBA note_label_color(const LookSettings& look, int note, std::uint8_t velocity, Hand hand) {
    return label_color_for(note_color(look, note, velocity, hand));
}

std::string note_label(int note, const LookSettings& look) {
    if (look.name_mode == NameMode::Off || !is_valid_note(note)) {
        return {};
    }
    const auto pc = static_cast<std::size_t>(pitch_class(note));
    const bool flats = look.names_prefer_flats;
    std::string out;
    if (look.name_mode == NameMode::Solfege) {
        out = flats ? kSolfegeFlat[pc] : kSolfegeSharp[pc];
    } else {
        out = flats ? kLetterFlat[pc] : kLetterSharp[pc];
    }
    if (look.names_show_octave) {
        out += std::to_string(octave_number(note, look));
    }
    return out;
}

std::string note_short_label(int note, const LookSettings& look) {
    std::string out = note_label(note, look);
    if (out.empty()) {
        return out;
    }
    // The letter or syllable comes first (capital, then lower case: "Sol"); a '#', a lower-case
    // 'b' after it or the octave number starts the rest.
    std::size_t keep = 1;
    while (keep < out.size() && std::islower(static_cast<unsigned char>(out[keep])) != 0 &&
           out[keep] != 'b') {
        ++keep;
    }
    out.resize(keep);
    return out;
}

int octave_number(int note, const LookSettings& look) {
    return octave(note) + look.octave_offset();
}

std::string c_marker_label(int note, const LookSettings& look) {
    if (!look.c_markers || !is_valid_note(note) || pitch_class(note) != 0) {
        return {};
    }
    return std::string(look.name_mode == NameMode::Solfege ? "Do" : "C") +
           std::to_string(octave_number(note, look));
}

bool is_middle_c(int note) { return note == kMiddleC; }

} // namespace toonval
