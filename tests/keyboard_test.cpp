// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/keyboard.hpp"
#include "toonval/pitch.hpp"

#include <doctest/doctest.h>

using namespace toonval;

TEST_CASE("standard ranges have the right key counts") {
    CHECK(kRange88.key_count() == 88);
    CHECK(kRange76.key_count() == 76);
    CHECK(kRange61.key_count() == 61);
    CHECK(kRange49.key_count() == 49);
    for (auto r : {kRange88, kRange76, kRange61, kRange49}) {
        CHECK(r.valid());
    }
}

TEST_CASE("ranges ending on black keys are invalid") {
    CHECK_FALSE(KeyRange{37, 96}.valid());
    CHECK_FALSE(KeyRange{36, 97}.valid());
    CHECK_FALSE(KeyRange{96, 36}.valid());
    CHECK_FALSE(KeyRange{-1, 60}.valid());
}

TEST_CASE("PSR-E383 profile is found by USB device name") {
    const auto* profile = find_profile("PSR-E383 MIDI 1");
    REQUIRE(profile != nullptr);
    CHECK(profile->range.lowest == 36);
    CHECK(profile->range.highest == 96);
    CHECK(find_profile("Unknown Controller") == nullptr);
}

TEST_CASE("white key counts") {
    CHECK(KeyboardLayout(kRange88).white_key_count() == 52);
    CHECK(KeyboardLayout(kRange61).white_key_count() == 36);
    CHECK(KeyboardLayout(kRange49).white_key_count() == 29);
}

TEST_CASE("white keys tile the full width with no gaps") {
    const KeyboardLayout layout(kRange61);
    float expected_x = 0.0f;
    for (int note = kRange61.lowest; note <= kRange61.highest; ++note) {
        const KeyRect k = layout.key(note);
        CHECK(k.black == is_black_key(note));
        if (k.black) {
            continue;
        }
        CHECK(k.x == doctest::Approx(expected_x));
        CHECK(k.width == doctest::Approx(layout.white_width()));
        expected_x += k.width;
    }
    CHECK(expected_x == doctest::Approx(1.0f));
}

TEST_CASE("black keys sit centred between their white neighbours") {
    const KeyboardLayout layout(kRange61);
    const KeyRect c = layout.key(60);
    const KeyRect c_sharp = layout.key(61);
    const KeyRect d = layout.key(62);
    CHECK(c_sharp.black);
    CHECK(c_sharp.width < c.width);
    CHECK(c_sharp.x + c_sharp.width / 2.0f == doctest::Approx(d.x));
    CHECK(c_sharp.x > c.x);
    CHECK(c_sharp.x + c_sharp.width < d.x + d.width);
}

TEST_CASE("notes outside the range have no rect") {
    const KeyboardLayout layout(kRange61);
    CHECK(layout.key(35).width == 0.0f);
    CHECK(layout.key(97).width == 0.0f);
}

TEST_CASE("a range with a black-key end widens outward to the nearest white key") {
    const KeyboardLayout low(KeyRange{37, 96});
    CHECK(low.range().lowest == 36);
    CHECK(low.range().highest == 96);
    CHECK(low.adjusted());

    const KeyboardLayout high(KeyRange{36, 97});
    CHECK(high.range().lowest == 36);
    CHECK(high.range().highest == 98);
    CHECK(high.adjusted());

    const KeyboardLayout both(KeyRange{1, 126});
    CHECK(both.range().lowest == 0);
    CHECK(both.range().highest == 127);
    CHECK(both.adjusted());
}

TEST_CASE("an out-of-range or unordered range falls back to 88 keys") {
    for (KeyRange r : {KeyRange{-1, 60}, KeyRange{60, 128}, KeyRange{96, 36}, KeyRange{200, 300}}) {
        const KeyboardLayout layout(r);
        CHECK(layout.range().lowest == kRange88.lowest);
        CHECK(layout.range().highest == kRange88.highest);
        CHECK(layout.adjusted());
    }
}

TEST_CASE("a valid range is kept and not reported as adjusted") {
    for (auto r : {kRange88, kRange76, kRange61, kRange49, KeyRange{0, 127}, KeyRange{60, 60}}) {
        const KeyboardLayout layout(r);
        CHECK(layout.range().lowest == r.lowest);
        CHECK(layout.range().highest == r.highest);
        CHECK_FALSE(layout.adjusted());
    }
}

TEST_CASE("unusual white-note range ends lay out correctly") {
    CHECK(kRange76.key_count() == 76);
    CHECK(KeyboardLayout(kRange76).white_key_count() == 45);
    CHECK(KeyboardLayout(KeyRange{0, 127}).white_key_count() == 75);
    CHECK(KeyboardLayout(KeyRange{0, 0}).white_key_count() == 1);
    CHECK(KeyboardLayout(KeyRange{127, 127}).white_key_count() == 1);
    // Ranges starting or ending on E/F and B/C boundaries.
    CHECK(KeyboardLayout(KeyRange{40, 77}).white_key_count() == 23);
    CHECK(KeyboardLayout(KeyRange{47, 72}).white_key_count() == 16);
}

TEST_CASE("white keys sum to full width for every standard and edge range") {
    for (auto r : {kRange88, kRange76, kRange61, kRange49, KeyRange{0, 127}, KeyRange{28, 103}}) {
        const KeyboardLayout layout(r);
        float total = 0.0f;
        for (int n = r.lowest; n <= r.highest; ++n) {
            const KeyRect k = layout.key(n);
            if (!k.black) {
                total += k.width;
            }
        }
        CHECK(total == doctest::Approx(1.0f));
        CHECK(layout.white_width() * static_cast<float>(layout.white_key_count()) ==
              doctest::Approx(1.0f));
    }
}

TEST_CASE("key() outside the range is a zero-width rect at every distance") {
    const KeyboardLayout layout(kRange61);
    for (int n : {-1000, -1, 0, 35, 97, 127, 128, 1000}) {
        const KeyRect k = layout.key(n);
        CHECK(k.width == 0.0f);
        CHECK(k.x == 0.0f);
        CHECK_FALSE(k.black);
    }
    const KeyboardLayout full(KeyRange{0, 127});
    CHECK(full.key(-1).width == 0.0f);
    CHECK(full.key(128).width == 0.0f);
}

TEST_CASE("every black key in every octave is centred on a white-key boundary") {
    const KeyboardLayout layout(KeyRange{0, 127});
    for (int n = 0; n <= 127; ++n) {
        if (!is_black_key(n)) {
            continue;
        }
        const KeyRect k = layout.key(n);
        const KeyRect below = layout.key(n - 1);
        const KeyRect above = layout.key(n + 1);
        CHECK(k.black);
        CHECK(k.width == doctest::Approx(layout.white_width() * KeyboardLayout::kBlackWidthRatio));
        CHECK(k.x + k.width / 2.0f == doctest::Approx(below.x + below.width));
        CHECK(k.x + k.width / 2.0f == doctest::Approx(above.x));
    }
}
