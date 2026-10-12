// SPDX-License-Identifier: GPL-3.0-or-later
// Bar style Dock: a floating dock in the middle of the top row with the song's transport (play
// the one filled button), the quick switches and the current look. The song overview hangs under
// the dock.
#include "bar.hpp"
#include "icons.hpp"
#include "transport.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace toonval::app::ui {

namespace {

constexpr float kCell = 48.0f; // design points
constexpr float kChip = 150.0f;

class DockBar final : public BarLook {
  public:
    float width(const Kit& k, const SongView& song, bool compact) const override {
        const float cell = k.s(kCell);
        float w = cell * 4.0f + 2.0f * k.s(tok::kS1);
        if (song.has_song) {
            w += transport_width(cell) + k.s(tok::kS2);
        }
        if (!compact) {
            w += k.s(tok::kS2) + k.s(kChip);
        }
        return w;
    }
    bool centred() const override { return true; }
    void draw(Kit& k, UiModel& model, SongPort& song, const Frame& f) override;
};

void DockBar::draw(Kit& k, UiModel& model, SongPort& song, const Frame& f) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const SongView& v = song.view();
    const float h = f.row_max.y - f.row_min.y;
    const float slide = (1.0f - f.reveal) * -k.s(12.0f);
    LookSettings& look = model.look;
    const float cell = k.s(kCell);
    const float dock_h = std::min(h - k.s(6.0f), k.s(46.0f));
    const float pad = k.s(tok::kS1);
    const float sep = k.s(tok::kS2);
    const float dx = f.bar_min.x;
    const float dock_w = f.bar_max.x - f.bar_min.x;
    const float dy = f.row_min.y + (h - dock_h) * 0.5f + slide;
    const float cy = dy + dock_h * 0.5f;
    const float row_cy = f.row_min.y + h * 0.5f + slide;
    card(k, dl, {dx, dy}, {dx + dock_w, dy + dock_h}, dock_h * 0.5f, col::kSurface,
         ImDrawFlags_RoundCornersAll, 1.0f);
    float x = dx + pad;
    if (v.has_song) {
        transport(k, song, x, row_cy, cell, h, dock_h - k.s(8.0f), f.live, true, false);
        const float sy = dy + dock_h + k.s(tok::kS1);
        const float shh = scrubber_height(k) - k.s(8.0f);
        const float inset = k.s(tok::kS4);
        card(k, dl, {dx + inset, sy}, {dx + dock_w - inset, sy + shh}, shh * 0.5f, col::kSurface,
             ImDrawFlags_RoundCornersAll, 0.8f);
        scrubber(k, song, {dx + inset + k.s(tok::kS1), sy},
                 {dx + dock_w - inset - k.s(tok::kS1), sy + shh}, f.live, dock_w > k.s(520.0f));
        x += transport_width(cell) + sep * 0.5f;
        dl->AddLine({x, cy - k.s(10.0f)}, {x, cy + k.s(10.0f)}, u32(col::kHairline));
        x += sep * 0.5f;
    }
    const auto button = [&](const char* id, const char* glyph_text, const char* label, bool on) {
        const bool t = icon_toggle(k, id, glyph_text, label, {x + cell * 0.5f, row_cy}, {cell, h},
                                   dock_h - k.s(8.0f), on, f.live);
        x += cell;
        return t;
    };
    if (button("##names", icon::kNames, "Note names", look.name_mode != NameMode::Off)) {
        toggle_names(model);
    }
    if (button("##cmark", icon::kCMarker, "C markers", look.c_markers)) {
        look.c_markers = !look.c_markers;
    }
    if (button("##contrast", icon::kContrast, "High contrast", look.high_contrast)) {
        look.high_contrast = !look.high_contrast;
    }
    if (button("##reduced", icon::kReduced, "Reduced effects", look.reduced_effects)) {
        look.reduced_effects = !look.reduced_effects;
    }
    if (!f.compact) {
        x += sep * 0.5f;
        dl->AddLine({x, cy - k.s(10.0f)}, {x, cy + k.s(10.0f)}, u32(col::kHairline));
        x += sep * 0.5f;
        // The look: a tap moves to the next one, the name says which.
        const float chip_w = dx + dock_w - pad - x;
        if (chip(k, "##look", icon::kPresets, current_preset_name(model), {x, f.row_min.y + slide},
                 {chip_w, h}, dock_h - k.s(10.0f), false, f.live)) {
            cycle_preset(model);
            k.message = "Look: " + current_preset_name(model);
        }
    }
}

} // namespace

std::unique_ptr<BarLook> make_dock_bar() { return std::make_unique<DockBar>(); }

} // namespace toonval::app::ui
