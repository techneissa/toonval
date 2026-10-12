// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <charconv>
#include <cmath>
#include <system_error>

#include "toonval/look.hpp"

namespace toonval {
namespace {

constexpr std::array<std::string_view, 6> kNoteStyleNames{"filled", "glassy", "frosted",
                                                          "shiny",  "ghost",  "neon"};
constexpr std::array<std::string_view, 5> kSchemeNames{"by_hand", "by_pitch_class", "by_octave",
                                                       "by_velocity", "single"};
constexpr std::array<std::string_view, 3> kNameModeNames{"off", "letters", "solfege"};
constexpr std::array<std::string_view, 3> kBackgroundNames{"plain", "gradient", "motion"};
constexpr std::array<std::string_view, 2> kOctaveNumberingNames{"scientific", "yamaha"};
constexpr std::array<std::string_view, 2> kGradePositionNames{"bottom", "top"};
constexpr std::array<std::string_view, 6> kKeySizeNames{"fit_keyboard", "fit_song", "small",
                                                        "medium",       "large",    "extra_large"};

std::string float_text(float v) {
    char buf[48];
    const auto r = std::to_chars(buf, buf + sizeof buf, v);
    return r.ec == std::errc() ? std::string(buf, r.ptr) : std::string("0");
}

// Every setting once, in file order. `V` is a writer or a reader; both see the same names, so
// the two directions cannot drift apart.
template <class S, class V> void visit_fields(S& s, V& v) {
    v.enumeration("note_style", s.note_style, kNoteStyleNames);
    v.real("style_intensity", s.style_intensity, kMinStyleIntensity, kMaxStyleIntensity);
    v.enumeration("color_scheme", s.color_scheme, kSchemeNames);
    v.color("left_color", s.left_color);
    v.color("right_color", s.right_color);
    v.color("unknown_color", s.unknown_color);
    for (std::size_t i = 0; i < s.pitch_colors.size(); ++i) {
        v.color("pitch_color_" + std::to_string(i), s.pitch_colors[i]);
    }
    v.color("single_color", s.single_color);
    v.flag("high_contrast", s.high_contrast);
    v.enumeration("name_mode", s.name_mode, kNameModeNames);
    v.flag("names_show_octave", s.names_show_octave);
    v.flag("names_prefer_flats", s.names_prefer_flats);
    v.flag("names_on_blocks", s.names_on_blocks);
    v.flag("names_on_keys", s.names_on_keys);
    v.flag("c_markers", s.c_markers);
    v.enumeration("octave_numbering", s.octave_numbering, kOctaveNumberingNames);
    v.flag("key_flash", s.key_flash);
    v.flag("held_glow", s.held_glow);
    v.flag("sparks", s.sparks);
    v.flag("trail_fade", s.trail_fade);
    v.enumeration("background", s.background, kBackgroundNames);
    v.flag("reduced_effects", s.reduced_effects);
    v.enumeration("key_size", s.key_size, kKeySizeNames);
    v.flag("show_typing_keys", s.show_typing_keys);
    v.flag("chord_label", s.chord_label);
    v.real("chord_label_size", s.chord_label_size, kMinChordLabelSize, kMaxChordLabelSize);
    v.real("visible_seconds", s.visible_seconds, kMinVisibleSeconds, kMaxVisibleSeconds);
    v.real("keyboard_height", s.keyboard_height, kMinKeyboardHeight, kMaxKeyboardHeight);
    v.flag("grade_words", s.grade_words);
    v.enumeration("grade_position", s.grade_position, kGradePositionNames);
    v.integer("grade_opacity", s.grade_opacity, kMinGradeOpacity, kMaxGradeOpacity);
}

struct Writer {
    std::string out;
    std::string prefix; // "slot_2_" while writing a user slot

    void line(std::string_view key, std::string_view value) {
        out += prefix;
        out += key;
        out += " = ";
        out += value;
        out += '\n';
    }
    template <class E, std::size_t N>
    void enumeration(std::string_view key, E value, const std::array<std::string_view, N>& names) {
        line(key, names[static_cast<std::size_t>(value)]);
    }
    void real(std::string_view key, float value, float, float) { line(key, float_text(value)); }
    void integer(std::string_view key, int value, int, int) { line(key, std::to_string(value)); }
    void color(std::string_view key, RGBA value) { line(key, to_hex(value)); }
    void flag(std::string_view key, bool value) { line(key, value ? "true" : "false"); }
};

enum class Outcome : std::uint8_t { NoMatch, Applied, BadValue, Clamped };

struct Reader {
    std::string_view key;
    std::string_view value;
    Outcome outcome = Outcome::NoMatch;

    template <class E, std::size_t N>
    void enumeration(std::string_view name, E& target,
                     const std::array<std::string_view, N>& names) {
        if (name != key) {
            return;
        }
        for (std::size_t i = 0; i < N; ++i) {
            if (names[i] == value) {
                target = static_cast<E>(i);
                outcome = Outcome::Applied;
                return;
            }
        }
        outcome = Outcome::BadValue;
    }
    void real(std::string_view name, float& target, float lo, float hi) {
        if (name != key) {
            return;
        }
        float parsed = 0.0f;
        const char* end = value.data() + value.size();
        const auto r = std::from_chars(value.data(), end, parsed);
        if (value.empty() || r.ec != std::errc() || r.ptr != end || !std::isfinite(parsed)) {
            outcome = Outcome::BadValue;
            return;
        }
        target = std::clamp(parsed, lo, hi);
        outcome = parsed < lo || parsed > hi ? Outcome::Clamped : Outcome::Applied;
    }
    void integer(std::string_view name, int& target, int lo, int hi) {
        if (name != key) {
            return;
        }
        int parsed = 0;
        const char* end = value.data() + value.size();
        const auto r = std::from_chars(value.data(), end, parsed);
        if (value.empty() || r.ec != std::errc() || r.ptr != end) {
            // A number too big for int is out of range, not unreadable: use the nearest end.
            if (r.ec == std::errc::result_out_of_range && r.ptr == end) {
                target = value.front() == '-' ? lo : hi;
                outcome = Outcome::Clamped;
            } else {
                outcome = Outcome::BadValue;
            }
            return;
        }
        target = std::clamp(parsed, lo, hi);
        outcome = parsed < lo || parsed > hi ? Outcome::Clamped : Outcome::Applied;
    }
    void color(const std::string& name, RGBA& target) {
        if (name != key) {
            return;
        }
        outcome = parse_hex(value, &target) ? Outcome::Applied : Outcome::BadValue;
    }
    void flag(std::string_view name, bool& target) {
        if (name != key) {
            return;
        }
        if (value == "true" || value == "false") {
            target = value == "true";
            outcome = Outcome::Applied;
        } else {
            outcome = Outcome::BadValue;
        }
    }
};

std::string_view trim(std::string_view s) {
    constexpr std::string_view kSpace = " \t\r";
    const std::size_t first = s.find_first_not_of(kSpace);
    if (first == std::string_view::npos) {
        return {};
    }
    return s.substr(first, s.find_last_not_of(kSpace) - first + 1);
}

bool valid_utf8(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        std::size_t extra = 0;
        unsigned min_cp = 0;
        unsigned cp = 0;
        if (c < 0x80) {
            ++i;
            continue;
        } else if (c >= 0xC2 && c <= 0xDF) {
            extra = 1, min_cp = 0x80, cp = c & 0x1Fu;
        } else if (c >= 0xE0 && c <= 0xEF) {
            extra = 2, min_cp = 0x800, cp = c & 0x0Fu;
        } else if (c >= 0xF0 && c <= 0xF4) {
            extra = 3, min_cp = 0x10000, cp = c & 0x07u;
        } else {
            return false;
        }
        if (i + extra >= s.size()) {
            return false;
        }
        for (std::size_t k = 1; k <= extra; ++k) {
            const auto cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        if (cp < min_cp || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            return false;
        }
        i += extra + 1;
    }
    return true;
}

std::string default_slot_name(int slot) { return "Preset " + std::to_string(slot + 1); }

bool valid_slot(int slot) { return slot >= 0 && slot < kUserPresetSlots; }

// Trimmed, without control characters (a name must stay on one line), at most
// `kMaxPresetNameBytes` cut at a character boundary. Empty for text that is not valid UTF-8.
std::string sanitize_preset_name(std::string_view name) {
    if (!valid_utf8(name)) {
        return {};
    }
    std::string out;
    for (char ch : name) {
        const auto c = static_cast<unsigned char>(ch);
        if (c >= 0x20 && c != 0x7F) {
            out += ch;
        }
    }
    if (out.size() > kMaxPresetNameBytes) {
        std::size_t cut = kMaxPresetNameBytes;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) {
            --cut;
        }
        out.resize(cut);
    }
    return std::string(trim(out));
}

} // namespace

bool UserPresets::is_used(int slot) const {
    return valid_slot(slot) && slots[static_cast<std::size_t>(slot)].used;
}

std::string_view UserPresets::slot_name(int slot) const {
    return is_used(slot) ? std::string_view(slots[static_cast<std::size_t>(slot)].name)
                         : std::string_view();
}

bool UserPresets::save_slot(int slot, std::string_view name, const LookSettings& look) {
    if (!valid_slot(slot)) {
        return false;
    }
    std::string clean = sanitize_preset_name(name);
    if (clean.empty()) {
        clean = default_slot_name(slot);
    }
    slots[static_cast<std::size_t>(slot)] = UserPresetSlot{true, std::move(clean), look.clamped()};
    return true;
}

bool UserPresets::load_slot(int slot, LookSettings* out) const {
    if (!is_used(slot)) {
        return false;
    }
    if (out != nullptr) {
        *out = slots[static_cast<std::size_t>(slot)].look;
    }
    return true;
}

bool UserPresets::clear_slot(int slot) {
    if (!valid_slot(slot)) {
        return false;
    }
    slots[static_cast<std::size_t>(slot)] = UserPresetSlot{};
    return true;
}

std::string serialize(const LookSettings& look) {
    Writer w;
    w.line("version", std::to_string(kLookFormatVersion));
    visit_fields(look, w);
    return std::move(w.out);
}

std::string serialize_document(const LookDocument& doc) {
    Writer w;
    w.line("version", std::to_string(kLookFormatVersion));
    visit_fields(doc.look, w);
    for (int i = 0; i < kUserPresetSlots; ++i) {
        const UserPresetSlot& slot = doc.user_presets.slots[static_cast<std::size_t>(i)];
        w.prefix = "slot_" + std::to_string(i + 1) + "_";
        w.line("used", slot.used ? "true" : "false");
        if (slot.used) {
            w.line("name", sanitize_preset_name(slot.name));
            visit_fields(slot.look, w);
        }
    }
    return std::move(w.out);
}

LookSettings parse_look(std::string_view text, ParseReport* report) {
    return parse_look_document(text, report).look;
}

LookDocument parse_look_document(std::string_view text, ParseReport* report) {
    LookDocument result;
    ParseReport local;
    ParseReport& rep = report != nullptr ? *report : local;
    rep = ParseReport{};

    const auto add_issue = [&rep](ParseIssue::Kind kind, int line, std::string_view key) {
        rep.issues.push_back({kind, line, std::string(key)});
    };
    const auto note_outcome = [&](Outcome outcome, int line, std::string_view key) {
        switch (outcome) {
        case Outcome::NoMatch:
            add_issue(ParseIssue::Kind::UnknownKey, line, key);
            break;
        case Outcome::BadValue:
            add_issue(ParseIssue::Kind::BadValue, line, key);
            break;
        case Outcome::Clamped:
            add_issue(ParseIssue::Kind::OutOfRange, line, key);
            ++rep.applied;
            break;
        case Outcome::Applied:
            ++rep.applied;
            break;
        }
    };

    int line_no = 0;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t eol = std::min(text.find('\n', pos), text.size());
        const std::string_view line = trim(text.substr(pos, eol - pos));
        pos = eol + 1;
        ++line_no;
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const std::size_t eq = line.find('=');
        const std::string_view key = eq == std::string_view::npos ? line : trim(line.substr(0, eq));
        if (eq == std::string_view::npos || key.empty()) {
            add_issue(ParseIssue::Kind::BadLine, line_no, key);
            continue;
        }
        const std::string_view value = trim(line.substr(eq + 1));

        if (key == "version") {
            int version = 0;
            const char* end = value.data() + value.size();
            const auto r = std::from_chars(value.data(), end, version);
            if (value.empty() || r.ec != std::errc() || r.ptr != end || version < 1) {
                add_issue(ParseIssue::Kind::BadValue, line_no, key);
                continue;
            }
            rep.version = version;
            if (version > kLookFormatVersion) {
                add_issue(ParseIssue::Kind::NewerVersion, line_no, key);
            }
            ++rep.applied;
            continue;
        }

        // "slot_1_..." to "slot_3_...": a user slot. Other "slot_" keys are unknown keys.
        if (key.size() > 7 && key.substr(0, 5) == "slot_" && key[5] >= '1' &&
            key[5] < '1' + kUserPresetSlots && key[6] == '_') {
            UserPresetSlot& slot =
                result.user_presets.slots[static_cast<std::size_t>(key[5] - '1')];
            const std::string_view sub = key.substr(7);
            if (sub == "used") {
                if (value == "true" || value == "false") {
                    slot.used = value == "true";
                    ++rep.applied;
                } else {
                    add_issue(ParseIssue::Kind::BadValue, line_no, key);
                }
            } else if (sub == "name") {
                if (valid_utf8(value)) {
                    slot.name = sanitize_preset_name(value);
                    ++rep.applied;
                } else {
                    add_issue(ParseIssue::Kind::BadValue, line_no, key);
                }
            } else {
                Reader reader{sub, value};
                visit_fields(slot.look, reader);
                note_outcome(reader.outcome, line_no, key);
            }
            continue;
        }

        Reader reader{key, value};
        visit_fields(result.look, reader);
        note_outcome(reader.outcome, line_no, key);
    }

    for (int i = 0; i < kUserPresetSlots; ++i) {
        UserPresetSlot& slot = result.user_presets.slots[static_cast<std::size_t>(i)];
        if (!slot.used) {
            slot = UserPresetSlot{};
        } else if (slot.name.empty()) {
            slot.name = default_slot_name(i);
        }
    }
    return result;
}

} // namespace toonval
