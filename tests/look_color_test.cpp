// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/look.hpp"

#include <algorithm>
#include <vector>

#include <doctest/doctest.h>

#include "toonval/pitch.hpp"

using namespace toonval;

namespace {

constexpr ColorScheme kSchemes[] = {ColorScheme::ByHand, ColorScheme::ByPitchClass,
                                    ColorScheme::ByOctave, ColorScheme::ByVelocity,
                                    ColorScheme::Single};
constexpr Hand kHands[] = {Hand::Unknown, Hand::Left, Hand::Right};
constexpr std::uint8_t kVelocities[] = {0, 1, 40, 64, 100, 127};

// The settings the color tests sweep: the default look in each scheme, with and without high
// contrast, plus every preset.
std::vector<LookSettings> sweep() {
    std::vector<LookSettings> out;
    for (ColorScheme scheme : kSchemes) {
        for (bool hc : {false, true}) {
            LookSettings s;
            s.color_scheme = scheme;
            s.high_contrast = hc;
            out.push_back(s);
        }
    }
    for (PresetId id : kAllPresets) {
        out.push_back(preset(id));
    }
    return out;
}

} // namespace

TEST_CASE("by hand uses the three hand colors on white keys") {
    LookSettings s;
    s.color_scheme = ColorScheme::ByHand;
    s.left_color = {1, 2, 3, 255};
    s.right_color = {4, 5, 6, 255};
    s.unknown_color = {7, 8, 9, 255};
    CHECK(note_color(s, 60, 80, Hand::Left) == s.left_color);
    CHECK(note_color(s, 60, 80, Hand::Right) == s.right_color);
    CHECK(note_color(s, 60, 80, Hand::Unknown) == s.unknown_color);
}

TEST_CASE("by pitch class uses the twelve colors, the same in every octave") {
    LookSettings s;
    s.color_scheme = ColorScheme::ByPitchClass;
    for (int pc = 0; pc < 12; ++pc) {
        s.pitch_colors[static_cast<std::size_t>(pc)] =
            RGBA{static_cast<std::uint8_t>(10 + pc), 20, 30, 255};
    }
    CHECK(note_color(s, 60, 64, Hand::Right) == s.pitch_colors[0]);
    CHECK(note_color(s, 72, 64, Hand::Left) == s.pitch_colors[0]);
    CHECK(note_color(s, 62, 64, Hand::Right) == s.pitch_colors[2]);
    CHECK(note_color(s, 64 + 24, 64, Hand::Right) == s.pitch_colors[4]);
}

TEST_CASE("by octave gives each octave its own color and ignores hand and velocity") {
    LookSettings s;
    s.color_scheme = ColorScheme::ByOctave;
    CHECK(note_color(s, 60, 10, Hand::Left) == note_color(s, 62, 120, Hand::Right));
    CHECK_FALSE(note_color(s, 60, 64, Hand::Right) == note_color(s, 72, 64, Hand::Right));
    for (int oct = 0; oct < 8; ++oct) {
        const int c = 12 * (oct + 1);
        CHECK_FALSE(note_color(s, c, 64, Hand::Right) == note_color(s, c + 12, 64, Hand::Right));
    }
}

TEST_CASE("by velocity runs a ramp: harder notes look different, equal velocity is equal") {
    LookSettings s;
    s.color_scheme = ColorScheme::ByVelocity;
    const RGBA soft = note_color(s, 60, 1, Hand::Right);
    const RGBA medium = note_color(s, 60, 64, Hand::Right);
    const RGBA hard = note_color(s, 60, 127, Hand::Right);
    CHECK_FALSE(soft == medium);
    CHECK_FALSE(medium == hard);
    CHECK(perceptual_distance(soft, hard) > 0.1);
    CHECK(note_color(s, 60, 64, Hand::Left) == medium);
    CHECK(note_color(s, 62, 64, Hand::Right) == medium);
    // The ends of the ramp are exact, and a velocity of 0 is a valid input.
    CHECK(note_color(s, 60, 0, Hand::Right) == RGBA{76, 110, 245, 255});
    CHECK(note_color(s, 60, 127, Hand::Right) == RGBA{255, 80, 80, 255});
}

TEST_CASE("single uses the one custom color") {
    LookSettings s;
    s.color_scheme = ColorScheme::Single;
    s.single_color = {200, 10, 90, 255};
    for (Hand h : kHands) {
        CHECK(note_color(s, 61 - 1, 64, h) == s.single_color);
    }
}

TEST_CASE("black-key notes are darker than white-key notes in every scheme") {
    for (const LookSettings& base : sweep()) {
        for (Hand h : kHands) {
            for (std::uint8_t v : kVelocities) {
                for (int note = 24; note <= 108; ++note) {
                    if (!is_black_key(note)) {
                        continue;
                    }
                    // Give every pitch class the black key's color, so the white key just below
                    // differs from it only by being a white key.
                    LookSettings s = base;
                    s.pitch_colors.fill(
                        base.pitch_colors[static_cast<std::size_t>(pitch_class(note))]);
                    CHECK(relative_luminance(note_color(s, note, v, h)) <
                          relative_luminance(note_color(s, note - 1, v, h)));
                }
            }
        }
    }
}

TEST_CASE("the default pitch-class wheel keeps neighbors clearly apart") {
    const std::array<RGBA, 12> wheel = default_pitch_colors();
    double min_adjacent = 10.0;
    double min_any = 10.0;
    for (std::size_t i = 0; i < 12; ++i) {
        min_adjacent = std::min(min_adjacent, perceptual_distance(wheel[i], wheel[(i + 1) % 12]));
        for (std::size_t j = i + 1; j < 12; ++j) {
            min_any = std::min(min_any, perceptual_distance(wheel[i], wheel[j]));
        }
    }
    CHECK(min_adjacent >= 0.1); // five times the smallest visible difference
    CHECK(min_any >= 0.08);
    for (const RGBA& c : wheel) {
        CHECK(c.a == 255);
    }
}

TEST_CASE("high contrast reaches 4.5:1 against the background for every note") {
    for (ColorScheme scheme : kSchemes) {
        LookSettings s;
        s.color_scheme = scheme;
        s.high_contrast = true;
        // Dark custom colors are the hard case: they must be lifted.
        s.left_color = {20, 20, 60, 255};
        s.right_color = {10, 40, 20, 255};
        s.unknown_color = {0, 0, 0, 255};
        s.single_color = {30, 0, 0, 255};
        for (auto& c : s.pitch_colors) {
            c = darken(c, 0.8f);
        }
        for (Hand h : kHands) {
            for (std::uint8_t v : kVelocities) {
                for (int note = 0; note <= 127; ++note) {
                    const RGBA c = note_color(s, note, v, h);
                    CHECK(contrast_ratio(c, kBackgroundColor) >= kHighContrastMinimum);
                    CHECK(contrast_ratio(c, kBackgroundGradientTop) >= kHighContrastMinimum);
                    if (!is_black_key(note)) {
                        CHECK(contrast_ratio(c, kBackgroundGradientTop) >= kHighContrastTarget);
                    }
                }
            }
        }
    }
}

TEST_CASE("high contrast keeps the hue of a bright custom color") {
    LookSettings s;
    s.high_contrast = true;
    s.right_color = {255, 215, 0, 255}; // already bright
    CHECK(note_color(s, 60, 64, Hand::Right) == s.right_color);
}

TEST_CASE("label text is black or white and reads at 4.5:1 on every block color") {
    for (const LookSettings& s : sweep()) {
        for (Hand h : kHands) {
            for (std::uint8_t v : kVelocities) {
                for (int note = 0; note <= 127; ++note) {
                    const RGBA block = note_color(s, note, v, h);
                    const RGBA text = note_label_color(s, note, v, h);
                    CHECK((text == RGBA{0, 0, 0, 255} || text == RGBA{255, 255, 255, 255}));
                    CHECK(contrast_ratio(text, block) >= kLabelMinContrast);
                }
            }
        }
    }
}

TEST_CASE("label text reads on every pitch-class color, hand color and ramp end") {
    LookSettings s;
    for (const RGBA& c : s.pitch_colors) {
        CHECK(contrast_ratio(label_color_for(c), c) >= kLabelMinContrast);
        CHECK(contrast_ratio(label_color_for(darken(c, 0.35f)), darken(c, 0.35f)) >=
              kLabelMinContrast);
    }
    for (const RGBA& c : {s.left_color, s.right_color, s.unknown_color, s.single_color}) {
        CHECK(contrast_ratio(label_color_for(c), c) >= kLabelMinContrast);
    }
    // The worst case for any text color: the grey where white and black text tie.
    for (int g = 0; g < 256; ++g) {
        const RGBA grey{static_cast<std::uint8_t>(g), static_cast<std::uint8_t>(g),
                        static_cast<std::uint8_t>(g), 255};
        CHECK(contrast_ratio(label_color_for(grey), grey) >= kLabelMinContrast);
    }
}

TEST_CASE("dark blocks get white text and light blocks get black text") {
    CHECK(label_color_for(RGBA{20, 20, 60, 255}) == RGBA{255, 255, 255, 255});
    CHECK(label_color_for(RGBA{250, 250, 120, 255}) == RGBA{0, 0, 0, 255});
}

TEST_CASE("a pressed key lights in exactly its note's color, under every scheme") {
    for (const LookSettings& s : sweep()) {
        for (Hand h : kHands) {
            for (std::uint8_t v : kVelocities) {
                for (int note = 0; note <= 127; ++note) {
                    CHECK(key_light_color(s, note, v, h) == note_color(s, note, v, h));
                }
            }
        }
    }
}

TEST_CASE("a key pressed with no note to play uses the same scheme") {
    // Free play: no hand, whatever the player hit. By hand gives the unknown color, the other
    // schemes give what they would give for a note.
    LookSettings s;
    s.color_scheme = ColorScheme::ByHand;
    CHECK(key_light_color(s, 60, 90, Hand::Unknown) == s.unknown_color);
    s.color_scheme = ColorScheme::ByPitchClass;
    CHECK(key_light_color(s, 62, 90, Hand::Unknown) == s.pitch_colors[2]);
    CHECK(key_light_color(s, 62, 90, Hand::Unknown) == note_color(s, 62, 90, Hand::Right));
}

TEST_CASE("a lit black key is darker than a lit white key, as its note is") {
    for (ColorScheme scheme : {ColorScheme::ByHand, ColorScheme::ByOctave, ColorScheme::ByVelocity,
                               ColorScheme::Single}) {
        LookSettings s;
        s.color_scheme = scheme;
        CHECK(relative_luminance(key_light_color(s, 61, 90, Hand::Right)) <
              relative_luminance(key_light_color(s, 60, 90, Hand::Right)));
    }
}
