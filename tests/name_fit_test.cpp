// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/keyboard.hpp"
#include "toonval/keyboard_view.hpp"
#include "toonval/look.hpp"
#include "toonval/screen_layout.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include <doctest/doctest.h>

using namespace toonval;

namespace {

const RectF kClip{0.0f, 0.0f, 4000.0f, 600.0f};

bool inside(const RectF& inner, const RectF& outer) {
    return inner.x >= outer.x && inner.y >= outer.y && inner.right() <= outer.right() &&
           inner.bottom() <= outer.bottom();
}

// A tall block `w` wide whose lower edge is on the clip's.
RectF tall_block(float w) { return RectF{100.0f, 100.0f, w, 400.0f}; }

// What every drawn name must satisfy: legible, inside the block with its padding.
void check_fits(const NamePlacement& p, const RectF& block, float min_font) {
    REQUIRE(p.form != NameForm::Hidden);
    CHECK(p.label.visible);
    CHECK(p.label.font_px >= min_font);
    CHECK(inside(p.label.rect, block));
    CHECK(p.label.rect.x - block.x >= kLabelPaddingPx - 0.001f);
    CHECK(block.right() - p.label.rect.right() >= kLabelPaddingPx - 0.001f);
    CHECK(block.bottom() - p.label.rect.bottom() >= kLabelPaddingPx - 0.001f);
    CHECK(p.label.rect.y - block.y >= kLabelPaddingPx - 0.001f);
}

} // namespace

TEST_CASE("the minimum name size is 9 px at 96 dpi and grows with the density") {
    CHECK(kMinNameFontPx == 9.0f);
    CHECK(min_name_font_px(kDefaultPxPerMm) == 9.0f);
    CHECK(min_name_font_px(2.0f * kDefaultPxPerMm) == 18.0f);
    CHECK(min_name_font_px(0.0f) == 9.0f);
    CHECK(min_name_font_px(-3.0f) == 9.0f);
    CHECK(min_name_font_px(std::numeric_limits<float>::quiet_NaN()) == 9.0f);
    CHECK(min_name_font_px(1000.0f) < 100.0f); // a silly density is capped
}

TEST_CASE("a block wide enough shows the whole name at a readable size") {
    const RectF block = tall_block(40.0f);
    const NamePlacement p = place_block_name(block, kClip, 14.0f, 2, 1, kMinNameFontPx);
    CHECK(p.form == NameForm::Full);
    CHECK(p.label.font_px == 14.0f);
    check_fits(p, block, kMinNameFontPx);
}

TEST_CASE("a block too narrow for the whole name shows the letter alone") {
    // "C#" needs 2 * 0.62 * 9 + 2 * 2 = 15.2 px at the smallest size; the letter needs 9.6.
    const RectF block = tall_block(13.0f);
    const NamePlacement p = place_block_name(block, kClip, 14.0f, 2, 1, kMinNameFontPx);
    CHECK(p.form == NameForm::Letter);
    check_fits(p, block, kMinNameFontPx);
    // The letter may use a bigger font than the whole name could: it fits the same width.
    CHECK(p.label.font_px > kMinNameFontPx);
}

TEST_CASE("a block too narrow even for the letter shows nothing") {
    const NamePlacement p = place_block_name(tall_block(9.0f), kClip, 14.0f, 2, 1, kMinNameFontPx);
    CHECK(p.form == NameForm::Hidden);
    CHECK_FALSE(p.label.visible);
    CHECK(p.label.rect.empty());
    CHECK(p.label.font_px == 0.0f);
}

TEST_CASE("a name is never drawn smaller than the minimum") {
    // Where the old label rule (9 px) would draw, a larger minimum hides.
    const RectF block = tall_block(15.5f);
    CHECK(place_block_name(block, kClip, 20.0f, 2, 1, 9.0f).form == NameForm::Full);
    const NamePlacement big_min = place_block_name(block, kClip, 20.0f, 2, 1, 12.0f);
    CHECK(big_min.form != NameForm::Full);
    if (big_min.form == NameForm::Letter) {
        CHECK(big_min.label.font_px >= 12.0f);
    }
    // A ceiling under the minimum leaves nothing to draw.
    CHECK(place_block_name(tall_block(80.0f), kClip, 8.0f, 2, 1, 9.0f).form == NameForm::Hidden);
}

TEST_CASE("a short block shows no name, whole or letter") {
    const float need = kMinNameFontPx * kBlockLabelLineHeightEm + 2.0f * kLabelPaddingPx;
    const RectF fits{100.0f, 300.0f - need - 0.5f, 40.0f, need + 1.0f};
    CHECK(place_block_name(fits, kClip, 9.0f, 2, 1, kMinNameFontPx).form == NameForm::Full);
    const RectF short_block{100.0f, 300.0f - need + 2.0f, 40.0f, need - 2.0f};
    CHECK(place_block_name(short_block, kClip, 9.0f, 2, 1, kMinNameFontPx).form ==
          NameForm::Hidden);
    CHECK(place_block_name(RectF{100.0f, 300.0f, 40.0f, 0.0f}, kClip, 14.0f, 2, 1, kMinNameFontPx)
              .form == NameForm::Hidden);
}

TEST_CASE("only the part of a block inside the clip counts") {
    const RectF clip{0.0f, 100.0f, 800.0f, 300.0f}; // the note area, bottom at 400
    // A block running far below the clip: the visible part is 10 px, too short.
    CHECK(place_block_name(RectF{100.0f, 390.0f, 40.0f, 400.0f}, clip, 14.0f, 2, 1, kMinNameFontPx)
              .form == NameForm::Hidden);
    const NamePlacement p =
        place_block_name(RectF{100.0f, 200.0f, 40.0f, 400.0f}, clip, 14.0f, 2, 1, kMinNameFontPx);
    REQUIRE(p.form == NameForm::Full);
    CHECK(p.label.rect.bottom() == doctest::Approx(400.0f - kLabelPaddingPx));
}

TEST_CASE("the letter is only tried when it is shorter than the whole name") {
    // "C" is one character: if it does not fit, there is nothing shorter.
    CHECK(place_block_name(tall_block(9.0f), kClip, 14.0f, 1, 1, kMinNameFontPx).form ==
          NameForm::Hidden);
    CHECK(place_block_name(tall_block(9.0f), kClip, 14.0f, 2, 0, kMinNameFontPx).form ==
          NameForm::Hidden);
    CHECK(place_block_name(tall_block(40.0f), kClip, 14.0f, 0, 0, kMinNameFontPx).form ==
          NameForm::Hidden);
}

TEST_CASE("bad numbers hide the name") {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK(place_block_name(RectF{nan, 100.0f, 40.0f, 400.0f}, kClip, 14.0f, 2, 1, 9.0f).form ==
          NameForm::Hidden);
    CHECK(place_block_name(tall_block(40.0f), kClip, nan, 2, 1, 9.0f).form == NameForm::Hidden);
    // A bad minimum falls back to the 96 dpi one.
    CHECK(place_block_name(tall_block(40.0f), kClip, 14.0f, 2, 1, nan).form == NameForm::Full);
}

TEST_CASE("the form follows the width exactly as the character widths say") {
    const float min = kMinNameFontPx;
    for (int full = 2; full <= 4; ++full) {
        const float need_full =
            static_cast<float>(full) * kLabelCharWidthEm * min + 2.0f * kLabelPaddingPx;
        const float need_letter = kLabelCharWidthEm * min + 2.0f * kLabelPaddingPx;
        for (float w = 4.0f; w <= 60.0f; w += 0.25f) {
            if (std::fabs(w - need_full) < 0.05f || std::fabs(w - need_letter) < 0.05f) {
                continue; // right on a threshold: float rounding decides
            }
            const NamePlacement p = place_block_name(tall_block(w), kClip, 30.0f, full, 1, min);
            if (w > need_full) {
                CHECK(p.form == NameForm::Full);
            } else if (w > need_letter) {
                CHECK(p.form == NameForm::Letter);
            } else {
                CHECK(p.form == NameForm::Hidden);
            }
            if (p.form != NameForm::Hidden) {
                check_fits(p, tall_block(w), min);
            }
        }
    }
}

TEST_CASE("names across key sizes and window widths are legible or absent") {
    const KeySize sizes[] = {KeySize::FitKeyboard, KeySize::FitSong, KeySize::Small,
                             KeySize::Medium,      KeySize::Large,   KeySize::ExtraLarge};
    int full_white = 0;
    int letter_black = 0;
    int hidden_black = 0;
    for (float ppmm : {kDefaultPxPerMm, 2.0f * kDefaultPxPerMm}) {
        const float min_font = min_name_font_px(ppmm);
        for (float width : {800.0f, 1024.0f, 1280.0f, 1920.0f, 2560.0f, 3440.0f}) {
            const float w = width * (ppmm / kDefaultPxPerMm); // a denser display has more pixels
            for (KeySize size : sizes) {
                KeyViewParams vp;
                vp.keyboard = kRange88;
                vp.width_px = w;
                vp.px_per_mm = ppmm;
                vp.size = size;
                const KeyViewTarget target = compute_key_view(vp);
                KeyViewFollower follower;
                follower.snap(vp.keyboard, target.range);
                const KeyViewFrame view = follower.frame();
                const KeyboardLayout layout(view.layout_range);
                for (int note = layout.range().lowest; note <= layout.range().highest; ++note) {
                    const KeyRect k = view.to_window(layout.key(note), layout.white_key_count());
                    const bool black = k.black;
                    // The block is the key less its inset on both sides.
                    const float inset = k.width * w * (black ? 0.10f : 0.08f);
                    const RectF block{k.x * w + inset, 50.0f, k.width * w - 2.0f * inset, 300.0f};
                    const int chars = black ? 2 : 1;
                    const NamePlacement p =
                        place_block_name(block, RectF{0.0f, 0.0f, w, 400.0f},
                                         40.0f * ppmm / kDefaultPxPerMm, chars, 1, min_font);
                    if (p.form != NameForm::Hidden) {
                        check_fits(p, block, min_font);
                    }
                    if (!black && p.form == NameForm::Full) {
                        ++full_white;
                    }
                    if (black && p.form == NameForm::Letter) {
                        ++letter_black;
                    }
                    if (black && p.form == NameForm::Hidden) {
                        ++hidden_black;
                    }
                }
            }
        }
    }
    // The sweep reaches all three outcomes: big keys show names, crowded black keys fall back to
    // the letter or nothing.
    CHECK(full_white > 0);
    CHECK(letter_black > 0);
    CHECK(hidden_black > 0);
}

TEST_CASE("a dense 800 px window at fit keyboard: black keys drop to the letter") {
    const float w = 800.0f;
    const KeyboardLayout layout(kRange61);
    int black_full = 0;
    int black_letter = 0;
    int white_full = 0;
    for (int note = layout.range().lowest; note <= layout.range().highest; ++note) {
        const KeyRect k = layout.key(note);
        const float inset = k.width * w * (k.black ? 0.10f : 0.08f);
        const RectF block{k.x * w + inset, 50.0f, k.width * w - 2.0f * inset, 300.0f};
        const NamePlacement p = place_block_name(block, RectF{0.0f, 0.0f, w, 400.0f}, 17.0f,
                                                 k.black ? 2 : 1, 1, kMinNameFontPx);
        if (k.black && p.form == NameForm::Full) {
            ++black_full;
        } else if (k.black && p.form == NameForm::Letter) {
            ++black_letter;
        } else if (!k.black && p.form == NameForm::Full) {
            ++white_full;
        }
    }
    CHECK(white_full == 36);
    CHECK(black_full == 0);
    CHECK(black_letter == 25);
}
