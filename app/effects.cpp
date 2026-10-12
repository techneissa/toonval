// SPDX-License-Identifier: GPL-3.0-or-later
#include "effects.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace toonval::app {

namespace {

const SDL_FColor kWhite{1.0f, 1.0f, 1.0f, 1.0f};
const SDL_FColor kBlack{0.0f, 0.0f, 0.0f, 1.0f};
constexpr float kTau = 6.28318530718f;
// One grain texel covers kGrainTilePx / kSize 1080p pixels: 2 px grains.
constexpr float kGrainTilePx = 128.0f;

SDL_Texture* make_texture(SDL_Renderer* renderer, const std::vector<std::uint32_t>& pixels,
                          int size) {
    SDL_Surface* surface =
        SDL_CreateSurfaceFrom(size, size, SDL_PIXELFORMAT_RGBA32,
                              const_cast<std::uint32_t*>(pixels.data()), size * 4);
    if (surface == nullptr) {
        return nullptr;
    }
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_DestroySurface(surface);
    if (texture != nullptr) {
        SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
    }
    return texture;
}

// White with alpha `a`, in SDL_PIXELFORMAT_RGBA32's byte order.
std::uint32_t white_alpha(float a) {
    const auto alpha = static_cast<std::uint32_t>(std::lround(std::clamp(a, 0.0f, 1.0f) * 255.0f));
    const std::uint8_t bytes[4] = {255, 255, 255, static_cast<std::uint8_t>(alpha)};
    std::uint32_t out = 0;
    std::memcpy(&out, bytes, 4);
    return out;
}

std::uint32_t hash(std::uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

} // namespace

EffectTextures::EffectTextures(SDL_Renderer* renderer) {
    constexpr int n = kSize;
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(n * n));
    // Glow: distance from the centre point, so the corners are round falloffs and the middle
    // lines (the nine-slice's stretched edges) a straight one. Gaussian-like, reaching 0 at the rim.
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            const float dx = std::max(0.0f, std::abs(static_cast<float>(x) + 0.5f - n * 0.5f) - 0.5f);
            const float dy = std::max(0.0f, std::abs(static_cast<float>(y) + 0.5f - n * 0.5f) - 0.5f);
            const float d = std::min(1.0f, std::sqrt(dx * dx + dy * dy) / (n * 0.5f - 0.5f));
            const float edge = 1.0f - d * d * (3.0f - 2.0f * d); // smooth to 0 at the rim
            const float a = std::exp(-3.2f * d * d) * edge;
            pixels[static_cast<std::size_t>(y * n + x)] = white_alpha(a);
        }
    }
    glow_ = make_texture(renderer, pixels, n);

    // Grain: white noise softened by a wrapping blur and mixed with a coarser layer, so it tiles
    // without seams and reads as frost rather than static.
    std::vector<float> fine(static_cast<std::size_t>(n * n));
    for (int i = 0; i < n * n; ++i) {
        fine[static_cast<std::size_t>(i)] =
            static_cast<float>(hash(static_cast<std::uint32_t>(i) + 0x5bd1e995u) & 0xffffu) /
            65535.0f;
    }
    const auto at = [&](const std::vector<float>& v, int x, int y) {
        return v[static_cast<std::size_t>(((y + n) % n) * n + (x + n) % n)];
    };
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            float blur = 0.0f;
            for (int j = -1; j <= 1; ++j) {
                for (int i = -1; i <= 1; ++i) {
                    blur += at(fine, x + i, y + j);
                }
            }
            blur /= 9.0f;
            // Coarse cells of 8 texels, smoothly interpolated, for patches of frost.
            const int cx = x / 8;
            const int cy = y / 8;
            const float fx = static_cast<float>(x % 8) / 8.0f;
            const float fy = static_cast<float>(y % 8) / 8.0f;
            const auto cell = [&](int i, int j) {
                const int ii = (i + n / 8) % (n / 8);
                const int jj = (j + n / 8) % (n / 8);
                return static_cast<float>(hash(static_cast<std::uint32_t>(jj * 97 + ii)) & 0xffu) /
                       255.0f;
            };
            const float top = cell(cx, cy) + (cell(cx + 1, cy) - cell(cx, cy)) * fx;
            const float bottom = cell(cx, cy + 1) + (cell(cx + 1, cy + 1) - cell(cx, cy + 1)) * fx;
            const float coarse = top + (bottom - top) * fy;
            const float g = 0.55f * at(fine, x, y) + 0.2f * blur + 0.25f * coarse;
            pixels[static_cast<std::size_t>(y * n + x)] = white_alpha(std::pow(g, 1.6f));
        }
    }
    grain_ = make_texture(renderer, pixels, n);
}

EffectTextures::~EffectTextures() {
    if (glow_ != nullptr) {
        SDL_DestroyTexture(glow_);
    }
    if (grain_ != nullptr) {
        SDL_DestroyTexture(grain_);
    }
}

SDL_FColor saturated(SDL_FColor c, float s) {
    const float grey = 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
    return {std::clamp(grey + (c.r - grey) * s, 0.0f, 1.0f),
            std::clamp(grey + (c.g - grey) * s, 0.0f, 1.0f),
            std::clamp(grey + (c.b - grey) * s, 0.0f, 1.0f), c.a};
}

void StylePainter::begin(const StyleRecipe& recipe, float unit) {
    recipe_ = recipe;
    unit_ = unit;
    body_.clear();
    inner_.clear();
    extras_.clear();
    grain_.clear();
    rim_.clear();
}

void StylePainter::add(const SDL_FRect& rect, float radius, SDL_FColor fill, SDL_FColor key_end,
                       float alpha, SDL_FColor underlay) {
    if (rect.w <= 0.0f || rect.h <= 0.0f || alpha <= 0.0f) {
        return;
    }
    const StyleRecipe& r = recipe_;
    const auto body_colour = [&](SDL_FColor c) {
        return mix(saturated(c, r.saturation), kBlack, r.core_darken);
    };
    const SDL_FColor far = mix(scaled(body_colour(fill), r.top_shade), kWhite, r.top_lighten);
    const SDL_FColor near = body_colour(key_end);
    const float a = alpha * fill.a;
    if (underlay.a > 0.0f) {
        body_.rounded_rect(rect, radius, underlay, underlay, 0.0f);
    }
    body_.rounded_rect(rect, radius, with_alpha(far, r.body_alpha_top * a),
                       with_alpha(near, r.body_alpha_bottom * a), r.edge_feather_px * unit_);

    const float w = rect.w;
    const float inner_w = r.inner_width_share * w;
    if (r.inner_shade_alpha > 0.0f) {
        inner_.ring(rect, radius, inner_w, with_alpha(kBlack, r.inner_shade_alpha * a),
                    with_alpha(kBlack, 0.0f));
    }
    const SDL_FColor bright = mix(saturated(fill, r.saturation), kWhite, 0.35f);
    if (r.inner_glow_alpha > 0.0f) {
        inner_.ring(rect, radius, inner_w, with_alpha(bright, r.inner_glow_alpha * a),
                    with_alpha(bright, 0.0f));
    }

    if (r.bottom_shade_alpha > 0.0f) {
        const float h = std::min(0.5f * rect.h, 1.4f * w);
        extras_.rounded_rect({rect.x, rect.y + rect.h - h, w, h}, radius, with_alpha(kBlack, 0.0f),
                             with_alpha(kBlack, r.bottom_shade_alpha * a), 0.0f);
    }
    if (r.gloss_alpha > 0.0f) {
        // A band down the left part, like light on a rounded candy surface, and a cap at the top.
        const float bw = 0.36f * w;
        const float inset = 0.12f * w;
        extras_.rounded_rect({rect.x + inset, rect.y + inset, bw, rect.h - 2.0f * inset}, 0.5f * bw,
                             with_alpha(kWhite, r.gloss_alpha * a),
                             with_alpha(kWhite, 0.3f * r.gloss_alpha * a), 0.0f);
        const float cap = std::min(0.45f * rect.h, 0.8f * w);
        extras_.rounded_rect({rect.x + 0.06f * w, rect.y + 0.05f * w, 0.88f * w, cap},
                             std::max(0.0f, radius - 0.05f * w),
                             with_alpha(kWhite, 0.75f * r.gloss_alpha * a),
                             with_alpha(kWhite, 0.0f), 0.0f);
    }
    if (r.specular_alpha > 0.0f) {
        const float sw = std::max(1.5f * unit_, 0.1f * w);
        const float top = rect.y + std::max(0.1f * w, 0.7f * radius);
        const float len = std::min(r.specular_length * rect.h, rect.y + rect.h - top - 0.1f * w);
        const float len_capped = r.specular_length < 0.5f ? std::min(len, 1.6f * w) : len;
        extras_.rounded_rect({rect.x + 0.16f * w, top, sw, len_capped}, 0.5f * sw,
                             with_alpha(kWhite, r.specular_alpha * a),
                             with_alpha(kWhite, 0.08f * r.specular_alpha * a), 0.0f);
    }
    if (r.noise_alpha > 0.0f) {
        grain_.rounded_rect_tiled(rect, radius, with_alpha(kWhite, r.noise_alpha * a),
                                  kGrainTilePx * unit_);
    }
    if (r.rim_alpha > 0.0f && r.rim_width_px > 0.0f) {
        const SDL_FColor rim =
            with_alpha(mix(saturated(fill, r.saturation), kWhite, r.rim_lighten), r.rim_alpha * a);
        rim_.ring(rect, radius, r.rim_width_px * unit_, rim, rim);
    }
}

void StylePainter::draw(SDL_Renderer* renderer, const EffectTextures& textures) const {
    body_.draw(renderer, SDL_BLENDMODE_BLEND);
    inner_.draw(renderer, SDL_BLENDMODE_BLEND);
    extras_.draw(renderer, SDL_BLENDMODE_BLEND);
    grain_.draw(renderer, SDL_BLENDMODE_BLEND, textures.grain());
    rim_.draw(renderer, SDL_BLENDMODE_BLEND);
}

float Sparks::random01() {
    rng_ = rng_ * 1664525u + 1013904223u;
    return static_cast<float>(rng_ >> 8) / 16777216.0f;
}

void Sparks::spawn(float x, float y, float key_width, SDL_FColor colour, int velocity, float unit) {
    const float v = static_cast<float>(std::clamp(velocity, 1, 127)) / 127.0f;
    const auto take = [&]() -> Spark& {
        Spark& s = pool_[static_cast<std::size_t>(next_)];
        next_ = (next_ + 1) % kCapacity;
        return s;
    };
    // The burst: a still, wide flash of the note's colour where it lands.
    Spark& burst = take();
    burst = {x, y, 0.0f, 0.0f, 0.0f, 0.3f, 2.4f * key_width + 34.0f * unit, 0.0f,
             mix(colour, kWhite, 0.35f)};
    const int count = 6 + static_cast<int>(10.0f * v);
    for (int i = 0; i < count; ++i) {
        Spark& s = take();
        const float angle = (-0.5f + (random01() - 0.5f) * 0.75f) * kTau * 0.5f; // mostly up
        const float speed = (420.0f + 780.0f * random01()) * (0.7f + 0.5f * v) * unit;
        s.x = x + (random01() - 0.5f) * key_width;
        s.y = y;
        s.vx = std::cos(angle) * speed;
        s.vy = std::sin(angle) * speed;
        s.age = 0.0f;
        s.life = 0.35f + 0.45f * random01();
        s.size = (6.0f + 6.0f * random01()) * unit;
        s.gravity = 1.0f;
        s.colour = mix(colour, kWhite, 0.45f + 0.3f * random01());
    }
}

void Sparks::update(std::int64_t now_ns, float unit) {
    if (!started_) {
        started_ = true;
        last_ns_ = now_ns;
        return;
    }
    const float dt = std::clamp(static_cast<float>(now_ns - last_ns_) * 1e-9f, 0.0f, 0.05f);
    last_ns_ = now_ns;
    const float g = 1700.0f * unit;
    const float drag = std::exp(-1.8f * dt);
    for (Spark& s : pool_) {
        if (s.life <= 0.0f) {
            continue;
        }
        s.age += dt;
        if (s.age >= s.life) {
            s.life = 0.0f;
            continue;
        }
        s.vx *= drag;
        s.vy = s.vy * drag + g * s.gravity * dt;
        s.x += s.vx * dt;
        s.y += s.vy * dt;
    }
}

void Sparks::build(Mesh& mesh) const {
    for (const Spark& s : pool_) {
        if (s.life <= 0.0f) {
            continue;
        }
        const float t = s.age / s.life;
        if (s.gravity == 0.0f) {
            const SDL_FColor c = with_alpha(s.colour, s.colour.a * (1.0f - t));
            // The burst: a flat ellipse along the hit line.
            const float w = s.size * (0.8f + 0.4f * t);
            const float h = 0.35f * w;
            const SDL_FPoint p[4] = {{s.x - w, s.y - h}, {s.x + w, s.y - h}, {s.x + w, s.y + h},
                                     {s.x - w, s.y + h}};
            mesh.quad(p, c);
            continue;
        }
        const SDL_FColor c = with_alpha(s.colour, s.colour.a * (1.0f - t * t));
        // A streak along the direction of travel, longer when faster, with a hot white head.
        const float speed = std::sqrt(s.vx * s.vx + s.vy * s.vy);
        const float dx = speed > 1e-3f ? s.vx / speed : 0.0f;
        const float dy = speed > 1e-3f ? s.vy / speed : -1.0f;
        const float half_len = s.size + 0.018f * speed;
        const float half_w = 0.6f * s.size;
        const SDL_FPoint p[4] = {
            {s.x - dx * half_len - dy * half_w, s.y - dy * half_len + dx * half_w},
            {s.x - dx * half_len + dy * half_w, s.y - dy * half_len - dx * half_w},
            {s.x + dx * half_len + dy * half_w, s.y + dy * half_len - dx * half_w},
            {s.x + dx * half_len - dy * half_w, s.y + dy * half_len + dx * half_w}};
        mesh.quad(p, c);
        const float head = 0.8f * s.size;
        const SDL_FPoint q[4] = {{s.x - head, s.y - head}, {s.x + head, s.y - head},
                                 {s.x + head, s.y + head}, {s.x - head, s.y + head}};
        mesh.quad(q, with_alpha(mix(s.colour, kWhite, 0.6f), c.a));
    }
}

void Sparks::clear() {
    for (Spark& s : pool_) {
        s.life = 0.0f;
    }
}

int Sparks::live() const {
    return static_cast<int>(
        std::count_if(pool_.begin(), pool_.end(), [](const Spark& s) { return s.life > 0.0f; }));
}

void build_motion_background(Mesh& mesh, const SDL_FRect& area, double seconds) {
    struct Blob {
        SDL_FColor colour;
        float period_x; // seconds per sweep
        float period_y;
        float phase;
        float size; // of the area's larger side
    };
    static const Blob kBlobs[] = {
        {rgba(90, 160, 255, 0.10f), 61.0f, 43.0f, 0.0f, 0.75f},
        {rgba(150, 100, 255, 0.08f), 47.0f, 71.0f, 2.1f, 0.65f},
        {rgba(40, 200, 190, 0.07f), 83.0f, 53.0f, 4.2f, 0.6f},
    };
    const float big = std::max(area.w, area.h);
    for (const Blob& b : kBlobs) {
        const auto px = static_cast<float>(std::fmod(seconds / b.period_x, 1.0));
        const auto py = static_cast<float>(std::fmod(seconds / b.period_y, 1.0));
        const float cx = area.x + area.w * (0.5f + 0.4f * std::sin(kTau * px + b.phase));
        const float cy = area.y + area.h * (0.5f + 0.32f * std::sin(kTau * py + 1.7f * b.phase));
        const float hw = 0.5f * b.size * big;
        const float hh = 0.5f * hw;
        const SDL_FPoint p[4] = {{cx - hw, cy - hh}, {cx + hw, cy - hh}, {cx + hw, cy + hh},
                                 {cx - hw, cy + hh}};
        mesh.quad(p, b.colour);
    }
}

} // namespace toonval::app
