// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/screen_layout.hpp"

#include <cmath>
#include <limits>
#include <vector>

#include <doctest/doctest.h>

using namespace toonval;

namespace {

constexpr float kPpmm96 = 96.0f / 25.4f;

struct Case {
    float w;
    float h;
    float ppmm;
};

std::vector<const RectF*> rects(const ScreenLayout& l) {
    return {&l.top_row, &l.chord_label, &l.note_area, &l.keyboard};
}

// Positive-area intersection, written out here on purpose and not shared with the code under test.
bool overlap(const RectF& a, const RectF& b) {
    if (a.w <= 0 || a.h <= 0 || b.w <= 0 || b.h <= 0) {
        return false;
    }
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

// Everything every layout must satisfy, at any size.
void check_invariants(const ScreenLayout& l, float width, float height, const char* what) {
    INFO(what);
    const float W = std::floor(std::isfinite(width) && width > 0 ? width : 0.0f);
    const float H = std::floor(std::isfinite(height) && height > 0 ? height : 0.0f);
    for (const RectF* r : {&l.key_bed, &l.key_area}) {
        CHECK(std::isfinite(r->y));
        CHECK(r->x >= 0.0f);
        CHECK(r->w >= 0.0f);
        CHECK(r->h >= 0.0f);
        if (l.keyboard.h > 0.0f) {
            CHECK(r->y >= l.keyboard.y);
            CHECK(r->bottom() <= l.keyboard.bottom());
        }
    }
    for (const RectF* r : rects(l)) {
        CHECK(std::isfinite(r->x));
        CHECK(std::isfinite(r->y));
        CHECK(std::isfinite(r->w));
        CHECK(std::isfinite(r->h));
        CHECK(r->x >= 0.0f);
        CHECK(r->y >= 0.0f);
        CHECK(r->w >= 0.0f);
        CHECK(r->h >= 0.0f);
        CHECK(r->x + r->w <= W);
        CHECK(r->y + r->h <= H);
    }
    if (W < 1.0f || H < 1.0f) {
        // No room at all: everything is empty and hidden.
        for (const RectF* r : rects(l)) {
            CHECK(r->empty());
        }
        CHECK_FALSE(l.chord_label_visible);
        CHECK_FALSE(l.status_visible);
        CHECK(l.gear.empty());
        CHECK(l.status_text.empty());
        CHECK_FALSE(l.block_labels_visible);
        CHECK_FALSE(l.key_labels_visible);
        CHECK_FALSE(l.c_markers_visible);
        return;
    }
    const auto all = rects(l);
    for (std::size_t i = 0; i < all.size(); ++i) {
        for (std::size_t j = i + 1; j < all.size(); ++j) {
            CHECK_FALSE(overlap(*all[i], *all[j]));
        }
    }
    // The key bed and the key area split the keyboard exactly: bed on top, keys start where it
    // ends.
    if (l.keyboard.h > 0.0f) {
        CHECK(l.key_bed.y == l.keyboard.y);
        CHECK(l.key_bed.bottom() == l.key_area.y);
        CHECK(l.key_area.bottom() == l.keyboard.bottom());
        // At least the minimum, unless the keyboard is so small that 20 percent of it is less.
        CHECK(l.key_bed.h >= std::min(kMinKeyBedPx, std::floor(l.keyboard.h * 0.2f)));
        CHECK(l.key_bed.h <= 0.2f * l.keyboard.h);
        CHECK(l.key_area.h > 0.0f);
        CHECK(l.key_bed.w == (l.key_bed.h > 0.0f ? l.keyboard.w : 0.0f));
        CHECK(l.key_area.w == l.keyboard.w);
        CHECK(l.felt_line_h >= (l.key_bed.h > 0.0f ? 1.0f : 0.0f));
        CHECK(l.felt_line_h <= l.key_bed.h);
        CHECK(l.key_shadow_h >= std::min(2.0f, l.key_area.h));
        CHECK(l.key_shadow_h <= l.key_area.h);
    } else {
        CHECK(l.key_bed.empty());
        CHECK(l.key_area.empty());
        CHECK(l.felt_line_h == 0.0f);
        CHECK(l.key_shadow_h == 0.0f);
    }
    // The keyboard sits flush with the bottom edge and nothing is drawn below it. The top row is
    // at the top, above the chord label.
    CHECK(l.keyboard.bottom() == H);
    for (const RectF* r : {&l.top_row, &l.chord_label, &l.note_area}) {
        CHECK(r->bottom() <= l.keyboard.y);
    }
    CHECK(l.top_row.y == 0.0f);
    CHECK(l.chord_label.y == l.top_row.bottom());
    CHECK(l.note_area.y == l.chord_label.bottom());
    // Together they fill the window height exactly.
    CHECK(l.top_row.h + l.chord_label.h + l.note_area.h + l.keyboard.h == H);
    // The gear and the status text sit inside the top row, side by side, never touching.
    if (!l.gear.empty()) {
        CHECK(l.gear.y == l.top_row.y);
        CHECK(l.gear.h == l.top_row.h);
        CHECK(l.gear.right() == l.top_row.right());
        CHECK(l.gear.w <= l.top_row.h);
    }
    if (!l.status_text.empty()) {
        CHECK(l.status_text.x > 0.0f);
        CHECK(l.status_text.y == l.top_row.y);
        CHECK(l.status_text.bottom() == l.top_row.bottom());
        CHECK(l.status_text.right() < l.gear.x);
    }
    CHECK(l.note_area.h >= kMinNoteAreaShare * H);
    // Text lives inside its rect.
    if (l.chord_label_visible) {
        CHECK(l.chord_label_font_px >= kMinChordFontPx);
        CHECK(l.chord_label_font_px * kLineHeightEm <= l.chord_label.h);
        CHECK(l.chord_label_font_px * kLabelCharWidthEm * static_cast<float>(kChordLabelMaxChars) <=
              l.chord_label.w);
    } else {
        CHECK(l.chord_label_font_px == 0.0f);
        CHECK(l.chord_label.empty());
    }
    if (l.status_visible) {
        CHECK(l.status_font_px >= kMinLabelFontPx);
        CHECK(l.status_font_px * kTopRowMinHeightEm <= l.top_row.h);
        CHECK_FALSE(l.status_text.empty());
    } else {
        CHECK(l.status_font_px == 0.0f);
    }
    if (l.block_labels_visible) {
        CHECK(l.note_label_font_px >= kMinLabelFontPx);
        CHECK(l.note_label_font_px <= l.keyboard.h);
    } else {
        CHECK(l.note_label_font_px == 0.0f);
    }
    if (l.c_markers_visible) {
        CHECK(l.c_marker_font_px >= kMinLabelFontPx);
        CHECK(l.c_marker_font_px <= l.keyboard.h);
    } else {
        CHECK(l.c_marker_font_px == 0.0f);
    }
    if (l.key_labels_visible) {
        CHECK(l.key_label_font_px >= kMinLabelFontPx);
        CHECK(l.key_label_font_px <= l.keyboard.h);
    } else {
        CHECK(l.key_label_font_px == 0.0f);
    }
}

std::vector<Case> sweep_cases() {
    std::vector<Case> out;
    const float densities[] = {2.0f, kPpmm96, 5.0f, 6.4f, 10.0f};
    // Named sizes: 4:3, 16:10, 16:9, 21:9, 32:9, portrait, the 800x480 floor, tiny, 4K, 8K.
    const float sizes[][2] = {
        {800, 480},   {1024, 600},  {1024, 768},  {1280, 720},  {1280, 800},  {1600, 1200},
        {1920, 1080}, {2560, 1080}, {3440, 1440}, {3840, 2160}, {5120, 1440}, {7680, 4320},
        {480, 800},   {720, 1280},  {1080, 1920}, {100, 50},    {320, 240},   {64, 64},
        {10, 10},     {1, 1},       {0, 0},       {800, 0},     {0, 480},     {799.5f, 479.5f}};
    for (const auto& s : sizes) {
        for (float d : densities) {
            out.push_back({s[0], s[1], d});
        }
    }
    // Every aspect ratio from 4:3 to 21:9 at several heights, including odd pixel counts.
    for (float h : {480.0f, 481.0f, 600.0f, 721.0f, 1080.0f, 1440.0f, 2160.0f}) {
        for (int step = 0; step <= 16; ++step) {
            const float aspect =
                4.0f / 3.0f + (21.0f / 9.0f - 4.0f / 3.0f) * static_cast<float>(step) / 16.0f;
            for (float d : {kPpmm96, 5.0f}) {
                out.push_back({std::round(h * aspect), h, d});
            }
        }
    }
    return out;
}

std::vector<LookSettings> sweep_looks() {
    std::vector<LookSettings> out;
    for (float kb = 0.10f; kb <= 0.3001f; kb += 0.02f) {
        LookSettings s;
        s.keyboard_height = kb;
        out.push_back(s);
    }
    // Out of range and nonsense heights are clamped, not trusted.
    for (float kb : {-1.0f, 0.0f, 0.5f, 1.0f, 99.0f, std::numeric_limits<float>::quiet_NaN()}) {
        LookSettings s;
        s.keyboard_height = kb;
        out.push_back(s);
    }
    for (float size : {kMinChordLabelSize, 1.0f, kMaxChordLabelSize}) {
        for (bool chord : {false, true}) {
            for (NameMode mode : {NameMode::Off, NameMode::Letters, NameMode::Solfege}) {
                LookSettings s;
                s.chord_label_size = size;
                s.chord_label = chord;
                s.name_mode = mode;
                s.names_on_keys = true;
                out.push_back(s);
            }
        }
    }
    for (PresetId id : kAllPresets) {
        out.push_back(preset(id));
    }
    return out;
}

} // namespace

TEST_CASE("a 1080p window is stacked top row, chord label, notes, keyboard") {
    const ScreenLayout l = compute_layout(1920, 1080, kPpmm96, LookSettings{});
    check_invariants(l, 1920, 1080, "1080p");
    CHECK(l.top_row.y == 0.0f);
    CHECK(l.chord_label.y == l.top_row.bottom());
    CHECK(l.note_area.y == l.chord_label.bottom());
    CHECK(l.keyboard.y == l.note_area.bottom());
    CHECK(l.keyboard.bottom() == 1080.0f);
    for (const RectF* r : rects(l)) {
        CHECK(r->x == 0.0f);
        CHECK(r->w == 1920.0f);
    }
    CHECK(l.chord_label_visible);
    CHECK(l.status_visible);
    CHECK(l.block_labels_visible);
    CHECK_FALSE(l.key_labels_visible); // names_on_keys is off by default
}

TEST_CASE("the keyboard is the requested fraction of the window height") {
    LookSettings s;
    s.keyboard_height = 0.25f;
    const ScreenLayout l = compute_layout(1920, 1080, kPpmm96, s);
    CHECK(l.keyboard.h == 270.0f);
    s.keyboard_height = 0.12f;
    CHECK(compute_layout(1920, 1080, kPpmm96, s).keyboard.h == 129.0f); // floor(129.6)
}

TEST_CASE("a taller keyboard setting leaves less for the notes") {
    float last_keyboard = 0.0f;
    float last_notes = 1e9f;
    for (float kb : {0.10f, 0.14f, 0.18f, 0.22f, 0.26f, 0.30f}) {
        LookSettings s;
        s.keyboard_height = kb;
        const ScreenLayout l = compute_layout(1280, 720, kPpmm96, s);
        CHECK(l.keyboard.h > last_keyboard);
        CHECK(l.note_area.h < last_notes);
        last_keyboard = l.keyboard.h;
        last_notes = l.note_area.h;
    }
}

TEST_CASE("no overlap, nothing outside the window, notes keep 40 percent: every size and setting") {
    const std::vector<LookSettings> looks = sweep_looks();
    for (const Case& c : sweep_cases()) {
        for (const LookSettings& look : looks) {
            check_invariants(compute_layout(c.w, c.h, c.ppmm, look), c.w, c.h, "sweep");
        }
    }
}

TEST_CASE("from 800x480 up the chord label, labels and status line all fit at the defaults") {
    for (const Case& c : sweep_cases()) {
        if (c.w < kMinWindowWidthPx || c.h < kMinWindowHeightPx || c.ppmm > 6.5f) {
            continue;
        }
        const ScreenLayout l = compute_layout(c.w, c.h, c.ppmm, LookSettings{});
        CHECK(l.chord_label_visible);
        CHECK(l.status_visible);
        CHECK(l.block_labels_visible);
        CHECK(l.top_row.h >= kMinTouchTargetMm * c.ppmm);
    }
}

TEST_CASE("the chord label shrinks with the window, then hides") {
    LookSettings s;
    float last = 1e9f;
    bool hidden = false;
    for (float h : {2160.0f, 1080.0f, 720.0f, 480.0f, 300.0f, 200.0f, 120.0f, 60.0f}) {
        const ScreenLayout l = compute_layout(1280, h, kPpmm96, s);
        check_invariants(l, 1280, h, "shrinking");
        if (!l.chord_label_visible) {
            hidden = true;
        } else {
            CHECK_FALSE(hidden); // once hidden, a smaller window never brings it back
            CHECK(l.chord_label_font_px <= last);
            last = l.chord_label_font_px;
        }
    }
    CHECK(hidden);
    CHECK(last >= kMinChordFontPx);
}

TEST_CASE("the chord label size setting scales the font until the strip is the limit") {
    LookSettings small;
    small.chord_label_size = kMinChordLabelSize;
    LookSettings big;
    big.chord_label_size = kMaxChordLabelSize;
    const ScreenLayout a = compute_layout(1920, 1080, kPpmm96, small);
    const ScreenLayout b = compute_layout(1920, 1080, kPpmm96, big);
    CHECK(b.chord_label_font_px > a.chord_label_font_px);
    CHECK(b.chord_label.h > a.chord_label.h);
    CHECK(b.note_area.h < a.note_area.h);
    // A short window caps the font at what the strip allows, whatever the setting.
    const ScreenLayout c = compute_layout(800, 480, kPpmm96, small);
    const ScreenLayout d = compute_layout(800, 480, kPpmm96, big);
    CHECK(d.chord_label_font_px <= 0.12f * 480.0f / kLineHeightEm);
    CHECK(d.chord_label_font_px >= c.chord_label_font_px);
}

TEST_CASE("chord label off gives no strip") {
    LookSettings s;
    s.chord_label = false;
    const ScreenLayout l = compute_layout(1280, 720, kPpmm96, s);
    CHECK_FALSE(l.chord_label_visible);
    CHECK(l.chord_label.h == 0.0f);
    CHECK(l.chord_label_font_px == 0.0f);
    check_invariants(l, 1280, 720, "chord off");
}

TEST_CASE("the status text hides in short windows before anything collides") {
    const ScreenLayout tall = compute_layout(1280, 720, kPpmm96, LookSettings{});
    CHECK(tall.status_visible);
    const ScreenLayout flat = compute_layout(1280, 40, kPpmm96, LookSettings{});
    CHECK_FALSE(flat.status_visible);
    check_invariants(flat, 1280, 40, "flat");
}

TEST_CASE("note labels follow the names setting and hide when too small to read") {
    LookSettings s;
    s.names_on_blocks = true;
    s.names_on_keys = true;
    const ScreenLayout on = compute_layout(1280, 720, kPpmm96, s);
    CHECK(on.block_labels_visible);
    CHECK(on.key_labels_visible);
    CHECK(on.note_label_font_px >= kMinLabelFontPx);

    s.name_mode = NameMode::Off;
    const ScreenLayout off = compute_layout(1280, 720, kPpmm96, s);
    CHECK_FALSE(off.block_labels_visible);
    CHECK_FALSE(off.key_labels_visible);

    s.name_mode = NameMode::Letters;
    s.names_on_blocks = false;
    const ScreenLayout keys_only = compute_layout(1280, 720, kPpmm96, s);
    CHECK_FALSE(keys_only.block_labels_visible);
    CHECK(keys_only.key_labels_visible);

    s.names_on_blocks = true;
    const ScreenLayout tiny = compute_layout(1280, 60, kPpmm96, s);
    CHECK_FALSE(tiny.block_labels_visible);
    CHECK_FALSE(tiny.key_labels_visible);
}

TEST_CASE("a label font shrinks to the cell, then hides") {
    // 14 px font, 3 characters: needs 3 * 0.62 * 14 + 4 = 30 px.
    CHECK(fit_label_font_px(14.0f, 100.0f, 3) == 14.0f);
    CHECK(fit_label_font_px(14.0f, 30.1f, 3) == 14.0f);
    CHECK(fit_label_font_px(14.0f, 30.0f, 3) == 13.0f); // floor of 13.98
    CHECK(fit_label_font_px(14.0f, 20.0f, 3) == 0.0f);  // would be 8.6 px: unreadable
    CHECK(fit_label_font_px(14.0f, 0.0f, 3) == 0.0f);
    CHECK(fit_label_font_px(14.0f, -5.0f, 3) == 0.0f);
    CHECK(fit_label_font_px(14.0f, 100.0f, 0) == 0.0f);
    CHECK(fit_label_font_px(0.0f, 100.0f, 3) == 0.0f);
    CHECK(fit_label_font_px(std::numeric_limits<float>::quiet_NaN(), 100.0f, 3) == 0.0f);
    CHECK(fit_label_font_px(14.0f, std::numeric_limits<float>::quiet_NaN(), 3) == 0.0f);
    // More characters need more room.
    CHECK(fit_label_font_px(14.0f, 40.0f, 1) == 14.0f);
    CHECK(fit_label_font_px(14.0f, 40.0f, 5) < 14.0f);
}

TEST_CASE("whatever fit_label_font_px returns, the text fits the cell") {
    for (float cell = 0.0f; cell < 80.0f; cell += 0.5f) {
        for (int chars = 1; chars <= 6; ++chars) {
            const float f = fit_label_font_px(16.0f, cell, chars);
            if (f > 0.0f) {
                CHECK(f >= kMinLabelFontPx);
                CHECK(f <= 16.0f);
                CHECK(static_cast<float>(chars) * kLabelCharWidthEm * f + 2.0f * kLabelPaddingPx <=
                      cell);
            }
        }
    }
}

TEST_CASE("bad inputs give empty layouts or the default density, never garbage") {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    for (float bad : {-5.0f, 0.0f, nan, -inf}) {
        const ScreenLayout l = compute_layout(bad, 480, kPpmm96, LookSettings{});
        check_invariants(l, bad, 480, "bad width");
        const ScreenLayout m = compute_layout(800, bad, kPpmm96, LookSettings{});
        check_invariants(m, 800, bad, "bad height");
    }
    const ScreenLayout reference = compute_layout(1280, 720, kDefaultPxPerMm, LookSettings{});
    for (float bad : {0.0f, -3.0f, nan, inf}) {
        const ScreenLayout l = compute_layout(1280, 720, bad, LookSettings{});
        check_invariants(l, 1280, 720, "bad density");
        CHECK(l.top_row.h == reference.top_row.h);
    }
    LookSettings wild;
    wild.keyboard_height = nan;
    wild.chord_label_size = inf;
    check_invariants(compute_layout(1280, 720, kPpmm96, wild), 1280, 720, "wild look");
}

// ---- Behaviour the first renderer's layout had and the renderer still relies on ----

TEST_CASE("at 1080p the chord label is large and the notes get most of the screen") {
    const ScreenLayout l = compute_layout(1920, 1080, kPpmm96, LookSettings{});
    check_invariants(l, 1920, 1080, "1080p");
    CHECK(l.chord_label_font_px >= 0.08f * 1080.0f); // sized from the window height, not 8 mm
    CHECK(l.note_area.h >= 0.5f * 1080.0f);
    CHECK(l.status_font_px >= 0.02f * 1080.0f);
}

TEST_CASE("the chord label gives way before the note area does, then hides") {
    LookSettings s;
    s.chord_label_size = kMaxChordLabelSize;
    for (float h : {480.0f, 600.0f, 720.0f, 1080.0f}) {
        const ScreenLayout l = compute_layout(1920, h, kPpmm96, s);
        check_invariants(l, 1920, h, "max chord size");
        CHECK(l.note_area.h >= kMinNoteAreaShare * h);
    }
    // No room left once the keyboard and top row are taken: the label hides instead of shrinking.
    s.keyboard_height = kMaxKeyboardHeight;
    const ScreenLayout tight = compute_layout(1280, 150, 5.0f, s);
    check_invariants(tight, 1280, 150, "tight");
    CHECK_FALSE(tight.chord_label_visible);
    CHECK(tight.chord_label_font_px == 0.0f);
    CHECK(tight.chord_label.h == 0.0f);
}

TEST_CASE("the keyboard height is the setting, whatever the key width") {
    // The first renderer sized the keyboard from the white-key width, clamped to 10..20 percent.
    // Now the look's keyboard_height decides, so layouts do not depend on the key count.
    LookSettings s;
    s.keyboard_height = 0.2f;
    const ScreenLayout a = compute_layout(1920, 1080, kPpmm96, s);
    const ScreenLayout b = compute_layout(800, 2000, kPpmm96, s);
    CHECK(a.keyboard.h == std::floor(1080.0f * 0.2f));
    CHECK(b.keyboard.h == std::floor(2000.0f * 0.2f));
}

TEST_CASE("the key bed sits on top of the keyboard and the keys start where it ends") {
    const ScreenLayout l = compute_layout(1920, 1080, kPpmm96, LookSettings{});
    CHECK(l.key_bed.y == l.keyboard.y);
    CHECK(l.key_bed.y == l.note_area.bottom()); // the hit line is the top of the bed
    CHECK(l.key_bed.bottom() == l.key_area.y);
    CHECK(l.key_area.bottom() == l.keyboard.bottom());
    CHECK(l.key_bed.h >= kMinKeyBedPx);
    CHECK(l.felt_line_h >= 1.0f);
    CHECK(l.felt_line_h < l.key_bed.h);
    // The bed scales with the keyboard, and stays usable at the 800x480 floor.
    const ScreenLayout small = compute_layout(800, 480, 5.0f, LookSettings{});
    const ScreenLayout big = compute_layout(3840, 2160, kPpmm96, LookSettings{});
    CHECK(small.key_bed.h >= kMinKeyBedPx);
    CHECK(big.key_bed.h > small.key_bed.h);
    CHECK(big.key_shadow_h > small.key_shadow_h);
}

TEST_CASE("C markers have their own switch and size, the same as the key labels") {
    LookSettings s;
    s.name_mode = NameMode::Off;
    s.names_on_keys = false;
    const ScreenLayout on = compute_layout(1280, 720, kPpmm96, s);
    CHECK(on.c_markers_visible); // on by default, with every name switch off
    CHECK_FALSE(on.key_labels_visible);
    CHECK_FALSE(on.block_labels_visible);
    CHECK(on.c_marker_font_px >= kMinLabelFontPx);
    s.names_on_keys = true;
    s.name_mode = NameMode::Letters;
    const ScreenLayout both = compute_layout(1280, 720, kPpmm96, s);
    CHECK(both.c_marker_font_px == both.key_label_font_px);
    s.c_markers = false;
    const ScreenLayout off = compute_layout(1280, 720, kPpmm96, s);
    CHECK_FALSE(off.c_markers_visible);
    CHECK(off.c_marker_font_px == 0.0f);
    // A keyboard too short for readable text hides them, like every other label.
    CHECK_FALSE(compute_layout(1280, 60, kPpmm96, LookSettings{}).c_markers_visible);
}

TEST_CASE("a C marker shrinks with the key width and hides before it collides") {
    // "C4" on a white key: 14 px font needs 2 * 0.62 * 14 + 4 = 21.4 px.
    CHECK(fit_label_font_px(14.0f, 24.0f, 2) == 14.0f);
    CHECK(fit_label_font_px(14.0f, 18.0f, 2) < 14.0f);
    CHECK(fit_label_font_px(14.0f, 18.0f, 2) >= kMinLabelFontPx);
    // 88 keys across 800 px: 15 px keys, "C-1" (3 characters) is unreadable there.
    CHECK(fit_label_font_px(14.0f, 800.0f / 52.0f - 1.6f, 3) == 0.0f);
}

// ---- The top row: status text, gear, and the bar when it is revealed ----

namespace {

std::vector<Case> wide_cases() {
    // 800x480 up to 21:9, at several densities.
    std::vector<Case> out;
    for (float h : {480.0f, 600.0f, 720.0f, 900.0f, 1080.0f, 1440.0f}) {
        for (int step = 0; step <= 8; ++step) {
            const float aspect =
                4.0f / 3.0f + (21.0f / 9.0f - 4.0f / 3.0f) * static_cast<float>(step) / 8.0f;
            const float w = std::max(800.0f, std::round(h * aspect));
            for (float d : {kPpmm96, 5.0f, 6.4f}) {
                out.push_back({w, h, d});
            }
        }
    }
    return out;
}

} // namespace

TEST_CASE("the top row is one touch target tall from 800x480 to 21:9, and the stack holds") {
    for (const Case& c : wide_cases()) {
        const ScreenLayout l = compute_layout(c.w, c.h, c.ppmm, LookSettings{});
        check_invariants(l, c.w, c.h, "wide");
        CHECK(l.top_row.h == std::ceil(kMinTouchTargetMm * c.ppmm));
        CHECK(l.top_row.w == c.w);
        CHECK(l.chord_label.y == l.top_row.bottom());
        CHECK(l.note_area.y == l.chord_label.bottom());
        CHECK(l.keyboard.y == l.note_area.bottom());
        CHECK(l.keyboard.bottom() == c.h); // nothing below the keyboard
        CHECK(l.status_visible);
    }
    // The documented floor: 800x480 on a 5 px/mm panel (a 7-inch touch screen).
    CHECK(compute_layout(800, 480, 5.0f, LookSettings{}).top_row.h >= 75.0f);
}

TEST_CASE("the top row is capped in windows too short for a 15 mm row") {
    const ScreenLayout l = compute_layout(1280, 150, kPpmm96, LookSettings{});
    check_invariants(l, 1280, 150, "short");
    CHECK(l.top_row.h <= kMaxTopRowShare * 150.0f);
}

TEST_CASE("the gear is a square in the top-right corner and the status area never reaches it") {
    for (const Case& c : wide_cases()) {
        const ScreenLayout l = compute_layout(c.w, c.h, c.ppmm, LookSettings{});
        CHECK(l.gear.w == l.top_row.h);
        CHECK(l.gear.h == l.top_row.h);
        CHECK(l.gear.right() == c.w);
        CHECK(l.gear.y == 0.0f);
        CHECK(l.status_text.x > 0.0f);
        CHECK(l.status_text.right() < l.gear.x);
        CHECK_FALSE(overlap(l.status_text, l.gear));
    }
}

TEST_CASE("the note area gets the height the old status line had") {
    // Before the top row, a 1280x720 window at 96 dpi had a 57 px control bar and a 26 px status
    // line under it. Now the status is in the top row and the notes start 26 px higher.
    const ScreenLayout l = compute_layout(1280, 720, kPpmm96, LookSettings{});
    CHECK(l.top_row.h == 57.0f);
    const float old_note_h = 720.0f - 57.0f - 26.0f - l.chord_label.h - l.keyboard.h;
    CHECK(l.note_area.h == old_note_h + 26.0f);
}

TEST_CASE("a revealed bar takes the right end of the status area and the text shrinks") {
    for (const Case& c : wide_cases()) {
        const ScreenLayout l = compute_layout(c.w, c.h, c.ppmm, LookSettings{});
        for (float bar_w : {100.0f, 400.0f, 700.0f, 5000.0f}) {
            const TopRowSplit s = split_top_row(l, bar_w, 0.0f, false);
            INFO("bar " << bar_w << " in " << c.w << "x" << c.h);
            CHECK(s.bar.w == std::min(bar_w, l.status_text.w));
            CHECK(s.bar.right() == l.status_text.right()); // next to the gear
            CHECK(s.bar.right() < l.gear.x);
            CHECK(s.bar.y == l.top_row.y);
            CHECK(s.bar.h == l.top_row.h);
            CHECK_FALSE(overlap(s.bar, l.gear));
            CHECK_FALSE(overlap(s.bar, s.status_text));
            if (!s.status_text.empty()) {
                CHECK(s.status_text.x == l.status_text.x);
                CHECK(s.status_text.right() < s.bar.x);
                CHECK(s.status_text.w < l.status_text.w);
            }
        }
    }
}

TEST_CASE(
    "a centred bar sits in the middle, moves right to keep status text, never past the gear") {
    const ScreenLayout l = compute_layout(1280, 720, kPpmm96, LookSettings{});
    const TopRowSplit mid = split_top_row(l, 400.0f, 0.0f, true);
    CHECK(mid.bar.x == 440.0f);
    CHECK(mid.status_text.right() < mid.bar.x);
    // Asked to keep 600 px of status text: the bar moves right of it.
    const TopRowSplit kept = split_top_row(l, 400.0f, 600.0f, true);
    CHECK(kept.bar.x > mid.bar.x);
    CHECK(kept.status_text.w >= 600.0f);
    CHECK(kept.bar.right() <= l.status_text.right());
    // Asked for more than fits: the bar stops at the gear and the text gives way.
    const TopRowSplit tight = split_top_row(l, 400.0f, 5000.0f, true);
    CHECK(tight.bar.right() == l.status_text.right());
    CHECK(tight.bar.w == 400.0f);
    CHECK_FALSE(overlap(tight.bar, tight.status_text));
}

TEST_CASE("no bar, or a bad width, leaves the status area whole") {
    const ScreenLayout l = compute_layout(1280, 720, kPpmm96, LookSettings{});
    for (float bad : {0.0f, -10.0f, std::numeric_limits<float>::quiet_NaN()}) {
        const TopRowSplit s = split_top_row(l, bad, 0.0f, false);
        CHECK(s.bar.empty());
        CHECK(s.status_text.x == l.status_text.x);
        CHECK(s.status_text.w == l.status_text.w);
    }
    // A bar as wide as the whole area leaves no status text at all, rather than a sliver under it.
    const TopRowSplit full = split_top_row(l, l.status_text.w, 0.0f, false);
    CHECK(full.status_text.empty());
    CHECK(full.bar.x == l.status_text.x);
}
