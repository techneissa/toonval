// SPDX-License-Identifier: GPL-3.0-or-later
// The touch UI's design tokens and the widgets the bar styles and the settings sheet are built
// from, drawn with Dear ImGui.
//
// Tokens. Every size is in design points; `Kit::u` turns them into ImGui points, so a denser
// display gets proportionally larger controls. The drawn controls are compact, their hit areas are
// not: every tappable thing is at least `kHit` tall (about 11.6 mm at 96 dpi) even where it draws
// smaller.
#pragma once

#include "../ui_model.hpp"

#include <imgui.h>

#include <cstdint>
#include <span>
#include <string>

struct SDL_Renderer;

namespace toonval::app::ui {

class LivePreview;

// --- Tokens ------------------------------------------------------------------------------------

namespace tok {
// Spacing scale.
inline constexpr float kS1 = 4.0f;
inline constexpr float kS2 = 8.0f;
inline constexpr float kS3 = 12.0f;
inline constexpr float kS4 = 16.0f;
inline constexpr float kS5 = 24.0f;
inline constexpr float kS6 = 32.0f;
// Radii.
inline constexpr float kRadiusSm = 8.0f;  // chips, swatches, segment thumbs
inline constexpr float kRadiusMd = 12.0f; // controls, tiles
inline constexpr float kRadiusLg = 18.0f; // cards and sheets
// Control sizes.
inline constexpr float kHit = 44.0f;     // smallest hit area (11.6 mm)
inline constexpr float kControl = 34.0f; // drawn height of a control inside its hit row
inline constexpr float kIcon = 20.0f;    // icon size in rows and bars
inline constexpr float kIconSm = 16.0f;
// Type.
inline constexpr float kTitle = 19.0f; // bold
inline constexpr float kBody = 16.0f;
inline constexpr float kLabel = 14.5f;
inline constexpr float kCaption = 12.0f;
// Motion, seconds. Ease-out, nothing loops; with reduced effects every move is instant.
inline constexpr float kFast = 0.12f;  // toggles, thumbs, pressed states
inline constexpr float kMove = 0.16f;  // page push and pop, popovers
inline constexpr float kSheet = 0.18f; // sheets and the bar reveal
inline constexpr float kLongPress = 0.45f;
} // namespace tok

// Colours: three surface tones, one accent, three text tones.
namespace col {
inline constexpr ImVec4 kScrim{0.016f, 0.024f, 0.04f, 0.46f};
inline constexpr ImVec4 kSurface{0.086f, 0.098f, 0.133f, 0.975f}; // sheets, cards, the bar
inline constexpr ImVec4 kSurfaceHi{0.125f, 0.141f, 0.188f, 1.0f}; // tiles, raised rows
inline constexpr ImVec4 kFill{1.0f, 1.0f, 1.0f, 0.055f};          // tracks, idle controls
inline constexpr ImVec4 kFillPressed{1.0f, 1.0f, 1.0f, 0.11f};    // a finger on it
inline constexpr ImVec4 kHairline{1.0f, 1.0f, 1.0f, 0.08f};
inline constexpr ImVec4 kAccent{0.353f, 0.667f, 1.0f, 1.0f};      // #5AAAFF, the app's blue
inline constexpr ImVec4 kAccentSoft{0.353f, 0.667f, 1.0f, 0.17f}; // selected fills
inline constexpr ImVec4 kAccentInk{0.031f, 0.055f, 0.094f, 1.0f}; // text on the accent
inline constexpr ImVec4 kText{0.933f, 0.949f, 0.98f, 1.0f};
inline constexpr ImVec4 kText2{0.604f, 0.643f, 0.722f, 1.0f};
inline constexpr ImVec4 kText3{0.42f, 0.455f, 0.533f, 1.0f};
inline constexpr ImVec4 kDanger{1.0f, 0.45f, 0.45f, 1.0f};
} // namespace col

ImVec4 with_alpha(ImVec4 c, float a);
ImVec4 mix(ImVec4 a, ImVec4 b, float t);

// --- Motion ------------------------------------------------------------------------------------

float ease_out(float t);
// Turns every motion below into a jump (reduced effects). Set once per frame.
void set_reduced_motion(bool reduced);
// Moves linear progress `p` toward `target` (0 or 1) over `duration` seconds.
float approach(float p, float target, float dt, float duration);
// A per-widget float that eases toward `target` (stored in ImGui's state storage under `id`).
float tween(ImGuiID id, float target, float duration);

// --- Kit ---------------------------------------------------------------------------------------

struct Kit {
    float u = 1.0f;            // ImGui points per design point
    ImFont* regular = nullptr; // Atkinson Hyperlegible with Lucide merged in
    ImFont* bold = nullptr;
    float dt = 0.016f;
    double now = 0.0;        // seconds
    std::int64_t now_ns = 0; // the same, for SongPort calls
    bool touch = false;      // the last pointer event came from a finger: no hover states
    std::string message;     // one line for the status line

    // A label for the icon-only control under a long press, drawn on top by `finish_frame`.
    std::string tip;
    ImVec2 tip_min{0, 0};
    ImVec2 tip_max{0, 0};
    // The open (i) bubble.
    ImGuiID info_id = 0;
    std::string info_text;
    ImVec2 info_anchor{0, 0};
    bool info_seen = false;
    // A destructive action waiting for its second tap.
    ImGuiID confirm_id = 0;
    double confirm_until = 0.0;

    bool reduced_motion = false; // LookSettings::reduced_effects: no UI animation either

    // The live look preview (pages draw it; the host owns it) and what it needs to render.
    LivePreview* preview = nullptr;
    SDL_Renderer* renderer = nullptr;
    float fb_scale = 1.0f;     // output pixels per ImGui point
    bool short_screen = false; // under 600 points tall: pages trim what they can

    // Drag to scroll.
    bool slider_active = false; // a slider is being dragged this frame: not a scroll
    bool scroll_press = false;
    bool scrolling = false;
    float press_y = 0.0f;

    float s(float v) const { return v * u; }
};

// Draws the long-press label and the (i) bubble on top of everything. Call once at the end.
void finish_frame(Kit& k);

// --- Drawing -----------------------------------------------------------------------------------

ImU32 u32(ImVec4 c); // multiplied by the current style alpha
void glyph(const Kit& k, ImDrawList* dl, const char* icon, ImVec2 centre, float size,
           ImVec4 colour);
void text_at(const Kit& k, ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImVec4 colour,
             const char* text);
ImVec2 text_size(ImFont* font, float size, const char* text);
// `text` cut to `max_w` with an ellipsis.
std::string fit(ImFont* font, float size, const std::string& text, float max_w);
// A soft shadow, then the surface, then a hairline and a faint top highlight.
void card(const Kit& k, ImDrawList* dl, ImVec2 a, ImVec2 b, float radius,
          ImVec4 fill = col::kSurface, ImDrawFlags corners = ImDrawFlags_RoundCornersAll,
          float shadow = 1.0f);
void shadow(const Kit& k, ImDrawList* dl, ImVec2 a, ImVec2 b, float radius, float strength);
// A small preview of one note style: a white-key block and a narrower, darker black-key block.
void note_preview(const Kit& k, ImDrawList* dl, ImVec2 a, ImVec2 b, NoteStyle style, RGBA colour,
                  float intensity);

// --- Controls ----------------------------------------------------------------------------------
// All of these lay out at ImGui's cursor and take the full content width unless a width is given.

// The always-visible settings button: icon only, no fill, no border. `active` while settings are
// open. Pressed: a little smaller and brighter. Returns true on a tap.
bool gear(Kit& k, ImVec2 centre, float hit, bool active, float alpha);

// An icon in a square hit area at `centre`. `on` draws the selected state (accent icon on a soft
// accent square); `caption` (may be null) is drawn under the icon; `label` shows on long press.
// `live` false: drawn but not pressable (a bar that is still fading in).
bool icon_toggle(Kit& k, const char* id, const char* icon, const char* label, ImVec2 centre,
                 ImVec2 hit, float drawn, bool on, bool live, const char* caption = nullptr);

// A text chip with an icon (the bar's look name). Returns true on a tap.
bool chip(Kit& k, const char* id, const char* icon, const std::string& text, ImVec2 pos, ImVec2 hit,
          float drawn_h, bool on, bool live);

void section(Kit& k, const char* text);
bool switch_row(Kit& k, const char* icon, const char* label, bool* value,
                const char* info = nullptr);
bool nav_row(Kit& k, const char* icon, const char* label, const std::string& value);

struct Seg {
    const char* icon = nullptr;  // may be null
    const char* label = nullptr; // may be null when there is an icon
};
// One row: an optional icon and label on the left, the segmented control on the right (or the
// full width when `label` is null). Returns true when the choice changed.
bool segmented(Kit& k, const char* id, const char* icon, const char* label,
               std::span<const Seg> items, int* current);
bool slider_row(Kit& k, const char* icon, const char* label, float* value, float lo, float hi,
                float shown_scale, const char* format, const char* info = nullptr);
bool slider_row_int(Kit& k, const char* icon, const char* label, int* value, int lo, int hi,
                    const char* unit, const char* info = nullptr);
// A number with a minus and a plus button on the right of the row, `step` per tap, kept in
// `lo`..`hi`; `unit` follows the number. Never stacks, so rows line up at any width.
bool stepper_row(Kit& k, const char* icon, const char* label, int* value, int lo, int hi,
                 const char* unit);
// An icon button with a long-press label, `size` square hit, drawn smaller. `danger` turns red.
bool icon_action(Kit& k, const char* id, const char* icon, const char* label, float size,
                 bool enabled, bool danger = false, bool armed = false);
// A full-width quiet button: icon + text. A second tap within 3 s is needed when `confirm`.
bool text_button(Kit& k, const char* id, const char* icon, const char* label, bool confirm,
                 const char* confirm_label = nullptr);
// A tile for a category home: icon in a tinted circle, name, the current value.
bool tile(Kit& k, const char* id, const char* icon, const char* label, const std::string& value,
          ImVec2 size);
// Sheet or card header: back chevron (when `back`), title, close (when `close`). Returns 1 for
// back, 2 for close, 0 otherwise.
int header(Kit& k, const char* title, bool back, bool close);
// Two-tap confirm state for `id`.
bool armed(const Kit& k, ImGuiID id);

// Drag anywhere to scroll the current child window (touch), except on a slider.
void drag_scroll(Kit& k);

} // namespace toonval::app::ui
