// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace toonval {

// An sRGB color, 8 bits per channel, straight (not premultiplied) alpha.
struct RGBA {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 255;

    friend bool operator==(const RGBA&, const RGBA&) = default;
};

// A color in the OKLab space (L 0..1, a and b about -0.4..0.4). Euclidean distance in it tracks
// how different two colors look.
struct Oklab {
    double L = 0.0;
    double a = 0.0;
    double b = 0.0;
};

// Hue in degrees, any value (wraps). Saturation, value, lightness and alpha are clamped to 0..1.
RGBA from_hsv(double hue_deg, double saturation, double value, float alpha = 1.0f);
RGBA from_hsl(double hue_deg, double saturation, double lightness, float alpha = 1.0f);

// OKLCH: lightness 0..1, chroma (0 is grey, about 0.15 is vivid), hue in degrees. A color outside
// the sRGB gamut keeps its lightness and hue and loses chroma until it fits.
RGBA from_oklch(double lightness, double chroma, double hue_deg, float alpha = 1.0f);

Oklab to_oklab(RGBA c);
// Distance in OKLab: 0 for equal colors, 1 between black and white. About 0.02 is the smallest
// difference most people can see, and 0.1 or more reads as clearly different hues.
double perceptual_distance(RGBA a, RGBA b);

// Per-channel mix in sRGB, alpha included. `t` is clamped to 0..1.
RGBA lerp(RGBA a, RGBA b, float t);
// Mix toward white or black by `amount` (0..1). Alpha stays.
RGBA lighten(RGBA c, float amount);
RGBA darken(RGBA c, float amount);

// WCAG 2.x relative luminance (0 black, 1 white) and contrast ratio (1 to 21). Alpha is ignored:
// both functions look at the opaque color.
double relative_luminance(RGBA c);
double contrast_ratio(RGBA a, RGBA b);

// Moves `fg` toward white (dark `bg`) or black (light `bg`) by the smallest step that reaches
// `min_ratio` against `bg`. Returns `fg` unchanged when it already does. Ratios above what white
// or black can reach give white or black.
RGBA ensure_contrast(RGBA fg, RGBA bg, double min_ratio);

// "#RRGGBB", or "#RRGGBBAA" when alpha is not 255.
std::string to_hex(RGBA c);
// Accepts "#RRGGBB" and "#RRGGBBAA" in either case. Leaves `*out` alone and returns false for
// anything else.
bool parse_hex(std::string_view text, RGBA* out);

} // namespace toonval
