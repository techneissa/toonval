// SPDX-License-Identifier: GPL-3.0-or-later
// The settings pages, built from the kit and editing the UiModel: the sheet (sheet.cpp) shows the
// top-level ones in its rail, Quick first. Each page lays itself out for the width it gets, in one
// column or two.
#pragma once

#include "kit.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace toonval::app::ui {

enum class Page : std::uint8_t {
    Quick,
    Look,
    Colours, // under Look
    Names,
    Effects,
    View,
    Keyboard, // under View
    Presets,
    Practice,
};

// The pages in the sheet's rail, in order.
inline constexpr std::array<Page, 7> kRail{Page::Quick, Page::Look,    Page::Names,   Page::Effects,
                                           Page::View,  Page::Presets, Page::Practice};

const char* page_title(Page p);
const char* page_icon(Page p);
// The page a sub-page's back button returns to (Colours to Look, Keyboard to View); a rail page
// returns itself.
Page parent_of(Page p);
inline bool is_subpage(Page p) { return parent_of(p) != p; }

// Draws the body of `p` at the cursor, as wide as the content region. Returns the page to open
// when a row drills further (Colours, Keyboard), else `p`.
Page draw_page(Kit& k, Page p, UiModel& model, int columns);

// Switches note names off, or back on to what they were.
void toggle_names(UiModel& model);
const char* style_label(NoteStyle s);
// Style tiles and the built-in look chips, shared with the strip bar's cards.
bool style_picker(Kit& k, LookSettings& look);
bool builtin_looks(Kit& k, UiModel& model, int columns_hint = 0);

// The name of the built-in or user look `model.look` is ("Custom" for none). The player's own
// preferences (octave numbering, key size, typing letters) do not count: built-in looks keep them.
std::string current_preset_name(const UiModel& model);
// Applies the next look in the cycle (built-ins, then the used user slots).
void cycle_preset(UiModel& model);
// Quick names for the naming dialog: a touch-only device has no keyboard to type on.
inline constexpr std::array<const char*, 6> kQuickNames{"My look", "Practice", "Bright",
                                                        "Night",   "Big keys", "Soft"};

} // namespace toonval::app::ui
