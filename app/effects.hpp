// SPDX-License-Identifier: GPL-3.0-or-later
// The note styles and effects on top of Mesh: the textures they share (made
// once), the painter that turns a toonval::StyleRecipe into a few batched layers, the spark pool
// and the drifting background light. Nothing here allocates per frame once the buffers have grown
// to the scene's size, and nothing does per-pixel work after construction.
#pragma once

#include "mesh.hpp"
#include "toonval/note_style.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>

namespace toonval::app {

// Two small textures, made once per renderer: a soft glow sprite (white, alpha falling off from
// its centre lines, so it works both as a whole-texture blob and as a nine-slice around a
// rectangle) and a tiling grain for the frosted style.
class EffectTextures {
  public:
    static constexpr int kSize = 64;

    explicit EffectTextures(SDL_Renderer* renderer);
    ~EffectTextures();
    EffectTextures(const EffectTextures&) = delete;
    EffectTextures& operator=(const EffectTextures&) = delete;

    SDL_Texture* glow() const { return glow_; }
    SDL_Texture* grain() const { return grain_; }

  private:
    SDL_Texture* glow_ = nullptr;
    SDL_Texture* grain_ = nullptr;
};

// `c` pushed away from its grey by `s` (1 keeps it).
SDL_FColor saturated(SDL_FColor c, float s);

// Paints note blocks and lit keys from one recipe, in layers that each become one draw call:
// body, inner shade and glow, extras, grain, rim. Shapes in one painter must not overlap each
// other (blocks in their lanes, white keys, black keys); a layer drawn over another shape's body
// would show. begin() starts a batch, add() queues a shape, draw() sends it.
class StylePainter {
  public:
    void begin(const StyleRecipe& recipe, float unit);
    // `fill` is the note colour at the far end, `key_end` at the key end (held notes are lighter
    // there), both with any press flash applied; `alpha` fades the whole shape. `underlay`, when
    // its alpha is above 0, is painted opaque under a see-through body, so a lit key shows the
    // style as its block does over the dark background.
    void add(const SDL_FRect& rect, float radius, SDL_FColor fill, SDL_FColor key_end, float alpha,
             SDL_FColor underlay = {0.0f, 0.0f, 0.0f, 0.0f});
    void draw(SDL_Renderer* renderer, const EffectTextures& textures) const;

  private:
    StyleRecipe recipe_;
    float unit_ = 1.0f;
    Mesh body_;
    Mesh inner_;
    Mesh extras_;
    Mesh grain_;
    Mesh rim_;
};

// A fixed pool of sparks thrown up from the hit line on note-on, plus a short burst of light where
// the note lands. When the pool is full the oldest spark is reused, so it never allocates.
class Sparks {
  public:
    static constexpr int kCapacity = 768;

    // Throws sparks from (`x`, `y`) for a note `key_width` wide; `velocity` 0..127 sets how many.
    void spawn(float x, float y, float key_width, SDL_FColor colour, int velocity, float unit);
    // Moves every spark to `now_ns` (the first call only starts the clock).
    void update(std::int64_t now_ns, float unit);
    // Adds a glow-sprite quad per live spark, for drawing with EffectTextures::glow() and add.
    void build(Mesh& mesh) const;
    void clear();
    int live() const;

  private:
    struct Spark {
        float x = 0.0f;
        float y = 0.0f;
        float vx = 0.0f;
        float vy = 0.0f;
        float age = 0.0f;
        float life = 0.0f; // 0 for a free slot
        float size = 0.0f;
        float gravity = 1.0f;
        SDL_FColor colour{};
    };
    float random01();

    std::array<Spark, kCapacity> pool_{};
    int next_ = 0;
    std::int64_t last_ns_ = 0;
    bool started_ = false;
    std::uint32_t rng_ = 0x9e3779b9u;
};

// The motion background: a few large, faint blobs of coloured light drifting slowly over `area`,
// one glow-sprite quad each. `seconds` is a steady clock.
void build_motion_background(Mesh& mesh, const SDL_FRect& area, double seconds);

} // namespace toonval::app
