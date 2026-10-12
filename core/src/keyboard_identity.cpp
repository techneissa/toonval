// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/keyboard_identity.hpp"

#include <array>

namespace toonval {

namespace {

char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool is_alnum(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool equal_ignore_case(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lower(a[i]) != lower(b[i])) {
            return false;
        }
    }
    return true;
}

// Words that appear in names without naming a model. Seen in real port names
// ("Digital Keyboard", "Digital Piano", "MIDI Keyboard", "CASIO USB-MIDI", "Yamaha Portatone").
constexpr std::array<std::string_view, 25> kGenericWords = {
    "digital",   "keyboard", "keyboards", "piano",  "midi",   "usb",   "port",
    "in",        "out",      "input",     "output", "device", "audio", "interface",
    "portatone", "yamaha",   "casio",     "roland", "korg",   "kawai", "akai",
    "m",         "arturia",  "novation",  "alesis",
};

bool is_generic_word(std::string_view word) {
    bool digits = true;
    for (char c : word) {
        digits = digits && c >= '0' && c <= '9';
    }
    if (digits) {
        return true; // port numbers: "MIDI 1"
    }
    for (auto generic : kGenericWords) {
        if (equal_ignore_case(word, generic)) {
            return true;
        }
    }
    return false;
}

// The name fields that may name a model. The manufacturer never does.
std::array<const std::optional<std::string>*, 4> model_fields(const KeyboardIdentity& id) {
    return {&id.port_name, &id.device_name, &id.display_name, &id.product};
}

bool has_usb(const UsbId& id) { return id.vendor != 0 && id.product != 0; }

} // namespace

bool contains_model_name(std::string_view name, std::string_view pattern) {
    if (pattern.empty() || pattern.size() > name.size()) {
        return false;
    }
    for (std::size_t at = 0; at + pattern.size() <= name.size(); ++at) {
        if (!equal_ignore_case(name.substr(at, pattern.size()), pattern)) {
            continue;
        }
        const bool start_ok = at == 0 || !is_alnum(name[at - 1]);
        const std::size_t end = at + pattern.size();
        const bool end_ok = end == name.size() || !is_alnum(name[end]);
        if (start_ok && end_ok) {
            return true;
        }
    }
    return false;
}

bool is_generic_keyboard_name(std::string_view name) {
    std::size_t at = 0;
    while (at < name.size()) {
        while (at < name.size() && !is_alnum(name[at])) {
            ++at;
        }
        std::size_t end = at;
        while (end < name.size() && is_alnum(name[end])) {
            ++end;
        }
        if (end > at && !is_generic_word(name.substr(at, end - at))) {
            return false;
        }
        at = end;
    }
    return true;
}

KeyboardMatch match_keyboard(const KeyboardIdentity& id,
                             std::span<const KeyboardProfile> profiles) {
    // Rule 1: USB id.
    const bool usb_known = id.usb_vendor && id.usb_product && *id.usb_vendor != 0;
    const UsbId reported = usb_known ? UsbId{*id.usb_vendor, *id.usb_product} : UsbId{};
    if (usb_known) {
        const KeyboardProfile* found = nullptr;
        int hits = 0;
        for (const auto& profile : profiles) {
            if (has_usb(profile.usb) && profile.usb == reported) {
                found = &profile;
                ++hits;
            }
        }
        if (hits == 1) {
            return {found, KeyboardMatchReason::UsbId, {}};
        }
        if (hits > 1) {
            return {nullptr, KeyboardMatchReason::Ambiguous, {}};
        }
    }

    // Rule 2: a model name, skipping generic patterns (rule 3).
    const KeyboardProfile* found = nullptr;
    std::string_view found_pattern;
    for (const auto& profile : profiles) {
        if (usb_known && has_usb(profile.usb) && profile.usb != reported) {
            continue; // the device says it is another product
        }
        for (auto pattern : profile.name_patterns) {
            if (pattern.empty() || is_generic_keyboard_name(pattern)) {
                continue;
            }
            bool hit = false;
            for (const auto* field : model_fields(id)) {
                hit = hit || (*field && contains_model_name(**field, pattern));
            }
            if (!hit) {
                continue;
            }
            if (found != nullptr && found != &profile) {
                return {nullptr, KeyboardMatchReason::Ambiguous, {}};
            }
            found = &profile;
            found_pattern = pattern;
        }
    }
    if (found != nullptr) {
        return {found, KeyboardMatchReason::Name, found_pattern};
    }

    bool any_name = false;
    bool all_generic = true;
    for (const auto* field : model_fields(id)) {
        if (*field && !(*field)->empty()) {
            any_name = true;
            all_generic = all_generic && is_generic_keyboard_name(**field);
        }
    }
    return {nullptr,
            any_name && all_generic ? KeyboardMatchReason::GenericName
                                    : KeyboardMatchReason::NoMatch,
            {}};
}

KeyboardMatch match_keyboard(const KeyboardIdentity& id) {
    return match_keyboard(id, builtin_keyboard_profiles());
}

std::string_view describe(KeyboardMatchReason reason) {
    switch (reason) {
    case KeyboardMatchReason::UsbId:
        return "matched by USB id";
    case KeyboardMatchReason::Name:
        return "matched by name";
    case KeyboardMatchReason::GenericName:
        return "generic name, unknown";
    case KeyboardMatchReason::Ambiguous:
        return "ambiguous, unknown";
    case KeyboardMatchReason::NoMatch:
        break;
    }
    return "no profile, unknown";
}

std::optional<std::uint16_t> parse_usb_hex_id(std::string_view text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == ' ' || text.back() == '\r' ||
                             text.back() == '\t')) {
        text.remove_suffix(1);
    }
    if (text.size() != 4) {
        return std::nullopt;
    }
    std::uint16_t value = 0;
    for (char c : text) {
        int digit = 0;
        if (c >= '0' && c <= '9') {
            digit = c - '0';
        } else if (lower(c) >= 'a' && lower(c) <= 'f') {
            digit = lower(c) - 'a' + 10;
        } else {
            return std::nullopt;
        }
        value = static_cast<std::uint16_t>(value * 16 + digit);
    }
    return value;
}

std::optional<UsbId> usb_id_from_core_midi(std::int64_t vendor_product) {
    const auto bits = static_cast<std::uint32_t>(static_cast<std::uint64_t>(vendor_product));
    const UsbId id{static_cast<std::uint16_t>(bits >> 16), static_cast<std::uint16_t>(bits)};
    if (id.vendor == 0) {
        return std::nullopt;
    }
    return id;
}

std::optional<SysfsUsbDevice> usb_device_from_sysfs(
    std::string_view device_path,
    const std::function<std::optional<std::string>(const std::string&)>& read_file) {
    constexpr std::string_view kSys = "/sys/";
    std::string dir(device_path);
    while (dir.size() > 1 && dir.back() == '/') {
        dir.pop_back();
    }
    for (int level = 0; level < kSysfsWalkLimit; ++level) {
        if (!dir.starts_with(kSys) || dir.size() <= kSys.size()) {
            return std::nullopt;
        }
        const auto vendor = read_file(dir + "/idVendor");
        const auto product = vendor ? read_file(dir + "/idProduct") : std::nullopt;
        if (vendor && product) {
            const auto v = parse_usb_hex_id(*vendor);
            const auto p = parse_usb_hex_id(*product);
            if (v && p) {
                SysfsUsbDevice dev{{*v, *p}, {}};
                auto number = [](const std::optional<std::string>& text) {
                    std::string out;
                    if (text) {
                        for (char c : *text) {
                            if (c >= '0' && c <= '9') {
                                out += c;
                            } else {
                                break;
                            }
                        }
                    }
                    return out;
                };
                const auto bus = number(read_file(dir + "/busnum"));
                const auto addr = number(read_file(dir + "/devnum"));
                if (!bus.empty() && !addr.empty()) {
                    dev.instance = bus + "-" + addr;
                }
                return dev;
            }
        }
        const auto slash = dir.rfind('/');
        if (slash == std::string::npos) {
            return std::nullopt;
        }
        dir.resize(slash);
    }
    return std::nullopt;
}

} // namespace toonval
