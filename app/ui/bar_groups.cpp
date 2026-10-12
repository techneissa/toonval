// SPDX-License-Identifier: GPL-3.0-or-later
// Bar style Groups: floating button groups, each icon with a short caption that says what it is or
// what it is set to. A tap acts at once (names on or off, the next style, the next look); a long
// press opens that switch's settings page.
#include "bar.hpp"
#include "icons.hpp"
#include "transport.hpp"

#include <imgui_internal.h> // ClearActiveID

#include <algorithm>
#include <cmath>
#include <string>

namespace toonval::app::ui {

namespace {

constexpr float kCell = 64.0f;  // a look switch with its caption, design points
constexpr float kTCell = 48.0f; // a transport button
constexpr float kPad = tok::kS1;

class GroupsBar final : public BarLook {
  public:
    float width(const Kit& k, const SongView& song, bool compact) const override {
        float w = group_w(k, compact);
        if (song.has_song) {
            w += transport_width(k.s(kTCell)) + 2.0f * k.s(kPad) + k.s(tok::kS2);
        }
        return w;
    }
    void draw(Kit& k, UiModel& model, SongPort& song, const Frame& f) override;
    bool wants_open(Page* p) override {
        if (!want_open_) {
            return false;
        }
        want_open_ = false;
        *p = pending_;
        return true;
    }

  private:
    static float group_w(const Kit& k, bool compact) {
        return k.s(kCell) * (compact ? 2.0f : 4.0f) + 2.0f * k.s(kPad);
    }

    Page pending_ = Page::Quick; // a long press asks the host to open this page
    bool want_open_ = false;
};

void GroupsBar::draw(Kit& k, UiModel& model, SongPort& song, const Frame& f) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const SongView& v = song.view();
    const float h = f.row_max.y - f.row_min.y;
    const float slide = (1.0f - f.reveal) * -k.s(10.0f);
    LookSettings& look = model.look;

    struct Item {
        const char* id;
        const char* icon;
        std::string caption;
        bool on;
        Page page; // what a long press opens
    };
    const std::string names = look.name_mode == NameMode::Off       ? "Names"
                              : look.name_mode == NameMode::Letters ? "C D E"
                                                                    : "Do Re Mi";
    const Item items[] = {
        {"##names", icon::kNames, names, look.name_mode != NameMode::Off, Page::Names},
        {"##cmark", icon::kCMarker, "C marks", look.c_markers, Page::Names},
        {"##style", icon::kStyle, style_label(look.note_style), false, Page::Look},
        {"##looks", icon::kPresets, current_preset_name(model), false, Page::Presets},
    };
    const int count = f.compact ? 2 : 4;
    const float cell_w = k.s(kCell);
    const float group_h = h - k.s(tok::kS2);
    const float pad = k.s(kPad);
    const float gw = group_w(k, f.compact);
    const float gap = k.s(tok::kS2);
    const float gy = f.row_min.y + (h - group_h) * 0.5f + slide;
    const float cy = f.row_min.y + h * 0.5f + slide;
    float x = f.bar_min.x;
    if (v.has_song) {
        // The song's group first, then a pill with the overview under both groups.
        const float tw = transport_width(k.s(kTCell)) + 2.0f * pad;
        card(k, dl, {x, gy}, {x + tw, gy + group_h}, k.s(14.0f), col::kSurface,
             ImDrawFlags_RoundCornersAll, 0.7f);
        transport(k, song, x + pad, cy, k.s(kTCell), h, k.s(32.0f), f.live, false, true);
        const float sy = gy + group_h + k.s(tok::kS1) + k.s(2.0f);
        const float shh = scrubber_height(k) - k.s(6.0f);
        const float end = x + tw + gap + gw;
        card(k, dl, {x, sy}, {end, sy + shh}, shh * 0.5f, col::kSurface,
             ImDrawFlags_RoundCornersAll, 0.7f);
        scrubber(k, song, {x + k.s(tok::kS1), sy}, {end - k.s(tok::kS1), sy + shh}, f.live);
        x += tw + gap;
    }
    card(k, dl, {x, gy}, {x + gw, gy + group_h}, k.s(14.0f), col::kSurface,
         ImDrawFlags_RoundCornersAll, 0.7f);
    x += pad;
    for (int i = 0; i < count; ++i) {
        const Item& it = items[i];
        const std::string cap = fit(k.regular, k.s(tok::kCaption), it.caption, cell_w - k.s(6.0f));
        // Hit: the full row height, so the screen's top edge counts too.
        const bool t = icon_toggle(k, it.id, it.icon, nullptr, {x + cell_w * 0.5f, cy}, {cell_w, h},
                                   k.s(28.0f), it.on, f.live, cap.c_str());
        // A long press opens the page behind the switch.
        if (f.live && ImGui::IsItemActive() &&
            ImGui::GetIO().MouseDownDuration[0] > tok::kLongPress && !want_open_) {
            pending_ = it.page;
            want_open_ = true;
            ImGui::ClearActiveID();
        }
        if (t) {
            switch (i) {
            case 0:
                toggle_names(model);
                break;
            case 1:
                look.c_markers = !look.c_markers;
                break;
            case 2:
                look.note_style =
                    static_cast<NoteStyle>((static_cast<int>(look.note_style) + 1) % 6);
                break;
            default:
                cycle_preset(model);
                k.message = "Look: " + current_preset_name(model);
                break;
            }
        }
        x += cell_w;
        if (i == 1 && count > 2) {
            dl->AddLine({x, gy + group_h * 0.25f}, {x, gy + group_h * 0.75f}, u32(col::kHairline));
        }
    }
}

} // namespace

std::unique_ptr<BarLook> make_groups_bar() { return std::make_unique<GroupsBar>(); }

} // namespace toonval::app::ui
