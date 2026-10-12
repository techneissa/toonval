// SPDX-License-Identifier: GPL-3.0-or-later
// Render-only constants of the free-play renderer: geometry shares, key and background colours,
// text colours. Note colours, names, layout sizes, the flash/glow/fade switches and how many
// seconds fit on screen all come from toonval::LookSettings (core/include/toonval/look.hpp), so
// the settings panel and presets drive them. What is here has no setting yet.
#pragma once

#include "mesh.hpp"
#include "toonval/color.hpp"
#include "toonval/hand.hpp"

#include <SDL3/SDL.h>

#include <cstdint>

namespace toonval::app {

// Free play has no song, so no note belongs to a hand. Its notes use the right-hand colour of the
// look: the default scheme gives the accent blue the first renderer drew, where Hand::Unknown
// would give a grey. The other schemes (pitch class, octave, velocity, single) ignore the hand.
inline constexpr Hand kFreePlayHand = Hand::Right;

// `RGBA` from the core look as an SDL colour.
inline SDL_FColor to_fcolor(RGBA c) {
    return {static_cast<float>(c.r) / 255.0f, static_cast<float>(c.g) / 255.0f,
            static_cast<float>(c.b) / 255.0f, static_cast<float>(c.a) / 255.0f};
}

// Smallest text drawn anywhere that is not a note label, in real pixels.
inline constexpr float kMinTextPx = 14.0f;
// Most note names drawn on blocks in one frame; the blocks nearest the keyboard come first.
inline constexpr int kMaxBlockLabelsPerFrame = 48;

struct Look {
    // Rising notes.
    float block_radius = 0.24f;     // corner radius as a share of the block width
    float block_radius_max = 9.0f;  // 1080p px
    float min_block_height = 10.0f; // 1080p px, so a staccato tap still shows
    float block_gap = 2.0f;         // 1080p px between repeated notes on one key
    float white_note_inset = 0.10f; // share of the key width left free on each side
    float black_note_inset = 0.06f;
    float held_highlight = 0.32f; // how far a held note's key end (and its key) blends to white
    float fade_start = 0.62f;     // released blocks fade out above this share of the note area
    float top_fade = 0.10f;       // share of the note area at its top where every block dissolves
    float glow_size = 0.45f;      // share of a white key's width
    float glow_strength = 0.14f;
    float held_glow_strength = 0.26f;

    // Press flash, on the key and on its new block alike.
    std::int64_t flash_ns = 160'000'000; // brighter for this long after each note-on
    float flash_whiten = 0.45f;          // how far a fresh press blends toward white
    float flash_glow_strength = 0.55f;

    // Keys.
    SDL_FColor key_white_top = rgba(226, 227, 224);
    SDL_FColor key_white_bottom = rgba(250, 250, 247);
    SDL_FColor key_black_top = rgba(44, 45, 52);
    SDL_FColor key_black_bottom = rgba(12, 12, 16);
    SDL_FColor key_black_lip = rgba(70, 72, 82);
    float black_length = 0.62f; // black keys' share of the white keys' length
    // A pressed key is tipped down: a darker band at its top, as tall as the bed's shadow.
    float pressed_band_darken = 0.55f;
    float pressed_band_alpha = 0.7f;

    // The key bed above the keys, with a felt line along its lower edge and the shadow it casts
    // on the keys. Sizes come from the layout (ScreenLayout::key_bed and friends).
    SDL_FColor bed_top = rgba(34, 37, 48);
    SDL_FColor bed_bottom = rgba(16, 18, 25);
    SDL_FColor felt_line = rgba(84, 22, 30); // deep red felt
    SDL_FColor accent = rgba(90, 180, 255);  // the hit line where notes meet the bed
    float shadow_alpha = 0.55f;

    // Background. The plain background is the gradient's bottom colour. Both are darker than the
    // backgrounds the core palettes are built against, so their contrast only goes up.
    SDL_FColor background_top = rgba(9, 11, 16);
    SDL_FColor background_bottom = rgba(20, 24, 34);
    SDL_FColor guide = rgba(255, 255, 255, 0.055f);       // lane line at every C
    SDL_FColor guide_faint = rgba(255, 255, 255, 0.025f); // and at every F

    // Text.
    SDL_FColor chord_text = rgba(242, 246, 255);
    SDL_FColor chord_backdrop = rgba(6, 8, 12, 0.62f);
    SDL_FColor status_text = rgba(186, 194, 210, 0.9f);
    SDL_FColor prompt_text = rgba(140, 150, 170, 0.85f);
    SDL_FColor key_label_white = rgba(88, 92, 104);   // names on unlit white keys
    SDL_FColor key_label_black = rgba(200, 204, 214); // names on unlit black keys
    // C markers on unlit white keys: middle C bold, fully opaque, with an accent underline; the
    // other Cs about half as strong. A lit key uses the label colour for its note colour instead,
    // at `c_marker_lit_alpha` (middle C at full strength).
    SDL_FColor c_marker_middle = rgba(18, 22, 32);
    SDL_FColor c_marker_other = rgba(60, 64, 76, 0.5f);
    float c_marker_lit_alpha = 0.85f;
    float c_underline_width = 0.8f;   // of the marker's text width
    float c_underline_height = 0.11f; // of the font size
};

} // namespace toonval::app
