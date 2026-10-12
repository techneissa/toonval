// SPDX-License-Identifier: GPL-3.0-or-later
// The Ui (app/ui.hpp): ImGui setup, the top row (status text, gear, the bar while revealed), when
// the bar shows and hides, the settings sheet and the resume countdown.
#include "../ui.hpp"

#include "bar.hpp"
#include "kit.hpp"
#include "pages.hpp"
#include "preview.hpp"
#include "sheet.hpp"
#include "transport.hpp"

#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_sdlrenderer3.h>
#include <imgui_internal.h> // BringWindowToDisplayFront

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

namespace toonval::app {

// Generated from the font files by cmake/EmbedFile.cmake.
std::span<const unsigned char> embedded_font();
std::span<const unsigned char> embedded_font_regular();
std::span<const unsigned char> embedded_icons();

bool parse_panel_tab(const char* text, PanelTab* out) {
    struct Entry {
        const char* name;
        PanelTab tab;
    };
    static const Entry kEntries[] = {
        {"quick", PanelTab::Quick},       {"look", PanelTab::Look}, {"names", PanelTab::Names},
        {"effects", PanelTab::Effects},   {"view", PanelTab::View}, {"presets", PanelTab::Presets},
        {"practice", PanelTab::Practice},
    };
    for (const Entry& e : kEntries) {
        if (std::strcmp(text, e.name) == 0) {
            *out = e.tab;
            return true;
        }
    }
    return false;
}

float display_px_per_mm(SDL_Window* window) {
    float px_per_mm = kDefaultPxPerMm;
    if (window != nullptr) {
        const float density = SDL_GetWindowPixelDensity(window);
        if (density > 0.0f) {
            px_per_mm = density * kDefaultPxPerMm;
        }
    }
    return px_per_mm;
}

namespace ui {

bool begin_layer(const char* id, ImVec2 pos, ImVec2 size, bool scroll) {
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
                             ImGuiWindowFlags_NoFocusOnAppearing;
    if (!scroll) {
        flags |= ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar;
    }
    return ImGui::Begin(id, nullptr, flags);
}

bool tapped_outside(const Frame& f, ImVec2 min, ImVec2 max) {
    if (!ImGui::IsMouseClicked(0) || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
        return false;
    }
    const ImVec2 m = ImGui::GetIO().MousePos;
    if (m.y < f.row_max.y) {
        return false; // the bar and the gear handle their own taps
    }
    return m.x < min.x || m.x > max.x || m.y < min.y || m.y > max.y;
}

} // namespace ui

namespace {

using namespace ui;

// The bar stays this long after the last use, shorter while the player is at the keys.
constexpr double kRevealHoldS = 3.0;
constexpr double kRevealHoldPlayingS = 1.0;
constexpr double kTouchHoldS = 4.0; // the gear stays bright this long after a touch
// The status text keeps at least this much of the row when the bar comes in, if it needs it
// (design points); the dock keeps a little more, being centred.
constexpr float kKeepStatus = 180.0f;
constexpr float kKeepStatusDock = 220.0f;
// Below this much status text (design points) the bar switches to its compact form.
constexpr float kMinStatus = 120.0f;

void apply_style(const Kit& k) {
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.WindowPadding = {0.0f, 0.0f};
    s.WindowRounding = 0.0f;
    s.WindowBorderSize = 0.0f;
    s.ChildBorderSize = 0.0f;
    s.PopupBorderSize = 0.0f;
    s.FrameBorderSize = 0.0f;
    s.ItemSpacing = {k.s(6.0f), k.s(4.0f)};
    s.ItemInnerSpacing = {k.s(6.0f), k.s(4.0f)};
    s.ScrollbarSize = k.s(4.0f);
    s.ScrollbarRounding = k.s(2.0f);
    s.FrameRounding = k.s(tok::kRadiusMd);
    s.TouchExtraPadding = {0.0f, 0.0f};
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = col::kText;
    c[ImGuiCol_TextDisabled] = col::kText3;
    c[ImGuiCol_WindowBg] = {0, 0, 0, 0};
    c[ImGuiCol_ChildBg] = {0, 0, 0, 0};
    c[ImGuiCol_PopupBg] = {0, 0, 0, 0};
    c[ImGuiCol_FrameBg] = col::kFill;
    c[ImGuiCol_FrameBgHovered] = col::kFill;
    c[ImGuiCol_FrameBgActive] = col::kFillPressed;
    c[ImGuiCol_ScrollbarBg] = {0, 0, 0, 0};
    c[ImGuiCol_ScrollbarGrab] = {1, 1, 1, 0.16f};
    c[ImGuiCol_ScrollbarGrabHovered] = {1, 1, 1, 0.24f};
    c[ImGuiCol_ScrollbarGrabActive] = {1, 1, 1, 0.3f};
    c[ImGuiCol_TextSelectedBg] = with_alpha(col::kAccent, 0.4f);
    c[ImGuiCol_ModalWindowDimBg] = col::kScrim;
    c[ImGuiCol_NavCursor] = {0, 0, 0, 0};
    c[ImGuiCol_InputTextCursor] = col::kAccent;
}

Page page_for(PanelTab tab) {
    switch (tab) {
    case PanelTab::Quick:
        return Page::Quick;
    case PanelTab::Look:
        return Page::Look;
    case PanelTab::Names:
        return Page::Names;
    case PanelTab::Effects:
        return Page::Effects;
    case PanelTab::View:
        return Page::View;
    case PanelTab::Presets:
        return Page::Presets;
    case PanelTab::Practice:
        return Page::Practice;
    }
    return Page::Quick;
}

std::unique_ptr<BarLook> make_bar(BarStyle style) {
    switch (style) {
    case BarStyle::Groups:
        return make_groups_bar();
    case BarStyle::Dock:
        return make_dock_bar();
    case BarStyle::Strip:
        break;
    }
    return make_strip_bar();
}

ImVec2 to_points(float x, float y, float scale) { return {x * scale, y * scale}; }

} // namespace

struct Ui::Impl {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    std::unique_ptr<LivePreview> preview;
    std::unique_ptr<BarLook> bar;
    BarStyle bar_style = BarStyle::Strip;
    SettingsSheet sheet;
    Kit kit;
    bool open = false;
    float open_p = 0.0f;   // linear progress of the settings open/close
    float reveal_p = 0.0f; // and of the bar
    bool revealed = false;
    double last_use = -1e9;
    double last_touch = -1e9;
};

Ui::Ui(SDL_Window* window, SDL_Renderer* renderer) : impl_(std::make_unique<Impl>()) {
    impl_->window = window;
    impl_->renderer = renderer;
    impl_->preview = std::make_unique<LivePreview>(renderer);
    impl_->kit.preview = impl_->preview.get();
    impl_->kit.renderer = renderer;
    impl_->bar = make_bar(impl_->bar_style);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // the settings file is ours, not imgui.ini
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    // Each text font gets the icon font merged in, so a string can mix words and icons.
    const auto add = [&io](std::span<const unsigned char> data, bool merge) {
        ImFontConfig cfg;
        cfg.FontDataOwnedByAtlas = false; // the bytes are compiled into the program
        cfg.MergeMode = merge;
        return io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(data.data()),
                                              static_cast<int>(data.size()), 0.0f, &cfg);
    };
    impl_->kit.regular = add(embedded_font_regular(), false);
    add(embedded_icons(), true);
    impl_->kit.bold = add(embedded_font(), false);
    add(embedded_icons(), true);
}

Ui::~Ui() {
    impl_->preview.reset(); // its textures belong to the renderer, still alive here
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}

bool Ui::process_event(const SDL_Event& event, std::int64_t now_ns) {
    ImGui_ImplSDL3_ProcessEvent(&event);
    switch (event.type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_FINGER_DOWN:
        impl_->last_touch = static_cast<double>(now_ns) * 1e-9;
        break;
    default:
        break;
    }
    if (ImGui::GetIO().WantTextInput) {
        return event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP ||
               event.type == SDL_EVENT_TEXT_INPUT;
    }
    return false;
}

bool Ui::panel_open() const { return impl_->open; }

void Ui::open_panel(PanelTab tab) {
    impl_->open = true;
    impl_->bar->close_popovers();
    impl_->sheet.open_at(page_for(tab));
}

void Ui::close_panel() { impl_->open = false; }

std::string Ui::take_message() {
    std::string out;
    out.swap(impl_->kit.message);
    return out;
}

void Ui::draw(UiModel& model, SongPort& song, const UiFrameInfo& info) {
    Impl& im = *impl_;
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    // The backend asks SDL for the display scale, which macOS reports as 1 on a high-density
    // window. The renderer's output size is what is drawn into, so take the scale from that.
    {
        ImGuiIO& frame_io = ImGui::GetIO();
        if (info.width_px > 0.0f && info.height_px > 0.0f && frame_io.DisplaySize.x > 0.0f &&
            frame_io.DisplaySize.y > 0.0f) {
            frame_io.DisplayFramebufferScale = {info.width_px / frame_io.DisplaySize.x,
                                                info.height_px / frame_io.DisplaySize.y};
        }
    }
    ImGui::NewFrame();
    const ImGuiIO& io = ImGui::GetIO();
    // ImGui works in window points; the layout is in output pixels.
    const float scale = info.width_px > 0.0f ? io.DisplaySize.x / info.width_px : 1.0f;
    Kit& k = im.kit;
    k.u = std::max(0.75f, info.px_per_mm * scale / kDefaultPxPerMm);
    k.dt = io.DeltaTime;
    k.now = static_cast<double>(info.now_ns) * 1e-9;
    k.now_ns = info.now_ns;
    k.touch = io.MouseSource == ImGuiMouseSource_TouchScreen;
    k.fb_scale = io.DisplayFramebufferScale.x > 0.0f ? io.DisplayFramebufferScale.x : 1.0f;
    k.short_screen = io.DisplaySize.y < k.s(600.0f);
    k.reduced_motion = model.look.reduced_effects;
    set_reduced_motion(k.reduced_motion);
    apply_style(k);

    if (model.app.bar_style != im.bar_style) {
        im.bar_style = model.app.bar_style;
        im.bar = make_bar(im.bar_style);
    }
    const SongView& view = song.view();

    const ScreenLayout& lay = info.layout;
    Frame f;
    f.display = io.DisplaySize;
    f.row_min = to_points(lay.top_row.x, lay.top_row.y, scale);
    f.row_max = to_points(lay.top_row.right(), lay.top_row.bottom(), scale);
    f.top = f.row_max.y + k.s(tok::kS2);
    f.keyboard_top = lay.keyboard.y * scale;
    const ImVec2 gear_min = to_points(lay.gear.x, lay.gear.y, scale);
    const ImVec2 gear_max = to_points(lay.gear.right(), lay.gear.bottom(), scale);
    const ImVec2 gear_c{(gear_min.x + gear_max.x) * 0.5f, (gear_min.y + gear_max.y) * 0.5f};

    // The status text: the label, then any notes, each a gap apart, at the layout's font size.
    const float status_size = lay.status_font_px * scale;
    std::string notes;
    for (const std::string_view n : info.status_notes) {
        if (!n.empty()) {
            notes += notes.empty() ? "" : "   ";
            notes += n;
        }
    }
    const std::string label(info.status_label);
    float status_w = 0.0f;
    if (lay.status_visible) {
        status_w = text_size(k.bold, status_size, label.c_str()).x;
        if (!notes.empty()) {
            status_w += k.s(tok::kS4) + text_size(k.regular, status_size, notes.c_str()).x;
        }
    }

    // The bar shows on a hover over the top row (a mouse) or a tap on it (a finger), and hides a
    // few seconds after the last use; never while the settings are open.
    const ImVec2 m = io.MousePos;
    const bool mouse_valid = m.x > -FLT_MAX * 0.5f && m.y > -FLT_MAX * 0.5f;
    const bool on_gear =
        m.x >= gear_min.x && m.x < gear_max.x && m.y >= gear_min.y && m.y < gear_max.y;
    // While the bar shows, the song overview under the top row counts as part of it.
    const float reach = im.revealed && view.has_song ? k.s(tok::kHit) + k.s(tok::kS2) : 0.0f;
    const bool in_row = mouse_valid && m.y >= f.row_min.y && m.y < f.row_max.y + reach &&
                        m.x >= 0.0f && m.x < f.display.x && !on_gear;
    const bool hovering = in_row && !k.touch;
    if (!im.open && ((ImGui::IsMouseClicked(0) && in_row) || hovering ||
                     (im.revealed && in_row && ImGui::IsMouseDown(0)) || im.bar->holds_bar())) {
        im.revealed = true;
        im.last_use = k.now;
    }
    const bool at_keys = info.playing || view.playing;
    const double hold = at_keys ? kRevealHoldPlayingS : kRevealHoldS;
    if (im.open || (im.revealed && k.now - im.last_use > hold)) {
        im.revealed = false;
        im.bar->close_popovers();
    }
    im.reveal_p = approach(im.reveal_p, im.revealed ? 1.0f : 0.0f, k.dt, tok::kMove);
    f.reveal = ease_out(im.reveal_p);
    f.live = im.revealed && im.reveal_p > 0.6f;

    im.open_p = approach(im.open_p, im.open ? 1.0f : 0.0f, k.dt, tok::kSheet);
    const float open = ease_out(im.open_p);

    // Where the bar goes: the layout gives it the right end of the status area (or its middle),
    // and the status text keeps the rest. Without room for the full bar it goes compact.
    TopRowSplit split{lay.status_text, {}};
    if (f.reveal > 0.001f) {
        const float keep_pt =
            std::min(status_w, k.s(im.bar->centred() ? kKeepStatusDock : kKeepStatus));
        const auto place = [&](bool compact) {
            return split_top_row(lay, im.bar->width(k, view, compact) / scale, keep_pt / scale,
                                 im.bar->centred());
        };
        split = place(false);
        const float full = im.bar->width(k, view, false);
        f.compact = split.bar.w * scale < full - 0.5f ||
                    split.status_text.w * scale < std::min(status_w, k.s(kMinStatus));
        if (f.compact) {
            split = place(true);
        }
        f.bar_min = to_points(split.bar.x, split.bar.y, scale);
        f.bar_max = to_points(split.bar.right(), split.bar.bottom(), scale);
    }

    ImGui::PushFont(k.regular, k.s(tok::kBody));

    // The sheet below, the bar and the gear above.
    if (im.open_p > 0.0f) {
        if (im.sheet.draw(k, model, f, open) && im.open) {
            im.open = false;
        }
    }
    if (f.reveal > 0.001f) {
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, f.reveal);
        const float under = view.has_song ? scrubber_height(k) + k.s(tok::kS3) : k.s(tok::kS1);
        if (begin_layer("##bar", {0.0f, f.row_min.y},
                        {f.display.x, f.row_max.y - f.row_min.y + under})) {
            im.bar->draw(k, model, song, f);
        }
        ImGui::End();
        ImGui::PopStyleVar();
        Page wanted = Page::Quick;
        if (im.bar->wants_open(&wanted)) {
            im.open = true;
            im.bar->close_popovers();
            im.sheet.open_at(wanted);
        }
    }
    {
        const bool recently = k.now - im.last_touch < kTouchHoldS;
        const float gear_alpha =
            tween(ImGui::GetID("##gear-alpha"),
                  at_keys && !im.open && !im.revealed && !recently ? 0.38f : 1.0f, 0.6f);
        const float hit = gear_max.y - gear_min.y;
        begin_layer("##gear-layer", gear_min, {gear_max.x - gear_min.x, hit});
        // Always on top: the bar's window spans the row and is often created later, which would
        // put it over the gear and take its taps.
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
        if (gear(k, gear_c, hit, im.open, gear_alpha)) {
            im.open = !im.open;
            if (im.open) {
                im.bar->close_popovers();
                im.sheet.reopen();
            }
        }
        ImGui::End();
    }
    // The status text ends where the bar begins (or before the gear while the bar is hidden),
    // shortened with an ellipsis rather than running under it.
    if (lay.status_visible && !label.empty()) {
        const float hidden_right = lay.status_text.right() * scale;
        const float shown_right = split.status_text.empty() ? lay.status_text.x * scale
                                                            : split.status_text.right() * scale;
        const float right = hidden_right + (shown_right - hidden_right) * f.reveal;
        const float x = lay.status_text.x * scale;
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const float cy = (f.row_min.y + f.row_max.y) * 0.5f;
        const std::string l = fit(k.bold, status_size, label, right - x);
        if (!l.empty()) {
            const ImVec2 ls = text_size(k.bold, status_size, l.c_str());
            dl->AddText(k.bold, status_size, {x, std::round(cy - ls.y * 0.5f)},
                        ImGui::GetColorU32(with_alpha(col::kText, 0.82f)), l.c_str());
            const float nx = x + ls.x + k.s(tok::kS4);
            if (!notes.empty() && l == label && right - nx > k.s(40.0f)) {
                const std::string n = fit(k.regular, status_size, notes, right - nx);
                dl->AddText(k.regular, status_size, {nx, std::round(cy - ls.y * 0.5f)},
                            ImGui::GetColorU32(col::kText2), n.c_str());
            }
        }
    }
    if (!im.open) {
        countdown_overlay(k, view, {0.0f, f.top}, {f.display.x, f.keyboard_top});
    }
    ImGui::PopFont();
    finish_frame(k);

    ImGui::Render();
    // This backend leaves the scaling to SDL's render scale: ImGui's points become output pixels.
    const ImVec2 fb = ImGui::GetDrawData()->FramebufferScale;
    SDL_SetRenderScale(im.renderer, fb.x, fb.y);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), im.renderer);
    SDL_SetRenderScale(im.renderer, 1.0f, 1.0f);
}

} // namespace toonval::app
