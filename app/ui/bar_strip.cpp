// SPDX-License-Identifier: GPL-3.0-or-later
// Bar style Strip: a full-width translucent strip across the top. Its buttons switch at once
// (names, C markers) or open a small card right under them (style, colour, looks).
#include "bar.hpp"
#include "icons.hpp"
#include "preview.hpp"
#include "transport.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace toonval::app::ui {

namespace {

enum class Pop : std::uint8_t { None, Style, Colour, Looks };

constexpr float kCell = 48.0f; // design points
constexpr float kDrawn = 36.0f;
constexpr float kChip = 150.0f;

class StripBar final : public BarLook {
  public:
    float width(const Kit& k, const SongView& song, bool compact) const override {
        const float cell = k.s(kCell);
        const float sep = k.s(tok::kS2);
        float w = cell * 4.0f + sep + (compact ? cell : sep + k.s(kChip));
        if (song.has_song) {
            w += transport_width(cell) + sep;
        }
        return w;
    }
    void draw(Kit& k, UiModel& model, SongPort& song, const Frame& f) override;
    bool holds_bar() const override { return pop_ != Pop::None; }
    void close_popovers() override { pop_ = Pop::None; }

  private:
    void popover(Kit& k, UiModel& model, const Frame& f);

    Pop pop_ = Pop::None;
    float pop_p_ = 0.0f;
    Pop shown_ = Pop::None; // the card drawn while it fades out
    float anchor_x_ = 0.0f;
    float below_ = 0.0f;  // the strip's bottom edge, where its cards start
    float card_h_ = 0.0f; // the card's height last frame
};

void StripBar::draw(Kit& k, UiModel& model, SongPort& song, const Frame& f) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const SongView& v = song.view();
    const float h = f.row_max.y - f.row_min.y;
    const float slide = (1.0f - f.reveal) * -k.s(10.0f);
    // The strip: the top row and, under it when a song is loaded, the song overview; translucent,
    // a hairline under it, no hard band.
    const float sh = v.has_song ? scrubber_height(k) + k.s(tok::kS1) : 0.0f;
    const float bottom = f.row_max.y + sh + slide;
    dl->AddRectFilledMultiColor(
        {0.0f, f.row_min.y + slide}, {f.display.x, bottom}, u32(with_alpha(col::kSurface, 0.96f)),
        u32(with_alpha(col::kSurface, 0.96f)), u32(with_alpha(col::kSurface, 0.86f)),
        u32(with_alpha(col::kSurface, 0.86f)));
    dl->AddLine({0.0f, bottom - 0.5f}, {f.display.x, bottom - 0.5f}, u32(col::kHairline));
    if (v.has_song) {
        scrubber(k, song, {k.s(tok::kS1), f.row_max.y + slide},
                 {f.bar_max.x, bottom - k.s(tok::kS1)}, f.live);
    }
    below_ = bottom;

    const float cy = f.row_min.y + h * 0.5f + slide;
    const ImVec2 hit{k.s(kCell), h};
    const float drawn = k.s(kDrawn);
    const float sep = k.s(tok::kS2);
    float x = f.bar_min.x;
    LookSettings& look = model.look;
    const auto divider = [&] {
        x += sep * 0.5f;
        dl->AddLine({x, cy - k.s(12.0f)}, {x, cy + k.s(12.0f)}, u32(col::kHairline), 1.0f);
        x += sep * 0.5f;
    };
    if (v.has_song) {
        transport(k, song, x, cy, hit.x, h, drawn, f.live, false, false);
        x += transport_width(hit.x);
        divider();
    }
    const auto button = [&](const char* id, const char* glyph_text, const char* label, bool on) {
        const bool t =
            icon_toggle(k, id, glyph_text, label, {x + hit.x * 0.5f, cy}, hit, drawn, on, f.live);
        x += hit.x;
        return t;
    };
    if (button("##names", icon::kNames, "Note names", look.name_mode != NameMode::Off)) {
        toggle_names(model);
    }
    if (button("##cmark", icon::kCMarker, "C markers", look.c_markers)) {
        look.c_markers = !look.c_markers;
    }
    divider();
    const auto opener = [&](const char* id, const char* glyph_text, const char* label, Pop which) {
        const float cx = x + hit.x * 0.5f;
        if (button(id, glyph_text, label, pop_ == which)) {
            pop_ = pop_ == which ? Pop::None : which;
            anchor_x_ = cx;
        }
    };
    opener("##style", icon::kStyle, "Note style", Pop::Style);
    opener("##colour", icon::kPalette, "Colours", Pop::Colour);
    // The look chip says which look is on: names are what a player recognises.
    if (!f.compact) {
        x += sep;
        const float chip_w = k.s(kChip);
        const float cx = x + chip_w * 0.5f;
        if (chip(k, "##looks", icon::kPresets, current_preset_name(model), {x, f.row_min.y + slide},
                 {chip_w, h}, k.s(34.0f), pop_ == Pop::Looks, f.live)) {
            pop_ = pop_ == Pop::Looks ? Pop::None : Pop::Looks;
            anchor_x_ = cx;
        }
    } else {
        opener("##looks", icon::kPresets, "Looks", Pop::Looks);
    }
    popover(k, model, f);
}

void StripBar::popover(Kit& k, UiModel& model, const Frame& f) {
    pop_p_ = approach(pop_p_, pop_ != Pop::None ? 1.0f : 0.0f, k.dt, tok::kMove);
    if (pop_ != Pop::None) {
        shown_ = pop_;
    }
    if (pop_p_ <= 0.0f || shown_ == Pop::None) {
        return;
    }
    const float t = ease_out(pop_p_);
    const float w =
        std::min(f.display.x - k.s(24.0f), shown_ == Pop::Looks ? k.s(380.0f) : k.s(400.0f));
    const float x = std::clamp(anchor_x_ - k.s(40.0f), k.s(12.0f), f.display.x - w - k.s(12.0f));
    const float y = below_ + k.s(tok::kS2) + (1.0f - t) * -k.s(8.0f);
    const float max_h = f.keyboard_top - y - k.s(tok::kS2);
    const float pad = k.s(tok::kS3);
    // The card is its own window: the bar's window would clip it.
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, t);
    if (begin_layer("##strip-card", {x, y},
                    {w, std::min(max_h, card_h_ > 0 ? card_h_ : k.s(200.0f))})) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->ChannelsSplit(2);
        dl->ChannelsSetCurrent(1);
        ImGui::SetCursorPos({pad, pad});
        ImGui::BeginGroup();
        ImGui::PushItemWidth(w - 2.0f * pad);
        ImGui::BeginChild("##strip-card-body", {w - 2.0f * pad, 0.0f}, ImGuiChildFlags_AutoResizeY,
                          ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
        LookSettings& look = model.look;
        switch (shown_) {
        case Pop::Style:
            if (k.preview != nullptr) {
                k.preview->draw(k, look, ImGui::GetContentRegionAvail().x);
            }
            style_picker(k, look);
            slider_row(k, nullptr, "Intensity", &look.style_intensity, kMinStyleIntensity,
                       kMaxStyleIntensity, 100.0f, "%.0f%%");
            break;
        case Pop::Colour: {
            constexpr Seg kSchemes[] = {{nullptr, "Hand"},
                                        {nullptr, "Pitch"},
                                        {nullptr, "Octave"},
                                        {nullptr, "Velocity"},
                                        {nullptr, "One"}};
            int scheme = static_cast<int>(look.color_scheme);
            if (segmented(k, "scheme", nullptr, nullptr, kSchemes, &scheme)) {
                look.color_scheme = static_cast<ColorScheme>(scheme);
            }
            switch_row(k, icon::kContrast, "High contrast", &look.high_contrast);
            break;
        }
        case Pop::Looks:
            builtin_looks(k, model, 2);
            break;
        case Pop::None:
            break;
        }
        ImGui::EndChild();
        const float body_h = ImGui::GetItemRectSize().y;
        ImGui::PopItemWidth();
        ImGui::EndGroup();
        card_h_ = body_h + 2.0f * pad;
        dl->ChannelsSetCurrent(0);
        const ImVec2 p = ImGui::GetWindowPos();
        card(k, dl, p, {p.x + w, p.y + std::min(max_h, card_h_)}, k.s(tok::kRadiusLg));
        dl->ChannelsMerge();
        if (pop_ != Pop::None && tapped_outside(f, p, {p.x + w, p.y + card_h_})) {
            pop_ = Pop::None;
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

} // namespace

std::unique_ptr<BarLook> make_strip_bar() { return std::make_unique<StripBar>(); }

} // namespace toonval::app::ui
