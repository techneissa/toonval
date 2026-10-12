// SPDX-License-Identifier: GPL-3.0-or-later
// Builds a KeyboardIdentity from a libremidi port. Header-only so the app and `midi-ports` share
// one implementation without another library target.
#pragma once

#include "toonval/keyboard_identity.hpp"

#include <libremidi/libremidi.hpp>

#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <type_traits>

namespace toonval::app {

struct PortIdentity {
    KeyboardIdentity identity;
    // Where the USB id came from, for `midi-ports`: "sysfs", "CoreMIDI USBVendorProduct
    // (encoding unverified)", or "not reported".
    std::string usb_source = "not reported";
    // Changes when the same port comes back as a new device (Linux: USB bus and device number).
    // Empty when the backend gives nothing; then only the port listing tells a replug apart.
    std::string instance;
};

namespace detail {

inline std::optional<std::string> nonempty(const std::string& s) {
    return s.empty() ? std::nullopt : std::optional<std::string>(s);
}

// Reads a small sysfs attribute. nullopt when it cannot be opened.
inline std::optional<std::string> read_small_file(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        return std::nullopt;
    }
    std::string text;
    std::getline(in, text);
    return text;
}

} // namespace detail

// libremidi gives the same fields on every backend; which ones are filled differs (see
// libremidi/port_information.hpp). The USB id comes from:
// - ALSA (Linux, built with udev): `device` is the sysfs path of the card's controlC node; the
//   USB device directory above it has idVendor and idProduct.
// - CoreMIDI: `device` is the device's USBVendorProduct property, encoding unverified.
// - Others: not reported.
inline PortIdentity identify_port(const libremidi::port_information& port) {
    PortIdentity out;
    KeyboardIdentity& id = out.identity;
    id.port_name = detail::nonempty(port.port_name);
    id.device_name = detail::nonempty(port.device_name);
    id.display_name = detail::nonempty(port.display_name);
    id.manufacturer = detail::nonempty(port.manufacturer);
    id.product = detail::nonempty(port.product);

    libremidi_variant_alias::visit(
        [&](const auto& device) {
            using T = std::decay_t<decltype(device)>;
            // Only ALSA gives a string that is a /sys path (others are rejected by the walk),
            // and only CoreMIDI gives an integer.
            if constexpr (std::is_same_v<T, std::string>) {
                if (const auto usb = usb_device_from_sysfs(device, detail::read_small_file)) {
                    id.usb_vendor = usb->id.vendor;
                    id.usb_product = usb->id.product;
                    out.usb_source = "sysfs";
                    out.instance = usb->instance;
                }
            } else if constexpr (std::is_same_v<T, std::uint64_t>) {
                if (const auto usb = usb_id_from_core_midi(static_cast<std::int64_t>(device))) {
                    id.usb_vendor = usb->vendor;
                    id.usb_product = usb->product;
                    out.usb_source = "CoreMIDI USBVendorProduct (encoding unverified)";
                }
            }
        },
        port.device);
    return out;
}

} // namespace toonval::app
