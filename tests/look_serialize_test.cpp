// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/look.hpp"

#include <random>
#include <string>

#include <doctest/doctest.h>

using namespace toonval;

namespace {

// A settings value where every field differs from the default.
LookSettings everything_changed() {
    LookSettings s;
    s.note_style = NoteStyle::Neon;
    s.style_intensity = 0.37f;
    s.color_scheme = ColorScheme::ByVelocity;
    s.left_color = {1, 2, 3, 255};
    s.right_color = {250, 251, 252, 128};
    s.unknown_color = {9, 99, 199, 255};
    for (std::size_t i = 0; i < s.pitch_colors.size(); ++i) {
        s.pitch_colors[i] =
            RGBA{static_cast<std::uint8_t>(i * 20), static_cast<std::uint8_t>(255 - i),
                 static_cast<std::uint8_t>(i), 255};
    }
    s.single_color = {17, 34, 51, 255};
    s.high_contrast = true;
    s.name_mode = NameMode::Solfege;
    s.names_show_octave = true;
    s.names_prefer_flats = true;
    s.names_on_blocks = false;
    s.names_on_keys = true;
    s.c_markers = false;
    s.octave_numbering = OctaveNumbering::Yamaha;
    s.key_size = KeySize::Large;
    s.show_typing_keys = true;
    s.key_flash = false;
    s.held_glow = false;
    s.sparks = false;
    s.trail_fade = false;
    s.background = BackgroundStyle::Motion;
    s.reduced_effects = true;
    s.chord_label = false;
    s.chord_label_size = 1.7f;
    s.visible_seconds = 7.3f;
    s.keyboard_height = 0.2345f;
    s.grade_words = false;
    s.grade_position = GradePosition::Top;
    s.grade_opacity = 37;
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

TEST_CASE("every preset survives a text round trip exactly") {
    for (PresetId id : kAllPresets) {
        const LookSettings p = preset(id);
        ParseReport report;
        const LookSettings back = parse_look(serialize(p), &report);
        CHECK(back == p);
        CHECK(report.clean());
        CHECK(report.version == kLookFormatVersion);
    }
}

TEST_CASE("a settings value with every field changed round-trips exactly") {
    const LookSettings s = everything_changed();
    REQUIRE(s.is_valid());
    for (PresetId id : kAllPresets) {
        REQUIRE_FALSE(s == preset(id));
    }
    ParseReport report;
    CHECK(parse_look(serialize(s), &report) == s);
    CHECK(report.clean());
}

TEST_CASE("floats round-trip bit for bit") {
    LookSettings s;
    s.style_intensity = 0.1f;
    s.visible_seconds = 3.14159274f;
    s.keyboard_height = 0.123456789f;
    s.chord_label_size = 1.0000001f;
    CHECK(parse_look(serialize(s)) == s);
}

TEST_CASE("the text starts with a version line and uses key = value") {
    const std::string text = serialize(LookSettings{});
    CHECK(text.rfind("version = 1\n", 0) == 0);
    CHECK(text.find("note_style = glassy\n") != std::string::npos);
    CHECK(text.find("right_color = #5AAAFF\n") != std::string::npos);
    CHECK(text.find("name_mode = letters\n") != std::string::npos);
    CHECK(text.find("background = gradient\n") != std::string::npos);
    CHECK(text.find("high_contrast = false\n") != std::string::npos);
    CHECK(text.back() == '\n');
}

TEST_CASE("empty text and missing keys keep the defaults") {
    ParseReport report;
    CHECK(parse_look("", &report) == LookSettings{});
    CHECK(report.clean());
    CHECK(report.applied == 0);
    CHECK(report.version == 0);

    const LookSettings s = parse_look("version = 1\nnote_style = ghost\n", &report);
    LookSettings expected;
    expected.note_style = NoteStyle::Ghost;
    CHECK(s == expected);
    CHECK(report.clean());
    CHECK(report.applied == 2);
}

TEST_CASE("octave numbering and C markers: defaults, text form and a file without them") {
    CHECK(LookSettings{}.octave_numbering == OctaveNumbering::Scientific);
    CHECK(LookSettings{}.c_markers);
    LookSettings s;
    s.octave_numbering = OctaveNumbering::Yamaha;
    s.c_markers = false;
    const std::string text = serialize(s);
    CHECK(text.find("octave_numbering = yamaha\n") != std::string::npos);
    CHECK(text.find("c_markers = false\n") != std::string::npos);
    CHECK(parse_look(text) == s);
    // A file saved before these settings existed keeps the defaults and is not an error.
    ParseReport report;
    const LookSettings old = parse_look("version = 1\nname_mode = solfege\n", &report);
    CHECK(old.octave_numbering == OctaveNumbering::Scientific);
    CHECK(old.c_markers);
    CHECK(report.clean());
    // A bad value is reported and the default stays.
    const LookSettings bad = parse_look("octave_numbering = roland\n", &report);
    CHECK(bad.octave_numbering == OctaveNumbering::Scientific);
    CHECK(has_issue(report, ParseIssue::Kind::BadValue, "octave_numbering"));
    // Each user slot carries its own numbering.
    LookDocument doc;
    doc.user_presets.save_slot(1, "Yamaha", s);
    CHECK(parse_look_document(serialize_document(doc)) == doc);
}

TEST_CASE("unknown keys are ignored and reported") {
    ParseReport report;
    const LookSettings s =
        parse_look("version = 1\nsparkle_factor = 11\nnote_style = neon\n", &report);
    CHECK(s.note_style == NoteStyle::Neon);
    CHECK(has_issue(report, ParseIssue::Kind::UnknownKey, "sparkle_factor"));
    CHECK(report.issues.size() == 1);
    CHECK(report.issues[0].line == 2);
}

TEST_CASE("bad values are ignored and reported, the setting keeps its default") {
    ParseReport report;
    const LookSettings s = parse_look("note_style = sparkly\n"
                                      "style_intensity = lots\n"
                                      "right_color = blue\n"
                                      "sparks = maybe\n"
                                      "visible_seconds = \n"
                                      "pitch_color_3 = #12\n"
                                      "color_scheme = by-hand\n",
                                      &report);
    CHECK(s == LookSettings{});
    CHECK(has_issue(report, ParseIssue::Kind::BadValue, "note_style"));
    CHECK(has_issue(report, ParseIssue::Kind::BadValue, "style_intensity"));
    CHECK(has_issue(report, ParseIssue::Kind::BadValue, "right_color"));
    CHECK(has_issue(report, ParseIssue::Kind::BadValue, "sparks"));
    CHECK(has_issue(report, ParseIssue::Kind::BadValue, "visible_seconds"));
    CHECK(has_issue(report, ParseIssue::Kind::BadValue, "pitch_color_3"));
    CHECK(has_issue(report, ParseIssue::Kind::BadValue, "color_scheme"));
    CHECK(report.applied == 0);
}

TEST_CASE("numbers outside their range are clamped and reported") {
    ParseReport report;
    const LookSettings s =
        parse_look("visible_seconds = 500\nkeyboard_height = -2\nstyle_intensity = 0.5\n", &report);
    CHECK(s.visible_seconds == kMaxVisibleSeconds);
    CHECK(s.keyboard_height == kMinKeyboardHeight);
    CHECK(s.style_intensity == doctest::Approx(0.5f));
    CHECK(s.is_valid());
    CHECK(has_issue(report, ParseIssue::Kind::OutOfRange, "visible_seconds"));
    CHECK(has_issue(report, ParseIssue::Kind::OutOfRange, "keyboard_height"));
    CHECK(report.applied == 3);
}

TEST_CASE("nan and inf are bad values, not numbers") {
    ParseReport report;
    const LookSettings s = parse_look("style_intensity = nan\nvisible_seconds = inf\n", &report);
    CHECK(s == LookSettings{});
    CHECK(has_issue(report, ParseIssue::Kind::BadValue, "style_intensity"));
    CHECK(has_issue(report, ParseIssue::Kind::BadValue, "visible_seconds"));
}

TEST_CASE("lines without key = value are reported, comments and blanks are not") {
    ParseReport report;
    parse_look("# my look\n\n   \nversion = 1\nthis is nonsense\n  = 5\n", &report);
    REQUIRE(report.issues.size() == 2);
    CHECK(report.issues[0].kind == ParseIssue::Kind::BadLine);
    CHECK(report.issues[0].line == 5);
    CHECK(report.issues[1].kind == ParseIssue::Kind::BadLine);
    CHECK(report.issues[1].line == 6);
}

TEST_CASE("whitespace, CRLF line ends and a missing final newline are fine") {
    ParseReport report;
    const LookSettings s =
        parse_look("  note_style=filled  \r\n\tsparks =\tfalse\r\nright_color = #010203", &report);
    CHECK(s.note_style == NoteStyle::Filled);
    CHECK_FALSE(s.sparks);
    CHECK(s.right_color == RGBA{1, 2, 3, 255});
    CHECK(report.clean());
}

TEST_CASE("a later line for the same key wins") {
    CHECK(parse_look("note_style = ghost\nnote_style = shiny\n").note_style == NoteStyle::Shiny);
}

TEST_CASE("a newer format version is reported but still read") {
    ParseReport report;
    const LookSettings s =
        parse_look("version = 99\nnote_style = ghost\nfuture_key = 1\n", &report);
    CHECK(s.note_style == NoteStyle::Ghost);
    CHECK(report.version == 99);
    CHECK(has_issue(report, ParseIssue::Kind::NewerVersion, "version"));
    CHECK(has_issue(report, ParseIssue::Kind::UnknownKey, "future_key"));
}

TEST_CASE("a bad version line is a bad value") {
    ParseReport report;
    parse_look("version = two\n", &report);
    CHECK(has_issue(report, ParseIssue::Kind::BadValue, "version"));
    CHECK(report.version == 0);
}

TEST_CASE("parsing never throws and always returns valid settings, whatever the input") {
    std::mt19937 rng(12345);
    const std::string base = serialize(everything_changed());
    for (int round = 0; round < 300; ++round) {
        std::string text = base;
        const int edits = 1 + static_cast<int>(rng() % 20);
        for (int e = 0; e < edits; ++e) {
            const std::size_t pos = rng() % (text.size() + 1);
            switch (rng() % 3) {
            case 0:
                text.insert(pos, 1, static_cast<char>(rng() % 256));
                break;
            case 1:
                if (pos < text.size()) {
                    text.erase(pos, 1 + rng() % 8);
                }
                break;
            default:
                if (pos < text.size()) {
                    text[pos] = static_cast<char>(rng() % 256);
                }
                break;
            }
        }
        ParseReport report;
        LookSettings parsed;
        CHECK_NOTHROW(parsed = parse_look(text, &report));
        CHECK(parsed.is_valid());
    }
    std::string noise(4096, '\0');
    for (char& c : noise) {
        c = static_cast<char>(rng() % 256);
    }
    CHECK(parse_look(noise).is_valid());
    CHECK(parse_look(std::string(100000, '=')).is_valid());
}

TEST_CASE("key size defaults to fit keyboard and round trips as text") {
    CHECK(LookSettings{}.key_size == KeySize::FitKeyboard);
    CHECK_FALSE(LookSettings{}.show_typing_keys);
    for (KeySize size : {KeySize::FitKeyboard, KeySize::FitSong, KeySize::Small, KeySize::Medium,
                         KeySize::Large, KeySize::ExtraLarge}) {
        LookSettings s;
        s.key_size = size;
        ParseReport report;
        CHECK(parse_look(serialize(s), &report).key_size == size);
        CHECK(report.clean());
    }
    LookSettings s;
    s.key_size = KeySize::ExtraLarge;
    CHECK(serialize(s).find("key_size = extra_large\n") != std::string::npos);
}

TEST_CASE("a file without key_size keeps fit keyboard, a bad value is reported") {
    CHECK(parse_look("style_intensity = 0.5\n").key_size == KeySize::FitKeyboard);
    ParseReport report;
    const LookSettings bad = parse_look("key_size = huge\n", &report);
    CHECK(bad.key_size == KeySize::FitKeyboard);
    CHECK(has_issue(report, ParseIssue::Kind::BadValue, "key_size"));
}

TEST_CASE("a stray key size value is brought back to the default") {
    LookSettings s;
    s.key_size = static_cast<KeySize>(9);
    CHECK_FALSE(s.is_valid());
    CHECK(s.clamped().key_size == KeySize::FitKeyboard);
}

TEST_CASE("grade words default to on, at the bottom, fully opaque") {
    const LookSettings d;
    CHECK(d.grade_words);
    CHECK(d.grade_position == GradePosition::Bottom);
    CHECK(d.grade_opacity == 100);
    CHECK(d.grade_words_shown());
    CHECK(d.grade_alpha() == doctest::Approx(1.0f));
    // A file from before the grade options keeps the defaults.
    const LookSettings old = parse_look("style_intensity = 0.5\n");
    CHECK(old.grade_words);
    CHECK(old.grade_position == GradePosition::Bottom);
    CHECK(old.grade_opacity == 100);
}

TEST_CASE("grade words are written under their names and round trip") {
    LookSettings s;
    s.grade_words = false;
    s.grade_position = GradePosition::Top;
    s.grade_opacity = 40;
    const std::string text = serialize(s);
    CHECK(text.find("grade_words = false\n") != std::string::npos);
    CHECK(text.find("grade_position = top\n") != std::string::npos);
    CHECK(text.find("grade_opacity = 40\n") != std::string::npos);
    ParseReport report;
    CHECK(parse_look(text, &report) == s);
    CHECK(report.clean());

    UserPresets presets;
    presets.save_slot(1, "Words", s);
    LookDocument doc{LookSettings{}, presets};
    CHECK(parse_look_document(serialize_document(doc)).user_presets.slots[1].look == s);
}

TEST_CASE("grade opacity outside 0..100 is clamped and reported") {
    ParseReport report;
    CHECK(parse_look("grade_opacity = 250\n", &report).grade_opacity == 100);
    CHECK(has_issue(report, ParseIssue::Kind::OutOfRange, "grade_opacity"));
    CHECK(parse_look("grade_opacity = -5\n").grade_opacity == 0);
    CHECK(parse_look("grade_opacity = 99999999999999999999\n").grade_opacity == 100);
    CHECK(parse_look("grade_opacity = -99999999999999999999\n").grade_opacity == 0);
    LookSettings s;
    s.grade_opacity = 400;
    CHECK_FALSE(s.is_valid());
    CHECK(s.clamped().grade_opacity == 100);
    s.grade_opacity = -1;
    CHECK(s.clamped().grade_opacity == 0);
}

TEST_CASE("unknown grade values are reported and keep the defaults") {
    ParseReport report;
    const LookSettings s =
        parse_look("grade_words = maybe\ngrade_position = middle\ngrade_opacity = lots\n", &report);
    CHECK(s.grade_words);
    CHECK(s.grade_position == GradePosition::Bottom);
    CHECK(s.grade_opacity == 100);
    CHECK(has_issue(report, ParseIssue::Kind::BadValue, "grade_words"));
    CHECK(has_issue(report, ParseIssue::Kind::BadValue, "grade_position"));
    CHECK(has_issue(report, ParseIssue::Kind::BadValue, "grade_opacity"));
    CHECK(parse_look("grade_opacity = 12.5\n").grade_opacity == 100); // whole percent only
    LookSettings stray;
    stray.grade_position = static_cast<GradePosition>(7);
    CHECK_FALSE(stray.is_valid());
    CHECK(stray.clamped().grade_position == GradePosition::Bottom);
}

TEST_CASE("the grade switch and the opacity slider stay consistent") {
    LookSettings s;
    s.grade_opacity = 40;
    // Switch off: hidden, the strength is kept, so on brings it back.
    set_grade_words(s, false);
    CHECK_FALSE(s.grade_words_shown());
    CHECK(s.grade_alpha() == 0.0f);
    CHECK(s.grade_opacity == 40);
    set_grade_words(s, true);
    CHECK(s.grade_words_shown());
    CHECK(s.grade_opacity == 40);
    // Slider to 0: off. The remembered strength goes back as the stored one.
    set_grade_opacity(s, 0, 40);
    CHECK_FALSE(s.grade_words);
    CHECK_FALSE(s.grade_words_shown());
    CHECK(s.grade_opacity == 40);
    // Slider up from 0 switches on at that value.
    set_grade_opacity(s, 65, 40);
    CHECK(s.grade_words);
    CHECK(s.grade_opacity == 65);
    CHECK(s.grade_alpha() == doctest::Approx(0.65f));
    // Opacity 0 with the switch on shows nothing, and the switch restores the last strength.
    s.grade_words = true;
    s.grade_opacity = 0;
    CHECK_FALSE(s.grade_words_shown());
    set_grade_words(s, true, 30);
    CHECK(s.grade_words_shown());
    CHECK(s.grade_opacity == 30);
    // Nothing known to restore: full strength; out-of-range input is clamped.
    s.grade_opacity = 0;
    set_grade_words(s, true);
    CHECK(s.grade_opacity == 100);
    set_grade_opacity(s, 500);
    CHECK(s.grade_opacity == 100);
    set_grade_opacity(s, -3, 0);
    CHECK_FALSE(s.grade_words);
    CHECK(s.grade_opacity == 1);
}

TEST_CASE("built-in presets set grade words that are valid and shown") {
    for (PresetId id : kAllPresets) {
        const LookSettings p = preset(id);
        CHECK(p.is_valid());
        CHECK(p.grade_words_shown());
        CHECK(p.grade_opacity >= 40);
    }
    CHECK(preset(PresetId::Default).grade_opacity == 100);
    // Applying a built-in look keeps the player's grade-word choices.
    LookSettings mine;
    mine.grade_words = false;
    mine.grade_position = GradePosition::Top;
    mine.grade_opacity = 35;
    const LookSettings applied = apply_preset(mine, PresetId::MinimalReduced);
    CHECK_FALSE(applied.grade_words);
    CHECK(applied.grade_position == GradePosition::Top);
    CHECK(applied.grade_opacity == 35);
    CHECK(is_preset(applied, PresetId::MinimalReduced));
}
