// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <span>

#include "toonval/look.hpp"
#include "toonval/screen_layout.hpp"

namespace toonval {

// Where Flow Mode's grade words (Perfect, Good, Early, Late, Miss) go in the note area. Pure
// placement: the renderer measures the words, this decides which fit and where, so no two words,
// and no word and a note name or the chord label, ever overlap. It uses fixed-size state only
// (no allocation), so it can run every frame.

// At most this many words are placed; the oldest beyond it are left out.
inline constexpr std::size_t kMaxGradeWords = 24;
// Words stack in this many rows from their edge of the note area (just above the keys for
// `GradePosition::Bottom`, the top of the area below the chord label for `Top`).
inline constexpr int kGradeWordRows = 2;
// Clear space around every word, between rows, and between a word and the area's edge.
inline constexpr float kGradeWordGapPx = 3.0f;
// A word fades in over this much of its age (0..1) so a word taking over a row does not pop.
inline constexpr float kGradeFadeIn = 0.08f;

// One word to place. `age` runs from 0 when it appears to 1 when it is gone.
struct GradeWordIn {
    float centre_x = 0.0f; // the middle of its key
    float width = 0.0f;    // the measured text width
    float age = 0.0f;
};

struct GradeWordOut {
    bool visible = false; // false: gave way or does not fit; draw nothing
    RectF rect;           // inside the note area; empty when not visible
    int row = 0;          // 0 is the row nearest its edge
};

struct GradeLayoutParams {
    RectF note_area;
    RectF chord_label; // the words stay below it; may be empty
    GradePosition position = GradePosition::Bottom;
    float word_h = 0.0f; // the measured text height, the same for every word
};

// Places `words` (any order) into `out`, which must hold at least `words.size()` entries (at most
// `kMaxGradeWords` are placed; the rest come back not visible). `avoid` is the rects to keep
// clear of, normally the note-name rects. The newest word is placed first, in the nearest free
// row; when every row is taken at its x, the older word gives way and is left out, so it
// disappears early instead of overlapping. A word wider than the area, or a band too small for
// even one row, is not shown. Returns how many words are visible.
std::size_t place_grade_words(const GradeLayoutParams& params, std::span<const RectF> avoid,
                              std::span<const GradeWordIn> words, std::span<GradeWordOut> out);

// The rectangle all rows of `params` lie in (empty when not even one row fits). Always inside the
// note area and, for `Top`, below the chord label.
RectF grade_band(const GradeLayoutParams& params);

// Opacity of a word at `age` before the look's own opacity: a short fade in, then fading out
// to nothing at age 1. 0 outside 0..1.
float grade_word_alpha(float age);

} // namespace toonval
