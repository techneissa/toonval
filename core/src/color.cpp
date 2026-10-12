// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/color.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace toonval {
namespace {

double clamp01(double v) {
    if (!(v > 0.0)) { // also catches NaN
        return 0.0;
    }
    return v > 1.0 ? 1.0 : v;
}

std::uint8_t to_byte(double unit) {
    return static_cast<std::uint8_t>(std::lround(clamp01(unit) * 255.0));
}

double wrap_hue(double deg) {
    if (!std::isfinite(deg)) {
        return 0.0;
    }
    double h = std::fmod(deg, 360.0);
    return h < 0.0 ? h + 360.0 : h;
}

double srgb_to_linear(std::uint8_t v) {
    const double c = static_cast<double>(v) / 255.0;
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double linear_to_srgb(double c) {
    return c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
}

struct Linear {
    double r;
    double g;
    double b;
};

Linear oklab_to_linear(double L, double a, double b) {
    const double l_ = L + 0.3963377774 * a + 0.2158037573 * b;
    const double m_ = L - 0.1055613458 * a - 0.0638541728 * b;
    const double s_ = L - 0.0894841775 * a - 1.2914855480 * b;
    const double l = l_ * l_ * l_;
    const double m = m_ * m_ * m_;
    const double s = s_ * s_ * s_;
    return {4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
            -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
            -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s};
}

bool in_gamut(const Linear& c) {
    constexpr double kEps = 1e-4;
    return c.r >= -kEps && c.r <= 1.0 + kEps && c.g >= -kEps && c.g <= 1.0 + kEps && c.b >= -kEps &&
           c.b <= 1.0 + kEps;
}

std::uint8_t lerp_byte(std::uint8_t a, std::uint8_t b, double t) {
    return static_cast<std::uint8_t>(std::lround(a + (static_cast<double>(b) - a) * t));
}

double clamp_t(float t) {
    return std::isnan(t) ? 0.0 : std::clamp(static_cast<double>(t), 0.0, 1.0);
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

} // namespace

RGBA from_hsv(double hue_deg, double saturation, double value, float alpha) {
    const double s = clamp01(saturation);
    const double v = clamp01(value);
    const double h = wrap_hue(hue_deg) / 60.0;
    const double c = v * s;
    const double x = c * (1.0 - std::fabs(std::fmod(h, 2.0) - 1.0));
    const double m = v - c;
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
    switch (static_cast<int>(h)) {
    case 0:
        r = c;
        g = x;
        break;
    case 1:
        r = x;
        g = c;
        break;
    case 2:
        g = c;
        b = x;
        break;
    case 3:
        g = x;
        b = c;
        break;
    case 4:
        r = x;
        b = c;
        break;
    default:
        r = c;
        b = x;
        break;
    }
    return {to_byte(r + m), to_byte(g + m), to_byte(b + m), to_byte(static_cast<double>(alpha))};
}

RGBA from_hsl(double hue_deg, double saturation, double lightness, float alpha) {
    const double s = clamp01(saturation);
    const double l = clamp01(lightness);
    const double v = l + s * std::min(l, 1.0 - l);
    const double sv = v == 0.0 ? 0.0 : 2.0 * (1.0 - l / v);
    return from_hsv(hue_deg, sv, v, alpha);
}

RGBA from_oklch(double lightness, double chroma, double hue_deg, float alpha) {
    const double L = clamp01(lightness);
    const double rad = wrap_hue(hue_deg) * 3.14159265358979323846 / 180.0;
    const double ca = std::cos(rad);
    const double sa = std::sin(rad);
    double c = std::isfinite(chroma) ? std::max(0.0, chroma) : 0.0;
    if (!in_gamut(oklab_to_linear(L, c * ca, c * sa))) {
        double lo = 0.0; // chroma 0 is always in gamut
        double hi = c;
        for (int i = 0; i < 24; ++i) {
            const double mid = 0.5 * (lo + hi);
            (in_gamut(oklab_to_linear(L, mid * ca, mid * sa)) ? lo : hi) = mid;
        }
        c = lo;
    }
    const Linear lin = oklab_to_linear(L, c * ca, c * sa);
    return {to_byte(linear_to_srgb(clamp01(lin.r))), to_byte(linear_to_srgb(clamp01(lin.g))),
            to_byte(linear_to_srgb(clamp01(lin.b))), to_byte(static_cast<double>(alpha))};
}

Oklab to_oklab(RGBA c) {
    const double r = srgb_to_linear(c.r);
    const double g = srgb_to_linear(c.g);
    const double b = srgb_to_linear(c.b);
    const double l = std::cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
    const double m = std::cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
    const double s = std::cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
    return {0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
            1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
            0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s};
}

double perceptual_distance(RGBA a, RGBA b) {
    const Oklab x = to_oklab(a);
    const Oklab y = to_oklab(b);
    return std::sqrt((x.L - y.L) * (x.L - y.L) + (x.a - y.a) * (x.a - y.a) +
                     (x.b - y.b) * (x.b - y.b));
}

RGBA lerp(RGBA a, RGBA b, float t) {
    const double k = clamp_t(t);
    return {lerp_byte(a.r, b.r, k), lerp_byte(a.g, b.g, k), lerp_byte(a.b, b.b, k),
            lerp_byte(a.a, b.a, k)};
}

RGBA lighten(RGBA c, float amount) {
    const RGBA white{255, 255, 255, c.a};
    return lerp(c, white, amount);
}

RGBA darken(RGBA c, float amount) {
    const RGBA black{0, 0, 0, c.a};
    return lerp(c, black, amount);
}

double relative_luminance(RGBA c) {
    return 0.2126 * srgb_to_linear(c.r) + 0.7152 * srgb_to_linear(c.g) +
           0.0722 * srgb_to_linear(c.b);
}

double contrast_ratio(RGBA a, RGBA b) {
    const double la = relative_luminance(a);
    const double lb = relative_luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

RGBA ensure_contrast(RGBA fg, RGBA bg, double min_ratio) {
    if (contrast_ratio(fg, bg) >= min_ratio) {
        return fg;
    }
    const RGBA target =
        relative_luminance(bg) < 0.5 ? RGBA{255, 255, 255, fg.a} : RGBA{0, 0, 0, fg.a};
    if (contrast_ratio(target, bg) < min_ratio) {
        return target;
    }
    // Smallest step toward the target that passes. Rounding to bytes can undershoot, so the
    // search works on the rounded result.
    float lo = 0.0f; // fails
    float hi = 1.0f; // passes
    for (int i = 0; i < 20; ++i) {
        const float mid = 0.5f * (lo + hi);
        (contrast_ratio(lerp(fg, target, mid), bg) >= min_ratio ? hi : lo) = mid;
    }
    return lerp(fg, target, hi);
}

std::string to_hex(RGBA c) {
    static constexpr char kDigits[] = "0123456789ABCDEF";
    std::string out = "#";
    const std::array<std::uint8_t, 4> bytes{c.r, c.g, c.b, c.a};
    const std::size_t n = c.a == 255 ? 3 : 4;
    for (std::size_t i = 0; i < n; ++i) {
        out += kDigits[bytes[i] >> 4];
        out += kDigits[bytes[i] & 0x0F];
    }
    return out;
}

bool parse_hex(std::string_view text, RGBA* out) {
    if ((text.size() != 7 && text.size() != 9) || text[0] != '#') {
        return false;
    }
    std::array<std::uint8_t, 4> bytes{0, 0, 0, 255};
    for (std::size_t i = 0; i < (text.size() - 1) / 2; ++i) {
        const int hi = hex_digit(text[1 + 2 * i]);
        const int lo = hex_digit(text[2 + 2 * i]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        bytes[i] = static_cast<std::uint8_t>(hi * 16 + lo);
    }
    if (out != nullptr) {
        *out = RGBA{bytes[0], bytes[1], bytes[2], bytes[3]};
    }
    return true;
}

} // namespace toonval
