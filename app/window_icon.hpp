// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <SDL3/SDL.h>

namespace toonval::app {

// Sets the app icon on `window` from the PNGs built into the binary (256 px, with 64 px as the
// alternate for small sizes). A failure is logged and the window keeps the system's default.
void set_window_icon(SDL_Window* window);

} // namespace toonval::app
