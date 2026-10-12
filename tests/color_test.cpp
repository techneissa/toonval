// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/color.hpp"

#include <cmath>

#include <doctest/doctest.h>

using namespace toonval;

namespace {
bool near(double a, double b, double eps) { return std::fabs(a - b) <= eps; }
} // namespace

TEST_CASE("hsv hits the primary and secondary colors") {
    CHECK(from_hsv(0, 1, 1) == RGBA{255, 0, 0, 255});
    CHECK(from_hsv(60, 1, 1) == RGBA{255, 255, 0, 255});
    CHECK(from_hsv(120, 1, 1) == RGBA{0, 255, 0, 255});
    CHECK(from_hsv(180, 1, 1) == RGBA{0, 255, 255, 255});
    CHECK(from_hsv(240, 1, 1) == RGBA{0, 0, 255, 255});
    CHECK(from_hsv(300, 1, 1) == RGBA{255, 0, 255, 255});
}

TEST_CASE("hsv wraps the hue and clamps saturation and value") {
    CHECK(from_hsv(360, 1, 1) == from_hsv(0, 1, 1));
    CHECK(from_hsv(-120, 1, 1) == from_hsv(240, 1, 1));
    CHECK(from_hsv(720 + 60, 1, 1) == from_hsv(60, 1, 1));
    CHECK(from_hsv(0, 0, 1) == RGBA{255, 255, 255, 255});
    CHECK(from_hsv(0, 5, 3) == from_hsv(0, 1, 1));
    CHECK(from_hsv(0, 1, -1) == RGBA{0, 0, 0, 255});
    CHECK(from_hsv(0, 1, 1, 0.5f).a == 128);
}

TEST_CASE("hsl hits known values") {
    CHECK(from_hsl(0, 1, 0.5) == RGBA{255, 0, 0, 255});
    CHECK(from_hsl(240, 1, 0.5) == RGBA{0, 0, 255, 255});
    CHECK(from_hsl(0, 0, 0.5) == RGBA{128, 128, 128, 255});
    CHECK(from_hsl(123, 0.4, 1.0) == RGBA{255, 255, 255, 255});
    CHECK(from_hsl(123, 0.4, 0.0) == RGBA{0, 0, 0, 255});
}

TEST_CASE("lerp mixes channels and alpha and clamps t") {
    const RGBA black{0, 0, 0, 0};
    const RGBA white{255, 255, 255, 255};
    CHECK(lerp(black, white, 0.0f) == black);
    CHECK(lerp(black, white, 1.0f) == white);
    CHECK(lerp(black, white, 0.5f) == RGBA{128, 128, 128, 128});
    CHECK(lerp(black, white, -3.0f) == black);
    CHECK(lerp(black, white, 7.0f) == white);
}

TEST_CASE("lighten moves toward white, darken toward black") {
    const RGBA c{100, 50, 200, 77};
    CHECK(lighten(c, 0.0f) == c);
    CHECK(lighten(c, 1.0f) == RGBA{255, 255, 255, 77});
    CHECK(darken(c, 0.0f) == c);
    CHECK(darken(c, 1.0f) == RGBA{0, 0, 0, 77});
    CHECK(darken(RGBA{200, 100, 50}, 0.5f) == RGBA{100, 50, 25, 255});
    CHECK(lighten(RGBA{0, 0, 0}, 0.5f) == RGBA{128, 128, 128, 255});
}

TEST_CASE("relative luminance follows WCAG 2.x") {
    CHECK(near(relative_luminance(RGBA{0, 0, 0}), 0.0, 1e-9));
    CHECK(near(relative_luminance(RGBA{255, 255, 255}), 1.0, 1e-9));
    CHECK(near(relative_luminance(RGBA{255, 0, 0}), 0.2126, 1e-4));
    CHECK(near(relative_luminance(RGBA{0, 255, 0}), 0.7152, 1e-4));
    CHECK(near(relative_luminance(RGBA{0, 0, 255}), 0.0722, 1e-4));
    // sRGB 119 is the classic "#777" grey, luminance 0.1845.
    CHECK(near(relative_luminance(RGBA{119, 119, 119}), 0.1845, 1e-3));
}

TEST_CASE("contrast ratio follows WCAG 2.x") {
    const RGBA black{0, 0, 0};
    const RGBA white{255, 255, 255};
    CHECK(near(contrast_ratio(black, white), 21.0, 1e-9));
    CHECK(near(contrast_ratio(white, black), 21.0, 1e-9));
    CHECK(near(contrast_ratio(white, white), 1.0, 1e-9));
    // #767676 on white is the well-known smallest grey that passes 4.5:1.
    CHECK(contrast_ratio(RGBA{0x76, 0x76, 0x76}, white) >= 4.5);
    CHECK(contrast_ratio(RGBA{0x77, 0x77, 0x77}, white) < 4.5);
}

TEST_CASE("oklab distance is zero for equal colors and about 1 for black vs white") {
    const RGBA a{90, 170, 255};
    CHECK(perceptual_distance(a, a) == 0.0);
    CHECK(near(perceptual_distance(RGBA{0, 0, 0}, RGBA{255, 255, 255}), 1.0, 0.01));
    CHECK(near(to_oklab(RGBA{255, 255, 255}).L, 1.0, 1e-3));
    CHECK(near(to_oklab(RGBA{0, 0, 0}).L, 0.0, 1e-9));
    // Pure red from the reference implementation: L 0.628, a 0.225, b 0.126.
    const Oklab red = to_oklab(RGBA{255, 0, 0});
    CHECK(near(red.L, 0.628, 2e-3));
    CHECK(near(red.a, 0.225, 2e-3));
    CHECK(near(red.b, 0.126, 2e-3));
}

TEST_CASE("oklch converts back to the lightness it was given and stays in gamut") {
    for (int hue = 0; hue < 360; hue += 15) {
        const RGBA c = from_oklch(0.75, 0.12, hue);
        CHECK(near(to_oklab(c).L, 0.75, 0.01));
    }
    // A chroma no display can show is pulled in instead of producing garbage.
    const RGBA wild = from_oklch(0.5, 5.0, 140);
    CHECK(near(to_oklab(wild).L, 0.5, 0.02));
    CHECK(from_oklch(1.0, 0.0, 0) == RGBA{255, 255, 255, 255});
    const RGBA dark = from_oklch(0.0, 0.2, 0);
    CHECK(dark.r <= 2);
    CHECK(dark.g <= 2);
    CHECK(dark.b <= 2);
}

TEST_CASE("ensure_contrast lightens on a dark background until the ratio holds") {
    const RGBA bg{18, 20, 26};
    const RGBA dim{40, 40, 90};
    REQUIRE(contrast_ratio(dim, bg) < 4.5);
    const RGBA fixed = ensure_contrast(dim, bg, 4.5);
    CHECK(contrast_ratio(fixed, bg) >= 4.5);
    // Already fine: unchanged.
    const RGBA bright{250, 250, 120};
    CHECK(ensure_contrast(bright, bg, 4.5) == bright);
    // On a light background it darkens.
    const RGBA light{250, 250, 250};
    const RGBA pale{200, 220, 255};
    CHECK(contrast_ratio(ensure_contrast(pale, light, 4.5), light) >= 4.5);
}

TEST_CASE("hex text round-trips and rejects bad input") {
    CHECK(to_hex(RGBA{90, 170, 255, 255}) == "#5AAAFF");
    CHECK(to_hex(RGBA{1, 2, 3, 4}) == "#01020304");
    RGBA out;
    CHECK(parse_hex("#5AAAFF", &out));
    CHECK(out == RGBA{90, 170, 255, 255});
    CHECK(parse_hex("#5aaaff80", &out));
    CHECK(out == RGBA{90, 170, 255, 128});
    CHECK_FALSE(parse_hex("5AAAFF", &out));
    CHECK_FALSE(parse_hex("#5AAAF", &out));
    CHECK_FALSE(parse_hex("#5AAAFG", &out));
    CHECK_FALSE(parse_hex("", &out));
    CHECK_FALSE(parse_hex("#5AAAFF00FF", &out));
}
