// SPDX-License-Identifier: GPL-3.0-or-later
// Batched triangles for SDL_RenderGeometry: soft-edged rounded rectangles with vertical gradients,
// outline rings, and textured quads (glow sprites, a tiling grain), so a frame of note blocks and
// keys is a handful of draw calls instead of thousands. A textured mesh is drawn with one texture.
#pragma once

#include <SDL3/SDL.h>

#include <cstddef>
#include <vector>

namespace toonval::app {

class Mesh {
  public:
    void clear() {
        vertices_.clear();
        indices_.clear();
    }

    // A plain rectangle, colour blended from `top` to `bottom`.
    void rect(const SDL_FRect& r, SDL_FColor top, SDL_FColor bottom);

    // A triangle with a colour at each corner.
    void triangle(SDL_FPoint a, SDL_FPoint b, SDL_FPoint c, SDL_FColor ca, SDL_FColor cb,
                  SDL_FColor cc);

    // A rounded rectangle, colour blended from `top` to `bottom`. Its outline fades to transparent
    // over `feather` pixels outward: about 1 pixel smooths the edge, a larger value makes a glow.
    // `radius` shrinks to fit small rectangles.
    void rounded_rect(const SDL_FRect& r, float radius, SDL_FColor top, SDL_FColor bottom,
                      float feather);

    // A band along the inside of a rounded rectangle's outline, `width` pixels deep, coloured
    // `outer` at the outline and `inner` at its inner edge. A crisp rim has both the same; an inner
    // glow or shade fades `inner` to transparent.
    void ring(const SDL_FRect& r, float radius, float width, SDL_FColor outer, SDL_FColor inner);

    // A rounded rectangle textured with a tiling texture: texture coordinates run from 0 at the
    // rectangle's top-left corner and grow by 1 every `tile` pixels.
    void rounded_rect_tiled(const SDL_FRect& r, float radius, SDL_FColor colour, float tile);

    // Nine-slice of a texture whose four corners are its quarters and whose middle is one texel
    // line (a blurred square): `r` is the outer rectangle, `corner` the size of each corner slice.
    // Draws a glow around a rectangle that is `r` shrunk by `corner` on each side.
    void nine_slice(const SDL_FRect& r, float corner, SDL_FColor colour);

    // A whole-texture quad with corners `p[0..3]` clockwise from the top-left of the texture.
    void quad(const SDL_FPoint (&p)[4], SDL_FColor colour);

    std::size_t vertex_count() const { return vertices_.size(); }

    // Draws everything added since clear(), in order, with `mode` (blend or add) and `texture`
    // (none for the untextured shapes).
    void draw(SDL_Renderer* renderer, SDL_BlendMode mode, SDL_Texture* texture = nullptr) const;

  private:
    std::vector<SDL_Vertex> vertices_;
    std::vector<int> indices_;
};

inline SDL_FColor rgba(int r, int g, int b, float a = 1.0f) {
    return {static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f,
            static_cast<float>(b) / 255.0f, a};
}

// `c` with its colour channels multiplied by `k` (alpha unchanged).
inline SDL_FColor scaled(SDL_FColor c, float k) { return {c.r * k, c.g * k, c.b * k, c.a}; }

inline SDL_FColor with_alpha(SDL_FColor c, float a) { return {c.r, c.g, c.b, a}; }

// Blend from `a` to `b` by `t` in 0..1, alpha included.
inline SDL_FColor mix(SDL_FColor a, SDL_FColor b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t,
            a.a + (b.a - a.a) * t};
}

} // namespace toonval::app
