// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/keyboard.hpp"

#include "toonval/keyboard_identity.hpp"
#include "toonval/pitch.hpp"

#include <array>
#include <string>

namespace toonval {

bool KeyRange::valid() const {
    return is_valid_note(lowest) && is_valid_note(highest) && lowest <= highest &&
           !is_black_key(lowest) && !is_black_key(highest);
}

namespace {

// Only entries with a source go here (the maker's manual or MIDI implementation chart). A model
// whose key range or names are not sourced is left to the setup wizard.
constexpr std::array kProfiles = {
    // 61 keys, 36..96: PSR-E383 Reference Manual, function 003. Yamaha's vendor id is 0499.
    // The PSR-E383's USB product id isn't known yet ({0x0499, 0x????}); `midi-ports` prints it when
    // run with the real keyboard. Until then it is matched by name only, and only if the name
    // names the model.
    KeyboardProfile{"Yamaha PSR-E383", kRange61, {}, {"PSR-E383"}},
};

} // namespace

std::span<const KeyboardProfile> builtin_keyboard_profiles() { return kProfiles; }

const KeyboardProfile* find_profile(std::string_view device_name) {
    KeyboardIdentity id;
    id.port_name = std::string(device_name);
    return match_keyboard(id).profile;
}

KeyboardLayout::KeyboardLayout(KeyRange requested) : range_(requested) {
    if (!range_.valid()) {
        if (is_valid_note(range_.lowest) && is_valid_note(range_.highest) &&
            range_.lowest <= range_.highest) {
            // Only a black-key end is wrong: the nearest white key outward is always one step
            // away and stays inside 0..127, because notes 0 and 127 are white.
            if (is_black_key(range_.lowest)) {
                --range_.lowest;
            }
            if (is_black_key(range_.highest)) {
                ++range_.highest;
            }
        } else {
            range_ = kRange88;
        }
    }
    adjusted_ = range_.lowest != requested.lowest || range_.highest != requested.highest;
    white_count_ = white_index(range_.highest);
    white_width_ = 1.0f / static_cast<float>(white_count_);
}

int KeyboardLayout::white_index(int note) const {
    int count = 0;
    for (int n = range_.lowest; n <= note; ++n) {
        if (!is_black_key(n)) {
            ++count;
        }
    }
    return count;
}

KeyRect KeyboardLayout::key(int note) const {
    if (!range_.contains(note)) {
        return {};
    }
    const int whites_up_to = white_index(note);
    if (!is_black_key(note)) {
        return {static_cast<float>(whites_up_to - 1) * white_width_, white_width_, false};
    }
    // A black key sits centred on the boundary after the white key below it.
    const float black_width = white_width_ * kBlackWidthRatio;
    const float boundary = static_cast<float>(whites_up_to) * white_width_;
    return {boundary - black_width / 2.0f, black_width, true};
}

} // namespace toonval
