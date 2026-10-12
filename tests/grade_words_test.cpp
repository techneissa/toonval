// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/grade_words.hpp"

#include "toonval/keyboard_view.hpp"
#include "toonval/screen_layout.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <random>
#include <vector>

#include <doctest/doctest.h>

using namespace toonval;

namespace {

constexpr float kPpmm = 96.0f / 25.4f;

bool intersects(const RectF& a, const RectF& b) {
    return a.x < b.right() && b.x < a.right() && a.y < b.bottom() && b.y < a.bottom();
}

bool inside(const RectF& inner, const RectF& outer) {
    return inner.x >= outer.x && inner.y >= outer.y && inner.right() <= outer.right() &&
           inner.bottom() <= outer.bottom();
}

GradeLayoutParams params_for(const ScreenLayout& lay, GradePosition pos, float word_h = 20.0f) {
    GradeLayoutParams p;
    p.note_area = lay.note_area;
    p.chord_label = lay.chord_label;
    p.position = pos;
    p.word_h = word_h;
    return p;
}

const RectF kArea{0.0f, 100.0f, 800.0f, 300.0f}; // bottom at 400
const RectF kChord{0.0f, 60.0f, 800.0f, 40.0f};

GradeLayoutParams simple(GradePosition pos) {
    GradeLayoutParams p;
    p.note_area = kArea;
    p.chord_label = kChord;
    p.position = pos;
    p.word_h = 20.0f;
    return p;
}

// The key centres of one view: the keys a window of `width` px shows at `size`.
std::vector<float> key_centres(float width, KeySize size, float ppmm) {
    KeyViewParams vp;
    vp.keyboard = kRange61;
    vp.width_px = width;
    vp.px_per_mm = ppmm;
    vp.size = size;
    const KeyViewTarget target = compute_key_view(vp);
    KeyViewFollower follower;
    follower.snap(vp.keyboard, target.range);
    const KeyViewFrame view = follower.frame();
    const KeyboardLayout layout(view.layout_range);
    std::vector<float> out;
    for (int note = layout.range().lowest; note <= layout.range().highest; ++note) {
        const KeyRect k = view.to_window(layout.key(note), layout.white_key_count());
        if (k.width > 0.0f && (k.x + k.width) * width > 0.0f && k.x * width < width) {
            out.push_back((k.x + 0.5f * k.width) * width);
        }
    }
    return out;
}

} // namespace

TEST_CASE("one word at the bottom sits just above the keys, centred on its key") {
    const GradeWordIn in{300.0f, 70.0f, 0.2f};
    GradeWordOut out[1];
    CHECK(place_grade_words(simple(GradePosition::Bottom), {}, std::span(&in, 1), out) == 1);
    REQUIRE(out[0].visible);
    CHECK(out[0].row == 0);
    CHECK(out[0].rect.x + 0.5f * out[0].rect.w == doctest::Approx(300.0f));
    CHECK(out[0].rect.h == doctest::Approx(20.0f));
    CHECK(out[0].rect.bottom() == doctest::Approx(kArea.bottom() - kGradeWordGapPx));
}

TEST_CASE("one word at the top sits at the top of the note area, below the chord label") {
    const GradeWordIn in{300.0f, 70.0f, 0.2f};
    GradeWordOut out[1];
    CHECK(place_grade_words(simple(GradePosition::Top), {}, std::span(&in, 1), out) == 1);
    REQUIRE(out[0].visible);
    CHECK(out[0].rect.y == doctest::Approx(kArea.y + kGradeWordGapPx));
    CHECK(out[0].rect.y >= kChord.bottom());
}

TEST_CASE("a chord label that reaches into the note area pushes the top rows below it") {
    GradeLayoutParams p = simple(GradePosition::Top);
    p.chord_label = RectF{0.0f, 80.0f, 800.0f, 60.0f}; // bottom at 140, inside the area
    const GradeWordIn in{300.0f, 70.0f, 0.2f};
    GradeWordOut out[1];
    place_grade_words(p, {}, std::span(&in, 1), out);
    REQUIRE(out[0].visible);
    CHECK(out[0].rect.y >= 140.0f);
    CHECK_FALSE(intersects(out[0].rect, p.chord_label));
    CHECK(grade_band(p).y >= 140.0f);
}

TEST_CASE("words on the same key stack in two rows, the newest nearest its edge") {
    for (GradePosition pos : {GradePosition::Bottom, GradePosition::Top}) {
        const GradeWordIn in[] = {{300.0f, 70.0f, 0.5f}, {300.0f, 60.0f, 0.1f}};
        GradeWordOut out[2];
        CHECK(place_grade_words(simple(pos), {}, in, out) == 2);
        REQUIRE(out[0].visible);
        REQUIRE(out[1].visible);
        CHECK(out[1].row == 0); // the newer one
        CHECK(out[0].row == 1);
        CHECK_FALSE(intersects(out[0].rect, out[1].rect));
    }
}

TEST_CASE("a third word on the same key makes the oldest give way") {
    for (GradePosition pos : {GradePosition::Bottom, GradePosition::Top}) {
        const GradeWordIn in[] = {
            {300.0f, 70.0f, 0.7f}, {300.0f, 60.0f, 0.1f}, {300.0f, 66.0f, 0.4f}};
        GradeWordOut out[3];
        CHECK(place_grade_words(simple(pos), {}, in, out) == 2);
        CHECK_FALSE(out[0].visible); // the oldest
        CHECK(out[0].rect.empty());
        CHECK(out[1].visible);
        CHECK(out[2].visible);
    }
}

TEST_CASE("words on distant keys share a row") {
    const GradeWordIn in[] = {{100.0f, 70.0f, 0.3f}, {400.0f, 70.0f, 0.3f}, {700.0f, 70.0f, 0.3f}};
    GradeWordOut out[3];
    CHECK(place_grade_words(simple(GradePosition::Bottom), {}, in, out) == 3);
    for (const GradeWordOut& o : out) {
        CHECK(o.row == 0);
    }
}

TEST_CASE("a note name in the way sends the word to the next row, or hides it") {
    const GradeWordIn in{300.0f, 70.0f, 0.2f};
    GradeWordOut out[1];
    // A name on row 0.
    const RectF name{290.0f, 380.0f, 20.0f, 18.0f};
    std::array<RectF, 1> avoid{name};
    CHECK(place_grade_words(simple(GradePosition::Bottom), avoid, std::span(&in, 1), out) == 1);
    CHECK(out[0].row == 1);
    CHECK_FALSE(intersects(out[0].rect, name));
    // Names in both rows: it gives way.
    std::array<RectF, 2> both{name, RectF{290.0f, 350.0f, 20.0f, 20.0f}};
    CHECK(place_grade_words(simple(GradePosition::Bottom), both, std::span(&in, 1), out) == 0);
    CHECK_FALSE(out[0].visible);
    // A name beside it does not matter.
    std::array<RectF, 1> beside{RectF{600.0f, 375.0f, 20.0f, 20.0f}};
    CHECK(place_grade_words(simple(GradePosition::Bottom), beside, std::span(&in, 1), out) == 1);
    CHECK(out[0].row == 0);
}

TEST_CASE("a word near an edge is moved inside the area") {
    const GradeWordIn in[] = {{5.0f, 90.0f, 0.3f}, {796.0f, 90.0f, 0.3f}};
    GradeWordOut out[2];
    CHECK(place_grade_words(simple(GradePosition::Bottom), {}, in, out) == 2);
    CHECK(out[0].rect.x == doctest::Approx(0.0f));
    CHECK(out[1].rect.right() == doctest::Approx(800.0f));
    CHECK(inside(out[0].rect, kArea));
    CHECK(inside(out[1].rect, kArea));
}

TEST_CASE("a word wider than the area, or a band too small for a row, is not shown") {
    const GradeWordIn wide{400.0f, 900.0f, 0.3f};
    GradeWordOut out[1];
    CHECK(place_grade_words(simple(GradePosition::Bottom), {}, std::span(&wide, 1), out) == 0);

    GradeLayoutParams tiny = simple(GradePosition::Bottom);
    tiny.note_area = RectF{0.0f, 100.0f, 800.0f, 18.0f};
    const GradeWordIn in{300.0f, 70.0f, 0.2f};
    CHECK(place_grade_words(tiny, {}, std::span(&in, 1), out) == 0);
    CHECK(grade_band(tiny).empty());

    GradeLayoutParams none = simple(GradePosition::Top);
    none.note_area = RectF{};
    CHECK(place_grade_words(none, {}, std::span(&in, 1), out) == 0);
    GradeLayoutParams zero_h = simple(GradePosition::Top);
    zero_h.word_h = 0.0f;
    CHECK(place_grade_words(zero_h, {}, std::span(&in, 1), out) == 0);
}

TEST_CASE("the result does not depend on the order the words come in") {
    std::vector<GradeWordIn> words;
    std::mt19937 rng(7);
    for (int i = 0; i < 12; ++i) {
        words.push_back({static_cast<float>(rng() % 700 + 50),
                         60.0f + static_cast<float>(rng() % 30),
                         0.05f + 0.07f * static_cast<float>(i)});
    }
    std::vector<GradeWordOut> a(words.size());
    place_grade_words(simple(GradePosition::Bottom), {}, words, a);
    std::vector<GradeWordIn> reversed(words.rbegin(), words.rend());
    std::vector<GradeWordOut> b(words.size());
    place_grade_words(simple(GradePosition::Bottom), {}, reversed, b);
    for (std::size_t i = 0; i < words.size(); ++i) {
        const GradeWordOut& x = a[i];
        const GradeWordOut& y = b[words.size() - 1 - i];
        CHECK(x.visible == y.visible);
        CHECK(x.rect.x == y.rect.x);
        CHECK(x.rect.y == y.rect.y);
    }
}

TEST_CASE("more words than the capacity keeps the newest and never overflows") {
    std::vector<GradeWordIn> words;
    for (int i = 0; i < 100; ++i) {
        words.push_back({8.0f * static_cast<float>(i), 20.0f, 0.01f * static_cast<float>(i)});
    }
    std::vector<GradeWordOut> out(words.size());
    const std::size_t shown = place_grade_words(simple(GradePosition::Top), {}, words, out);
    CHECK(shown <= kMaxGradeWords);
    // Words past the capacity are the oldest ones.
    for (std::size_t i = kMaxGradeWords; i < words.size(); ++i) {
        CHECK_FALSE(out[i].visible);
    }
}

TEST_CASE("bad numbers are skipped, not placed") {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const GradeWordIn in[] = {
        {nan, 70.0f, 0.2f}, {300.0f, nan, 0.2f}, {300.0f, 70.0f, nan}, {300.0f, 70.0f, 0.2f}};
    GradeWordOut out[4];
    CHECK(place_grade_words(simple(GradePosition::Bottom), {}, in, out) == 1);
    CHECK(out[3].visible);
}

TEST_CASE("the band lies inside the note area and, at the top, below the chord label") {
    const RectF bottom = grade_band(simple(GradePosition::Bottom));
    const RectF top = grade_band(simple(GradePosition::Top));
    REQUIRE_FALSE(bottom.empty());
    REQUIRE_FALSE(top.empty());
    CHECK(inside(bottom, kArea));
    CHECK(inside(top, kArea));
    CHECK(top.y >= kChord.bottom());
    CHECK(bottom.bottom() <= kArea.bottom() - kGradeWordGapPx + 0.001f);
    CHECK(bottom.h == doctest::Approx(2.0f * 20.0f + kGradeWordGapPx));
}

TEST_CASE("a grade word fades in quickly and out to nothing") {
    CHECK(grade_word_alpha(0.0f) == 0.0f);
    CHECK(grade_word_alpha(kGradeFadeIn) > 0.98f);
    CHECK(grade_word_alpha(0.5f) == doctest::Approx(0.75f));
    CHECK(grade_word_alpha(1.0f) == doctest::Approx(0.0f));
    CHECK(grade_word_alpha(-0.1f) == 0.0f);
    CHECK(grade_word_alpha(1.5f) == 0.0f);
    CHECK(grade_word_alpha(std::numeric_limits<float>::quiet_NaN()) == 0.0f);
    float last = 2.0f;
    for (float a = kGradeFadeIn; a <= 1.0f; a += 0.05f) {
        CHECK(grade_word_alpha(a) <= last);
        last = grade_word_alpha(a);
    }
}

TEST_CASE("nothing overlaps from 800x480 to 21:9 at every key size") {
    const struct {
        float w;
        float h;
    } windows[] = {{800, 480},   {1024, 600},  {1280, 720},  {1280, 800},
                   {1920, 1080}, {2560, 1080}, {3440, 1440}, {5120, 1440}};
    const KeySize sizes[] = {KeySize::FitKeyboard, KeySize::FitSong, KeySize::Small,
                             KeySize::Medium,      KeySize::Large,   KeySize::ExtraLarge};
    std::mt19937 rng(2026);
    std::size_t placed_total = 0;
    for (const auto& win : windows) {
        for (KeySize size : sizes) {
            for (GradePosition pos : {GradePosition::Bottom, GradePosition::Top}) {
                for (bool chord : {true, false}) {
                    LookSettings look;
                    look.chord_label = chord;
                    look.key_size = size;
                    const ScreenLayout lay = compute_layout(win.w, win.h, kPpmm, look);
                    const std::vector<float> centres = key_centres(win.w, size, kPpmm);
                    REQUIRE_FALSE(centres.empty());
                    const float word_h = std::max(14.0f, 18.0f * win.h / 1080.0f) * 1.3f;
                    const GradeLayoutParams p = params_for(lay, pos, word_h);
                    const RectF band = grade_band(p);
                    for (int frame = 0; frame < 40; ++frame) {
                        std::array<GradeWordIn, 16> words{};
                        std::array<GradeWordOut, 16> out{};
                        const std::size_t n = 1 + rng() % words.size();
                        for (std::size_t i = 0; i < n; ++i) {
                            words[i].centre_x = centres[rng() % centres.size()];
                            words[i].width =
                                (3.0f + static_cast<float>(rng() % 5)) * 0.58f * word_h * 0.8f;
                            words[i].age = static_cast<float>(rng() % 1000) / 1000.0f;
                        }
                        std::array<RectF, 24> names{};
                        const std::size_t name_count = rng() % names.size();
                        for (std::size_t i = 0; i < name_count; ++i) {
                            const float nw = 14.0f + static_cast<float>(rng() % 30);
                            const float nh = 12.0f + static_cast<float>(rng() % 10);
                            const float nx = centres[rng() % centres.size()] - 0.5f * nw;
                            const float ny = lay.note_area.y + static_cast<float>(rng() % 1000) /
                                                                   1000.0f * (lay.note_area.h - nh);
                            names[i] = RectF{nx, ny, nw, nh};
                        }
                        const std::span<const RectF> avoid(names.data(), name_count);
                        const std::size_t shown = place_grade_words(
                            p, avoid, std::span<const GradeWordIn>(words.data(), n),
                            std::span<GradeWordOut>(out.data(), n));
                        std::size_t count = 0;
                        for (std::size_t i = 0; i < n; ++i) {
                            if (!out[i].visible) {
                                CHECK(out[i].rect.empty());
                                continue;
                            }
                            ++count;
                            const RectF& r = out[i].rect;
                            CHECK(inside(r, lay.note_area));
                            CHECK(inside(r, band));
                            CHECK_FALSE(intersects(r, lay.chord_label));
                            if (pos == GradePosition::Top && chord) {
                                CHECK(r.y >= lay.chord_label.bottom());
                            }
                            for (const RectF& name : avoid) {
                                CHECK_FALSE(intersects(r, name));
                            }
                            for (std::size_t j = i + 1; j < n; ++j) {
                                if (out[j].visible) {
                                    CHECK_FALSE(intersects(r, out[j].rect));
                                }
                            }
                        }
                        CHECK(count == shown);
                        placed_total += count;
                    }
                }
            }
        }
    }
    CHECK(placed_total > 1000); // the sweep really placed words
}

TEST_CASE("the Bach prelude's crowd stays clear: eight words on neighbouring keys") {
    // Neighbouring white keys 40 px apart with 100 px words: they cannot share a row.
    std::array<GradeWordIn, 8> words{};
    for (std::size_t i = 0; i < words.size(); ++i) {
        words[i] = {200.0f + 40.0f * static_cast<float>(i), 100.0f, 0.1f * static_cast<float>(i)};
    }
    std::array<GradeWordOut, 8> out{};
    const std::size_t shown = place_grade_words(simple(GradePosition::Bottom), {}, words, out);
    CHECK(shown >= 2);
    CHECK(shown < words.size()); // the crowd thins out: older words gave way
    for (std::size_t i = 0; i < words.size(); ++i) {
        for (std::size_t j = i + 1; j < words.size(); ++j) {
            if (out[i].visible && out[j].visible) {
                CHECK_FALSE(intersects(out[i].rect, out[j].rect));
            }
        }
    }
    // The newest word is always among those shown.
    CHECK(out[0].visible);
}
