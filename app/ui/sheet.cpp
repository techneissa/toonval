// SPDX-License-Identifier: GPL-3.0-or-later
#include "sheet.hpp"

#include "icons.hpp"

#include <algorithm>
#include <cmath>

namespace toonval::app::ui {

void SettingsSheet::open_at(Page p) {
    nav_ = Nav{};
    nav_.page = p;
    last_ = parent_of(p);
}

bool SettingsSheet::draw(Kit& k, UiModel& model, const Frame& f, float open) {
    nav_.update(k.dt);
    const float margin = k.s(tok::kS3);
    const float w = std::min(f.display.x - 2.0f * margin, k.s(860.0f));
    const float avail_h = f.display.y - f.top - margin;
    const float h = std::min(avail_h, k.s(452.0f));
    const float x = std::round((f.display.x - w) * 0.5f);
    const float y = f.top + (1.0f - open) * k.s(12.0f);
    bool close = false;

    // The scrim dims what the sheet does not cover, from under the top row down.
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, open);
    if (begin_layer("##sheet-scrim", {0.0f, f.row_max.y},
                    {f.display.x, f.display.y - f.row_max.y})) {
        ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetWindowPos(), {f.display.x, f.display.y},
                                                  u32(col::kScrim));
        ImGui::SetCursorPos({0, 0});
        ImGui::InvisibleButton("##scrim", ImGui::GetWindowSize());
        if (ImGui::IsItemClicked()) {
            close = true;
        }
    }
    ImGui::End();

    if (begin_layer("##sheet", {x, y}, {w, h})) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        card(k, dl, {x, y}, {x + w, y + h}, k.s(tok::kRadiusLg), col::kSurface,
             ImDrawFlags_RoundCornersAll, 1.4f);
        // The rail: names beside icons when there is room, captions under icons when not.
        const bool wide = w >= k.s(640.0f);
        const float rail_w = wide ? k.s(176.0f) : k.s(76.0f);
        const float pad = k.s(tok::kS2);
        dl->AddRectFilled({x, y}, {x + rail_w, y + h}, u32({1, 1, 1, 0.025f}), k.s(tok::kRadiusLg),
                          ImDrawFlags_RoundCornersLeft);
        dl->AddLine({x + rail_w, y + k.s(tok::kS3)}, {x + rail_w, y + h - k.s(tok::kS3)},
                    u32(col::kHairline));
        const float row_h = std::min(k.s(tok::kHit) + (wide ? 0.0f : k.s(8.0f)),
                                     (h - 2.0f * pad) / static_cast<float>(kRail.size()));
        for (std::size_t i = 0; i < kRail.size(); ++i) {
            const Page p = kRail[i];
            const bool on = parent_of(nav_.page) == p;
            const ImVec2 a{x + pad, y + pad + row_h * static_cast<float>(i)};
            ImGui::SetCursorScreenPos(a);
            ImGui::PushID(static_cast<int>(i));
            const bool pressed = ImGui::InvisibleButton("##rail", {rail_w - 2.0f * pad, row_h});
            const bool held = ImGui::IsItemActive();
            ImGui::PopID();
            const ImVec2 b{a.x + rail_w - 2.0f * pad, a.y + row_h};
            const float sel =
                tween(ImGui::GetID(static_cast<int>(i) + 9000), on ? 1.0f : 0.0f, tok::kFast);
            if (sel > 0.0f || held) {
                dl->AddRectFilled(
                    {a.x, a.y + k.s(2.0f)}, {b.x, b.y - k.s(2.0f)},
                    u32(held && !on ? col::kFillPressed : with_alpha(col::kAccentSoft, sel)),
                    k.s(tok::kRadiusMd));
            }
            const ImVec4 ink = mix(col::kText2, col::kAccent, sel);
            if (wide) {
                glyph(k, dl, page_icon(p), {a.x + k.s(22.0f), (a.y + b.y) * 0.5f},
                      k.s(tok::kIcon) * 0.9f, ink);
                const ImVec2 ts = text_size(k.regular, k.s(tok::kBody), page_title(p));
                text_at(k, dl, on ? k.bold : k.regular, k.s(tok::kBody),
                        {a.x + k.s(44.0f), (a.y + b.y - ts.y) * 0.5f},
                        on ? col::kText : col::kText2, page_title(p));
            } else {
                const float cap = k.s(tok::kCaption) * 0.9f;
                const float icon_y = (a.y + b.y) * 0.5f - cap * 0.55f;
                glyph(k, dl, page_icon(p), {(a.x + b.x) * 0.5f, icon_y}, k.s(tok::kIcon) * 0.9f,
                      ink);
                const char* label = p == Page::Names ? "Names" : page_title(p);
                const ImVec2 ts = text_size(k.regular, cap, label);
                text_at(k, dl, k.regular, cap, {(a.x + b.x - ts.x) * 0.5f, icon_y + k.s(11.0f)},
                        on ? col::kText : col::kText3, label);
            }
            if (pressed && nav_.page != p) {
                nav_.go(p, 0.0f);
                last_ = p;
            }
        }
        // The page.
        const float cx = rail_w + k.s(tok::kS4);
        const float cw = w - cx - k.s(tok::kS4);
        ImGui::SetCursorPos({cx, k.s(tok::kS2)});
        ImGui::BeginChild("##sheet-head", {cw + k.s(tok::kS1), k.s(tok::kHit) + k.s(4.0f)},
                          ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
        const int hd = header(k, page_title(nav_.page), is_subpage(nav_.page), true);
        ImGui::EndChild();
        if (hd == 1) {
            nav_.go(parent_of(nav_.page), -1.0f);
        } else if (hd == 2) {
            close = true;
        }
        ImGui::SetCursorPosX(cx);
        const float lift = (1.0f - ease_out(nav_.t)) * k.s(6.0f);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (nav_.dir == 0.0f ? lift : 0.0f));
        const float body_h = h - ImGui::GetCursorPosY() - k.s(tok::kS3);
        {
            const ImVec2 cur = ImGui::GetCursorPos();
            ImGui::SetCursorPos({cur.x + nav_.offset(k.s(28.0f)), cur.y});
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * nav_.alpha());
            ImGui::BeginChild("##sheet-page", {cw, body_h}, ImGuiChildFlags_None,
                              ImGuiWindowFlags_NoBackground);
            const int columns = cw >= k.s(540.0f) ? 2 : 1;
            const Page next = draw_page(k, nav_.page, model, columns);
            if (next != nav_.page) {
                nav_.go(next, 1.0f);
            }
            drag_scroll(k);
            ImGui::EndChild();
            ImGui::PopStyleVar();
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
    return close;
}

} // namespace toonval::app::ui
