// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/look.hpp"

#include <string>

#include <doctest/doctest.h>

using namespace toonval;

namespace {

LookSettings odd_look() {
    LookSettings s = preset(PresetId::NeonNight);
    s.visible_seconds = 7.5f;
    s.names_on_keys = true;
    return s;
}

bool has_issue(const ParseReport& r, ParseIssue::Kind kind, const std::string& key) {
    for (const ParseIssue& i : r.issues) {
        if (i.kind == kind && i.key == key) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("there are three slots and they start empty") {
    const UserPresets p;
    CHECK(p.slots.size() == 3);
    CHECK(kUserPresetSlots == 3);
    for (int i = 0; i < kUserPresetSlots; ++i) {
        CHECK_FALSE(p.is_used(i));
        CHECK(p.slot_name(i).empty());
        LookSettings out = preset(PresetId::Glass);
        CHECK_FALSE(p.load_slot(i, &out));
        CHECK(out == preset(PresetId::Glass)); // an empty slot leaves the output alone
    }
}

TEST_CASE("save then load gives the look back, under its name") {
    UserPresets p;
    REQUIRE(p.save_slot(1, "Evening practice", odd_look()));
    CHECK(p.is_used(1));
    CHECK(p.slot_name(1) == "Evening practice");
    CHECK_FALSE(p.is_used(0));
    CHECK_FALSE(p.is_used(2));
    LookSettings out;
    REQUIRE(p.load_slot(1, &out));
    CHECK(out == odd_look());
}

TEST_CASE("saving into a used slot replaces it, clearing empties it") {
    UserPresets p;
    p.save_slot(0, "One", preset(PresetId::Glass));
    p.save_slot(0, "Two", preset(PresetId::Classic));
    CHECK(p.slot_name(0) == "Two");
    LookSettings out;
    p.load_slot(0, &out);
    CHECK(out == preset(PresetId::Classic));
    CHECK(p.clear_slot(0));
    CHECK_FALSE(p.is_used(0));
    CHECK(p == UserPresets{});
}

TEST_CASE("a slot index out of range is refused") {
    UserPresets p;
    LookSettings out;
    for (int bad : {-1, 3, 100}) {
        CHECK_FALSE(p.save_slot(bad, "x", LookSettings{}));
        CHECK_FALSE(p.load_slot(bad, &out));
        CHECK_FALSE(p.clear_slot(bad));
        CHECK_FALSE(p.is_used(bad));
        CHECK(p.slot_name(bad).empty());
    }
    CHECK(p == UserPresets{});
}

TEST_CASE("loading with no output pointer just says whether the slot is used") {
    UserPresets p;
    p.save_slot(2, "x", LookSettings{});
    CHECK(p.load_slot(2, nullptr));
    CHECK_FALSE(p.load_slot(1, nullptr));
}

TEST_CASE("a saved look is stored valid") {
    LookSettings wild;
    wild.visible_seconds = 9999.0f;
    UserPresets p;
    p.save_slot(0, "wild", wild);
    LookSettings out;
    p.load_slot(0, &out);
    CHECK(out.is_valid());
    CHECK(out.visible_seconds == kMaxVisibleSeconds);
}

TEST_CASE("names are trimmed, stripped of control characters and capped at a character boundary") {
    UserPresets p;
    p.save_slot(0, "  spaced out  ", LookSettings{});
    CHECK(p.slot_name(0) == "spaced out");
    p.save_slot(0, "line\none\ttab\r\x01\x7f", LookSettings{});
    CHECK(p.slot_name(0) == "lineonetab");
    // An empty name gets a placeholder instead of an unnamed slot.
    p.save_slot(1, "   ", LookSettings{});
    CHECK_FALSE(p.slot_name(1).empty());
    CHECK(p.is_used(1));
    // 3-byte characters: the cap never cuts one in half.
    std::string long_name;
    for (int i = 0; i < 40; ++i) {
        long_name += "\xE6\x97\xA5"; // 日
    }
    p.save_slot(2, long_name, LookSettings{});
    const std::string_view saved = p.slot_name(2);
    CHECK(saved.size() <= kMaxPresetNameBytes);
    CHECK(saved.size() % 3 == 0);
    CHECK(saved.substr(0, 3) == "\xE6\x97\xA5");
}

TEST_CASE("empty user presets add nothing but three empty-slot lines") {
    LookDocument doc;
    const std::string text = serialize_document(doc);
    CHECK(text.find("slot_1_used = false\n") != std::string::npos);
    CHECK(text.find("slot_2_used = false\n") != std::string::npos);
    CHECK(text.find("slot_3_used = false\n") != std::string::npos);
    CHECK(text.find("slot_1_name") == std::string::npos);
    ParseReport report;
    const LookDocument back = parse_look_document(text, &report);
    CHECK(back == doc);
    CHECK(report.clean());
}

TEST_CASE("slots round-trip with the look, including spaces and unicode in names") {
    LookDocument doc;
    doc.look = preset(PresetId::FrostedPastel);
    doc.user_presets.save_slot(0, "My favourite look", odd_look());
    doc.user_presets.save_slot(2, "Café ☕ 練習 = #1", preset(PresetId::HighContrast));
    REQUIRE_FALSE(doc.user_presets.is_used(1));

    ParseReport report;
    const LookDocument back = parse_look_document(serialize_document(doc), &report);
    CHECK(report.clean());
    CHECK(back == doc);
    CHECK(back.user_presets.slot_name(0) == "My favourite look");
    CHECK(back.user_presets.slot_name(2) == "Café ☕ 練習 = #1");
    CHECK_FALSE(back.user_presets.is_used(1));
    CHECK(back.look == preset(PresetId::FrostedPastel));
}

TEST_CASE("every slot filled with a different built-in preset round-trips") {
    LookDocument doc;
    doc.look = preset(PresetId::Classic);
    for (int i = 0; i < kUserPresetSlots; ++i) {
        doc.user_presets.save_slot(i, std::string("slot ") + std::to_string(i),
                                   preset(kAllPresets[static_cast<std::size_t>(i + 2)]));
    }
    CHECK(parse_look_document(serialize_document(doc)) == doc);
}

TEST_CASE("the look-only serialize and parse keep working and ignore slots") {
    LookDocument doc;
    doc.look = preset(PresetId::Glass);
    doc.user_presets.save_slot(0, "kept elsewhere", odd_look());
    const std::string text = serialize_document(doc);
    ParseReport report;
    CHECK(parse_look(text, &report) == doc.look);
    CHECK(report.clean());
    CHECK(serialize(doc.look).find("slot_") == std::string::npos);
}

TEST_CASE("a damaged slot is dropped and reported, the rest still loads") {
    LookDocument doc;
    doc.look = preset(PresetId::Glass);
    doc.user_presets.save_slot(0, "good", odd_look());
    doc.user_presets.save_slot(1, "bad", odd_look());
    std::string text = serialize_document(doc);
    const std::size_t at = text.find("slot_2_note_style = neon");
    REQUIRE(at != std::string::npos);
    text.replace(at, std::string("slot_2_note_style = neon").size(), "slot_2_note_style = ???");
    ParseReport report;
    const LookDocument back = parse_look_document(text, &report);
    CHECK(has_issue(report, ParseIssue::Kind::BadValue, "slot_2_note_style"));
    CHECK(back.look == doc.look);
    CHECK(back.user_presets.slot_name(0) == "good");
    LookSettings first;
    back.user_presets.load_slot(0, &first);
    CHECK(first == odd_look());
    // The damaged field keeps its default; the slot stays and the rest of it is intact.
    CHECK(back.user_presets.is_used(1));
    LookSettings second;
    back.user_presets.load_slot(1, &second);
    LookSettings expected = odd_look();
    expected.note_style = LookSettings{}.note_style;
    CHECK(second == expected);
}

TEST_CASE("slot lines for unused slots, a bad slot number, or a bad name are handled") {
    ParseReport report;
    const LookDocument d = parse_look_document("slot_1_used = false\n"
                                               "slot_1_note_style = neon\n"
                                               "slot_4_used = true\n"
                                               "slot_0_name = x\n"
                                               "slot_2_used = true\n"
                                               "slot_2_name = \xFF\xFE broken\n",
                                               &report);
    CHECK_FALSE(d.user_presets.is_used(0)); // stray fields of an unused slot are dropped
    CHECK(d.user_presets.slots[0] == UserPresetSlot{});
    CHECK(has_issue(report, ParseIssue::Kind::UnknownKey, "slot_4_used"));
    CHECK(has_issue(report, ParseIssue::Kind::UnknownKey, "slot_0_name"));
    CHECK(has_issue(report, ParseIssue::Kind::BadValue, "slot_2_name"));
    CHECK(d.user_presets.is_used(1));
    CHECK_FALSE(d.user_presets.slot_name(1).empty()); // a used slot always has a name
}

TEST_CASE("a name with a newline put in by hand cannot break the file") {
    LookDocument doc;
    doc.user_presets.slots[0].used = true;
    doc.user_presets.slots[0].name = "evil\nversion = 99\nnote_style = neon";
    ParseReport report;
    const LookDocument back = parse_look_document(serialize_document(doc), &report);
    CHECK(report.clean());
    CHECK(back.look == LookSettings{});
    CHECK(back.user_presets.is_used(0));
    CHECK(back.user_presets.slot_name(0).find('\n') == std::string_view::npos);
}
