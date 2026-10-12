// SPDX-License-Identifier: GPL-3.0-or-later
#include "pages.hpp"

#include "icons.hpp"
#include "preview.hpp"

#include "toonval/color.hpp"
#include "toonval/keyboard.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace toonval::app::ui {

namespace {

NameMode g_last_names = NameMode::Letters; // what the names switch turns back on

// The colour being edited on the Colours page: 0 left, 1 right, 2 other, 3..14 C..B, 15 single.
int g_picker = -1;

// The last non-zero grade opacity seen at rest, which the switch and a slider dragged to 0 restore.
int g_grade_restore = kMaxGradeOpacity;

// The slot whose name is being edited, -1 for none.
int g_rename_slot = -1;
bool g_rename_open = false;
std::array<char, 64> g_name{};

ImVec4 to_vec(RGBA c) {
    return {static_cast<float>(c.r) / 255.0f, static_cast<float>(c.g) / 255.0f,
            static_cast<float>(c.b) / 255.0f, 1.0f};
}

// Lays rows out in one or two columns; `next()` moves to the second.
class Columns {
  public:
    Columns(Kit& k, int columns) : two_(columns > 1) {
        if (two_) {
            ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {k.s(tok::kS3), 0.0f});
            two_ = ImGui::BeginTable("##cols", 2, ImGuiTableFlags_SizingStretchSame);
            ImGui::PopStyleVar();
            if (two_) {
                ImGui::TableNextColumn();
            }
        }
    }
    ~Columns() {
        if (two_) {
            ImGui::EndTable();
        }
    }
    Columns(const Columns&) = delete;
    Columns& operator=(const Columns&) = delete;
    void next() {
        if (two_) {
            ImGui::TableNextColumn();
        }
    }
    bool two() const { return two_; }

  private:
    bool two_ = false;
};

const char* const kPitchNames[12] = {"C",  "C#", "D",  "D#", "E",  "F",
                                     "F#", "G",  "G#", "A",  "A#", "B"};

RGBA* colour_slot(LookSettings& look, int which) {
    switch (which) {
    case 0:
        return &look.left_color;
    case 1:
        return &look.right_color;
    case 2:
        return &look.unknown_color;
    case 15:
        return &look.single_color;
    default:
        break;
    }
    if (which >= 3 && which < 15) {
        return &look.pitch_colors[static_cast<std::size_t>(which - 3)];
    }
    return nullptr;
}

const std::vector<RGBA>& palette() {
    static const std::vector<RGBA> kPalette = [] {
        std::vector<RGBA> p;
        for (const double lightness : {0.86, 0.74, 0.62}) {
            for (int h = 0; h < 12; ++h) {
                p.push_back(from_oklch(lightness, 0.15, 30.0 + 30.0 * h));
            }
        }
        for (const double lightness : {1.0, 0.9, 0.78, 0.66, 0.54, 0.42}) {
            p.push_back(from_oklch(lightness, 0.0, 0.0));
        }
        return p;
    }();
    return kPalette;
}

// A round swatch in a square hit cell; returns true on a tap.
bool swatch(Kit& k, const char* id, RGBA c, const char* label, float cell, bool selected) {
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, {cell, cell});
    const bool held = ImGui::IsItemActive();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 centre{pos.x + cell * 0.5f, pos.y + cell * 0.5f};
    const float r = std::min(cell * 0.5f - k.s(5.0f), k.s(16.0f)) * (held ? 0.92f : 1.0f);
    if (selected) {
        dl->AddCircle(centre, r + k.s(3.5f), u32(col::kText), 40, k.s(2.0f));
    }
    dl->AddCircleFilled(centre, r, u32(to_vec(c)), 40);
    dl->AddCircle(centre, r, u32({1, 1, 1, 0.12f}), 40, 1.0f);
    if (label != nullptr && label[0] != '\0') {
        const RGBA ink = label_color_for(c);
        const float size = k.s(tok::kCaption);
        const ImVec2 ts = text_size(k.bold, size, label);
        text_at(k, dl, k.bold, size, {centre.x - ts.x * 0.5f, centre.y - ts.y * 0.5f}, to_vec(ink),
                label);
    }
    return pressed;
}

// Swatches in rows of as many cells as fit (at least the hit size each).
template <class Colour, class Label>
int swatch_rows(Kit& k, const char* id, int count, Colour colour, Label label, int selected,
                float min_cell) {
    const float avail = ImGui::GetContentRegionAvail().x;
    int per_row = std::clamp(static_cast<int>(avail / min_cell), 1, count);
    const int rows = (count + per_row - 1) / per_row;
    per_row = (count + rows - 1) / rows;
    const float cell = std::min(avail / static_cast<float>(per_row), min_cell * 1.25f);
    int tapped = -1;
    ImGui::PushID(id);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {0.0f, 0.0f});
    for (int i = 0; i < count; ++i) {
        if (i % per_row != 0) {
            ImGui::SameLine();
        }
        ImGui::PushID(i);
        if (swatch(k, "##sw", colour(i), label(i), cell, i == selected)) {
            tapped = i;
        }
        ImGui::PopID();
    }
    ImGui::PopStyleVar();
    ImGui::PopID();
    return tapped;
}

void page_colours(Kit& k, UiModel& model, int columns) {
    LookSettings& look = model.look;
    const float cell = k.s(tok::kHit);
    if (g_picker >= 0) {
        RGBA* target = colour_slot(look, g_picker);
        const char* names[] = {"Left hand", "Right hand", "Other notes"};
        std::string title = g_picker < 3    ? names[g_picker]
                            : g_picker < 15 ? std::string(kPitchNames[g_picker - 3])
                                            : std::string("All notes");
        section(k, title.c_str());
        int sel = -1;
        for (std::size_t i = 0; i < palette().size(); ++i) {
            if (palette()[i] == *target) {
                sel = static_cast<int>(i);
            }
        }
        const int pick = swatch_rows(
            k, "palette", static_cast<int>(palette().size()),
            [](int i) { return palette()[static_cast<std::size_t>(i)]; }, [](int) { return ""; },
            sel, k.s(40.0f));
        if (pick >= 0) {
            *target = palette()[static_cast<std::size_t>(pick)];
        }
        ImGui::Dummy({1.0f, k.s(tok::kS1)});
        const float w = ImGui::GetContentRegionAvail().x;
        const float half = (w - k.s(tok::kS2)) * 0.5f;
        ImGui::BeginGroup();
        ImGui::PushItemWidth(half);
        ImGui::BeginChild("##reset", {half, k.s(tok::kHit)}, ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
        if (text_button(k, "##reset-colour", icon::kReset, "Default", false)) {
            LookSettings defaults;
            *target = *colour_slot(defaults, g_picker);
        }
        ImGui::EndChild();
        ImGui::SameLine(0.0f, k.s(tok::kS2));
        ImGui::BeginChild("##done", {half, k.s(tok::kHit)}, ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
        if (text_button(k, "##done-colour", icon::kCheck, "Done", false)) {
            g_picker = -1;
        }
        ImGui::EndChild();
        ImGui::PopItemWidth();
        ImGui::EndGroup();
        return;
    }
    Columns cols(k, columns);
    switch_row(k, icon::kContrast, "High contrast", &look.high_contrast);
    section(k, "Hands");
    const char* hand_labels[] = {"L", "R", ""};
    const int hand = swatch_rows(
        k, "hands", 3, [&](int i) { return *colour_slot(look, i); },
        [&](int i) { return hand_labels[i]; }, -1, cell);
    if (hand >= 0) {
        g_picker = hand;
    }
    section(k, "One colour");
    if (swatch_rows(
            k, "single", 1, [&](int) { return look.single_color; }, [](int) { return ""; }, -1,
            cell) >= 0) {
        g_picker = 15;
    }
    cols.next();
    section(k, "Pitch classes");
    const int pc = swatch_rows(
        k, "pitch", 12, [&](int i) { return look.pitch_colors[static_cast<std::size_t>(i)]; },
        [&](int i) { return kPitchNames[i]; }, -1, cell);
    if (pc >= 0) {
        g_picker = 3 + pc;
    }
}

constexpr Seg kSchemes[] = {{nullptr, "Hand"},
                            {nullptr, "Pitch"},
                            {nullptr, "Octave"},
                            {nullptr, "Velocity"},
                            {nullptr, "One"}};

// Flow Mode's grade words: the switch, where they sit and how strong. The switch and the slider
// tell the same story: the slider reads 0 while the words are off, and the switch turns itself
// off when the slider reaches 0.
void grade_word_rows(Kit& k, LookSettings& look) {
    if (look.grade_opacity > kMinGradeOpacity && !k.slider_active) {
        g_grade_restore = look.grade_opacity;
    }
    bool on = look.grade_words_shown();
    if (switch_row(k, icon::kPractice, "Grade words", &on,
                   "Perfect, Good, Early, Late and Miss over the notes in Flow Mode.")) {
        set_grade_words(look, on, g_grade_restore);
    }
    constexpr Seg kWhere[] = {{nullptr, "Bottom"}, {nullptr, "Top"}};
    int where = static_cast<int>(look.grade_position);
    if (segmented(k, "gradepos", icon::kHeight, "Position", kWhere, &where)) {
        look.grade_position = static_cast<GradePosition>(where);
    }
    int percent = look.grade_words_shown() ? look.grade_opacity : kMinGradeOpacity;
    if (slider_row_int(k, icon::kDrop, "Opacity", &percent, kMinGradeOpacity, kMaxGradeOpacity,
                       "%")) {
        set_grade_opacity(look, percent, g_grade_restore);
    }
}

Page page_look(Kit& k, UiModel& model, int columns) {
    LookSettings& look = model.look;
    Page next = Page::Look;
    Columns cols(k, columns);
    // The live preview first: every control below changes it at once. No headings in one
    // column; the preview says what the page is about and the page fits without scrolling.
    if (k.preview != nullptr) {
        k.preview->draw(k, look, ImGui::GetContentRegionAvail().x);
        ImGui::Dummy({1.0f, k.s(2.0f)});
    }
    style_picker(k, look);
    slider_row(k, nullptr, "Intensity", &look.style_intensity, kMinStyleIntensity,
               kMaxStyleIntensity, 100.0f, "%.0f%%");
    cols.next();
    if (cols.two()) {
        section(k, "Colour");
    }
    int scheme = static_cast<int>(look.color_scheme);
    if (segmented(k, "scheme", nullptr, nullptr, kSchemes, &scheme)) {
        look.color_scheme = static_cast<ColorScheme>(scheme);
    }
    if (cols.two()) {
        switch_row(k, icon::kContrast, "High contrast", &look.high_contrast);
    }
    if (nav_row(k, icon::kPalette, "Colours",
                look.high_contrast && !cols.two() ? "High contrast" : "")) {
        next = Page::Colours;
        g_picker = -1;
    }
    // No heading of its own: the first row says what the three are, and a heading would push the
    // column past the sheet at 800x480.
    ImGui::Dummy({1.0f, k.s(tok::kS1)});
    grade_word_rows(k, look);
    return next;
}

void page_names(Kit& k, UiModel& model, int columns) {
    LookSettings& look = model.look;
    Columns cols(k, columns);
    constexpr Seg kModes[] = {{nullptr, "Off"}, {nullptr, "C D E"}, {nullptr, "Do Re Mi"}};
    int mode = static_cast<int>(look.name_mode);
    if (segmented(k, "mode", nullptr, nullptr, kModes, &mode)) {
        look.name_mode = static_cast<NameMode>(mode);
        if (look.name_mode != NameMode::Off) {
            g_last_names = look.name_mode;
            if (!look.names_on_blocks && !look.names_on_keys) {
                look.names_on_blocks = true;
            }
        }
    }
    constexpr Seg kWhere[] = {{nullptr, "Notes"}, {nullptr, "Keys"}, {nullptr, "Both"}};
    int where = look.names_on_blocks && look.names_on_keys ? 2
                : look.names_on_blocks                     ? 0
                : look.names_on_keys                       ? 1
                                                           : -1;
    if (segmented(k, "where", nullptr, "On", kWhere, &where)) {
        look.names_on_blocks = where != 1;
        look.names_on_keys = where != 0;
    }
    constexpr Seg kAcc[] = {{nullptr, "C#"}, {nullptr, "Db"}};
    int flats = look.names_prefer_flats ? 1 : 0;
    if (segmented(k, "acc", nullptr, "Black keys", kAcc, &flats)) {
        look.names_prefer_flats = flats == 1;
    }
    cols.next();
    switch_row(k, icon::kOctave, "Octave number", &look.names_show_octave);
    switch_row(k, icon::kCMarker, "C markers", &look.c_markers);
    constexpr Seg kNumbering[] = {{nullptr, "C4"}, {nullptr, "C3"}};
    int numbering = static_cast<int>(look.octave_numbering);
    // Vague without context: which octave numbering the player's manuals use.
    if (segmented(k, "num", nullptr, "Middle C", kNumbering, &numbering)) {
        look.octave_numbering = static_cast<OctaveNumbering>(numbering);
    }
}

void page_effects(Kit& k, UiModel& model, int columns) {
    LookSettings& look = model.look;
    Columns cols(k, columns);
    switch_row(k, icon::kFlash, "Key flash", &look.key_flash);
    switch_row(k, icon::kGlow, "Held glow", &look.held_glow);
    switch_row(k, icon::kSparks, "Sparks", &look.sparks);
    cols.next();
    switch_row(k, icon::kTrail, "Trail fade", &look.trail_fade,
               "Released notes fade out as they rise.");
    constexpr Seg kBg[] = {{nullptr, "Plain"}, {nullptr, "Gradient"}, {nullptr, "Motion"}};
    int bg = static_cast<int>(look.background);
    if (segmented(k, "bg", icon::kBackground, nullptr, kBg, &bg)) {
        look.background = static_cast<BackgroundStyle>(bg);
    }
    switch_row(k, icon::kReduced, "Reduced effects", &look.reduced_effects,
               "Turns off background motion and sparks.");
}

std::string keyboard_summary(const SessionState& s) {
    switch (s.keyboard) {
    case KeyboardChoice::Auto:
        return "Auto";
    case KeyboardChoice::Profile:
        return s.keyboard_name;
    case KeyboardChoice::Manual:
        return std::to_string(s.keyboard_highest - s.keyboard_lowest + 1) + " keys";
    }
    return {};
}

const char* key_size_label(KeySize s) {
    static constexpr const char* kNames[] = {"Fit keyboard", "Fit song", "Small",
                                             "Medium",       "Large",    "Extra large"};
    return kNames[static_cast<std::size_t>(s)];
}

void bar_style_row(Kit& k, UiModel& model) {
    constexpr Seg kBars[] = {{nullptr, "Strip"}, {nullptr, "Groups"}, {nullptr, "Dock"}};
    int bar = static_cast<int>(model.app.bar_style);
    if (segmented(k, "barstyle", icon::kBar, "Bar style", kBars, &bar)) {
        model.app.bar_style = static_cast<BarStyle>(bar);
    }
}

Page page_view(Kit& k, UiModel& model, int columns) {
    LookSettings& look = model.look;
    Page next = Page::View;
    Columns cols(k, columns);
    switch_row(k, icon::kChord, "Chord name", &look.chord_label);
    slider_row(k, nullptr, "Size", &look.chord_label_size, kMinChordLabelSize, kMaxChordLabelSize,
               100.0f, "%.0f%%");
    slider_row(k, icon::kSeconds, "Notes ahead", &look.visible_seconds, kMinVisibleSeconds,
               kMaxVisibleSeconds, 1.0f, "%.1f s");
    slider_row(k, icon::kHeight, "Keyboard", &look.keyboard_height, kMinKeyboardHeight,
               kMaxKeyboardHeight, 100.0f, "%.0f%%");
    cols.next();
    // Key size: the two fits as icons with a word, the fixed sizes as letters. A long press on a
    // letter names it.
    constexpr Seg kSizes[] = {{icon::kFitKeys, "Fit keyboard"},
                              {icon::kFitSong, "Fit song"},
                              {nullptr, "S"},
                              {nullptr, "M"},
                              {nullptr, "L"},
                              {nullptr, "XL"}};
    int size = static_cast<int>(look.key_size);
    if (segmented(k, "keysize", icon::kSize, "Key size", kSizes, &size)) {
        look.key_size = static_cast<KeySize>(size);
        k.message = std::string("Key size: ") + key_size_label(look.key_size);
    }
    bar_style_row(k, model);
    switch_row(k, icon::kTyping, "Typing letters", &look.show_typing_keys,
               "Shows which computer key plays each piano key.");
    if (nav_row(k, icon::kKeyboard, "Keyboard", keyboard_summary(model.session))) {
        next = Page::Keyboard;
    }
    return next;
}

// One choice in a list: the name, a check mark when chosen.
bool radio_row(Kit& k, const char* label, const char* detail, bool on) {
    const float w = ImGui::GetContentRegionAvail().x;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float h = k.s(tok::kHit);
    const bool pressed = ImGui::InvisibleButton(label, {w, h});
    const bool held = ImGui::IsItemActive();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (held) {
        dl->AddRectFilled(pos, {pos.x + w, pos.y + h}, u32(col::kFillPressed), k.s(tok::kRadiusMd));
    }
    const float size = k.s(tok::kBody);
    const ImVec2 ts = text_size(k.regular, size, label);
    const float x = pos.x + k.s(tok::kS3);
    text_at(k, dl, k.regular, size, {x, pos.y + (h - ts.y) * 0.5f}, on ? col::kAccent : col::kText,
            label);
    if (detail != nullptr) {
        const float ds = k.s(tok::kLabel);
        const ImVec2 dsz = text_size(k.regular, ds, detail);
        text_at(k, dl, k.regular, ds, {x + ts.x + k.s(tok::kS2), pos.y + (h - dsz.y) * 0.5f},
                col::kText3, detail);
    }
    if (on) {
        glyph(k, dl, icon::kCheck, {pos.x + w - k.s(tok::kS3) - k.s(9.0f), pos.y + h * 0.5f},
              k.s(tok::kIcon) * 0.9f, col::kAccent);
    }
    return pressed && !on;
}

void page_keyboard(Kit& k, UiModel& model, int columns) {
    SessionState& s = model.session;
    Columns cols(k, columns);
    if (radio_row(k, "Auto", "plugged-in keyboard", s.keyboard == KeyboardChoice::Auto)) {
        s.keyboard = KeyboardChoice::Auto;
    }
    for (const KeyboardProfile& p : builtin_keyboard_profiles()) {
        const std::string name(p.name);
        const std::string keys = std::to_string(p.range.key_count()) + " keys";
        if (radio_row(k, name.c_str(), keys.c_str(),
                      s.keyboard == KeyboardChoice::Profile && s.keyboard_name == name)) {
            s.keyboard = KeyboardChoice::Profile;
            s.keyboard_name = name;
            s.keyboard_lowest = p.range.lowest;
            s.keyboard_highest = p.range.highest;
        }
    }
    cols.next();
    if (radio_row(k, "Manual", nullptr, s.keyboard == KeyboardChoice::Manual)) {
        s.keyboard = KeyboardChoice::Manual;
    }
    if (s.keyboard == KeyboardChoice::Manual) {
        const KeyRange sizes[4] = {kRange88, kRange76, kRange61, kRange49};
        int size = -1;
        for (int i = 0; i < 4; ++i) {
            if (sizes[i].lowest == s.keyboard_lowest && sizes[i].highest == s.keyboard_highest) {
                size = i;
            }
        }
        constexpr Seg kKeys[] = {
            {nullptr, "88"}, {nullptr, "76"}, {nullptr, "61"}, {nullptr, "49"}};
        if (segmented(k, "keys", nullptr, "Keys", kKeys, &size)) {
            s.keyboard_lowest = sizes[size].lowest;
            s.keyboard_highest = sizes[size].highest;
        }
    }
}

void open_rename(UiModel& model, int slot) {
    g_rename_slot = slot;
    g_rename_open = false;
    const std::string_view name = model.user_presets.slot_name(slot);
    std::snprintf(g_name.data(), g_name.size(), "%.*s", static_cast<int>(name.size()), name.data());
}

void rename_dialog(Kit& k, UiModel& model) {
    if (g_rename_slot < 0) {
        return;
    }
    if (!g_rename_open) {
        ImGui::OpenPopup("##rename");
        g_rename_open = true;
    }
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float w = std::min(display.x - k.s(32.0f), k.s(360.0f));
    ImGui::SetNextWindowPos({display.x * 0.5f, display.y * 0.42f}, ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({w, 0.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {k.s(tok::kS4), k.s(tok::kS3)});
    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_AlwaysAutoResize;
    const bool open = ImGui::BeginPopupModal("##rename", nullptr, kFlags);
    ImGui::PopStyleVar();
    if (!open) {
        g_rename_slot = -1;
        g_rename_open = false;
        return;
    }
    {
        const ImVec2 p = ImGui::GetWindowPos();
        const ImVec2 s = ImGui::GetWindowSize();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->ChannelsSplit(2);
        dl->ChannelsSetCurrent(0);
        card(k, dl, p, {p.x + s.x, p.y + s.y}, k.s(tok::kRadiusLg), col::kSurfaceHi);
        dl->ChannelsSetCurrent(1);
    }
    bool close = false;
    if (header(k, "Name", false, true) == 2) {
        close = true;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {k.s(tok::kS3), k.s(11.0f)});
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, k.s(tok::kRadiusMd));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, col::kFill);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::IsWindowAppearing()) {
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::PushFont(k.regular, k.s(tok::kBody));
    ImGui::InputText("##name", g_name.data(), g_name.size());
    ImGui::PopFont();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    ImGui::Dummy({1.0f, k.s(tok::kS1)});
    // Quick names: a touch screen has no keyboard to type on.
    const float avail = ImGui::GetContentRegionAvail().x;
    const int per_row = 3;
    const float cw = (avail - k.s(tok::kS2) * (per_row - 1)) / per_row;
    for (std::size_t i = 0; i < kQuickNames.size(); ++i) {
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::PushID(static_cast<int>(i));
        if (chip(k, "##q", nullptr, kQuickNames[i], pos, {cw, k.s(tok::kHit)}, k.s(tok::kControl),
                 false, true)) {
            std::snprintf(g_name.data(), g_name.size(), "%s", kQuickNames[i]);
        }
        ImGui::PopID();
        ImGui::SetCursorScreenPos(pos);
        ImGui::Dummy({cw, k.s(tok::kHit)});
        if ((i + 1) % per_row != 0) {
            ImGui::SameLine(0.0f, k.s(tok::kS2));
        }
    }
    ImGui::Dummy({1.0f, k.s(tok::kS2)});
    // The one prominent action.
    {
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float w2 = ImGui::GetContentRegionAvail().x;
        const float h = k.s(tok::kHit);
        const bool pressed = ImGui::InvisibleButton("##save", {w2, h});
        const bool held = ImGui::IsItemActive();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled({pos.x, pos.y + k.s(2.0f)}, {pos.x + w2, pos.y + h - k.s(2.0f)},
                          u32(held ? mix(col::kAccent, {1, 1, 1, 1}, 0.2f) : col::kAccent),
                          k.s(tok::kRadiusMd));
        const ImVec2 ts = text_size(k.bold, k.s(tok::kBody), "Save");
        text_at(k, dl, k.bold, k.s(tok::kBody),
                {pos.x + (w2 - ts.x) * 0.5f, pos.y + (h - ts.y) * 0.5f}, col::kAccentInk, "Save");
        if (pressed) {
            LookSettings look;
            model.user_presets.load_slot(g_rename_slot, &look);
            if (model.user_presets.save_slot(g_rename_slot, g_name.data(), look)) {
                k.message =
                    "Renamed to " + std::string(model.user_presets.slot_name(g_rename_slot));
            }
            close = true;
        }
    }
    ImGui::GetWindowDrawList()->ChannelsMerge();
    if (close) {
        ImGui::CloseCurrentPopup();
        g_rename_slot = -1;
        g_rename_open = false;
    }
    ImGui::EndPopup();
}

// The built-in looks as one row of previews, the name of the one in use beside the heading; a
// long press names any of them. For a single narrow column, where the named grid would scroll.
void builtin_strip(Kit& k, UiModel& model) {
    std::string in_use;
    for (const PresetId id : kAllPresets) {
        if (is_preset(model.look, id)) {
            in_use = preset_name(id);
        }
    }
    {
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        section(k, "Built in");
        if (!in_use.empty()) {
            const float size = k.s(tok::kCaption);
            const ImVec2 ts = text_size(k.regular, size, in_use.c_str());
            text_at(k, ImGui::GetWindowDrawList(), k.regular, size,
                    {pos.x + w - ts.x - k.s(tok::kS1), pos.y + k.s(26.0f) - ts.y - k.s(3.0f)},
                    col::kText2, in_use.c_str());
        }
    }
    const int n = static_cast<int>(kAllPresets.size());
    const float avail = ImGui::GetContentRegionAvail().x;
    const float gap = k.s(tok::kS1);
    const float cw = (avail - gap * static_cast<float>(n - 1)) / static_cast<float>(n);
    const float h = k.s(tok::kHit) + k.s(4.0f);
    ImGui::PushID("strip");
    for (int i = 0; i < n; ++i) {
        if (i > 0) {
            ImGui::SameLine(0.0f, gap);
        }
        const PresetId id = kAllPresets[static_cast<std::size_t>(i)];
        const LookSettings p = preset(id);
        const bool on = std::string(preset_name(id)) == in_use;
        ImGui::PushID(i);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const bool pressed = ImGui::InvisibleButton("##p", {cw, h});
        const bool held = ImGui::IsItemActive();
        if (held && ImGui::GetIO().MouseDownDuration[0] > tok::kLongPress) {
            k.tip = preset_name(id);
            k.tip_min = ImGui::GetItemRectMin();
            k.tip_max = ImGui::GetItemRectMax();
        }
        ImGui::PopID();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 b{pos.x + cw, pos.y + h};
        dl->AddRectFilled(pos, b,
                          u32(on ? col::kAccentSoft : (held ? col::kFillPressed : col::kFill)),
                          k.s(tok::kRadiusMd));
        if (on) {
            dl->AddRect(pos, b, u32(with_alpha(col::kAccent, 0.7f)), k.s(tok::kRadiusMd), 0,
                        k.s(1.5f));
        }
        const float ph = h - k.s(16.0f);
        const float pw = std::min(cw - k.s(10.0f), ph * 0.95f);
        note_preview(k, dl, {pos.x + (cw - pw) * 0.5f, pos.y + (h - ph) * 0.5f},
                     {pos.x + (cw + pw) * 0.5f, pos.y + (h + ph) * 0.5f}, p.note_style,
                     p.color_scheme == ColorScheme::Single ? p.single_color : p.right_color,
                     p.style_intensity);
        if (pressed && !on && ImGui::GetIO().MouseDownDurationPrev[0] < tok::kLongPress) {
            model.look = apply_preset(model.look, id);
            k.message = "Look: " + std::string(preset_name(id));
        }
    }
    ImGui::PopID();
}

void page_presets(Kit& k, UiModel& model, int columns) {
    Columns cols(k, columns);
    if (cols.two()) {
        section(k, "Built in");
        builtin_looks(k, model);
    } else {
        builtin_strip(k, model);
    }
    cols.next();
    section(k, "My looks");
    const std::string current = current_preset_name(model);
    for (int slot = 0; slot < kUserPresetSlots; ++slot) {
        ImGui::PushID(slot);
        const bool used = model.user_presets.is_used(slot);
        const float w = ImGui::GetContentRegionAvail().x;
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float h = k.s(tok::kHit);
        const float act = k.s(tok::kHit) - k.s(4.0f); // each action's hit square
        const int actions = used ? 3 : 1;
        const float name_w = w - act * static_cast<float>(actions);
        LookSettings saved;
        model.user_presets.load_slot(slot, &saved);
        const bool on = used && saved == model.look;
        // Tapping the slot's name loads it.
        ImGui::BeginDisabled(!used);
        const bool load = ImGui::InvisibleButton("##load", {name_w, h});
        ImGui::EndDisabled();
        const bool held = ImGui::IsItemActive();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(pos, {pos.x + w, pos.y + h},
                          u32(on ? col::kAccentSoft : (held ? col::kFillPressed : col::kFill)),
                          k.s(tok::kRadiusMd));
        const ImVec2 badge{pos.x + k.s(tok::kS3) + k.s(11.0f), pos.y + h * 0.5f};
        if (used) {
            note_preview(k, dl, {badge.x - k.s(9.0f), badge.y - k.s(11.0f)},
                         {badge.x + k.s(11.0f), badge.y + k.s(11.0f)}, saved.note_style,
                         saved.right_color, saved.style_intensity);
        } else {
            dl->AddCircle(badge, k.s(10.0f), u32(col::kText3), 32, 1.2f);
            const std::string n = std::to_string(slot + 1);
            const ImVec2 ns = text_size(k.regular, k.s(tok::kCaption), n.c_str());
            text_at(k, dl, k.regular, k.s(tok::kCaption),
                    {badge.x - ns.x * 0.5f, badge.y - ns.y * 0.5f}, col::kText3, n.c_str());
        }
        const float tx = pos.x + k.s(tok::kS3) + k.s(22.0f) + k.s(tok::kS3);
        const std::string name =
            used ? std::string(model.user_presets.slot_name(slot)) : std::string("Empty");
        const std::string shown = fit(k.regular, k.s(tok::kBody), name, pos.x + name_w - tx);
        const ImVec2 ts = text_size(k.regular, k.s(tok::kBody), shown.c_str());
        text_at(k, dl, k.regular, k.s(tok::kBody), {tx, pos.y + (h - ts.y) * 0.5f},
                used ? (on ? col::kAccent : col::kText) : col::kText3, shown.c_str());
        // Actions: save the current look here, rename, clear (second tap confirms).
        float ax = pos.x + name_w;
        ImGui::SetCursorScreenPos({ax, pos.y + (h - act) * 0.5f});
        if (icon_action(k, "##save", icon::kSave, used ? "Save over" : "Save here", act, true)) {
            const std::string keep = used ? std::string(model.user_presets.slot_name(slot))
                                          : "My look " + std::to_string(slot + 1);
            model.user_presets.save_slot(slot, keep, model.look);
            k.message = "Saved as " + std::string(model.user_presets.slot_name(slot));
        }
        if (used) {
            ax += act;
            ImGui::SetCursorScreenPos({ax, pos.y + (h - act) * 0.5f});
            if (icon_action(k, "##rename", icon::kRename, "Rename", act, true)) {
                open_rename(model, slot);
            }
            ax += act;
            ImGui::SetCursorScreenPos({ax, pos.y + (h - act) * 0.5f});
            const ImGuiID clear_id = ImGui::GetID("##clear");
            const bool is_armed = armed(k, clear_id);
            if (icon_action(k, "##clear", icon::kClear, is_armed ? "Tap again to clear" : "Clear",
                            act, true, true, is_armed)) {
                if (is_armed) {
                    model.user_presets.clear_slot(slot);
                    k.message = "Slot " + std::to_string(slot + 1) + " cleared";
                    k.confirm_id = 0;
                } else {
                    k.confirm_id = clear_id;
                    k.confirm_until = k.now + 3.0;
                    k.message = "Tap again to clear";
                }
            }
        }
        if (load && used) {
            model.look = saved;
            k.message = "Look: " + name;
        }
        ImGui::SetCursorScreenPos(pos);
        ImGui::Dummy({w, h});
        ImGui::PopID();
    }
    (void)current;
    ImGui::Dummy({1.0f, k.s(tok::kS1)});
    if (text_button(k, "##reset", icon::kReset, "Reset all", true, "Tap again to reset")) {
        model.look = apply_preset(model.look, PresetId::Default);
        k.message = "Settings reset to defaults";
    }
    rename_dialog(k, model);
}

void page_practice(Kit& k, UiModel& model, int columns) {
    Columns cols(k, columns);
    slider_row_int(k, icon::kTimer, "Hit window", &model.app.hit_window_ms, 20, 250, "ms",
                   "How early or late a note may be and still count, in Flow mode.");
    slider_row_int(k, icon::kGauge, "Latency", &model.app.latency_correction_ms, -250, 250, "ms",
                   "Shifts when key presses count, to make up for the delay of the keyboard and "
                   "screen.");
    cols.next();
    // Closing the settings while a song plays resumes it after this many seconds.
    constexpr Seg kSeconds[] = {{nullptr, "1"}, {nullptr, "2"}, {nullptr, "3"},
                                {nullptr, "4"}, {nullptr, "5"}, {nullptr, "6"}};
    int resume = model.app.resume_countdown_s - 1;
    if (segmented(k, "resume", icon::kCountdown, "Resume after", kSeconds, &resume)) {
        model.app.resume_countdown_s = resume + 1;
    }
    // How far one tap of each skip button moves; holding it repeats the step.
    stepper_row(k, icon::kSkipBack, "Skip back", &model.app.skip_back_seconds, 1, 30, "s");
    stepper_row(k, icon::kSkipForward, "Skip forward", &model.app.skip_forward_seconds, 1, 30, "s");
}

void page_quick(Kit& k, UiModel& model, int columns) {
    LookSettings& look = model.look;
    Columns cols(k, columns);
    if (k.preview != nullptr) {
        k.preview->draw(k, look, ImGui::GetContentRegionAvail().x);
        ImGui::Dummy({1.0f, k.s(2.0f)});
    }
    style_picker(k, look);
    constexpr Seg kModes[] = {{nullptr, "Off"}, {nullptr, "C D E"}, {nullptr, "Do Re Mi"}};
    int mode = static_cast<int>(look.name_mode);
    if (segmented(k, "mode", icon::kNames, nullptr, kModes, &mode)) {
        look.name_mode = static_cast<NameMode>(mode);
        if (look.name_mode != NameMode::Off && !look.names_on_blocks && !look.names_on_keys) {
            look.names_on_blocks = true;
        }
    }
    switch_row(k, icon::kCMarker, "C markers", &look.c_markers);
    cols.next();
    section(k, "Look");
    builtin_looks(k, model, 2);
    switch_row(k, icon::kContrast, "High contrast", &look.high_contrast);
    // In two columns there is room for it without scrolling; in one it stays on the View page.
    if (cols.two()) {
        bar_style_row(k, model);
    }
}

} // namespace

std::string current_preset_name(const UiModel& model) {
    for (const PresetId id : kAllPresets) {
        if (is_preset(model.look, id)) {
            return std::string(preset_name(id));
        }
    }
    for (int slot = 0; slot < kUserPresetSlots; ++slot) {
        LookSettings saved;
        if (model.user_presets.load_slot(slot, &saved) && saved == model.look) {
            return std::string(model.user_presets.slot_name(slot));
        }
    }
    return "Custom";
}

void cycle_preset(UiModel& model) {
    struct Entry {
        bool builtin;
        int index;
    };
    std::vector<Entry> list;
    for (int i = 0; i < static_cast<int>(kAllPresets.size()); ++i) {
        list.push_back({true, i});
    }
    for (int slot = 0; slot < kUserPresetSlots; ++slot) {
        if (model.user_presets.is_used(slot)) {
            list.push_back({false, slot});
        }
    }
    // Where the look is now, found the way current_preset_name finds it.
    int at = -1;
    for (std::size_t i = 0; i < list.size() && at < 0; ++i) {
        const Entry& e = list[i];
        LookSettings saved;
        if (e.builtin ? is_preset(model.look, kAllPresets[static_cast<std::size_t>(e.index)])
                      : model.user_presets.load_slot(e.index, &saved) && saved == model.look) {
            at = static_cast<int>(i);
        }
    }
    const Entry& next = list[static_cast<std::size_t>((at + 1) % static_cast<int>(list.size()))];
    if (next.builtin) {
        model.look = apply_preset(model.look, kAllPresets[static_cast<std::size_t>(next.index)]);
    } else {
        model.user_presets.load_slot(next.index, &model.look);
    }
}

const char* style_label(NoteStyle s) {
    static constexpr const char* kNames[] = {"Filled", "Glassy", "Frosted",
                                             "Shiny",  "Ghost",  "Neon"};
    return kNames[static_cast<std::size_t>(s) % 6];
}

void toggle_names(UiModel& model) {
    LookSettings& look = model.look;
    if (look.name_mode == NameMode::Off) {
        look.name_mode = g_last_names == NameMode::Off ? NameMode::Letters : g_last_names;
        if (!look.names_on_blocks && !look.names_on_keys) {
            look.names_on_blocks = true;
        }
    } else {
        g_last_names = look.name_mode;
        look.name_mode = NameMode::Off;
    }
}

bool style_picker(Kit& k, LookSettings& look) {
    const float avail = ImGui::GetContentRegionAvail().x;
    const float gap = k.s(tok::kS1) + k.s(2.0f);
    const int per_row = avail >= 6 * k.s(44.0f) + 5 * gap ? 6 : 3;
    const float tw = (avail - gap * static_cast<float>(per_row - 1)) / static_cast<float>(per_row);
    const float th = std::clamp(tw * 1.05f, k.s(54.0f), k.s(66.0f));
    bool changed = false;
    ImGui::PushID("styles");
    for (int i = 0; i < 6; ++i) {
        if (i % per_row != 0) {
            ImGui::SameLine(0.0f, gap);
        }
        const auto style = static_cast<NoteStyle>(i);
        const bool on = look.note_style == style;
        ImGui::PushID(i);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const bool pressed = ImGui::InvisibleButton("##style", {tw, th});
        const bool held = ImGui::IsItemActive();
        ImGui::PopID();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 b{pos.x + tw, pos.y + th};
        dl->AddRectFilled(pos, b,
                          u32(on ? col::kAccentSoft : (held ? col::kFillPressed : col::kFill)),
                          k.s(tok::kRadiusMd));
        if (on) {
            dl->AddRect(pos, b, u32(with_alpha(col::kAccent, 0.7f)), k.s(tok::kRadiusMd), 0,
                        k.s(1.5f));
        }
        const float cap = k.s(tok::kCaption);
        const float ph = th - cap - k.s(tok::kS3) - k.s(tok::kS2);
        const float pw = std::min(tw * 0.62f, ph * 1.05f);
        note_preview(k, dl, {pos.x + (tw - pw) * 0.5f, pos.y + k.s(tok::kS2)},
                     {pos.x + (tw + pw) * 0.5f, pos.y + k.s(tok::kS2) + ph}, style,
                     look.color_scheme == ColorScheme::Single ? look.single_color
                                                              : look.right_color,
                     look.style_intensity);
        const char* name = style_label(style);
        const ImVec2 ts = text_size(on ? k.bold : k.regular, cap, name);
        text_at(k, dl, on ? k.bold : k.regular, cap,
                {pos.x + (tw - ts.x) * 0.5f, b.y - cap - k.s(6.0f)}, on ? col::kText : col::kText2,
                name);
        if (pressed && !on) {
            look.note_style = style;
            changed = true;
        }
    }
    ImGui::PopID();
    return changed;
}

bool builtin_looks(Kit& k, UiModel& model, int columns_hint) {
    const float avail = ImGui::GetContentRegionAvail().x;
    const float gap = k.s(tok::kS1) + k.s(2.0f);
    int per_row = columns_hint > 0
                      ? columns_hint
                      : std::clamp(static_cast<int>((avail + gap) / (k.s(128.0f) + gap)), 2, 4);
    const int n = static_cast<int>(kAllPresets.size());
    const float cw = (avail - gap * static_cast<float>(per_row - 1)) / static_cast<float>(per_row);
    const float h = k.s(tok::kHit) - k.s(4.0f);
    bool changed = false;
    ImGui::PushID("builtin");
    for (int i = 0; i < n; ++i) {
        if (i % per_row != 0) {
            ImGui::SameLine(0.0f, gap);
        }
        const PresetId id = kAllPresets[static_cast<std::size_t>(i)];
        const LookSettings p = preset(id);
        const bool on = is_preset(model.look, id);
        ImGui::PushID(i);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const bool pressed = ImGui::InvisibleButton("##preset", {cw, h});
        const bool held = ImGui::IsItemActive();
        ImGui::PopID();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 b{pos.x + cw, pos.y + h};
        dl->AddRectFilled(pos, b,
                          u32(on ? col::kAccentSoft : (held ? col::kFillPressed : col::kFill)),
                          k.s(tok::kRadiusMd));
        if (on) {
            dl->AddRect(pos, b, u32(with_alpha(col::kAccent, 0.7f)), k.s(tok::kRadiusMd), 0,
                        k.s(1.5f));
        }
        const float pv = h - k.s(14.0f);
        note_preview(k, dl, {pos.x + k.s(tok::kS2), pos.y + (h - pv) * 0.5f},
                     {pos.x + k.s(tok::kS2) + pv * 0.9f, pos.y + (h + pv) * 0.5f}, p.note_style,
                     p.color_scheme == ColorScheme::Single ? p.single_color : p.right_color,
                     p.style_intensity);
        const float size = k.s(tok::kLabel);
        const float tx = pos.x + k.s(tok::kS2) + pv * 0.9f + k.s(tok::kS2);
        const std::string shown =
            fit(k.regular, size, std::string(preset_name(id)), b.x - tx - k.s(6.0f));
        const ImVec2 ts = text_size(k.regular, size, shown.c_str());
        text_at(k, dl, k.regular, size, {tx, pos.y + (h - ts.y) * 0.5f},
                on ? col::kText : col::kText2, shown.c_str());
        if (pressed && !on) {
            model.look = apply_preset(model.look, id);
            k.message = "Look: " + std::string(preset_name(id));
            changed = true;
        }
    }
    ImGui::PopID();
    return changed;
}

const char* page_title(Page p) {
    switch (p) {
    case Page::Quick:
        return "Quick";
    case Page::Look:
        return "Look";
    case Page::Colours:
        return "Colours";
    case Page::Names:
        return "Note names";
    case Page::Effects:
        return "Effects";
    case Page::View:
        return "View";
    case Page::Keyboard:
        return "Keyboard";
    case Page::Presets:
        return "Presets";
    case Page::Practice:
        return "Practice";
    }
    return "";
}

const char* page_icon(Page p) {
    switch (p) {
    case Page::Quick:
        return icon::kHome;
    case Page::Look:
        return icon::kStyle;
    case Page::Colours:
        return icon::kPalette;
    case Page::Names:
        return icon::kNames;
    case Page::Effects:
        return icon::kEffects;
    case Page::View:
        return icon::kView;
    case Page::Keyboard:
        return icon::kKeyboard;
    case Page::Presets:
        return icon::kPresets;
    case Page::Practice:
        return icon::kPractice;
    }
    return "";
}

Page parent_of(Page p) {
    switch (p) {
    case Page::Colours:
        return Page::Look;
    case Page::Keyboard:
        return Page::View;
    default:
        return p;
    }
}

Page draw_page(Kit& k, Page p, UiModel& model, int columns) {
    switch (p) {
    case Page::Look:
        return page_look(k, model, columns);
    case Page::Colours:
        page_colours(k, model, columns);
        break;
    case Page::Names:
        page_names(k, model, columns);
        break;
    case Page::Effects:
        page_effects(k, model, columns);
        break;
    case Page::View:
        return page_view(k, model, columns);
    case Page::Keyboard:
        page_keyboard(k, model, columns);
        break;
    case Page::Presets:
        page_presets(k, model, columns);
        break;
    case Page::Practice:
        page_practice(k, model, columns);
        break;
    case Page::Quick:
        page_quick(k, model, columns);
        break;
    }
    return p;
}

} // namespace toonval::app::ui
