// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string_view>

#include "toonval/color.hpp"
#include "toonval/effects_governor.hpp"
#include "toonval/hand.hpp"
#include "toonval/look.hpp"

namespace toonval {

// How each note style is painted, as numbers the renderer reads. One recipe
// paints a note block and its lit key alike, so the two always match. Black-key notes are darker
// through `note_color` and narrower through the renderer's insets, in every style.
//
// Layers, bottom to top: glow (additive, around the block), body (a vertical gradient), inner
// shade and inner glow (fading in from the edge), the style extras (gloss band, bottom shade,
// specular streak, frost grain) and the rim. Lengths ending in `_px` are 1080p pixels; widths
// ending in `_share` are a share of the block's width; alphas are 0..1.
struct StyleRecipe {
    // Body. The far end (top of a rising block) and the key end each have an opacity. The colour is
    // first saturated by `saturation` (1 keeps it), mixed toward black by `core_darken`, then the
    // far end is scaled by `top_shade` and mixed toward white by `top_lighten`.
    float body_alpha_top = 1.0f;
    float body_alpha_bottom = 1.0f;
    float saturation = 1.0f;
    float core_darken = 0.0f;
    float top_shade = 1.0f;
    float top_lighten = 0.0f;
    float edge_feather_px = 1.2f; // soft edge outside the body

    // Fading in from the edge: a dark shade (depth) and a glow in the note's colour lightened.
    float inner_shade_alpha = 0.0f;
    float inner_glow_alpha = 0.0f;
    float inner_width_share = 0.3f;

    // Extras, dropped at EffectsLevel::NoStyleExtras.
    float gloss_alpha = 0.0f;        // shiny: bright band down the left part, brightest at the top
    float bottom_shade_alpha = 0.0f; // shiny: darker toward the key end
    float specular_alpha = 0.0f;     // glassy: thin bright streak near the left edge
    float specular_length = 0.85f;   // share of the block's height the streak runs down
    float noise_alpha = 0.0f;        // frosted: grain texture
    float rim_alpha = 0.0f;          // outline in the note's colour mixed toward white
    float rim_width_px = 0.0f;
    float rim_lighten = 0.0f;
    bool rim_is_extra = true; // ghost and neon keep their rim at every level: it is their body

    // Glow around the block, as a share of a white key's width, and its strength when the note is
    // neither held nor just pressed (held glow and the press flash add to it).
    float glow_alpha = 0.0f;
    float glow_width_share = 0.45f;
};

// The recipe for `style` at `intensity` (0..1, clamped; NaN counts as 0). Every strength in the
// recipe grows with the intensity, never shrinks.
StyleRecipe style_recipe(NoteStyle style, float intensity);
inline StyleRecipe style_recipe(const LookSettings& look) {
    return style_recipe(look.note_style, look.style_intensity);
}

// The recipe as drawn at an effects level: the glow narrower, the extras off. The body, inner
// shade and glow, and a rim that is the style's body (ghost, neon) are never touched, so notes and
// keys look the same at every level apart from the extras.
StyleRecipe degraded(StyleRecipe recipe, const EffectsBudget& budget);

// The colour a block's key end shows over `background`, where its note name sits: the body
// (saturated, darkened) blended at `body_alpha_bottom`, plus the inner glow.
RGBA style_surface_color(const StyleRecipe& recipe, RGBA note, RGBA background);

// Black or white for a note name on a block in the look's style: whichever reads better on
// `style_surface_color` over `kBackgroundColor`. At least `kLabelMinContrast`.
RGBA style_label_color(const LookSettings& look, int note, std::uint8_t velocity, Hand hand);

// A lit key under a see-through style (body less than opaque) is painted over the background
// mixed this far toward the note colour, so it shows the style as its block does and still reads
// as lit.
inline constexpr float kKeyUnderlayTint = 0.4f;
bool style_is_see_through(const StyleRecipe& recipe);
// The colour under a lit key's body: `kBackgroundColor` tinted toward `note` for a see-through
// style, the note itself (unused) for an opaque one.
RGBA key_underlay_color(const StyleRecipe& recipe, RGBA note);
// Black or white for a name on a lit key: as style_label_color, on the key's own underlay.
RGBA style_key_label_color(const LookSettings& look, int note, std::uint8_t velocity, Hand hand);

// "filled", "glassy", "frosted", "shiny", "ghost", "neon".
std::string_view note_style_name(NoteStyle style);
// The reverse, for the command line. False, `*out` untouched, for an unknown name.
bool parse_note_style(std::string_view name, NoteStyle* out);
inline constexpr NoteStyle kAllNoteStyles[] = {NoteStyle::Filled, NoteStyle::Glassy,
                                               NoteStyle::Frosted, NoteStyle::Shiny,
                                               NoteStyle::Ghost,  NoteStyle::Neon};

} // namespace toonval
