// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/look.hpp"

#include <limits>
#include <set>
#include <string>

#include <doctest/doctest.h>

using namespace toonval;

TEST_CASE("defaults are valid and match the documented values") {
    const LookSettings d;
    CHECK(d.is_valid());
    CHECK(d.note_style == NoteStyle::Glassy);
    CHECK(d.style_intensity == doctest::Approx(0.6f));
    CHECK(d.color_scheme == ColorScheme::ByHand);
    CHECK(d.right_color == RGBA{90, 170, 255, 255}); // the app's accent
    CHECK_FALSE(d.high_contrast);
    CHECK(d.name_mode == NameMode::Letters);
    CHECK(d.names_on_blocks);
    CHECK_FALSE(d.names_on_keys);
    CHECK_FALSE(d.names_show_octave);
    CHECK(d.c_markers);
    CHECK(d.octave_numbering == OctaveNumbering::Scientific);
    CHECK_FALSE(d.names_prefer_flats);
    CHECK(d.background == BackgroundStyle::Gradient);
    CHECK_FALSE(d.reduced_effects);
    CHECK(d.chord_label);
    CHECK(d.chord_label_size == doctest::Approx(1.0f));
    CHECK(d.visible_seconds == doctest::Approx(4.0f));
    CHECK(d.keyboard_height == doctest::Approx(0.18f));
    CHECK(d.pitch_colors == default_pitch_colors());
    CHECK(kBackgroundColor == RGBA{18, 20, 26, 255}); // what the app clears the screen to
}

TEST_CASE("clamped brings every number back into its range") {
    LookSettings s;
    s.style_intensity = 7.0f;
    s.chord_label_size = 0.0f;
    s.visible_seconds = 1000.0f;
    s.keyboard_height = -1.0f;
    CHECK_FALSE(s.is_valid());
    const LookSettings c = s.clamped();
    CHECK(c.is_valid());
    CHECK(c.style_intensity == kMaxStyleIntensity);
    CHECK(c.chord_label_size == kMinChordLabelSize);
    CHECK(c.visible_seconds == kMaxVisibleSeconds);
    CHECK(c.keyboard_height == kMinKeyboardHeight);
    // Untouched fields stay.
    CHECK(c.note_style == s.note_style);
    CHECK(c.right_color == s.right_color);
}

TEST_CASE("clamped leaves in-range values alone and validate() clamps in place") {
    LookSettings s;
    s.visible_seconds = kMinVisibleSeconds;
    s.keyboard_height = kMaxKeyboardHeight;
    CHECK(s.is_valid());
    CHECK(s.clamped() == s);
    s.visible_seconds = 99.0f;
    s.validate();
    CHECK(s.visible_seconds == kMaxVisibleSeconds);
    CHECK(s.is_valid());
}

TEST_CASE("NaN and infinity fall back to the default or the nearest end") {
    LookSettings s;
    s.style_intensity = std::numeric_limits<float>::quiet_NaN();
    s.visible_seconds = std::numeric_limits<float>::infinity();
    s.keyboard_height = -std::numeric_limits<float>::infinity();
    CHECK_FALSE(s.is_valid());
    const LookSettings c = s.clamped();
    CHECK(c.is_valid());
    CHECK(c.style_intensity == LookSettings{}.style_intensity);
    CHECK(c.visible_seconds == kMaxVisibleSeconds);
    CHECK(c.keyboard_height == kMinKeyboardHeight);
}

TEST_CASE("a stray enum value is reset to its default") {
    LookSettings s;
    s.note_style = static_cast<NoteStyle>(200);
    s.color_scheme = static_cast<ColorScheme>(99);
    s.name_mode = static_cast<NameMode>(9);
    s.background = static_cast<BackgroundStyle>(77);
    s.octave_numbering = static_cast<OctaveNumbering>(5);
    CHECK_FALSE(s.is_valid());
    const LookSettings c = s.clamped();
    CHECK(c.is_valid());
    CHECK(c.note_style == LookSettings{}.note_style);
    CHECK(c.color_scheme == LookSettings{}.color_scheme);
    CHECK(c.name_mode == LookSettings{}.name_mode);
    CHECK(c.background == LookSettings{}.background);
    CHECK(c.octave_numbering == LookSettings{}.octave_numbering);
}

TEST_CASE("reduced effects forces motion and particles off, and only those") {
    LookSettings s;
    s.background = BackgroundStyle::Motion;
    s.sparks = true;
    CHECK(s.effective_background() == BackgroundStyle::Motion);
    CHECK(s.effective_sparks());

    s.reduced_effects = true;
    CHECK(s.effective_background() == BackgroundStyle::Gradient);
    CHECK_FALSE(s.effective_sparks());

    s.background = BackgroundStyle::Plain;
    CHECK(s.effective_background() == BackgroundStyle::Plain);
    s.background = BackgroundStyle::Gradient;
    CHECK(s.effective_background() == BackgroundStyle::Gradient);

    // The stored choice is not touched, so switching the flag off brings it back.
    s.background = BackgroundStyle::Motion;
    s.reduced_effects = false;
    CHECK(s.effective_background() == BackgroundStyle::Motion);
}

TEST_CASE("sparks stay off when the option itself is off") {
    LookSettings s;
    s.sparks = false;
    CHECK_FALSE(s.effective_sparks());
}

TEST_CASE("every preset is valid, named, and different from the others") {
    std::set<std::string> names;
    std::vector<std::string> texts;
    for (PresetId id : kAllPresets) {
        const LookSettings p = preset(id);
        CHECK(p.is_valid());
        CHECK_FALSE(preset_name(id).empty());
        names.insert(std::string(preset_name(id)));
        texts.push_back(serialize(p));
    }
    CHECK(names.size() == kAllPresets.size());
    std::set<std::string> distinct(texts.begin(), texts.end());
    CHECK(distinct.size() == kAllPresets.size());
}

TEST_CASE("the Default preset is the default settings") {
    CHECK(preset(PresetId::Default) == LookSettings{});
}

TEST_CASE("Classic is plain, filled and by hand") {
    const LookSettings p = preset(PresetId::Classic);
    CHECK(p.note_style == NoteStyle::Filled);
    CHECK(p.color_scheme == ColorScheme::ByHand);
    CHECK(p.background == BackgroundStyle::Plain);
    CHECK_FALSE(p.sparks);
    CHECK_FALSE(p.held_glow);
}

TEST_CASE("the other presets set what their names say") {
    CHECK(preset(PresetId::NeonNight).note_style == NoteStyle::Neon);
    CHECK(preset(PresetId::NeonNight).color_scheme == ColorScheme::ByPitchClass);
    CHECK(preset(PresetId::Glass).note_style == NoteStyle::Glassy);
    CHECK(preset(PresetId::FrostedPastel).note_style == NoteStyle::Frosted);
    CHECK(preset(PresetId::HighContrast).high_contrast);
    CHECK(preset(PresetId::HighContrast).note_style == NoteStyle::Filled);
    const LookSettings m = preset(PresetId::MinimalReduced);
    CHECK(m.reduced_effects);
    CHECK_FALSE(m.effective_sparks());
    CHECK(m.effective_background() != BackgroundStyle::Motion);
}

TEST_CASE("no preset asks for more than reduced effects allow when it sets the switch") {
    for (PresetId id : kAllPresets) {
        const LookSettings p = preset(id);
        if (p.reduced_effects) {
            CHECK(p.effective_background() != BackgroundStyle::Motion);
            CHECK_FALSE(p.effective_sparks());
        }
    }
}

TEST_CASE("a built-in preset keeps the octave numbering, key size and typing letters") {
    LookSettings mine;
    mine.octave_numbering = OctaveNumbering::Yamaha;
    mine.key_size = KeySize::Large;
    mine.show_typing_keys = true;
    mine.note_style = NoteStyle::Filled;
    for (const PresetId id : kAllPresets) {
        const LookSettings out = apply_preset(mine, id);
        CHECK(out.octave_numbering == OctaveNumbering::Yamaha);
        CHECK(out.key_size == KeySize::Large);
        CHECK(out.show_typing_keys);
        CHECK(out.note_style == preset(id).note_style);
        CHECK(out.color_scheme == preset(id).color_scheme);
        CHECK(is_preset(out, id));
    }
    // Any other change makes it not the preset any more.
    LookSettings changed = apply_preset(mine, PresetId::Default);
    changed.sparks = !changed.sparks;
    CHECK_FALSE(is_preset(changed, PresetId::Default));
    CHECK(is_preset(LookSettings{}, PresetId::Default));
}
