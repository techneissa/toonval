// SPDX-License-Identifier: GPL-3.0-or-later
#include "window_icon.hpp"

#include <span>

namespace toonval::app {

std::span<const unsigned char> embedded_icon_256();
std::span<const unsigned char> embedded_icon_64();

namespace {

SDL_Surface* decode_png(std::span<const unsigned char> bytes) {
    SDL_IOStream* io = SDL_IOFromConstMem(bytes.data(), bytes.size());
    return io == nullptr ? nullptr : SDL_LoadPNG_IO(io, true);
}

} // namespace

void set_window_icon(SDL_Window* window) {
    SDL_Surface* big = decode_png(embedded_icon_256());
    if (big == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "window icon: %s", SDL_GetError());
        return;
    }
    if (SDL_Surface* small = decode_png(embedded_icon_64())) {
        // The surface keeps its own reference to the alternate image.
        SDL_AddSurfaceAlternateImage(big, small);
        SDL_DestroySurface(small);
    }
    if (!SDL_SetWindowIcon(window, big)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "window icon: %s", SDL_GetError());
    }
    SDL_DestroySurface(big);
}

} // namespace toonval::app
