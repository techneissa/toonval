// SPDX-License-Identifier: GPL-3.0-or-later
// The control bar's three styles (Settings > View > Bar style). The host (host.cpp) owns the top
// row, the gear, the status text and when the bar shows; a style only draws the bar's buttons in
// the room the layout gives it (toonval::split_top_row) and, under the row, the song overview.
//
//   Strip   a full-width translucent strip; the look buttons open small cards under them.
//   Groups  floating button groups with a caption under every icon; a tap acts at once and a long
//           press opens that switch's settings page.
//   Dock    a centred floating dock, play the one filled button, the overview hanging under it.
#pragma once

#include "kit.hpp"
#include "pages.hpp"
#include "song_port.hpp"

#include "toonval/session_state.hpp"

#include <imgui.h>

#include <memory>

namespace toonval::app::ui {

struct Frame {
    ImVec2 display{0, 0}; // ImGui points
    ImVec2 row_min{0, 0}; // the top row
    ImVec2 row_max{0, 0};
    ImVec2 bar_min{0, 0}; // the part of the top row the bar may use (split_top_row)
    ImVec2 bar_max{0, 0};
    float top = 0.0f; // overlays start here, below the top row
    float keyboard_top = 0.0f;
    float reveal = 0.0f;  // the bar's buttons, eased 0..1
    bool live = false;    // the bar is far enough in to take taps
    bool compact = false; // drawn in its narrow form (see BarLook::width)
};

class BarLook {
  public:
    virtual ~BarLook() = default;
    // The bar's width in points, full or `compact` (fewer buttons), for split_top_row.
    virtual float width(const Kit& k, const SongView& song, bool compact) const = 0;
    // True when the bar should be centred in the window rather than set against the gear.
    virtual bool centred() const { return false; }
    // The bar's buttons, while `f.reveal` > 0, inside a window covering the top row and the
    // overview under it.
    virtual void draw(Kit& k, UiModel& model, SongPort& song, const Frame& f) = 0;
    // True while something the bar opened is showing: the bar stays.
    virtual bool holds_bar() const { return false; }
    // Any card the bar's buttons opened closes (the settings opened, or the style changed).
    virtual void close_popovers() {}
    // A bar button asked for the settings to open on `*p` (a long press in the groups style).
    virtual bool wants_open(Page* p) {
        (void)p;
        return false;
    }
};

std::unique_ptr<BarLook> make_strip_bar();
std::unique_ptr<BarLook> make_groups_bar();
std::unique_ptr<BarLook> make_dock_bar();

// Helpers shared by the bars and the sheet.

// Begins a borderless, backgroundless window at `pos`, `size`; always paired with ImGui::End().
bool begin_layer(const char* id, ImVec2 pos, ImVec2 size, bool scroll = false);
// A tap that began outside `min`..`max` and below the top row: an overlay should close.
bool tapped_outside(const Frame& f, ImVec2 min, ImVec2 max);

} // namespace toonval::app::ui
