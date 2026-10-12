// SPDX-License-Identifier: GPL-3.0-or-later
// What a MIDI input says about the keyboard behind it, and which profile that names.
#pragma once

#include "toonval/keyboard.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace toonval {

// Everything a MIDI backend reports about one input port, as plain values. The app fills it from
// libremidi; a field the backend does not report stays nullopt.
struct KeyboardIdentity {
    std::optional<std::string> port_name;    // ALSA port, CoreMIDI endpoint name
    std::optional<std::string> device_name;  // ALSA client, CoreMIDI model
    std::optional<std::string> display_name; // CoreMIDI display name; ALSA repeats the port name
    std::optional<std::string> manufacturer; // never used to pick a model
    std::optional<std::string> product;      // ALSA: the USB id database's model name, via udev
    std::optional<std::uint16_t> usb_vendor;
    std::optional<std::uint16_t> usb_product;
};

enum class KeyboardMatchReason : std::uint8_t {
    UsbId,       // the USB vendor and product id are a profile's
    Name,        // a name names one profile's model
    GenericName, // every reported name is generic ("Digital Keyboard"): unknown
    Ambiguous,   // more than one profile fits: unknown, never a guess
    NoMatch,     // nothing fits: unknown
};

struct KeyboardMatch {
    const KeyboardProfile* profile = nullptr; // nullptr = unknown keyboard, ask the player
    KeyboardMatchReason reason = KeyboardMatchReason::NoMatch;
    std::string_view pattern; // Name: the model name that matched
};

// Picks the profile for a keyboard:
// 1. USB vendor and product id, when both are reported. One profile with that id wins; two are
//    Ambiguous.
// 2. Otherwise a model-specific name pattern found in the port, device, display or product name,
//    case-insensitively and on word boundaries ("PSR-E38" does not match "PSR-E383"). A profile
//    whose own USB id is known but differs from the reported one is skipped.
// 3. Generic names never match, even when a profile lists one as a pattern.
KeyboardMatch match_keyboard(const KeyboardIdentity& id, std::span<const KeyboardProfile> profiles);
// Same, against the built-in table.
KeyboardMatch match_keyboard(const KeyboardIdentity& id);

// "matched by USB id", "matched by name", "generic name, unknown", ...
std::string_view describe(KeyboardMatchReason reason);

// True when the name says nothing about the model: only words like "Digital", "Keyboard",
// "Piano", "USB", "MIDI", brand names and port numbers, e.g. "CASIO USB-MIDI MIDI 1".
bool is_generic_keyboard_name(std::string_view name);

// True when `pattern` occurs in `name` ignoring ASCII case, with no letter or digit right before
// or after it. An empty pattern never matches.
bool contains_model_name(std::string_view name, std::string_view pattern);

// Parses a sysfs idVendor / idProduct file: four hex digits, optional trailing whitespace.
std::optional<std::uint16_t> parse_usb_hex_id(std::string_view text);

// CoreMIDI's USBVendorProduct device property as libremidi reports it (an SInt32, possibly sign-
// extended). Assumed to be vendor << 16 | product. UNVERIFIED: check against System Information
// on a Mac with a USB keyboard. nullopt when the vendor is 0.
std::optional<UsbId> usb_id_from_core_midi(std::int64_t vendor_product);

// A USB device found in sysfs.
struct SysfsUsbDevice {
    UsbId id;
    // "busnum-devnum". The kernel gives a device a new devnum each time it enumerates, so a
    // different instance with the same port listing means the keyboard was unplugged and back.
    // Empty when the files are missing.
    std::string instance;
};

// Directories the sysfs walk visits at most.
inline constexpr int kSysfsWalkLimit = 8;

// Finds the USB device that owns an ALSA card: walks up from `device_path` (libremidi's
// port_information::device on Linux, e.g. ".../usb1/1-1/1-1:1.0/sound/card1/controlC1") to the
// first directory with readable idVendor and idProduct, at most kSysfsWalkLimit levels and never
// outside /sys. `read_file` returns a file's contents or nullopt; core does no file I/O itself.
std::optional<SysfsUsbDevice> usb_device_from_sysfs(
    std::string_view device_path,
    const std::function<std::optional<std::string>(const std::string&)>& read_file);

} // namespace toonval
