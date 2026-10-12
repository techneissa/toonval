// SPDX-License-Identifier: GPL-3.0-or-later
// Text drawing with the embedded font through SDL3_ttf. Each piece of text keeps its own cached
// texture in a TextSlot, so a label is only rasterised again when its text or size changes. If
// the font cannot be opened, text falls back to SDL's built-in 8-pixel debug font, scaled up.
#pragma once

#include <SDL3/SDL.h>

#include <string>
#include <string_view>

typedef struct TTF_Font TTF_Font;

namespace toonval::app {

// One cached line of text. Owned by the caller, one per place text is drawn.
struct TextSlot {
    TextSlot() = default;
    TextSlot(const TextSlot&) = delete;
    TextSlot& operator=(const TextSlot&) = delete;
    ~TextSlot();

    std::string text;
    float size = 0.0f; // pixel height the texture was made for
    bool bold = false;
    SDL_Texture* texture = nullptr;
    float width = 0.0f; // of the drawn text, in pixels
    float height = 0.0f;
    // Inputs of the last prepare_fit(), so an unchanged label is not measured again.
    float fit_size = 0.0f;
    float fit_width = 0.0f;
    bool hidden = false; // prepare_fit() found no readable size
};

enum class Align { Left, Centre };

class Text {
  public:
    explicit Text(SDL_Renderer* renderer);
    ~Text();
    Text(const Text&) = delete;
    Text& operator=(const Text&) = delete;

    // False when the font could not be opened and the debug font is used.
    bool has_font() const { return font_ != nullptr; }

    // Updates `slot` for `text` at `size` pixels and returns its width and height. Only rasterises
    // when the text, size or weight changed. `bold` asks for the font's synthesized bold style.
    SDL_FPoint prepare(TextSlot& slot, std::string_view text, float size, bool bold = false);

    // Like prepare(), but shrinks the text to fit `max_width`. When that would go below
    // `min_size` the slot is hidden and draw() does nothing. Cached on its inputs.
    SDL_FPoint prepare_fit(TextSlot& slot, std::string_view text, float size, float max_width,
                           float min_size);

    // Draws the prepared slot with its top edge at `y`, left-aligned at `x` or centred on it.
    void draw(TextSlot& slot, float x, float y, Align align, SDL_FColor colour);

  private:
    SDL_Renderer* renderer_;
    TTF_Font* font_ = nullptr;
};

} // namespace toonval::app
