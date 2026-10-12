// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/screen_layout.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>

#include <doctest/doctest.h>

using namespace toonval;

namespace {

// The note area of a 800x400 window: the labels may only appear inside it.
const RectF kClip{0.0f, 0.0f, 800.0f, 400.0f};

bool inside(const RectF& inner, const RectF& outer) {
    return inner.x >= outer.x && inner.y >= outer.y && inner.right() <= outer.right() &&
           inner.bottom() <= outer.bottom();
}

RectF intersect(const RectF& a, const RectF& b) {
    const float x0 = std::max(a.x, b.x);
    const float y0 = std::max(a.y, b.y);
    const float x1 = std::min(a.right(), b.right());
    const float y1 = std::min(a.bottom(), b.bottom());
    return RectF{x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)};
}

float label_width(float font, int chars) {
    return static_cast<float>(chars) * kLabelCharWidthEm * font;
}
float label_height(float font) { return font * kBlockLabelLineHeightEm; }

} // namespace

TEST_CASE("a tall block carries its label at the bottom edge, centered, with padding") {
    const RectF block{100.0f, 50.0f, 40.0f, 300.0f}; // bottom at 350
    const LabelPlacement p = place_block_label(block, kClip, 14.0f, 3);
    REQUIRE(p.visible);
    CHECK(p.font_px == 14.0f);
    CHECK(p.rect.w == doctest::Approx(label_width(14.0f, 3)));
    CHECK(p.rect.h == doctest::Approx(label_height(14.0f)));
    CHECK(p.rect.bottom() == doctest::Approx(350.0f - kLabelPaddingPx));
    CHECK(p.rect.x + p.rect.w / 2.0f == doctest::Approx(120.0f)); // block center
    CHECK(inside(p.rect, block));
}

TEST_CASE("a block just tall enough shows its label, one pixel shorter hides it") {
    const float need = label_height(14.0f) + 2.0f * kLabelPaddingPx; // 20.8
    const RectF fits{100.0f, 300.0f - need - 0.01f, 40.0f, need + 0.02f};
    const LabelPlacement ok = place_block_label(fits, kClip, 14.0f, 2);
    CHECK(ok.visible);
    CHECK(inside(ok.rect, fits));

    const RectF short_block{100.0f, 300.0f - need + 1.0f, 40.0f, need - 1.0f};
    const LabelPlacement hidden = place_block_label(short_block, kClip, 14.0f, 2);
    CHECK_FALSE(hidden.visible); // hidden, never drawn clipped
    CHECK(hidden.font_px == 0.0f);
    CHECK(hidden.rect.empty());
}

TEST_CASE("a thin short block (a staccato note) has no label") {
    CHECK_FALSE(place_block_label(RectF{200.0f, 200.0f, 40.0f, 8.0f}, kClip, 14.0f, 1).visible);
    CHECK_FALSE(place_block_label(RectF{200.0f, 200.0f, 40.0f, 0.0f}, kClip, 14.0f, 1).visible);
}

TEST_CASE("a narrow black-key block shrinks the label, then hides it") {
    const RectF white{100.0f, 100.0f, 30.0f, 200.0f};
    const RectF black{140.0f, 100.0f, 18.0f, 200.0f}; // black keys are 60 percent as wide
    const LabelPlacement w = place_block_label(white, kClip, 14.0f, 2);
    REQUIRE(w.visible);
    CHECK(w.font_px == 14.0f);
    const LabelPlacement b = place_block_label(black, kClip, 14.0f, 2);
    REQUIRE(b.visible);
    CHECK(b.font_px < 14.0f); // shrunk to the width
    CHECK(inside(b.rect, black));
    CHECK(b.rect.w + 2.0f * kLabelPaddingPx <= black.w);
    const RectF too_narrow{140.0f, 100.0f, 10.0f, 200.0f};
    CHECK_FALSE(place_block_label(too_narrow, kClip, 14.0f, 2).visible);
    // A one-letter name still fits where "C#" does not.
    CHECK(place_block_label(RectF{140.0f, 100.0f, 14.0f, 200.0f}, kClip, 14.0f, 1).visible);
    CHECK_FALSE(place_block_label(RectF{140.0f, 100.0f, 14.0f, 200.0f}, kClip, 14.0f, 2).visible);
}

TEST_CASE("a block running below the keyboard edge keeps its label in the visible part") {
    // The note area ends at y = 400 (the keyboard starts there); the block goes on to 600.
    const RectF block{100.0f, 340.0f, 40.0f, 260.0f};
    const LabelPlacement p = place_block_label(block, kClip, 14.0f, 2);
    REQUIRE(p.visible);
    CHECK(p.rect.bottom() == doctest::Approx(400.0f - kLabelPaddingPx));
    CHECK(inside(p.rect, intersect(block, kClip)));
}

TEST_CASE("a block mostly below the keyboard edge hides its label instead of clipping it") {
    // Only 12 px of the block are above the edge.
    const RectF block{100.0f, 388.0f, 40.0f, 200.0f};
    CHECK_FALSE(place_block_label(block, kClip, 14.0f, 2).visible);
    // Entirely below.
    CHECK_FALSE(place_block_label(RectF{100.0f, 450.0f, 40.0f, 200.0f}, kClip, 14.0f, 2).visible);
}

TEST_CASE("a block running above the top keeps its label in the visible part") {
    // The block starts 200 px above the screen and ends at y = 120.
    const RectF block{100.0f, -200.0f, 40.0f, 320.0f};
    const LabelPlacement p = place_block_label(block, kClip, 14.0f, 2);
    REQUIRE(p.visible);
    CHECK(p.rect.bottom() == doctest::Approx(120.0f - kLabelPaddingPx));
    CHECK(p.rect.y >= 0.0f);
    CHECK(inside(p.rect, intersect(block, kClip)));
}

TEST_CASE("a block that has only just entered at the top, or is almost gone, has no label") {
    // Bottom edge 10 px below the top: nothing of the label could be seen.
    CHECK_FALSE(place_block_label(RectF{100.0f, -300.0f, 40.0f, 310.0f}, kClip, 14.0f, 2).visible);
    // Entirely above the screen.
    CHECK_FALSE(place_block_label(RectF{100.0f, -300.0f, 40.0f, 200.0f}, kClip, 14.0f, 2).visible);
}

TEST_CASE("a block partly off the side keeps the label inside the visible part") {
    const RectF block{-10.0f, 100.0f, 50.0f, 200.0f}; // visible x range is 0..40
    const LabelPlacement p = place_block_label(block, kClip, 14.0f, 2);
    REQUIRE(p.visible);
    CHECK(inside(p.rect, intersect(block, kClip)));
}

TEST_CASE("bad input hides the label") {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const RectF ok{100.0f, 100.0f, 40.0f, 200.0f};
    CHECK_FALSE(place_block_label(ok, kClip, 14.0f, 0).visible);
    CHECK_FALSE(place_block_label(ok, kClip, 0.0f, 2).visible);
    CHECK_FALSE(place_block_label(ok, kClip, nan, 2).visible);
    CHECK_FALSE(place_block_label(RectF{nan, 100.0f, 40.0f, 200.0f}, kClip, 14.0f, 2).visible);
    CHECK_FALSE(place_block_label(RectF{100.0f, 100.0f, nan, 200.0f}, kClip, 14.0f, 2).visible);
    CHECK_FALSE(place_block_label(ok, RectF{}, 14.0f, 2).visible);
}

TEST_CASE("for any block, a shown label lies inside the visible part with padding at the bottom") {
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> pos(-300.0f, 700.0f);
    std::uniform_real_distribution<float> size(0.0f, 400.0f);
    int shown = 0;
    for (int i = 0; i < 20000; ++i) {
        const RectF block{pos(rng), pos(rng), size(rng) / 4.0f, size(rng)};
        const int chars = 1 + static_cast<int>(rng() % 4);
        const LabelPlacement p = place_block_label(block, kClip, 16.0f, chars);
        if (!p.visible) {
            CHECK(p.rect.empty());
            CHECK(p.font_px == 0.0f);
            continue;
        }
        ++shown;
        const RectF vis = intersect(block, kClip);
        CHECK(p.font_px >= kMinLabelFontPx);
        CHECK(p.font_px <= 16.0f);
        CHECK(inside(p.rect, vis));
        CHECK(p.rect.bottom() == doctest::Approx(vis.bottom() - kLabelPaddingPx));
        CHECK(p.rect.y >= vis.y + kLabelPaddingPx - 1e-3f);
        CHECK(p.rect.x >= vis.x + kLabelPaddingPx - 1e-3f);
        CHECK(p.rect.right() <= vis.right() - kLabelPaddingPx + 1e-3f);
    }
    CHECK(shown > 1000); // the sweep really exercises the visible case
}
