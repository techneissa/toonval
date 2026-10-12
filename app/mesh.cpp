// SPDX-License-Identifier: GPL-3.0-or-later
#include "mesh.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace toonval::app {

namespace {

// Segments per rounded corner. Four is smooth at note-block sizes and keeps the vertex count low.
constexpr int kCornerSegments = 4;
constexpr int kCornerPoints = kCornerSegments + 1;
constexpr int kOutlinePoints = 4 * kCornerPoints;

struct Direction {
    float x;
    float y;
};

// Unit vectors around the outline, clockwise on screen (y down) from the top-left corner. Each
// doubles as the outward normal at its outline point, so the feather ring follows the corners.
const std::array<Direction, kOutlinePoints>& outline_directions() {
    static const std::array<Direction, kOutlinePoints> table = [] {
        std::array<Direction, kOutlinePoints> t{};
        constexpr float kQuarter = 1.57079632679f;
        for (int corner = 0; corner < 4; ++corner) {
            // Top-left starts at 180 degrees, then top-right 270, bottom-right 0, bottom-left 90.
            const float start = kQuarter * static_cast<float>((corner + 2) % 4);
            for (int i = 0; i < kCornerPoints; ++i) {
                const float a =
                    start + kQuarter * static_cast<float>(i) / static_cast<float>(kCornerSegments);
                t[static_cast<std::size_t>(corner * kCornerPoints + i)] = {std::cos(a),
                                                                           std::sin(a)};
            }
        }
        return t;
    }();
    return table;
}

SDL_Vertex vertex(float x, float y, SDL_FColor c) { return {{x, y}, c, {0.0f, 0.0f}}; }
SDL_Vertex vertex(float x, float y, SDL_FColor c, float u, float v) { return {{x, y}, c, {u, v}}; }

// The outline of a rounded rectangle shrunk by `inset` (corner radius shrinking with it), as
// kOutlinePoints positions in the order of outline_directions().
void outline(const SDL_FRect& r, float radius, float inset, std::array<SDL_FPoint, kOutlinePoints>& out) {
    const float left = r.x + radius;
    const float right = r.x + r.w - radius;
    const float upper = r.y + radius;
    const float lower = r.y + r.h - radius;
    const float rr = std::max(0.0f, radius - inset);
    const auto& dirs = outline_directions();
    for (int i = 0; i < kOutlinePoints; ++i) {
        const int corner = i / kCornerPoints;
        // When the inset is deeper than the radius the corner centres move in too.
        const float extra = std::max(0.0f, inset - radius);
        const float cx = (corner == 0 || corner == 3) ? std::min(left + extra, r.x + r.w * 0.5f)
                                                      : std::max(right - extra, r.x + r.w * 0.5f);
        const float cy = (corner < 2) ? std::min(upper + extra, r.y + r.h * 0.5f)
                                      : std::max(lower - extra, r.y + r.h * 0.5f);
        const Direction d = dirs[static_cast<std::size_t>(i)];
        out[static_cast<std::size_t>(i)] = {cx + d.x * rr, cy + d.y * rr};
    }
}

} // namespace

void Mesh::rect(const SDL_FRect& r, SDL_FColor top, SDL_FColor bottom) {
    const int base = static_cast<int>(vertices_.size());
    vertices_.push_back(vertex(r.x, r.y, top));
    vertices_.push_back(vertex(r.x + r.w, r.y, top));
    vertices_.push_back(vertex(r.x + r.w, r.y + r.h, bottom));
    vertices_.push_back(vertex(r.x, r.y + r.h, bottom));
    for (int i : {0, 1, 2, 0, 2, 3}) {
        indices_.push_back(base + i);
    }
}

void Mesh::triangle(SDL_FPoint a, SDL_FPoint b, SDL_FPoint c, SDL_FColor ca, SDL_FColor cb,
                    SDL_FColor cc) {
    const int base = static_cast<int>(vertices_.size());
    vertices_.push_back(vertex(a.x, a.y, ca));
    vertices_.push_back(vertex(b.x, b.y, cb));
    vertices_.push_back(vertex(c.x, c.y, cc));
    for (int i : {0, 1, 2}) {
        indices_.push_back(base + i);
    }
}

void Mesh::rounded_rect(const SDL_FRect& r, float radius, SDL_FColor top, SDL_FColor bottom,
                        float feather) {
    if (r.w <= 0.0f || r.h <= 0.0f) {
        return;
    }
    radius = std::clamp(radius, 0.0f, std::min(r.w, r.h) * 0.5f);
    const float left = r.x + radius;
    const float right = r.x + r.w - radius;
    const float upper = r.y + radius;
    const float lower = r.y + r.h - radius;
    const auto colour_at = [&](float y) { return mix(top, bottom, (y - r.y) / r.h); };

    const int centre = static_cast<int>(vertices_.size());
    vertices_.push_back(vertex(r.x + r.w * 0.5f, r.y + r.h * 0.5f, colour_at(r.y + r.h * 0.5f)));
    const int inner = centre + 1;
    const auto& dirs = outline_directions();
    for (int i = 0; i < kOutlinePoints; ++i) {
        const int corner = i / kCornerPoints;
        const float cx = (corner == 0 || corner == 3) ? left : right;
        const float cy = (corner < 2) ? upper : lower;
        const Direction d = dirs[static_cast<std::size_t>(i)];
        const float y = cy + d.y * radius;
        vertices_.push_back(vertex(cx + d.x * radius, y, colour_at(y)));
    }
    for (int i = 0; i < kOutlinePoints; ++i) {
        const int next = (i + 1) % kOutlinePoints;
        indices_.push_back(centre);
        indices_.push_back(inner + i);
        indices_.push_back(inner + next);
    }
    if (feather <= 0.0f) {
        return;
    }
    const int outer = inner + kOutlinePoints;
    for (int i = 0; i < kOutlinePoints; ++i) {
        const SDL_Vertex& v = vertices_[static_cast<std::size_t>(inner + i)];
        const Direction d = dirs[static_cast<std::size_t>(i)];
        vertices_.push_back(vertex(v.position.x + d.x * feather, v.position.y + d.y * feather,
                                   with_alpha(v.color, 0.0f)));
    }
    for (int i = 0; i < kOutlinePoints; ++i) {
        const int next = (i + 1) % kOutlinePoints;
        for (int idx :
             {inner + i, outer + i, outer + next, inner + i, outer + next, inner + next}) {
            indices_.push_back(idx);
        }
    }
}

void Mesh::ring(const SDL_FRect& r, float radius, float width, SDL_FColor outer,
                SDL_FColor inner) {
    if (r.w <= 0.0f || r.h <= 0.0f || width <= 0.0f) {
        return;
    }
    radius = std::clamp(radius, 0.0f, std::min(r.w, r.h) * 0.5f);
    width = std::min(width, std::min(r.w, r.h) * 0.5f);
    std::array<SDL_FPoint, kOutlinePoints> out_pts{};
    std::array<SDL_FPoint, kOutlinePoints> in_pts{};
    outline(r, radius, 0.0f, out_pts);
    outline(r, radius, width, in_pts);
    const int base = static_cast<int>(vertices_.size());
    for (int i = 0; i < kOutlinePoints; ++i) {
        const SDL_FPoint p = out_pts[static_cast<std::size_t>(i)];
        vertices_.push_back(vertex(p.x, p.y, outer));
    }
    for (int i = 0; i < kOutlinePoints; ++i) {
        const SDL_FPoint p = in_pts[static_cast<std::size_t>(i)];
        vertices_.push_back(vertex(p.x, p.y, inner));
    }
    for (int i = 0; i < kOutlinePoints; ++i) {
        const int next = (i + 1) % kOutlinePoints;
        const int o = base + i;
        const int on = base + next;
        const int in = base + kOutlinePoints + i;
        const int inn = base + kOutlinePoints + next;
        for (int idx : {o, in, inn, o, inn, on}) {
            indices_.push_back(idx);
        }
    }
}

void Mesh::rounded_rect_tiled(const SDL_FRect& r, float radius, SDL_FColor colour, float tile) {
    if (r.w <= 0.0f || r.h <= 0.0f || tile <= 0.0f) {
        return;
    }
    radius = std::clamp(radius, 0.0f, std::min(r.w, r.h) * 0.5f);
    std::array<SDL_FPoint, kOutlinePoints> pts{};
    outline(r, radius, 0.0f, pts);
    const float k = 1.0f / tile;
    const int centre = static_cast<int>(vertices_.size());
    vertices_.push_back(vertex(r.x + r.w * 0.5f, r.y + r.h * 0.5f, colour, r.w * 0.5f * k,
                               r.h * 0.5f * k));
    for (int i = 0; i < kOutlinePoints; ++i) {
        const SDL_FPoint p = pts[static_cast<std::size_t>(i)];
        vertices_.push_back(vertex(p.x, p.y, colour, (p.x - r.x) * k, (p.y - r.y) * k));
    }
    for (int i = 0; i < kOutlinePoints; ++i) {
        const int next = (i + 1) % kOutlinePoints;
        indices_.push_back(centre);
        indices_.push_back(centre + 1 + i);
        indices_.push_back(centre + 1 + next);
    }
}

void Mesh::nine_slice(const SDL_FRect& r, float corner, SDL_FColor colour) {
    if (r.w <= 0.0f || r.h <= 0.0f || corner <= 0.0f) {
        return;
    }
    const float cx = std::min(corner, r.w * 0.5f);
    const float cy = std::min(corner, r.h * 0.5f);
    const float xs[4] = {r.x, r.x + cx, r.x + r.w - cx, r.x + r.w};
    const float ys[4] = {r.y, r.y + cy, r.y + r.h - cy, r.y + r.h};
    // A corner cut short (a rectangle smaller than two corners) samples less of the falloff.
    const float us[4] = {0.0f, 0.5f * cx / corner, 1.0f - 0.5f * cx / corner, 1.0f};
    const float vs[4] = {0.0f, 0.5f * cy / corner, 1.0f - 0.5f * cy / corner, 1.0f};
    const int base = static_cast<int>(vertices_.size());
    for (int j = 0; j < 4; ++j) {
        for (int i = 0; i < 4; ++i) {
            vertices_.push_back(vertex(xs[i], ys[j], colour, us[i], vs[j]));
        }
    }
    for (int j = 0; j < 3; ++j) {
        for (int i = 0; i < 3; ++i) {
            const int a = base + j * 4 + i;
            for (int idx : {a, a + 1, a + 5, a, a + 5, a + 4}) {
                indices_.push_back(idx);
            }
        }
    }
}

void Mesh::quad(const SDL_FPoint (&p)[4], SDL_FColor colour) {
    const int base = static_cast<int>(vertices_.size());
    vertices_.push_back(vertex(p[0].x, p[0].y, colour, 0.0f, 0.0f));
    vertices_.push_back(vertex(p[1].x, p[1].y, colour, 1.0f, 0.0f));
    vertices_.push_back(vertex(p[2].x, p[2].y, colour, 1.0f, 1.0f));
    vertices_.push_back(vertex(p[3].x, p[3].y, colour, 0.0f, 1.0f));
    for (int i : {0, 1, 2, 0, 2, 3}) {
        indices_.push_back(base + i);
    }
}

void Mesh::draw(SDL_Renderer* renderer, SDL_BlendMode mode, SDL_Texture* texture) const {
    if (indices_.empty()) {
        return;
    }
    SDL_SetRenderDrawBlendMode(renderer, mode);
    if (texture != nullptr) {
        SDL_SetTextureBlendMode(texture, mode);
    }
    SDL_RenderGeometry(renderer, texture, vertices_.data(), static_cast<int>(vertices_.size()),
                       indices_.data(), static_cast<int>(indices_.size()));
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
}

} // namespace toonval::app
