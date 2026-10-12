// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/keyboard_identity.hpp"

#include <array>
#include <map>
#include <optional>
#include <string>

#include <doctest/doctest.h>

using namespace toonval;

namespace {
// A table with USB ids, for the rules the built-in table does not exercise yet.
constexpr std::array kTestProfiles = {
    KeyboardProfile{"Yamaha PSR-E383", kRange61, {}, {"PSR-E383"}},
    KeyboardProfile{"Yamaha PSR-E38", kRange61, {}, {"PSR-E38"}},
    KeyboardProfile{"M-Audio Keystation 88es", kRange88, {0x0763, 0x0192}, {"Keystation 88es"}},
    KeyboardProfile{"Korg nanoKEY", KeyRange{48, 72}, {0x0944, 0x010d}, {"nanoKEY"}},
    KeyboardProfile{"Twin A", kRange49, {0x1234, 0x0001}, {}},
    KeyboardProfile{"Twin B", kRange61, {0x1234, 0x0001}, {}},
    KeyboardProfile{"Bad generic", kRange88, {}, {"Digital Piano"}},
    KeyboardProfile{"Akai MPK49", kRange49, {}, {"MPK49"}},
};

KeyboardIdentity named(std::string port) {
    KeyboardIdentity id;
    id.port_name = std::move(port);
    return id;
}

KeyboardIdentity usb(std::uint16_t vendor, std::uint16_t product, std::string port = {}) {
    KeyboardIdentity id;
    if (!port.empty()) {
        id.port_name = std::move(port);
    }
    id.usb_vendor = vendor;
    id.usb_product = product;
    return id;
}

KeyboardMatch match(const KeyboardIdentity& id) { return match_keyboard(id, kTestProfiles); }
} // namespace

// --- Rule 1: USB VID:PID first -----------------------------------------------------------------

TEST_CASE("an exact USB vendor and product id matches its profile") {
    const auto m = match(usb(0x0944, 0x010d));
    REQUIRE(m.profile != nullptr);
    CHECK(m.profile->name == "Korg nanoKEY");
    CHECK(m.reason == KeyboardMatchReason::UsbId);
}

TEST_CASE("the USB id wins over a name that names another model") {
    const auto m = match(usb(0x0944, 0x010d, "PSR-E383 MIDI 1"));
    REQUIRE(m.profile != nullptr);
    CHECK(m.profile->name == "Korg nanoKEY");
    CHECK(m.reason == KeyboardMatchReason::UsbId);
}

TEST_CASE("a vendor id alone never matches") {
    KeyboardIdentity id;
    id.usb_vendor = 0x0944;
    CHECK(match(id).profile == nullptr);
    id.usb_product = 0x9999;
    CHECK(match(id).profile == nullptr);
}

TEST_CASE("a USB id shared by two profiles is ambiguous, never a guess") {
    const auto m = match(usb(0x1234, 0x0001));
    CHECK(m.profile == nullptr);
    CHECK(m.reason == KeyboardMatchReason::Ambiguous);
}

TEST_CASE("a model name does not match a profile whose known USB id differs") {
    // The name says Keystation 88es but the device reports another product id: don't trust it.
    CHECK(match(usb(0x0763, 0x0999, "Keystation 88es MIDI 1")).profile == nullptr);
    // With no USB id reported, the name is enough.
    const auto m = match(named("Keystation 88es MIDI 1"));
    REQUIRE(m.profile != nullptr);
    CHECK(m.reason == KeyboardMatchReason::Name);
}

TEST_CASE("an unknown USB id falls through to the model name") {
    // The E383's product id is not known yet, so it is matched by name.
    const auto m = match(usb(0x0499, 0x1234, "PSR-E383 MIDI 1"));
    REQUIRE(m.profile != nullptr);
    CHECK(m.profile->name == "Yamaha PSR-E383");
    CHECK(m.reason == KeyboardMatchReason::Name);
    CHECK(m.pattern == "PSR-E383");
}

// --- Rule 2: model-specific names --------------------------------------------------------------

TEST_CASE("model names match case-insensitively in every name field") {
    for (auto field : {0, 1, 2, 3}) {
        KeyboardIdentity id;
        const std::string name = "yamaha psr-e383 digital keyboard";
        if (field == 0) {
            id.port_name = name;
        } else if (field == 1) {
            id.device_name = name;
        } else if (field == 2) {
            id.display_name = name;
        } else {
            id.product = name;
        }
        const auto m = match(id);
        REQUIRE(m.profile != nullptr);
        CHECK(m.profile->name == "Yamaha PSR-E383");
    }
}

TEST_CASE("the manufacturer field never matches a model") {
    KeyboardIdentity id;
    id.manufacturer = "PSR-E383";
    CHECK(match(id).profile == nullptr);
}

TEST_CASE("model names match only on word boundaries") {
    // PSR-E38 must not match PSR-E383, nor PSR-E383 match PSR-E3830.
    const auto e383 = match(named("PSR-E383 MIDI 1"));
    REQUIRE(e383.profile != nullptr);
    CHECK(e383.profile->name == "Yamaha PSR-E383");

    const auto e38 = match(named("PSR-E38 MIDI 1"));
    REQUIRE(e38.profile != nullptr);
    CHECK(e38.profile->name == "Yamaha PSR-E38");

    CHECK(match(named("PSR-E3830")).profile == nullptr);
    CHECK(match(named("XPSR-E383")).profile == nullptr);
    CHECK(match(named("Akai MPK49 MIDI 2")).profile != nullptr);
    CHECK(match(named("Akai MPK490")).profile == nullptr);
    CHECK(contains_model_name("Akai MPK49", "mpk49"));
    CHECK(contains_model_name("(MPK49)", "MPK49"));
    CHECK_FALSE(contains_model_name("MPK49", ""));
    CHECK_FALSE(contains_model_name("MPK", "MPK49"));
}

TEST_CASE("two profiles named by one device are ambiguous") {
    const auto m = match(named("PSR-E383 / MPK49"));
    CHECK(m.profile == nullptr);
    CHECK(m.reason == KeyboardMatchReason::Ambiguous);
}

// --- Rule 3: generic names never match ---------------------------------------------------------

TEST_CASE("generic names are recognized") {
    for (const char* name :
         {"Digital Keyboard", "DIGITAL KEYBOARD", "Digital Keyboard MIDI 1", "Digital Piano",
          "USB MIDI", "USB-MIDI", "CASIO USB-MIDI", "CASIO USB-MIDI MIDI 1", "MIDI Keyboard",
          "USB MIDI Keyboard", "Yamaha Portatone", "Roland Digital Piano", "MIDI 1", ""}) {
        CHECK_MESSAGE(is_generic_keyboard_name(name), name);
    }
    for (const char* name : {"PSR-E383", "Akai MPK49 MIDI 1", "midi-demo", "nanoKEY2"}) {
        CHECK_MESSAGE(!is_generic_keyboard_name(name), name);
    }
}

TEST_CASE("a generic name never matches, even a profile that lists it") {
    for (const char* name : {"Digital Piano", "Digital Keyboard", "USB MIDI", "MIDI Keyboard"}) {
        const auto m = match(named(name));
        CHECK(m.profile == nullptr);
        CHECK(m.reason == KeyboardMatchReason::GenericName);
    }
}

TEST_CASE("an unknown specific name is reported as no profile, not generic") {
    const auto m = match(named("midi-demo"));
    CHECK(m.profile == nullptr);
    CHECK(m.reason == KeyboardMatchReason::NoMatch);
    CHECK(match(KeyboardIdentity{}).reason == KeyboardMatchReason::NoMatch);
}

TEST_CASE("match reasons read as midi-ports prints them") {
    CHECK(describe(KeyboardMatchReason::UsbId) == "matched by USB id");
    CHECK(describe(KeyboardMatchReason::Name) == "matched by name");
    CHECK(describe(KeyboardMatchReason::GenericName) == "generic name, unknown");
    CHECK(describe(KeyboardMatchReason::Ambiguous) == "ambiguous, unknown");
    CHECK(describe(KeyboardMatchReason::NoMatch) == "no profile, unknown");
}

// --- Built-in table ----------------------------------------------------------------------------

TEST_CASE("the built-in table matches the PSR-E383 by name") {
    const auto m = match_keyboard(named("PSR-E383 MIDI 1"));
    REQUIRE(m.profile != nullptr);
    CHECK(m.profile->range.lowest == 36);
    CHECK(m.profile->range.highest == 96);
    CHECK(m.reason == KeyboardMatchReason::Name);
    // Yamaha's vendor id alone is not the E383.
    CHECK(match_keyboard(usb(0x0499, 0x1617)).profile == nullptr);
}

TEST_CASE("no built-in profile has a generic, empty-only, or invalid entry") {
    for (const auto& profile : builtin_keyboard_profiles()) {
        CHECK(profile.range.valid());
        bool has_key = profile.usb.vendor != 0 && profile.usb.product != 0;
        for (auto pattern : profile.name_patterns) {
            if (!pattern.empty()) {
                CHECK_MESSAGE(!is_generic_keyboard_name(pattern), pattern);
                has_key = true;
            }
        }
        CHECK_MESSAGE(has_key, profile.name);
    }
}

// --- USB ids from the platform -----------------------------------------------------------------

TEST_CASE("sysfs hex ids parse") {
    CHECK(parse_usb_hex_id("0499\n") == std::optional<std::uint16_t>{0x0499});
    CHECK(parse_usb_hex_id("010d") == std::optional<std::uint16_t>{0x010d});
    CHECK(parse_usb_hex_id("FFFF") == std::optional<std::uint16_t>{0xFFFF});
    CHECK_FALSE(parse_usb_hex_id(""));
    CHECK_FALSE(parse_usb_hex_id("12345"));
    CHECK_FALSE(parse_usb_hex_id("04g9"));
    CHECK_FALSE(parse_usb_hex_id("0x49"));
}

TEST_CASE("CoreMIDI USBVendorProduct splits as vendor in the high half") {
    // Encoding unverified on a real device; this pins the assumption.
    const auto id = usb_id_from_core_midi(0x0499'1617);
    REQUIRE(id);
    CHECK(id->vendor == 0x0499);
    CHECK(id->product == 0x1617);
    // libremidi sign-extends the SInt32 into 64 bits for vendors from 0x8000 up.
    const auto high = usb_id_from_core_midi(static_cast<std::int64_t>(std::int32_t(0x8086'0001u)));
    REQUIRE(high);
    CHECK(high->vendor == 0x8086);
    CHECK(high->product == 0x0001);
    CHECK_FALSE(usb_id_from_core_midi(0));
    CHECK_FALSE(usb_id_from_core_midi(0x0000'1234)); // vendor 0 is not a USB vendor
}

namespace {
struct FakeSysfs {
    std::map<std::string, std::string> files;
    int reads = 0;
    std::optional<std::string> operator()(const std::string& path) {
        ++reads;
        const auto it = files.find(path);
        if (it == files.end()) {
            return std::nullopt;
        }
        return it->second;
    }
};

constexpr const char* kUsbDir =
    "/sys/devices/platform/axi/1000120000.pcie/1f00200000.usb/xhci-hcd.0/"
    "usb1/1-1";
} // namespace

TEST_CASE("sysfs walk finds the USB device above the ALSA card") {
    FakeSysfs fs;
    const std::string dir = kUsbDir;
    fs.files[dir + "/idVendor"] = "0499\n";
    fs.files[dir + "/idProduct"] = "1617\n";
    fs.files[dir + "/busnum"] = "1\n";
    fs.files[dir + "/devnum"] = "7\n";
    // The interface directory has no idVendor; the walk must pass it.
    const auto dev = usb_device_from_sysfs(dir + "/1-1:1.0/sound/card1/controlC1",
                                           [&fs](const std::string& p) { return fs(p); });
    REQUIRE(dev);
    CHECK(dev->id.vendor == 0x0499);
    CHECK(dev->id.product == 0x1617);
    CHECK(dev->instance == "1-7");
}

TEST_CASE("sysfs walk without bus and device numbers still gives the id") {
    FakeSysfs fs;
    const std::string dir = kUsbDir;
    fs.files[dir + "/idVendor"] = "0944";
    fs.files[dir + "/idProduct"] = "010d";
    const auto dev = usb_device_from_sysfs(dir + "/1-1:1.0/sound/card2/controlC2",
                                           [&fs](const std::string& p) { return fs(p); });
    REQUIRE(dev);
    CHECK(dev->id.product == 0x010d);
    CHECK(dev->instance.empty());
}

TEST_CASE("sysfs walk is bounded and stays inside /sys") {
    FakeSysfs fs;
    auto read = [&fs](const std::string& p) { return fs(p); };
    CHECK_FALSE(usb_device_from_sysfs("", read));
    CHECK_FALSE(usb_device_from_sysfs("relative/path/controlC0", read));
    CHECK_FALSE(usb_device_from_sysfs("/etc/controlC0", read));
    CHECK(fs.reads == 0);

    // A PCI sound card: no USB device anywhere up the path.
    CHECK_FALSE(
        usb_device_from_sysfs("/sys/devices/pci0000:00/0000:00:1f.3/sound/card0/controlC0", read));
    // A very deep path stops after the walk limit.
    std::string deep = "/sys/devices";
    for (int i = 0; i < 40; ++i) {
        deep += "/d" + std::to_string(i);
    }
    fs.reads = 0;
    CHECK_FALSE(usb_device_from_sysfs(deep, read));
    CHECK(fs.reads <= 2 * kSysfsWalkLimit);
}
