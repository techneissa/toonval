// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

#include "toonval/look.hpp"

namespace toonval {

// Axis-aligned rectangle in window pixels, y down. `w` and `h` are never negative.
struct RectF {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;

    float right() const { return x + w; }
    float bottom() const { return y + h; }
    bool empty() const { return !(w > 0.0f) || !(h > 0.0f); }
};

// Layout rules. All numbers are in pixels unless a name says mm.
inline constexpr float kMinTouchTargetMm = 15.0f;  // top row height
inline constexpr float kMinWindowWidthPx = 800.0f; // the layout rules are promised from here up
inline constexpr float kMinWindowHeightPx = 480.0f;
inline constexpr float kDefaultPxPerMm = 96.0f / 25.4f; // used when the caller passes a bad value
inline constexpr float kMinNoteAreaShare = 0.40f;       // of the window height, always
inline constexpr float kMaxTopRowShare = 0.30f;         // the top row never takes more than this
inline constexpr float kMaxChordLabelShare = 0.12f;
inline constexpr float kTopRowMarginShare = 0.2f; // of the row height: left margin, gaps
inline constexpr float kMinChordFontPx = 14.0f;   // smaller than this, the chord label hides
inline constexpr float kMinLabelFontPx = 9.0f;    // smaller than this, a note label hides
inline constexpr float kLineHeightEm = 1.3f;      // chord label strip height / font size
inline constexpr float kTopRowMinHeightEm = 1.5f; // the top row is at least this many fonts tall
inline constexpr float kLabelCharWidthEm = 0.62f; // widest average advance, for fitting text
inline constexpr float kLabelPaddingPx = 2.0f;    // each side of a label inside its cell
inline constexpr int kChordLabelMaxChars = 14;    // longest chord text the label must fit
inline constexpr float kBlockLabelLineHeightEm = 1.2f; // height of a block label's text box
inline constexpr float kMinKeyBedPx = 3.0f;            // the key bed is never thinner than this
// The smallest note name that stays legible, at 96 dpi; `min_name_font_px` scales it to the
// screen. A name that would need a smaller font is cut to its letter, or not drawn.
inline constexpr float kMinNameFontPx = 9.0f;

// Where everything goes in one window. Rects are whole pixels, stacked top to bottom in this
// order, full window width, touching but never overlapping, and together exactly as tall as the
// window: top row, chord label, note area, keyboard. The keyboard ends at the bottom edge of the
// window and nothing is below it. A rect that is not shown is empty (zero height) at its place in
// the stack, and the room goes to the note area.
//
// The top row is one touch target tall and always there: the status text (keyboard name,
// messages) on the left and the settings button (`gear`, a square) at the right end. The control
// bar has no row of its own: when revealed, its buttons take the right end of the status area
// (see split_top_row), so the status text shrinks and the stack below never moves.
//
// Priority when space runs short: the note area keeps at least `kMinNoteAreaShare` of the window
// height; after that the top row, the keyboard and the chord label, in that order, take what is
// left. Text shrinks with its rect and hides (flag false, font 0) rather than overflow.
struct ScreenLayout {
    RectF top_row;
    RectF chord_label;
    RectF note_area;
    RectF keyboard;

    // Inside `top_row`: the settings button's square at its right end, and the status text area
    // from a left margin to a gap before the gear. Never overlapping; empty when there is no room.
    RectF gear;
    RectF status_text;

    // The keyboard rect split in two: the key bed (the fallboard strip the notes land on) on top
    // and the key area below it. They touch, never overlap, and together are exactly `keyboard`.
    // Keys are drawn only inside `key_area`, so nothing crosses them; both are empty when the
    // keyboard is. The hit line, where notes meet the keyboard, is the top of `key_bed`.
    RectF key_bed;
    RectF key_area;
    float felt_line_h = 0.0f;  // thin felt line along the bottom edge of the bed, in pixels
    float key_shadow_h = 0.0f; // shadow the bed casts onto the top of the keys, in pixels

    bool chord_label_visible = false;
    bool status_visible = false; // the status font is readable in the top row

    // Font sizes in pixels, 0 when the text is hidden. The note label sizes are ceilings: a
    // block or key narrower than its label needs shrinks it further or hides it, see
    // `fit_label_font_px`.
    float chord_label_font_px = 0.0f;
    float status_font_px = 0.0f;
    float note_label_font_px = 0.0f; // labels on falling blocks
    float key_label_font_px = 0.0f;  // labels on keys
    // False when names are off, switched off for blocks/keys (`LookSettings::shows_block_names()`
    // and `shows_key_names()`), or the font would be unreadable.
    bool block_labels_visible = false;
    bool key_labels_visible = false;
    // C markers (`LookSettings::c_markers`) have their own switch and ceiling, the same size as the
    // key labels; false and 0 when off or the font would be unreadable.
    bool c_markers_visible = false;
    float c_marker_font_px = 0.0f;
};

// `px_per_mm` is the screen's pixel density (3.78 for a 96 dpi display) and sets the 15 mm
// touch target. A bad value (zero, negative, NaN) falls back to `kDefaultPxPerMm`. A bad window
// size gives all-empty rects. `look` is clamped first, so out-of-range values are safe.
ScreenLayout compute_layout(float width_px, float height_px, float px_per_mm,
                            const LookSettings& look);

// The top row while the control bar is revealed: the bar and what is left for the status text.
struct TopRowSplit {
    RectF status_text; // empty when the bar left no room
    RectF bar;         // empty when `bar_w` is not positive
};

// Places a bar `bar_w` pixels wide in `layout.status_text`: against its right end (next to the
// gear), or centred in the window when `centred`. Either way it moves right as far as needed to
// leave the status text `keep_status_w` pixels, and never past the status area's right end, so it
// never reaches the gear. A bar wider than the status area is cut to it. The status text keeps
// what is left of the bar, less a gap, so the two never overlap. Bad values count as 0.
TopRowSplit split_top_row(const ScreenLayout& layout, float bar_w, float keep_status_w,
                          bool centred);

// Font size for a label of `char_count` characters inside a block or key `cell_width_px` wide:
// `max_font_px` when it fits, smaller when it has to shrink, 0 (hide it) when the result would
// fall under `kMinLabelFontPx`. Always a whole number of pixels.
float fit_label_font_px(float max_font_px, float cell_width_px, int char_count);

// Where a note name goes on its block.
struct LabelPlacement {
    bool visible = false;
    RectF rect;           // the text box, empty when hidden
    float font_px = 0.0f; // 0 when hidden
};

// Places the name of a falling or rising block at its bottom edge, the end that meets the
// keyboard. `block` is the whole block in window pixels (it may run past the window); `clip` is
// where labels may appear, normally `ScreenLayout::note_area`. Only the part of the block inside
// `clip` counts: the label sits `kLabelPaddingPx` above the bottom of that part, centered. The font
// is `max_font_px` shrunk by `fit_label_font_px` to the visible width. The label is hidden, never
// clipped, when that font is unreadable or the visible part is shorter than the text box plus
// padding top and bottom (`kBlockLabelLineHeightEm` per font pixel). `char_count` is the length of
// the name; the text box is `char_count * kLabelCharWidthEm * font_px` wide.
LabelPlacement place_block_label(const RectF& block, const RectF& clip, float max_font_px,
                                 int char_count);

// What a block shows of its note name.
enum class NameForm : std::uint8_t {
    Hidden, // nothing fits
    Full,   // the whole name ("C#", "Do", "C#4")
    Letter, // the letter or syllable alone, no accidental or octave ("C")
};

struct NamePlacement {
    NameForm form = NameForm::Hidden;
    LabelPlacement label; // where and how big; empty when hidden
};

// `kMinNameFontPx` on a screen of `px_per_mm` pixels per millimetre (96 dpi is 3.78): the same
// physical size on a denser display. A bad value counts as 96 dpi.
float min_name_font_px(float px_per_mm);

// Decides what a block shows of its note name. The whole name (`full_chars` characters) when it
// fits the block at `min_font_px` or more, padding included, else the letter (`letter_chars`,
// normally 1) when that fits, else nothing. Sizes and rules as `place_block_label`, with
// `min_font_px` in place of `kMinLabelFontPx`; a name never shrinks below it, and the letter is
// tried only when it is shorter than the whole name.
NamePlacement place_block_name(const RectF& block, const RectF& clip, float max_font_px,
                               int full_chars, int letter_chars, float min_font_px);

} // namespace toonval
