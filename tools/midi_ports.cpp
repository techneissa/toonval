// SPDX-License-Identifier: GPL-3.0-or-later
// Lists MIDI input and output ports. For each input it prints everything the backend reports about
// the device and the keyboard profile the app would pick, with the reason, so one run with a new
// keyboard plugged in gives what its profile needs. The output is plain text, easy to paste.
#include "../app/midi_port_identity.hpp"
#include "toonval/keyboard_identity.hpp"

#include <libremidi/libremidi.hpp>

#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

namespace {

void field(const char* label, const std::optional<std::string>& value) {
    std::printf("    %-14s %s\n", label, value ? ("\"" + *value + "\"").c_str() : "(not reported)");
}

std::string transport(libremidi::transport_type type) {
    std::string out;
    auto add = [&out, type](libremidi::transport_type bit, const char* name) {
        if ((type & bit) != 0) {
            out += out.empty() ? name : std::string(", ") + name;
        }
    };
    add(libremidi::transport_type::software, "software");
    add(libremidi::transport_type::loopback, "loopback");
    add(libremidi::transport_type::hardware, "hardware");
    add(libremidi::transport_type::usb, "usb");
    add(libremidi::transport_type::bluetooth, "bluetooth");
    add(libremidi::transport_type::pci, "pci");
    add(libremidi::transport_type::network, "network");
    return out.empty() ? "unknown" : out;
}

std::string_view sv_or(std::string_view s, std::string_view fallback) {
    return s.empty() ? fallback : s;
}

} // namespace

int main() {
    // Virtual ports too, as the app opens them (midi-demo, software keyboards).
    libremidi::observer_configuration conf;
    conf.track_hardware = true;
    conf.track_virtual = true;
    libremidi::observer observer(conf);
    const auto api = libremidi::get_api_display_name(observer.get_current_api());

    const auto inputs = observer.get_input_ports();
    std::printf("MIDI inputs (%zu), backend %.*s\n", inputs.size(), static_cast<int>(api.size()),
                api.data());
    int index = 0;
    for (const auto& port : inputs) {
        const auto pid = toonval::app::identify_port(port);
        const auto& id = pid.identity;
        const auto match = toonval::match_keyboard(id);

        std::printf("[%d] %s\n", ++index,
                    std::string(sv_or(port.display_name, port.port_name)).c_str());
        field("port name:", id.port_name);
        field("device name:", id.device_name);
        field("display name:", id.display_name);
        field("manufacturer:", id.manufacturer);
        field("product:", id.product);
        if (id.usb_vendor && id.usb_product) {
            std::printf("    %-14s %04x:%04x (from %s)\n", "USB VID:PID:", *id.usb_vendor,
                        *id.usb_product, pid.usb_source.c_str());
        } else {
            std::printf("    %-14s unknown (%s)\n", "USB VID:PID:", pid.usb_source.c_str());
        }
        if (const auto* path = std::get_if<std::string>(&port.device)) {
            std::printf("    %-14s %s\n", "sysfs path:", path->c_str());
        }
        if (!pid.instance.empty()) {
            std::printf("    %-14s %s\n", "USB bus-dev:", pid.instance.c_str());
        }
        std::printf("    %-14s %s\n", "transport:", transport(port.type).c_str());
        const auto reason = toonval::describe(match.reason);
        if (match.profile != nullptr) {
            std::printf("    %-14s %.*s, %d keys, notes %d..%d (%.*s",
                        "profile:", static_cast<int>(match.profile->name.size()),
                        match.profile->name.data(), match.profile->range.key_count(),
                        match.profile->range.lowest, match.profile->range.highest,
                        static_cast<int>(reason.size()), reason.data());
            if (!match.pattern.empty()) {
                std::printf(" \"%.*s\"", static_cast<int>(match.pattern.size()),
                            match.pattern.data());
            }
            std::printf(")\n");
        } else {
            std::printf("    %-14s none (%.*s)\n", "profile:", static_cast<int>(reason.size()),
                        reason.data());
        }
    }

    const auto outputs = observer.get_output_ports();
    std::printf("MIDI outputs (%zu)\n", outputs.size());
    for (const auto& port : outputs) {
        std::printf("  %s\n", port.port_name.c_str());
    }
    return 0;
}
