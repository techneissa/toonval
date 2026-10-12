// SPDX-License-Identifier: GPL-3.0-or-later
#include "kit.hpp"

#include "icons.hpp"

#include <imgui_internal.h> // ClearActiveID

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace toonval::app::ui {

ImVec4 with_alpha(ImVec4 c, float a) { return {c.x, c.y, c.z, c.w * a}; }

ImVec4 mix(ImVec4 a, ImVec4 b, float t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t,
            a.w + (b.w - a.w) * t};
}

float ease_out(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    const float inv = 1.0f - t;
    return 1.0f - inv * inv * inv;
}

namespace {
bool g_reduced_motion = false;
} // namespace

void set_reduced_motion(bool reduced) { g_reduced_motion = reduced; }

float approach(float p, float target, float dt, float duration) {
    const float step = duration > 0.0f && !g_reduced_motion ? dt / duration : 1.0f;
    return p < target ? std::min(target, p + step) : std::max(target, p - step);
}

float tween(ImGuiID id, float target, float duration) {
    ImGuiStorage* st = ImGui::GetStateStorage();
    // Stored as the start value, the target and the progress, so the ease has a fixed path. By
    // value: a pointer into the storage dies when a later insert grows it.
    const float to = st->GetFloat(id + 1, NAN);
    if (std::isnan(to)) {
        st->SetFloat(id, target);
        st->SetFloat(id + 1, target);
        st->SetFloat(id + 2, 1.0f);
        return target;
    }
    float from = st->GetFloat(id, target);
    float p = st->GetFloat(id + 2, 1.0f);
    if (to != target) {
        from = from + (to - from) * ease_out(p);
        p = 0.0f;
        st->SetFloat(id, from);
        st->SetFloat(id + 1, target);
    }
    p = approach(p, 1.0f, ImGui::GetIO().DeltaTime, duration);
    st->SetFloat(id + 2, p);
    return from + (target - from) * ease_out(p);
}

ImU32 u32(ImVec4 c) { return ImGui::GetColorU32(c); }

ImVec2 text_size(ImFont* font, float size, const char* text) {
    return font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
}

void text_at(const Kit& k, ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImVec4 colour,
             const char* text) {
    (void)k;
    dl->AddText(font, size, {std::round(pos.x), std::round(pos.y)}, u32(colour), text);
}

void glyph(const Kit& k, ImDrawList* dl, const char* icon, ImVec2 centre, float size,
           ImVec4 colour) {
    // By content, not by pointer: string literals are not always merged across files.
    if (std::strcmp(icon, icon::kCMarker) == 0) {
        // Drawn, not a font icon: the key's own C marker, a bold C over a short accent line.
        const float ts = size * 0.92f;
        const ImVec2 sz = text_size(k.bold, ts, "C");
        dl->AddText(k.bold, ts,
                    {std::round(centre.x - sz.x * 0.5f), std::round(centre.y - sz.y * 0.58f)},
                    u32(colour), "C");
        const float y = centre.y + size * 0.42f;
        dl->AddLine({centre.x - size * 0.26f, y}, {centre.x + size * 0.26f, y}, u32(colour),
                    std::max(1.5f, size * 0.09f));
        return;
    }
    unsigned int cp = 0;
    const int len = ImTextCharFromUtf8(&cp, icon, nullptr);
    if (len > 0 && icon[len] == '\0' && cp >= 0xE000 && cp <= 0xF8FF) {
        // An icon: centre its drawn bounds, not its line box, which sits it too high.
        if (const ImFontGlyph* g =
                k.regular->GetFontBaked(size)->FindGlyphNoFallback(static_cast<ImWchar>(cp))) {
            const ImVec2 at{std::round(centre.x - (g->X0 + g->X1) * 0.5f),
                            std::round(centre.y - (g->Y0 + g->Y1) * 0.5f)};
            dl->AddText(k.regular, size, at, u32(colour), icon);
            return;
        }
    }
    const ImVec2 ts = text_size(k.regular, size, icon);
    dl->AddText(k.regular, size,
                {std::round(centre.x - ts.x * 0.5f), std::round(centre.y - ts.y * 0.5f)},
                u32(colour), icon);
}

std::string fit(ImFont* font, float size, const std::string& text, float max_w) {
    if (text_size(font, size, text.c_str()).x <= max_w) {
        return text;
    }
    std::string out = text;
    while (!out.empty()) {
        // Drop one UTF-8 character.
        std::size_t cut = out.size() - 1;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) {
            --cut;
        }
        out.resize(cut);
        const std::string candidate = out + "\xE2\x80\xA6";
        if (text_size(font, size, candidate.c_str()).x <= max_w) {
            return candidate;
        }
    }
    return {};
}

void shadow(const Kit& k, ImDrawList* dl, ImVec2 a, ImVec2 b, float radius, float strength) {
    // Layered rounded rects, wider and fainter, offset down: a soft shadow without a blur.
    constexpr int kLayers = 7;
    for (int i = kLayers; i >= 1; --i) {
        const float t = static_cast<float>(i) / kLayers;
        const float grow = k.s(2.0f + 16.0f * t);
        const float drop = k.s(3.0f + 6.0f * t);
        const float alpha = 0.055f * strength * (1.0f - t * 0.75f);
        dl->AddRectFilled({a.x - grow, a.y - grow + drop}, {b.x + grow, b.y + grow + drop},
                          u32({0.0f, 0.0f, 0.0f, alpha}), radius + grow);
    }
}

void card(const Kit& k, ImDrawList* dl, ImVec2 a, ImVec2 b, float radius, ImVec4 fill,
          ImDrawFlags corners, float shadow_strength) {
    dl->PushClipRectFullScreen();
    if (shadow_strength > 0.0f) {
        shadow(k, dl, a, b, radius, shadow_strength);
    }
    dl->AddRectFilled(a, b, u32(fill), radius, corners);
    // A faint lighter band at the top edge reads as a lit surface.
    // Not on a disc or a pill: the band is square and would show past the curve.
    if (radius * 2.0f < b.y - a.y - 1.0f) {
        dl->AddRectFilledMultiColor({a.x + radius * 0.5f, a.y + 1.0f},
                                    {b.x - radius * 0.5f, a.y + k.s(18.0f)}, u32({1, 1, 1, 0.025f}),
                                    u32({1, 1, 1, 0.025f}), u32({1, 1, 1, 0.0f}),
                                    u32({1, 1, 1, 0.0f}));
    }
    dl->AddRect(a, b, u32(col::kHairline), radius, corners, 1.0f);
    dl->PopClipRect();
}

namespace {

ImVec4 to_vec(RGBA c, float a = 1.0f) {
    return {static_cast<float>(c.r) / 255.0f, static_cast<float>(c.g) / 255.0f,
            static_cast<float>(c.b) / 255.0f, a};
}

void one_block(const Kit& k, ImDrawList* dl, ImVec2 a, ImVec2 b, NoteStyle style, ImVec4 c,
               float intensity) {
    const float r = std::min(k.s(4.0f), (b.x - a.x) * 0.3f);
    const ImVec4 white{1, 1, 1, 1};
    switch (style) {
    case NoteStyle::Filled:
        dl->AddRectFilled(a, b, u32(c), r);
        break;
    case NoteStyle::Glassy: {
        dl->AddRectFilled(a, b, u32(with_alpha(c, 0.82f)), r);
        const float mid = a.y + (b.y - a.y) * 0.42f;
        dl->AddRectFilledMultiColor({a.x + 1.5f, a.y + 1.5f}, {b.x - 1.5f, mid},
                                    u32(with_alpha(white, 0.18f + 0.22f * intensity)),
                                    u32(with_alpha(white, 0.18f + 0.22f * intensity)),
                                    u32(with_alpha(white, 0.0f)), u32(with_alpha(white, 0.0f)));
        dl->AddRect(a, b, u32(with_alpha(mix(c, white, 0.5f), 0.8f)), r, 0, 1.0f);
        break;
    }
    case NoteStyle::Frosted:
        dl->AddRectFilled(a, b, u32(with_alpha(mix(c, white, 0.35f + 0.25f * intensity), 0.72f)),
                          r);
        dl->AddRect(a, b, u32(with_alpha(white, 0.35f)), r, 0, 1.0f);
        break;
    case NoteStyle::Shiny: {
        const ImVec4 top = mix(c, white, 0.45f * intensity + 0.15f);
        dl->AddRectFilled(a, b, u32(c), r);
        dl->AddRectFilledMultiColor({a.x + 1.0f, a.y + 1.0f}, {b.x - 1.0f, b.y - 1.0f}, u32(top),
                                    u32(top), u32(with_alpha(c, 0.0f)), u32(with_alpha(c, 0.0f)));
        const float x = a.x + (b.x - a.x) * 0.28f;
        dl->AddLine({x, a.y + r}, {x, b.y - r}, u32(with_alpha(white, 0.55f)), k.s(1.5f));
        break;
    }
    case NoteStyle::Ghost:
        dl->AddRectFilled(a, b, u32(with_alpha(c, 0.14f)), r);
        dl->AddRect(a, b, u32(c), r, 0, k.s(1.6f));
        break;
    case NoteStyle::Neon:
        for (int i = 4; i >= 1; --i) {
            const float g = k.s(1.6f * static_cast<float>(i));
            dl->AddRectFilled({a.x - g, a.y - g}, {b.x + g, b.y + g},
                              u32(with_alpha(c, 0.07f * (0.5f + intensity))), r + g);
        }
        dl->AddRectFilled(a, b, u32(with_alpha(mix(c, {0, 0, 0, 1}, 0.55f), 1.0f)), r);
        dl->AddRect(a, b, u32(mix(c, white, 0.35f)), r, 0, k.s(1.6f));
        break;
    }
}

} // namespace

void note_preview(const Kit& k, ImDrawList* dl, ImVec2 a, ImVec2 b, NoteStyle style, RGBA colour,
                  float intensity) {
    const float w = b.x - a.x;
    const float h = b.y - a.y;
    const ImVec4 c = to_vec(colour);
    const ImVec4 dark = mix(c, {0, 0, 0, 1}, 0.32f);
    // White-key note: tall, left; black-key note: narrower, shorter, darker, right.
    one_block(k, dl, {a.x + w * 0.10f, a.y + h * 0.04f}, {a.x + w * 0.50f, b.y - h * 0.04f}, style,
              c, intensity);
    one_block(k, dl, {a.x + w * 0.60f, a.y + h * 0.28f}, {a.x + w * 0.86f, b.y - h * 0.14f}, style,
              dark, intensity);
}

// --- Controls ----------------------------------------------------------------------------------

namespace {

bool tapped(bool pressed) {
    return pressed && ImGui::GetIO().MouseDownDurationPrev[0] < tok::kLongPress;
}

void long_press_tip(Kit& k, const char* label) {
    if (label != nullptr && ImGui::IsItemActive() &&
        ImGui::GetIO().MouseDownDuration[0] > tok::kLongPress) {
        k.tip = label;
        k.tip_min = ImGui::GetItemRectMin();
        k.tip_max = ImGui::GetItemRectMax();
    }
}

bool hovered_by_mouse(const Kit& k) { return !k.touch && ImGui::IsItemHovered(); }

ImVec2 row_start(float* w) {
    *w = ImGui::GetContentRegionAvail().x;
    return ImGui::GetCursorScreenPos();
}

// Ends a row whose parts were placed by hand: one item covering it, so the cursor moves on.
void end_row(ImVec2 pos, ImVec2 size) {
    ImGui::SetCursorScreenPos(pos);
    ImGui::Dummy(size);
}

bool info_button(Kit& k, const char* text, ImVec2 centre) {
    const float hit = k.s(tok::kHit);
    ImGui::SetCursorScreenPos({centre.x - hit * 0.4f, centre.y - hit * 0.5f});
    ImGui::PushID(text);
    const bool pressed = ImGui::InvisibleButton("##info", {hit * 0.8f, hit});
    const ImGuiID id = ImGui::GetItemID();
    ImGui::PopID();
    const bool open = k.info_id == id;
    glyph(k, ImGui::GetWindowDrawList(), icon::kInfo, centre, k.s(tok::kIconSm),
          open ? col::kAccent : col::kText3);
    if (pressed) {
        k.info_seen = true;
        if (open) {
            k.info_id = 0;
        } else {
            k.info_id = id;
            k.info_text = text;
        }
    }
    if (k.info_id == id) {
        k.info_anchor = {centre.x, centre.y + k.s(12.0f)};
    }
    return pressed;
}

void switch_knob(Kit& k, ImDrawList* dl, ImVec2 a, bool on, ImGuiID id) {
    const float w = k.s(40.0f);
    const float h = k.s(24.0f);
    const float t = tween(id, on ? 1.0f : 0.0f, tok::kFast);
    const ImVec4 track = mix({1, 1, 1, 0.14f}, col::kAccent, t);
    dl->AddRectFilled(a, {a.x + w, a.y + h}, u32(track), h * 0.5f);
    const float kr = h * 0.5f - k.s(3.0f);
    const float kx = a.x + h * 0.5f + (w - h) * t;
    dl->AddCircleFilled({kx, a.y + h * 0.5f + k.s(0.8f)}, kr + k.s(0.6f), u32({0, 0, 0, 0.25f}),
                        24);
    dl->AddCircleFilled({kx, a.y + h * 0.5f}, kr, u32(mix(col::kText2, {1, 1, 1, 1}, t)), 24);
}

} // namespace

bool gear(Kit& k, ImVec2 centre, float hit, bool active, float alpha) {
    ImGui::SetCursorScreenPos({centre.x - hit * 0.5f, centre.y - hit * 0.5f});
    const bool pressed = ImGui::InvisibleButton("##gear", {hit, hit});
    const bool held = ImGui::IsItemActive();
    const bool hover = hovered_by_mouse(k);
    const float scale = tween(ImGui::GetItemID(), held ? 0.88f : 1.0f, tok::kFast);
    ImVec4 c = active ? col::kAccent : with_alpha(col::kText, hover || held ? 1.0f : 0.74f);
    c.w *= alpha;
    glyph(k, ImGui::GetWindowDrawList(), icon::kSettings, centre, k.s(22.0f) * scale, c);
    return pressed;
}

bool icon_toggle(Kit& k, const char* id, const char* icon, const char* label, ImVec2 centre,
                 ImVec2 hit, float drawn, bool on, bool live, const char* caption) {
    ImGui::SetCursorScreenPos({centre.x - hit.x * 0.5f, centre.y - hit.y * 0.5f});
    const bool pressed = ImGui::InvisibleButton(id, hit);
    const bool held = ImGui::IsItemActive() && live;
    const bool hover = live && hovered_by_mouse(k);
    if (live) {
        long_press_tip(k, label);
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float cap_h = caption != nullptr ? k.s(14.0f) : 0.0f;
    const ImVec2 ic{centre.x, centre.y - cap_h * 0.5f};
    const float half = drawn * 0.5f;
    const float sel = tween(ImGui::GetItemID(), on ? 1.0f : 0.0f, tok::kFast);
    if (sel > 0.0f) {
        dl->AddRectFilled({ic.x - half, ic.y - half}, {ic.x + half, ic.y + half},
                          u32(with_alpha(col::kAccentSoft, sel)), k.s(tok::kRadiusMd));
    }
    if (held || hover) {
        dl->AddRectFilled({ic.x - half, ic.y - half}, {ic.x + half, ic.y + half},
                          u32(held ? col::kFillPressed : col::kFill), k.s(tok::kRadiusMd));
    }
    const ImVec4 ink = mix(col::kText2, col::kAccent, sel);
    glyph(k, dl, icon, ic, k.s(tok::kIcon), held ? mix(ink, {1, 1, 1, 1}, 0.3f) : ink);
    if (caption != nullptr) {
        const ImVec2 ts = text_size(k.regular, k.s(tok::kCaption), caption);
        text_at(k, dl, k.regular, k.s(tok::kCaption), {centre.x - ts.x * 0.5f, ic.y + half},
                mix(col::kText2, col::kAccent, sel), caption);
    }
    return live && tapped(pressed);
}

bool chip(Kit& k, const char* id, const char* icon, const std::string& text, ImVec2 pos, ImVec2 hit,
          float drawn_h, bool on, bool live) {
    ImGui::SetCursorScreenPos(pos);
    const bool pressed = ImGui::InvisibleButton(id, hit);
    const bool held = ImGui::IsItemActive() && live;
    const bool hover = live && hovered_by_mouse(k);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a{pos.x, pos.y + (hit.y - drawn_h) * 0.5f};
    const ImVec2 b{pos.x + hit.x, a.y + drawn_h};
    ImVec4 fill = on ? col::kAccentSoft : col::kFill;
    if (held) {
        fill = col::kFillPressed;
    } else if (hover) {
        fill = mix(fill, col::kFillPressed, 0.5f);
    }
    dl->AddRectFilled(a, b, u32(fill), drawn_h * 0.5f);
    const float pad = k.s(tok::kS3);
    float x = a.x + pad;
    if (icon != nullptr) {
        glyph(k, dl, icon, {x + k.s(tok::kIconSm) * 0.5f, (a.y + b.y) * 0.5f}, k.s(tok::kIconSm),
              on ? col::kAccent : col::kText2);
        x += k.s(tok::kIconSm) + k.s(tok::kS2);
    }
    const float size = k.s(tok::kLabel);
    const std::string shown = fit(k.regular, size, text, b.x - pad - x);
    const ImVec2 ts = text_size(k.regular, size, shown.c_str());
    text_at(k, dl, k.regular, size, {x, (a.y + b.y - ts.y) * 0.5f}, col::kText, shown.c_str());
    return live && tapped(pressed);
}

void section(Kit& k, const char* text) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float h = k.s(26.0f);
    const float size = k.s(tok::kCaption);
    const ImVec2 ts = text_size(k.bold, size, text);
    text_at(k, dl, k.bold, size, {pos.x + k.s(tok::kS1), pos.y + h - ts.y - k.s(3.0f)}, col::kText3,
            text);
    ImGui::Dummy({1.0f, h});
}

bool switch_row(Kit& k, const char* icon, const char* label, bool* value, const char* info) {
    float w = 0;
    const ImVec2 pos = row_start(&w);
    const float h = k.s(tok::kHit);
    ImGui::PushID(label);
    ImGui::SetNextItemAllowOverlap();
    const bool pressed = ImGui::InvisibleButton("##row", {w, h});
    const bool held = ImGui::IsItemActive();
    const ImGuiID id = ImGui::GetItemID();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (held || hovered_by_mouse(k)) {
        dl->AddRectFilled(pos, {pos.x + w, pos.y + h}, u32(held ? col::kFillPressed : col::kFill),
                          k.s(tok::kRadiusMd));
    }
    float x = pos.x + k.s(tok::kS3);
    if (icon != nullptr) {
        glyph(k, dl, icon, {x + k.s(tok::kIcon) * 0.5f, pos.y + h * 0.5f}, k.s(tok::kIcon) * 0.9f,
              col::kText2);
    }
    x += k.s(tok::kIcon) + k.s(tok::kS3);
    const float size = k.s(tok::kBody);
    const float sw = k.s(40.0f);
    const float room = pos.x + w - k.s(tok::kS3) - sw - x - (info ? k.s(32.0f) : 0.0f);
    const std::string shown = fit(k.regular, size, label, room);
    const ImVec2 ts = text_size(k.regular, size, shown.c_str());
    text_at(k, dl, k.regular, size, {x, pos.y + (h - ts.y) * 0.5f}, col::kText, shown.c_str());
    switch_knob(k, dl, {pos.x + w - k.s(tok::kS3) - sw, pos.y + (h - k.s(24.0f)) * 0.5f}, *value,
                id);
    if (info != nullptr) {
        info_button(k, info, {x + ts.x + k.s(16.0f), pos.y + h * 0.5f});
    }
    ImGui::PopID();
    end_row(pos, {w, h});
    if (tapped(pressed)) {
        *value = !*value;
        return true;
    }
    return false;
}

bool nav_row(Kit& k, const char* icon, const char* label, const std::string& value) {
    float w = 0;
    const ImVec2 pos = row_start(&w);
    const float h = k.s(tok::kHit);
    ImGui::PushID(label);
    const bool pressed = ImGui::InvisibleButton("##nav", {w, h});
    const bool held = ImGui::IsItemActive();
    ImGui::PopID();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (held || hovered_by_mouse(k)) {
        dl->AddRectFilled(pos, {pos.x + w, pos.y + h}, u32(held ? col::kFillPressed : col::kFill),
                          k.s(tok::kRadiusMd));
    }
    float x = pos.x + k.s(tok::kS3);
    if (icon != nullptr) {
        glyph(k, dl, icon, {x + k.s(tok::kIcon) * 0.5f, pos.y + h * 0.5f}, k.s(tok::kIcon) * 0.9f,
              col::kText2);
    }
    x += k.s(tok::kIcon) + k.s(tok::kS3);
    const float size = k.s(tok::kBody);
    const ImVec2 ls = text_size(k.regular, size, label);
    text_at(k, dl, k.regular, size, {x, pos.y + (h - ls.y) * 0.5f}, col::kText, label);
    const float chevron_x = pos.x + w - k.s(tok::kS3) - k.s(tok::kIconSm) * 0.5f;
    glyph(k, dl, icon::kChevron, {chevron_x, pos.y + h * 0.5f}, k.s(tok::kIconSm), col::kText3);
    const float vs = k.s(tok::kLabel);
    const float room = chevron_x - k.s(tok::kIconSm) - (x + ls.x + k.s(tok::kS3));
    const std::string shown = fit(k.regular, vs, value, room);
    const ImVec2 ts = text_size(k.regular, vs, shown.c_str());
    text_at(k, dl, k.regular, vs, {chevron_x - k.s(tok::kIconSm) - ts.x, pos.y + (h - ts.y) * 0.5f},
            col::kText2, shown.c_str());
    return tapped(pressed);
}

bool segmented(Kit& k, const char* id, const char* icon, const char* label,
               std::span<const Seg> items, int* current) {
    float w = 0;
    ImVec2 pos = row_start(&w);
    const float h = k.s(tok::kHit);
    const int n = static_cast<int>(items.size());
    const float lsize = k.s(tok::kBody);
    const float csize = k.s(tok::kLabel);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // The narrowest each cell can be: its icon and label, with padding.
    float min_cell = 0.0f;
    for (const Seg& s : items) {
        float cw = 0.0f;
        if (s.icon != nullptr) {
            cw += k.s(tok::kIconSm);
        }
        if (s.label != nullptr) {
            cw += (cw > 0 ? k.s(6.0f) : 0.0f) + text_size(k.regular, csize, s.label).x;
        }
        min_cell = std::max(min_cell, cw + k.s(tok::kS4));
    }
    // The label, or an icon alone, sits left of the track; text lines up with the other rows.
    float label_w = 0.0f;
    if (label != nullptr) {
        label_w = k.s(tok::kS3) + k.s(tok::kIcon) + k.s(tok::kS3) +
                  text_size(k.regular, lsize, label).x + k.s(tok::kS4);
    } else if (icon != nullptr) {
        label_w = k.s(tok::kS3) + k.s(tok::kIcon) + k.s(tok::kS3);
    }
    const bool stacked = label != nullptr && label_w + min_cell * static_cast<float>(n) > w;
    const ImVec2 start = pos;
    if (label != nullptr || icon != nullptr) {
        const float lh = stacked ? k.s(26.0f) : h;
        float x = pos.x + k.s(tok::kS3);
        if (icon != nullptr) {
            glyph(k, dl, icon, {x + k.s(tok::kIcon) * 0.5f, pos.y + lh * 0.5f},
                  k.s(tok::kIcon) * 0.9f, col::kText2);
        }
        x += k.s(tok::kIcon) + k.s(tok::kS3);
        if (label != nullptr) {
            const ImVec2 ls = text_size(k.regular, lsize, label);
            text_at(k, dl, k.regular, lsize, {x, pos.y + (lh - ls.y) * 0.5f}, col::kText, label);
        }
        if (stacked) {
            pos.y += lh;
        }
    }
    const float x0 = stacked ? pos.x : pos.x + label_w;
    const float track_w = pos.x + w - x0;
    const float ch = k.s(tok::kControl);
    const ImVec2 ta{x0, pos.y + (h - ch) * 0.5f};
    const ImVec2 tb{x0 + track_w, ta.y + ch};
    dl->AddRectFilled(ta, tb, u32(col::kFill), k.s(tok::kRadiusSm) + k.s(2.0f));
    const float cell = track_w / static_cast<float>(n);

    ImGui::PushID(id);
    const ImGuiID thumb_id = ImGui::GetID("##thumb");
    if (*current >= 0 && *current < n) {
        const float at = tween(thumb_id, static_cast<float>(*current), tok::kFast);
        const float inset = k.s(2.5f);
        const ImVec2 a{ta.x + cell * at + inset, ta.y + inset};
        const ImVec2 b{ta.x + cell * (at + 1.0f) - inset, tb.y - inset};
        dl->AddRectFilled(a, b, u32(with_alpha(col::kAccent, 0.22f)), k.s(tok::kRadiusSm));
        dl->AddRect(a, b, u32(with_alpha(col::kAccent, 0.45f)), k.s(tok::kRadiusSm), 0, 1.0f);
    }
    bool changed = false;
    for (int i = 0; i < n; ++i) {
        const Seg& s = items[static_cast<std::size_t>(i)];
        ImGui::SetCursorScreenPos({ta.x + cell * static_cast<float>(i), pos.y});
        ImGui::PushID(i);
        const bool pressed = ImGui::InvisibleButton("##seg", {cell, h});
        const bool held = ImGui::IsItemActive();
        ImGui::PopID();
        const bool on = i == *current;
        if (held && !on) {
            dl->AddRectFilled(
                {ta.x + cell * static_cast<float>(i) + k.s(2.5f), ta.y + k.s(2.5f)},
                {ta.x + cell * static_cast<float>(i + 1) - k.s(2.5f), tb.y - k.s(2.5f)},
                u32(col::kFillPressed), k.s(tok::kRadiusSm));
        }
        if (i > 0 && !on && i - 1 != *current) {
            const float lx = ta.x + cell * static_cast<float>(i);
            dl->AddLine({lx, ta.y + ch * 0.28f}, {lx, tb.y - ch * 0.28f}, u32(col::kHairline),
                        1.0f);
        }
        const ImVec4 ink = on ? col::kText : col::kText2;
        float cw = 0.0f;
        const bool show_label =
            s.label != nullptr &&
            (s.icon == nullptr || text_size(k.regular, csize, s.label).x + k.s(tok::kIconSm) +
                                          k.s(6.0f) + k.s(tok::kS2) <=
                                      cell);
        if (s.icon != nullptr) {
            cw += k.s(tok::kIconSm);
        }
        if (show_label) {
            cw += (cw > 0 ? k.s(6.0f) : 0.0f) + text_size(k.regular, csize, s.label).x;
        }
        float x = ta.x + cell * static_cast<float>(i) + (cell - cw) * 0.5f;
        const float cy = (ta.y + tb.y) * 0.5f;
        if (s.icon != nullptr) {
            glyph(k, dl, s.icon, {x + k.s(tok::kIconSm) * 0.5f, cy}, k.s(tok::kIconSm),
                  on ? col::kAccent : ink);
            x += k.s(tok::kIconSm) + k.s(6.0f);
        }
        if (show_label) {
            const std::string shown = fit(k.regular, csize, s.label, cell - k.s(6.0f));
            const ImVec2 ts = text_size(k.regular, csize, shown.c_str());
            if (s.icon == nullptr) {
                x = ta.x + cell * static_cast<float>(i) + (cell - ts.x) * 0.5f;
            }
            text_at(k, dl, on ? k.bold : k.regular, csize, {x, cy - ts.y * 0.5f}, ink,
                    shown.c_str());
        }
        if (s.label != nullptr && !show_label) {
            long_press_tip(k, s.label);
        }
        if (tapped(pressed) && !on) {
            *current = i;
            changed = true;
        }
    }
    ImGui::PopID();
    end_row(start, {w, pos.y - start.y + h});
    return changed;
}

namespace {

// Width, in design points, a slider's icon, gaps and label are taken to need at most, and the
// shortest track worth drawing beside them. Below their sum plus the value, a row stacks.
constexpr float kSliderLabelRef = 132.0f;
constexpr float kSliderMinTrack = 60.0f;

bool slider_core(Kit& k, const char* icon, const char* label, float* t, const char* value_text,
                 const char* info) {
    float w = 0;
    const ImVec2 start = row_start(&w);
    ImVec2 pos = start;
    const float h = k.s(tok::kHit);
    const float lsize = k.s(tok::kBody);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::PushID(label);
    const float value_w = k.s(52.0f);
    float x = pos.x + k.s(tok::kS3);
    float label_end = x;
    const ImVec2 ls = text_size(k.regular, lsize, label);
    const float label_w =
        k.s(tok::kS3) + k.s(tok::kIcon) + k.s(tok::kS3) + ls.x + (info ? k.s(28.0f) : 0.0f);
    // Sliders in a column stack together: the decision uses at least the width of the longest
    // label in use today ("Notes ahead"), so rows only differ if a new label is wider than that.
    const bool stacked =
        std::max(label_w, k.s(kSliderLabelRef)) + k.s(kSliderMinTrack) + value_w + k.s(tok::kS4) >
        w;
    const float lh = stacked ? k.s(26.0f) : h;
    if (icon != nullptr) {
        glyph(k, dl, icon, {x + k.s(tok::kIcon) * 0.5f, pos.y + lh * 0.5f}, k.s(tok::kIcon) * 0.9f,
              col::kText2);
    }
    x += k.s(tok::kIcon) + k.s(tok::kS3);
    text_at(k, dl, k.regular, lsize, {x, pos.y + (lh - ls.y) * 0.5f}, col::kText, label);
    label_end = x + ls.x;
    if (info != nullptr) {
        info_button(k, info, {label_end + k.s(16.0f), pos.y + lh * 0.5f});
        label_end += k.s(28.0f);
    }
    const float vsize = k.s(tok::kLabel);
    const ImVec2 vs = text_size(k.regular, vsize, value_text);
    text_at(k, dl, k.regular, vsize, {pos.x + w - k.s(tok::kS3) - vs.x, pos.y + (lh - vs.y) * 0.5f},
            col::kText2, value_text);
    float tx0 = 0;
    float tx1 = 0;
    if (stacked) {
        pos.y += lh;
        tx0 = pos.x + k.s(tok::kS3) + k.s(9.0f);
        tx1 = pos.x + w - k.s(tok::kS3) - k.s(9.0f);
    } else {
        tx0 = std::max(label_end + k.s(tok::kS5), pos.x + w * 0.42f);
        tx1 = pos.x + w - k.s(tok::kS3) - value_w;
    }
    // The hit area is the whole track row, the knob's radius past each end.
    ImGui::SetCursorScreenPos({tx0 - k.s(14.0f), pos.y});
    ImGui::InvisibleButton("##track", {tx1 - tx0 + k.s(28.0f), h});
    const bool active = ImGui::IsItemActive();
    bool changed = false;
    if (active) {
        k.slider_active = true;
        const float nt = std::clamp((ImGui::GetIO().MousePos.x - tx0) / (tx1 - tx0), 0.0f, 1.0f);
        if (nt != *t) {
            *t = nt;
            changed = true;
        }
    }
    const float cy = pos.y + h * 0.5f;
    const float th = k.s(4.0f);
    dl->AddRectFilled({tx0, cy - th * 0.5f}, {tx1, cy + th * 0.5f}, u32({1, 1, 1, 0.12f}), th);
    const float kx = tx0 + (tx1 - tx0) * *t;
    dl->AddRectFilled({tx0, cy - th * 0.5f}, {kx, cy + th * 0.5f}, u32(col::kAccent), th);
    const float kr = k.s(active ? 10.5f : 9.0f);
    dl->AddCircleFilled({kx, cy + k.s(1.0f)}, kr + k.s(1.0f), u32({0, 0, 0, 0.3f}), 24);
    dl->AddCircleFilled({kx, cy}, kr, u32({0.96f, 0.97f, 1.0f, 1.0f}), 24);
    ImGui::PopID();
    end_row(start, {w, pos.y - start.y + h});
    return changed;
}

} // namespace

bool slider_row(Kit& k, const char* icon, const char* label, float* value, float lo, float hi,
                float shown_scale, const char* format, const char* info) {
    char text[32];
    std::snprintf(text, sizeof text, format, static_cast<double>(*value * shown_scale));
    float t = (*value - lo) / (hi - lo);
    if (slider_core(k, icon, label, &t, text, info)) {
        *value = lo + t * (hi - lo);
        return true;
    }
    return false;
}

bool slider_row_int(Kit& k, const char* icon, const char* label, int* value, int lo, int hi,
                    const char* unit, const char* info) {
    char text[32];
    std::snprintf(text, sizeof text, "%d %s", *value, unit);
    float t = static_cast<float>(*value - lo) / static_cast<float>(hi - lo);
    if (slider_core(k, icon, label, &t, text, info)) {
        const int v = lo + static_cast<int>(std::lround(t * static_cast<float>(hi - lo)));
        if (v != *value) {
            *value = v;
            return true;
        }
    }
    return false;
}

bool stepper_row(Kit& k, const char* icon, const char* label, int* value, int lo, int hi,
                 const char* unit) {
    float w = 0;
    const ImVec2 pos = row_start(&w);
    const float h = k.s(tok::kHit);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::PushID(label);
    float x = pos.x + k.s(tok::kS3);
    if (icon != nullptr) {
        glyph(k, dl, icon, {x + k.s(tok::kIcon) * 0.5f, pos.y + h * 0.5f}, k.s(tok::kIcon) * 0.9f,
              col::kText2);
    }
    x += k.s(tok::kIcon) + k.s(tok::kS3);
    // The control: a track with a minus cell, the value and a plus cell, each a full hit.
    const float cell = k.s(tok::kHit);
    const float value_w = k.s(56.0f);
    const float track_w = 2.0f * cell + value_w;
    const float x0 = pos.x + w - track_w;
    const float lsize = k.s(tok::kBody);
    const std::string shown = fit(k.regular, lsize, label, x0 - k.s(tok::kS2) - x);
    const ImVec2 ls = text_size(k.regular, lsize, shown.c_str());
    text_at(k, dl, k.regular, lsize, {x, pos.y + (h - ls.y) * 0.5f}, col::kText, shown.c_str());
    const float ch = k.s(tok::kControl);
    const ImVec2 ta{x0, pos.y + (h - ch) * 0.5f};
    dl->AddRectFilled(ta, {ta.x + track_w, ta.y + ch}, u32(col::kFill),
                      k.s(tok::kRadiusSm) + k.s(2.0f));
    bool changed = false;
    const auto button = [&](const char* id, const char* glyph_text, float bx, int delta) {
        ImGui::SetCursorScreenPos({bx, pos.y});
        const bool enabled = delta < 0 ? *value > lo : *value < hi;
        const bool pressed = ImGui::InvisibleButton(id, {cell, h});
        const bool held = ImGui::IsItemActive() && enabled;
        const ImVec2 c{bx + cell * 0.5f, pos.y + h * 0.5f};
        if (held) {
            dl->AddRectFilled({bx + k.s(2.5f), ta.y + k.s(2.5f)},
                              {bx + cell - k.s(2.5f), ta.y + ch - k.s(2.5f)},
                              u32(col::kFillPressed), k.s(tok::kRadiusSm));
        }
        glyph(k, dl, glyph_text, c, k.s(tok::kIconSm),
              enabled ? col::kText : with_alpha(col::kText3, 0.6f));
        if (pressed && enabled) {
            *value = std::clamp(*value + delta, lo, hi);
            changed = true;
        }
    };
    button("##minus", icon::kMinus, x0, -1);
    button("##plus", icon::kPlus, x0 + cell + value_w, 1);
    char text[32];
    std::snprintf(text, sizeof text, "%d %s", *value, unit);
    const float vsize = k.s(tok::kLabel);
    const ImVec2 vs = text_size(k.bold, vsize, text);
    text_at(k, dl, k.bold, vsize, {x0 + cell + (value_w - vs.x) * 0.5f, pos.y + (h - vs.y) * 0.5f},
            col::kText, text);
    ImGui::PopID();
    end_row(pos, {w, h});
    return changed;
}

bool armed(const Kit& k, ImGuiID id) { return k.confirm_id == id && k.now < k.confirm_until; }

bool icon_action(Kit& k, const char* id, const char* icon, const char* label, float size,
                 bool enabled, bool danger, bool is_armed) {
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::InvisibleButton(id, {size, size});
    ImGui::EndDisabled();
    const bool held = ImGui::IsItemActive();
    if (enabled) {
        long_press_tip(k, label);
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float d = k.s(tok::kControl);
    const ImVec2 a{pos.x + (size - d) * 0.5f, pos.y + (size - d) * 0.5f};
    const ImVec2 b{a.x + d, a.y + d};
    ImVec4 fill = held ? col::kFillPressed : (enabled ? col::kFill : with_alpha(col::kFill, 0.4f));
    ImVec4 ink = enabled ? (danger ? col::kDanger : col::kText) : with_alpha(col::kText3, 0.6f);
    if (is_armed) {
        fill = with_alpha(col::kDanger, 0.22f);
        ink = col::kDanger;
    }
    dl->AddRectFilled(a, b, u32(fill), k.s(tok::kRadiusSm) + k.s(1.0f));
    glyph(k, dl, icon, {(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f}, k.s(tok::kIconSm) * 1.05f, ink);
    return enabled && tapped(pressed);
}

bool text_button(Kit& k, const char* id, const char* icon, const char* label, bool confirm,
                 const char* confirm_label) {
    float w = 0;
    const ImVec2 pos = row_start(&w);
    const float h = k.s(tok::kHit);
    const bool pressed = ImGui::InvisibleButton(id, {w, h});
    const ImGuiID item = ImGui::GetItemID();
    const bool held = ImGui::IsItemActive();
    const bool is_armed = confirm && armed(k, item);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float ch = k.s(tok::kControl) + k.s(4.0f);
    const ImVec2 a{pos.x, pos.y + (h - ch) * 0.5f};
    const ImVec2 b{pos.x + w, a.y + ch};
    ImVec4 fill = held ? col::kFillPressed : col::kFill;
    ImVec4 ink = col::kText;
    if (is_armed) {
        fill = with_alpha(col::kDanger, 0.18f);
        ink = col::kDanger;
    }
    dl->AddRectFilled(a, b, u32(fill), k.s(tok::kRadiusMd));
    const char* text = is_armed && confirm_label ? confirm_label : label;
    const float size = k.s(tok::kLabel);
    const ImVec2 ts = text_size(k.regular, size, text);
    const float total = ts.x + (icon ? k.s(tok::kIconSm) + k.s(tok::kS2) : 0.0f);
    float x = pos.x + (w - total) * 0.5f;
    if (icon != nullptr) {
        glyph(k, dl, icon, {x + k.s(tok::kIconSm) * 0.5f, pos.y + h * 0.5f}, k.s(tok::kIconSm),
              ink);
        x += k.s(tok::kIconSm) + k.s(tok::kS2);
    }
    text_at(k, dl, k.regular, size, {x, pos.y + (h - ts.y) * 0.5f}, ink, text);
    if (!tapped(pressed)) {
        return false;
    }
    if (!confirm) {
        return true;
    }
    if (is_armed) {
        k.confirm_id = 0;
        return true;
    }
    k.confirm_id = item;
    k.confirm_until = k.now + 3.0;
    return false;
}

bool tile(Kit& k, const char* id, const char* icon, const char* label, const std::string& value,
          ImVec2 size) {
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, size);
    const bool held = ImGui::IsItemActive();
    const bool hover = hovered_by_mouse(k);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 b{pos.x + size.x, pos.y + size.y};
    dl->AddRectFilled(pos, b,
                      u32(held ? col::kFillPressed
                               : (hover ? mix(col::kFill, col::kFillPressed, 0.5f) : col::kFill)),
                      k.s(tok::kRadiusMd));
    dl->AddRect(pos, b, u32(col::kHairline), k.s(tok::kRadiusMd), 0, 1.0f);
    const float pad = k.s(tok::kS3);
    const float r = k.s(16.0f);
    const ImVec2 ic{pos.x + pad + r, pos.y + pad + r};
    dl->AddCircleFilled(ic, r, u32(col::kAccentSoft), 32);
    glyph(k, dl, icon, ic, k.s(tok::kIconSm) * 1.1f, col::kAccent);
    const float ts = k.s(tok::kBody);
    const float vs = k.s(tok::kCaption) + k.s(1.0f);
    const float avail = size.x - 2.0f * pad;
    const float label_y = b.y - pad - vs - k.s(3.0f) - ts;
    text_at(k, dl, k.bold, ts, {pos.x + pad, label_y}, col::kText, label);
    const std::string shown = fit(k.regular, vs, value, avail);
    text_at(k, dl, k.regular, vs, {pos.x + pad, b.y - pad - vs}, col::kText2, shown.c_str());
    return tapped(pressed);
}

int header(Kit& k, const char* title, bool back, bool close) {
    float w = 0;
    const ImVec2 pos = row_start(&w);
    const float h = k.s(tok::kHit) + k.s(4.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    int result = 0;
    float x = pos.x;
    const float hit = k.s(tok::kHit);
    if (back) {
        ImGui::SetCursorScreenPos({pos.x - k.s(tok::kS1), pos.y + (h - hit) * 0.5f});
        const bool pressed = ImGui::InvisibleButton("##back", {hit, hit});
        const bool held = ImGui::IsItemActive();
        const ImVec2 c{pos.x - k.s(tok::kS1) + hit * 0.5f, pos.y + h * 0.5f};
        if (held) {
            dl->AddCircleFilled(c, hit * 0.42f, u32(col::kFillPressed), 32);
        }
        glyph(k, dl, icon::kBack, c, k.s(22.0f), col::kAccent);
        if (tapped(pressed)) {
            result = 1;
        }
        x += hit - k.s(tok::kS2);
    } else {
        x += k.s(tok::kS3);
    }
    const float size = k.s(tok::kTitle);
    const ImVec2 ts = text_size(k.bold, size, title);
    text_at(k, dl, k.bold, size, {x, pos.y + (h - ts.y) * 0.5f}, col::kText, title);
    if (close) {
        ImGui::SetCursorScreenPos({pos.x + w - hit + k.s(tok::kS1), pos.y + (h - hit) * 0.5f});
        const bool pressed = ImGui::InvisibleButton("##close", {hit, hit});
        const bool held = ImGui::IsItemActive();
        const ImVec2 c{pos.x + w - hit * 0.5f + k.s(tok::kS1), pos.y + h * 0.5f};
        dl->AddCircleFilled(c, k.s(15.0f), u32(held ? col::kFillPressed : col::kFill), 32);
        glyph(k, dl, icon::kClose, c, k.s(tok::kIconSm), col::kText2);
        if (tapped(pressed)) {
            result = 2;
        }
    }
    end_row(pos, {w, h});
    return result;
}

void drag_scroll(Kit& k) {
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsMouseClicked(0) &&
        ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
        k.scroll_press = true;
        k.scrolling = false;
        k.press_y = io.MousePos.y;
    }
    if (!ImGui::IsMouseDown(0)) {
        k.scroll_press = false;
        k.scrolling = false;
        return;
    }
    if (!k.scroll_press || k.slider_active || ImGui::GetScrollMaxY() <= 0.0f) {
        return;
    }
    if (!k.scrolling && std::fabs(io.MousePos.y - k.press_y) > k.s(10.0f)) {
        k.scrolling = true;
        ImGui::ClearActiveID();
    }
    if (k.scrolling) {
        ImGui::SetScrollY(ImGui::GetScrollY() - io.MouseDelta.y);
    }
}

void finish_frame(Kit& k) {
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    if (!k.tip.empty()) {
        const float size = k.s(tok::kLabel);
        const ImVec2 ts = text_size(k.regular, size, k.tip.c_str());
        const float pad_x = k.s(tok::kS3);
        const float bh = ts.y + k.s(tok::kS2) * 2.0f;
        const float cx = (k.tip_min.x + k.tip_max.x) * 0.5f;
        // Above the control unless that leaves the screen; a finger covers the control itself.
        float y = k.tip_min.y - bh - k.s(tok::kS2);
        if (y < k.s(tok::kS1)) {
            y = k.tip_max.y + k.s(tok::kS2);
        }
        float x = std::clamp(cx - ts.x * 0.5f - pad_x, k.s(tok::kS2),
                             display.x - ts.x - 2.0f * pad_x - k.s(tok::kS2));
        card(k, dl, {x, y}, {x + ts.x + 2.0f * pad_x, y + bh}, bh * 0.5f, col::kSurfaceHi,
             ImDrawFlags_RoundCornersAll, 0.8f);
        text_at(k, dl, k.regular, size, {x + pad_x, y + k.s(tok::kS2)}, col::kText, k.tip.c_str());
        k.tip.clear();
    }
    if (k.info_id != 0) {
        if (ImGui::IsMouseClicked(0) && !k.info_seen) {
            k.info_id = 0;
        } else {
            const float size = k.s(tok::kLabel);
            const float wrap = std::min(k.s(260.0f), display.x - k.s(32.0f));
            const ImVec2 ts = k.regular->CalcTextSizeA(size, FLT_MAX, wrap, k.info_text.c_str());
            const float pad = k.s(tok::kS3);
            const float bw = ts.x + 2.0f * pad;
            const float bh = ts.y + 2.0f * pad;
            const float x = std::clamp(k.info_anchor.x - bw * 0.5f, k.s(tok::kS2),
                                       display.x - bw - k.s(tok::kS2));
            float y = k.info_anchor.y + k.s(tok::kS1);
            if (y + bh > display.y - k.s(tok::kS2)) {
                y = k.info_anchor.y - k.s(30.0f) - bh;
            }
            card(k, dl, {x, y}, {x + bw, y + bh}, k.s(tok::kRadiusMd), col::kSurfaceHi);
            dl->AddText(k.regular, size, {x + pad, y + pad}, u32(col::kText), k.info_text.c_str(),
                        nullptr, wrap);
        }
    }
    k.info_seen = false;
    k.slider_active = false;
}

} // namespace toonval::app::ui
