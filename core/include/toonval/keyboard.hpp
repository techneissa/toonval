// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace toonval {

// The playable range of a physical keyboard, inclusive on both ends.
struct KeyRange {
    int lowest = 21;   // A0 on an 88-key piano
    int highest = 108; // C8

    int key_count() const { return highest - lowest + 1; }
    bool contains(int note) const { return note >= lowest && note <= highest; }
    // Both ends in 0..127, ordered, and on white keys, like every real keyboard.
    bool valid() const;
};

inline constexpr KeyRange kRange88{21, 108};
inline constexpr KeyRange kRange76{28, 103};
inline constexpr KeyRange kRange61{36, 96};
inline constexpr KeyRange kRange49{36, 84};

// A USB vendor and product id. Vendor 0 is not assigned to anyone, so {0, 0} means "none".
struct UsbId {
    std::uint16_t vendor = 0;
    std::uint16_t product = 0;

    bool operator==(const UsbId&) const = default;
};

// One known keyboard model. Matched by match_keyboard() in keyboard_identity.hpp: USB id first,
// then a model-specific name. Every entry needs a source: the maker's manual or MIDI implementation chart.
struct KeyboardProfile {
    std::string_view name; // shown in the UI, e.g. "Yamaha PSR-E383"
    KeyRange range;
    UsbId usb{}; // {0, 0} when the model's USB id is not known
    // Substrings that name this model and no other, e.g. "PSR-E383". Matched case-insensitively
    // on word boundaries. Generic names ("Digital Piano") are never matched. Empty = unused.
    std::array<std::string_view, 2> name_patterns{};
};

// The built-in profile table.
std::span<const KeyboardProfile> builtin_keyboard_profiles();

// The built-in profile a single port or device name names, or nullptr. Same name rules as
// match_keyboard(); use that with the full identity when it is available.
const KeyboardProfile* find_profile(std::string_view device_name);

// Horizontal position of one key, in units where the whole keyboard is 1.0 wide.
struct KeyRect {
    float x = 0.0f;
    float width = 0.0f;
    bool black = false;
};

// Maps notes to horizontal positions for drawing the keyboard and the falling notes above it.
class KeyboardLayout {
  public:
    // Black keys are this fraction of a white key's width.
    static constexpr float kBlackWidthRatio = 0.6f;

    // A range with a black-key end widens outward to the nearest white key. A range outside
    // 0..127 or out of order falls back to 88 keys. `adjusted()` tells which happened.
    explicit KeyboardLayout(KeyRange range);

    const KeyRange& range() const { return range_; }
    // True when the layout's range differs from the requested one.
    bool adjusted() const { return adjusted_; }
    int white_key_count() const { return white_count_; }
    float white_width() const { return white_width_; }

    // Rect for a note in the range. Notes outside the range return a zero-width rect.
    KeyRect key(int note) const;

  private:
    int white_index(int note) const; // white keys from `range_.lowest` up to and including `note`

    KeyRange range_;
    int white_count_ = 0;
    float white_width_ = 0.0f;
    bool adjusted_ = false;
};

} // namespace toonval
