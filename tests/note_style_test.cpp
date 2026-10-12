// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/note_style.hpp"

#include <doctest/doctest.h>

#include <cmath>

using namespace toonval;

namespace {

// Every number in a recipe that measures how strong something is.
float strength_sum(const StyleRecipe& r) {
    return r.inner_shade_alpha + r.inner_glow_alpha + r.gloss_alpha + r.bottom_shade_alpha +
           r.specular_alpha + r.noise_alpha + r.rim_alpha + r.rim_width_px + r.glow_alpha +
           r.glow_width_share + std::abs(1.0f - r.top_shade) + r.top_lighten +
           std::abs(1.0f - r.saturation);
}

} // namespace

TEST_CASE("names round-trip and unknown names are refused") {
    for (NoteStyle s : kAllNoteStyles) {
        NoteStyle back = NoteStyle::Filled;
        CHECK(parse_note_style(note_style_name(s), &back));
        CHECK(back == s);
    }
    NoteStyle untouched = NoteStyle::Ghost;
    CHECK_FALSE(parse_note_style("sparkly", &untouched));
    CHECK(untouched == NoteStyle::Ghost);
}

TEST_CASE("each style has its own look: opacity, grain, gloss, rim and glow") {
    const StyleRecipe filled = style_recipe(NoteStyle::Filled, 0.6f);
    CHECK(filled.body_alpha_top == 1.0f);
    CHECK(filled.body_alpha_bottom == 1.0f);
    CHECK(filled.noise_alpha == 0.0f);
    CHECK(filled.specular_alpha == 0.0f);

    const StyleRecipe glassy = style_recipe(NoteStyle::Glassy, 0.6f);
    CHECK(glassy.body_alpha_top < glassy.body_alpha_bottom); // a gradient you can see through
    CHECK(glassy.body_alpha_bottom < 1.0f);
    CHECK(glassy.specular_alpha > 0.3f);
    CHECK(glassy.rim_alpha > 0.0f);

    const StyleRecipe frosted = style_recipe(NoteStyle::Frosted, 0.6f);
    CHECK(frosted.noise_alpha > 0.0f);
    CHECK(frosted.edge_feather_px > filled.edge_feather_px);
    CHECK(frosted.body_alpha_bottom < 1.0f);

    const StyleRecipe shiny = style_recipe(NoteStyle::Shiny, 0.6f);
    CHECK(shiny.saturation > 1.0f);
    CHECK(shiny.gloss_alpha > 0.3f);
    CHECK(shiny.bottom_shade_alpha > 0.0f);

    const StyleRecipe ghost = style_recipe(NoteStyle::Ghost, 0.6f);
    CHECK(ghost.body_alpha_bottom < 0.3f);
    CHECK(ghost.rim_alpha > 0.7f);
    CHECK(ghost.inner_glow_alpha > 0.0f);

    const StyleRecipe neon = style_recipe(NoteStyle::Neon, 0.6f);
    CHECK(neon.core_darken > 0.5f);
    CHECK(neon.rim_alpha == 1.0f);
    CHECK(neon.glow_alpha > glassy.glow_alpha);
    CHECK(neon.glow_width_share > 1.0f);
}

TEST_CASE("intensity only ever strengthens a style, and is clamped") {
    for (NoteStyle s : kAllNoteStyles) {
        CAPTURE(note_style_name(s));
        float last = -1.0f;
        for (float k : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
            const float sum = strength_sum(style_recipe(s, k));
            CHECK(sum >= last);
            last = sum;
        }
        CHECK(strength_sum(style_recipe(s, 0.0f)) < strength_sum(style_recipe(s, 1.0f)));
        CHECK(strength_sum(style_recipe(s, -3.0f)) == strength_sum(style_recipe(s, 0.0f)));
        CHECK(strength_sum(style_recipe(s, 7.0f)) == strength_sum(style_recipe(s, 1.0f)));
        CHECK(strength_sum(style_recipe(s, std::nanf(""))) ==
              strength_sum(style_recipe(s, 0.0f)));
    }
}

TEST_CASE("degrading never touches the body: notes and keys stay drawn at every level") {
    for (NoteStyle s : kAllNoteStyles) {
        CAPTURE(note_style_name(s));
        const StyleRecipe r = style_recipe(s, 0.8f);
        for (EffectsLevel level : kAllEffectsLevels) {
            CAPTURE(effects_level_name(level));
            const StyleRecipe d = degraded(r, effects_budget(level));
            CHECK(d.body_alpha_top == r.body_alpha_top);
            CHECK(d.body_alpha_bottom == r.body_alpha_bottom);
            CHECK(d.saturation == r.saturation);
            CHECK(d.core_darken == r.core_darken);
            CHECK(d.inner_glow_alpha == r.inner_glow_alpha);
            CHECK(d.inner_shade_alpha == r.inner_shade_alpha);
            CHECK(d.glow_alpha == r.glow_alpha);
            if (!r.rim_is_extra) {
                CHECK(d.rim_alpha == r.rim_alpha);
            }
            CHECK(d.body_alpha_bottom > 0.0f);
        }
    }
}

TEST_CASE("the extras go only at the last level, after the glow narrows") {
    const StyleRecipe r = style_recipe(NoteStyle::Glassy, 1.0f);
    CHECK(degraded(r, effects_budget(EffectsLevel::NoMotion)).specular_alpha == r.specular_alpha);
    CHECK(degraded(r, effects_budget(EffectsLevel::NoMotion)).glow_width_share ==
          r.glow_width_share);
    const StyleRecipe narrow = degraded(r, effects_budget(EffectsLevel::NarrowGlow));
    CHECK(narrow.glow_width_share < r.glow_width_share);
    CHECK(narrow.specular_alpha == r.specular_alpha);
    const StyleRecipe floor = degraded(r, effects_budget(EffectsLevel::NoStyleExtras));
    CHECK(floor.specular_alpha == 0.0f);
    CHECK(floor.rim_alpha == 0.0f);
    CHECK(degraded(style_recipe(NoteStyle::Frosted, 1.0f),
                   effects_budget(EffectsLevel::NoStyleExtras))
              .noise_alpha == 0.0f);
    // Ghost and neon keep their outline: it is what the note is.
    CHECK(degraded(style_recipe(NoteStyle::Neon, 1.0f), effects_budget(kLowestEffectsLevel))
              .rim_alpha > 0.0f);
    CHECK(degraded(style_recipe(NoteStyle::Ghost, 1.0f), effects_budget(kLowestEffectsLevel))
              .rim_alpha > 0.0f);
}

TEST_CASE("names stay readable on every style, scheme and preset") {
    for (PresetId id : kAllPresets) {
        for (NoteStyle s : kAllNoteStyles) {
            for (float k : {0.0f, 0.6f, 1.0f}) {
                LookSettings look = preset(id);
                look.note_style = s;
                look.style_intensity = k;
                for (int note = 21; note <= 108; ++note) {
                    for (std::uint8_t v : {std::uint8_t{20}, std::uint8_t{100}}) {
                        const RGBA surface = style_surface_color(
                            style_recipe(look), note_color(look, note, v, Hand::Right),
                            kBackgroundColor);
                        const RGBA label = style_label_color(look, note, v, Hand::Right);
                        CHECK(contrast_ratio(label, surface) >= kLabelMinContrast);
                        // A lit key: the same style over its tinted underlay.
                        const RGBA c = note_color(look, note, v, Hand::Right);
                        const RGBA key_surface = style_surface_color(
                            style_recipe(look), c, key_underlay_color(style_recipe(look), c));
                        CHECK(contrast_ratio(style_key_label_color(look, note, v, Hand::Right),
                                             key_surface) >= kLabelMinContrast);
                    }
                }
            }
        }
    }
}

TEST_CASE("translucent styles take light names on the dark background") {
    LookSettings look;
    look.right_color = {255, 220, 120, 255}; // a light color that takes black text when solid
    look.note_style = NoteStyle::Filled;
    CHECK(style_label_color(look, 64, 100, Hand::Right) == RGBA{0, 0, 0, 255});
    look.note_style = NoteStyle::Ghost;
    CHECK(style_label_color(look, 64, 100, Hand::Right) == RGBA{255, 255, 255, 255});
    look.note_style = NoteStyle::Neon;
    CHECK(style_label_color(look, 64, 100, Hand::Right) == RGBA{255, 255, 255, 255});
}

TEST_CASE("black-key notes are darker than white-key notes in every style") {
    LookSettings look;
    for (NoteStyle s : kAllNoteStyles) {
        look.note_style = s;
        const StyleRecipe r = style_recipe(look);
        const RGBA white_key = style_surface_color(r, note_color(look, 60, 100, Hand::Right),
                                                   kBackgroundColor);
        const RGBA black_key = style_surface_color(r, note_color(look, 61, 100, Hand::Right),
                                                   kBackgroundColor);
        CHECK(relative_luminance(black_key) < relative_luminance(white_key));
    }
}

TEST_CASE("a lit key under a see-through style sits on a tinted underlay") {
    const RGBA blue{90, 170, 255, 255};
    CHECK_FALSE(style_is_see_through(style_recipe(NoteStyle::Filled, 0.6f)));
    CHECK_FALSE(style_is_see_through(style_recipe(NoteStyle::Shiny, 0.6f)));
    for (NoteStyle s : {NoteStyle::Glassy, NoteStyle::Frosted, NoteStyle::Ghost, NoteStyle::Neon}) {
        const StyleRecipe r = style_recipe(s, 0.6f);
        CHECK(style_is_see_through(r));
        const RGBA under = key_underlay_color(r, blue);
        CHECK(perceptual_distance(under, kBackgroundColor) > 0.05);
        CHECK(relative_luminance(under) < relative_luminance(blue));
    }
}
