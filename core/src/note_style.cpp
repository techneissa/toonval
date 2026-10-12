// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/note_style.hpp"

#include <algorithm>
#include <cmath>

namespace toonval {
namespace {

float mixf(float a, float b, float t) { return a + (b - a) * t; }

std::uint8_t to_byte(float v) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 255.0f)));
}

// Pushes each channel away from (s > 1) or toward (s < 1) the color's grey.
RGBA saturate(RGBA c, float s) {
    const float grey = 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
    return {to_byte(grey + (c.r - grey) * s), to_byte(grey + (c.g - grey) * s),
            to_byte(grey + (c.b - grey) * s), c.a};
}

RGBA over(RGBA fg, float alpha, RGBA bg) {
    alpha = std::clamp(alpha, 0.0f, 1.0f);
    return {to_byte(mixf(bg.r, fg.r, alpha)), to_byte(mixf(bg.g, fg.g, alpha)),
            to_byte(mixf(bg.b, fg.b, alpha)), 255};
}

} // namespace

StyleRecipe style_recipe(NoteStyle style, float intensity) {
    const float k = std::isnan(intensity) ? 0.0f : std::clamp(intensity, 0.0f, 1.0f);
    StyleRecipe r;
    switch (style) {
    case NoteStyle::Filled:
        // Solid and crisp: a gentle gradient, a hairline edge, a faint glow.
        r.top_shade = 1.0f - 0.28f * k;
        r.edge_feather_px = 1.0f;
        r.glow_alpha = 0.06f + 0.08f * k;
        break;
    case NoteStyle::Glassy:
        // See-through body with a gradient, a bright streak near the left edge, a thin light rim.
        r.body_alpha_top = 0.30f + 0.15f * k;
        r.body_alpha_bottom = 0.62f + 0.18f * k;
        r.saturation = 1.05f + 0.1f * k;
        r.top_shade = 0.8f;
        r.inner_shade_alpha = 0.06f + 0.08f * k;
        r.inner_width_share = 0.22f;
        r.specular_alpha = 0.35f + 0.5f * k;
        r.rim_alpha = 0.40f + 0.45f * k;
        r.rim_width_px = 1.3f + 0.4f * k;
        r.rim_lighten = 0.6f;
        r.glow_alpha = 0.10f + 0.08f * k;
        break;
    case NoteStyle::Frosted:
        // Soft, milky body with grain, a soft edge and a muted highlight at the top.
        r.body_alpha_top = 0.62f + 0.12f * k;
        r.body_alpha_bottom = 0.74f + 0.14f * k;
        r.saturation = 0.85f - 0.1f * k;
        r.top_lighten = 0.14f + 0.14f * k;
        r.edge_feather_px = 2.5f + 2.5f * k;
        r.inner_glow_alpha = 0.10f + 0.14f * k;
        r.inner_width_share = 0.35f;
        r.noise_alpha = 0.16f + 0.2f * k;
        r.rim_alpha = 0.16f + 0.16f * k;
        r.rim_width_px = 1.5f;
        r.rim_lighten = 0.85f;
        r.glow_alpha = 0.08f + 0.06f * k;
        break;
    case NoteStyle::Shiny:
        // Candy: saturated, a strong gloss band, a soft shade toward the key end.
        r.saturation = 1.15f + 0.3f * k;
        r.top_lighten = 0.10f * k;
        r.inner_shade_alpha = 0.16f + 0.16f * k;
        r.inner_width_share = 0.3f;
        r.gloss_alpha = 0.30f + 0.4f * k;
        r.bottom_shade_alpha = 0.18f + 0.24f * k;
        r.specular_alpha = 0.2f + 0.4f * k;
        r.specular_length = 0.35f;
        r.glow_alpha = 0.12f + 0.10f * k;
        break;
    case NoteStyle::Ghost:
        // Nearly see-through, a luminous outline and a faint glow inside it.
        r.body_alpha_top = 0.05f + 0.05f * k;
        r.body_alpha_bottom = 0.12f + 0.12f * k;
        r.inner_glow_alpha = 0.16f + 0.22f * k;
        r.inner_width_share = 0.2f;
        r.rim_alpha = 0.8f + 0.2f * k;
        r.rim_width_px = 1.6f + 1.2f * k;
        r.rim_lighten = 0.45f;
        r.rim_is_extra = false;
        r.edge_feather_px = 1.5f;
        r.glow_alpha = 0.08f + 0.12f * k;
        r.glow_width_share = 0.55f;
        break;
    case NoteStyle::Neon:
        // A dark core, a bright saturated tube around it and a wide bloom.
        r.body_alpha_top = 0.88f;
        r.body_alpha_bottom = 0.92f;
        r.saturation = 1.25f + 0.25f * k;
        r.core_darken = 0.72f - 0.1f * k;
        r.inner_glow_alpha = 0.30f + 0.3f * k;
        r.inner_width_share = 0.32f;
        r.rim_alpha = 1.0f;
        r.rim_width_px = 2.0f + 1.4f * k;
        r.rim_lighten = 0.2f + 0.15f * k;
        r.rim_is_extra = false;
        r.edge_feather_px = 1.5f;
        r.glow_alpha = 0.30f + 0.40f * k;
        r.glow_width_share = 0.9f + 0.8f * k;
        break;
    }
    return r;
}

StyleRecipe degraded(StyleRecipe r, const EffectsBudget& budget) {
    r.glow_width_share *= std::clamp(budget.glow_width, 0.0f, 1.0f);
    if (!budget.style_extras) {
        r.gloss_alpha = 0.0f;
        r.bottom_shade_alpha = 0.0f;
        r.specular_alpha = 0.0f;
        r.noise_alpha = 0.0f;
        if (r.rim_is_extra) {
            r.rim_alpha = 0.0f;
        }
    }
    return r;
}

RGBA style_surface_color(const StyleRecipe& r, RGBA note, RGBA background) {
    const RGBA body = darken(saturate(note, r.saturation), r.core_darken);
    RGBA c = over(body, r.body_alpha_bottom, background);
    // The name sits near the key end, inside the inner glow's reach: count half of it.
    c = over(lighten(note, 0.35f), 0.5f * r.inner_glow_alpha, c);
    return c;
}

RGBA style_label_color(const LookSettings& look, int note, std::uint8_t velocity, Hand hand) {
    const RGBA surface =
        style_surface_color(style_recipe(look), note_color(look, note, velocity, hand),
                            kBackgroundColor);
    return label_color_for(surface);
}

bool style_is_see_through(const StyleRecipe& r) {
    return std::min(r.body_alpha_top, r.body_alpha_bottom) < 0.99f;
}

RGBA key_underlay_color(const StyleRecipe& r, RGBA note) {
    if (!style_is_see_through(r)) {
        return note;
    }
    return lerp(kBackgroundColor, RGBA{note.r, note.g, note.b, 255}, kKeyUnderlayTint);
}

RGBA style_key_label_color(const LookSettings& look, int note, std::uint8_t velocity, Hand hand) {
    const StyleRecipe r = style_recipe(look);
    const RGBA c = note_color(look, note, velocity, hand);
    return label_color_for(style_surface_color(r, c, key_underlay_color(r, c)));
}

std::string_view note_style_name(NoteStyle style) {
    switch (style) {
    case NoteStyle::Filled:
        return "filled";
    case NoteStyle::Glassy:
        return "glassy";
    case NoteStyle::Frosted:
        return "frosted";
    case NoteStyle::Shiny:
        return "shiny";
    case NoteStyle::Ghost:
        return "ghost";
    case NoteStyle::Neon:
        return "neon";
    }
    return "filled";
}

bool parse_note_style(std::string_view name, NoteStyle* out) {
    for (NoteStyle s : kAllNoteStyles) {
        if (note_style_name(s) == name) {
            *out = s;
            return true;
        }
    }
    return false;
}

} // namespace toonval
