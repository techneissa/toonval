// SPDX-License-Identifier: GPL-3.0-or-later
#include "text.hpp"

#include <SDL3_ttf/SDL_ttf.h>

#include <algorithm>
#include <cmath>
#include <span>

namespace toonval::app {

// Generated from the font file by cmake/EmbedFile.cmake.
std::span<const unsigned char> embedded_font();

namespace {
// Font sizes are rounded to this step so a slow window resize does not re-rasterise every frame.
constexpr float kSizeStep = 2.0f;
} // namespace

TextSlot::~TextSlot() {
    if (texture != nullptr) {
        SDL_DestroyTexture(texture);
    }
}

Text::Text(SDL_Renderer* renderer) : renderer_(renderer) {
    if (!TTF_Init()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "TTF_Init failed, using the debug font: %s",
                    SDL_GetError());
        return;
    }
    const auto data = embedded_font();
    SDL_IOStream* io = SDL_IOFromConstMem(data.data(), data.size());
    font_ = io != nullptr ? TTF_OpenFontIO(io, true, 32.0f) : nullptr;
    if (font_ == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "font failed to open, using the debug font: %s",
                    SDL_GetError());
    }
}

Text::~Text() {
    if (font_ != nullptr) {
        TTF_CloseFont(font_);
    }
    TTF_Quit();
}

SDL_FPoint Text::prepare(TextSlot& slot, std::string_view text, float size, bool bold) {
    size = std::max(kSizeStep, std::round(size / kSizeStep) * kSizeStep);
    if (slot.size == size && slot.bold == bold && slot.text == text) {
        return {slot.width, slot.height};
    }
    slot.text.assign(text);
    slot.size = size;
    slot.bold = bold;
    slot.fit_size = 0.0f;
    slot.hidden = false;
    if (slot.texture != nullptr) {
        SDL_DestroyTexture(slot.texture);
        slot.texture = nullptr;
    }
    if (font_ == nullptr || text.empty()) {
        const float scale = size / SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE;
        slot.width = static_cast<float>(text.size()) * SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * scale;
        slot.height = text.empty() ? 0.0f : size;
        return {slot.width, slot.height};
    }
    TTF_SetFontSize(font_, size);
    TTF_SetFontStyle(font_, bold ? TTF_STYLE_BOLD : TTF_STYLE_NORMAL);
    // White glyphs, so the colour and alpha come from the texture's modulation at draw time.
    SDL_Surface* surface =
        TTF_RenderText_Blended(font_, text.data(), text.size(), {255, 255, 255, 255});
    if (surface != nullptr) {
        slot.texture = SDL_CreateTextureFromSurface(renderer_, surface);
        slot.width = static_cast<float>(surface->w);
        slot.height = static_cast<float>(surface->h);
        SDL_DestroySurface(surface);
    }
    if (slot.texture == nullptr) {
        slot.width = 0.0f;
        slot.height = 0.0f;
    }
    return {slot.width, slot.height};
}

SDL_FPoint Text::prepare_fit(TextSlot& slot, std::string_view text, float size, float max_width,
                             float min_size) {
    if (slot.fit_size == size && slot.fit_width == max_width && slot.text == text) {
        return slot.hidden ? SDL_FPoint{0.0f, 0.0f} : SDL_FPoint{slot.width, slot.height};
    }
    SDL_FPoint extent = prepare(slot, text, size);
    if (extent.x > max_width && extent.x > 0.0f) {
        const float fitted = size * max_width / extent.x;
        // Rounding the size can overshoot by a step, so aim one step lower.
        extent = fitted - kSizeStep >= min_size ? prepare(slot, text, fitted - kSizeStep)
                                                : SDL_FPoint{0.0f, 0.0f};
    }
    slot.fit_size = size;
    slot.fit_width = max_width;
    slot.hidden = extent.x <= 0.0f && !text.empty();
    return slot.hidden ? SDL_FPoint{0.0f, 0.0f} : extent;
}

void Text::draw(TextSlot& slot, float x, float y, Align align, SDL_FColor colour) {
    if (slot.text.empty() || slot.hidden) {
        return;
    }
    const float left = align == Align::Centre ? x - slot.width * 0.5f : x;
    if (slot.texture != nullptr) {
        SDL_SetTextureColorModFloat(slot.texture, colour.r, colour.g, colour.b);
        SDL_SetTextureAlphaModFloat(slot.texture, colour.a);
        const SDL_FRect dst{std::round(left), std::round(y), slot.width, slot.height};
        SDL_RenderTexture(renderer_, slot.texture, nullptr, &dst);
        return;
    }
    // Debug-font fallback.
    const float scale = slot.size / SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE;
    SDL_SetRenderScale(renderer_, scale, scale);
    SDL_SetRenderDrawColorFloat(renderer_, colour.r, colour.g, colour.b, colour.a);
    SDL_RenderDebugText(renderer_, left / scale, y / scale, slot.text.c_str());
    SDL_SetRenderScale(renderer_, 1.0f, 1.0f);
}

} // namespace toonval::app
