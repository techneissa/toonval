// SPDX-License-Identifier: GPL-3.0-or-later
// The state the settings screens edit and the settings file keeps. Plain data, no SDL.
#pragma once

#include "toonval/look.hpp"
#include "toonval/session_state.hpp"

namespace toonval::app {

struct UiModel {
    LookSettings look;
    UserPresets user_presets;
    AppSettings app;
    SessionState session; // the keyboard choice is the only part free play edits

    friend bool operator==(const UiModel&, const UiModel&) = default;
};

} // namespace toonval::app
