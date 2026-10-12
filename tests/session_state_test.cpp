// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/session_state.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

using namespace toonval;
using Kind = SettingsIssueKind;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();

bool has_issue(const std::vector<SettingsIssue>& issues, Kind kind, const std::string& key = "") {
    for (const SettingsIssue& i : issues) {
        if (i.kind == kind && (key.empty() || i.key == key)) {
            return true;
        }
    }
    return false;
}

SessionState read(const std::string& text, std::vector<SettingsIssue>* issues = nullptr) {
    return read_session(SettingsDocument::parse(text), issues);
}

AppSettings read_app(const std::string& text, std::vector<SettingsIssue>* issues = nullptr) {
    return read_app_settings(SettingsDocument::parse(text), issues);
}

SessionState busy_session() {
    SessionState s;
    s.song_path = "Songs/F\xC3\xBCr Elise #2.mid";
    s.song_hash = 0xdeadbeefULL;
    s.position_s = 12.5;
    s.speed_percent = 75;
    s.practice_hands = HandFilter::Right;
    s.piano_plays = PianoPlays::OtherHand;
    s.loop_start_s = 8.0;
    s.loop_end_s = 16.25;
    s.loop_enabled = true;
    s.auto_speed_enabled = true;
    s.count_in_beats = 4;
    s.metronome = true;
    s.mode = PracticeMode::Wait;
    s.transpose_semitones = -2;
    s.section_id = 3;
    s.plan_item = 2;
    s.keyboard = KeyboardChoice::Profile;
    s.keyboard_name = "PSR-E383";
    return s;
}

} // namespace

TEST_CASE("the defaults are what the documentation says") {
    SessionState s;
    CHECK(s.song_path.empty());
    CHECK(s.song_hash == 0);
    CHECK(s.position_s == 0.0);
    CHECK(s.speed_percent == 100);
    CHECK(s.practice_hands == HandFilter::Both);
    CHECK(s.piano_plays == PianoPlays::Nothing); // song sound is Mute by default
    CHECK_FALSE(s.has_loop());
    CHECK_FALSE(s.loop_enabled);
    CHECK_FALSE(s.auto_speed_enabled);
    CHECK(s.auto_speed_start_percent == 60);
    CHECK(s.auto_speed_step_percent == 5);
    CHECK(s.auto_speed_max_percent == 100);
    CHECK(s.count_in_beats == 0);
    CHECK_FALSE(s.metronome);
    CHECK(s.mode == PracticeMode::FreePlay);
    CHECK(s.transpose_semitones == 0);
    CHECK(s.section_id == 0);
    CHECK(s.plan_item == 0);
    CHECK(s.keyboard == KeyboardChoice::Auto);
    CHECK(s.keyboard_range().lowest == 36);
    CHECK(s.keyboard_range().highest == 96);
    CHECK(s.clamped() == s);

    AppSettings a;
    CHECK(a.hit_window_ms == 80);
    CHECK(a.latency_correction_ms == 0);
    CHECK(a.midi_in_port.empty());
    CHECK(a.midi_out_port.empty());
    CHECK(a.daily_goal_minutes == 15);
    CHECK(a.resume_countdown_s == 3);
    CHECK(a.skip_back_seconds == 5);
    CHECK(a.skip_forward_seconds == 5);
    CHECK(a.clamped() == a);
}

TEST_CASE("clamped() holds each number to its documented range") {
    SessionState s;
    s.song_path = "a.mid";
    s.position_s = -3.0;
    s.speed_percent = 5;
    s.count_in_beats = 99;
    s.transpose_semitones = 40;
    s.auto_speed_start_percent = 1000;
    s.auto_speed_step_percent = 0;
    SessionState c = s.clamped();
    CHECK(c.position_s == 0.0);
    CHECK(c.speed_percent == 25);
    CHECK(c.count_in_beats == 16);
    CHECK(c.transpose_semitones == 12);
    CHECK(c.auto_speed_start_percent == 150);
    CHECK(c.auto_speed_step_percent == 5);

    s.position_s = 1e12;
    s.speed_percent = 500;
    s.count_in_beats = -1;
    s.transpose_semitones = -13;
    s.auto_speed_step_percent = 90;
    c = s.clamped();
    CHECK(c.position_s == kMaxSessionSeconds);
    CHECK(c.speed_percent == 150);
    CHECK(c.count_in_beats == 0);
    CHECK(c.transpose_semitones == -12);
    CHECK(c.auto_speed_step_percent == 25);
    CHECK(c.clamped() == c);
}

TEST_CASE("clamped() holds the section and the plan item to their ranges") {
    SessionState s;
    s.song_path = "a.mid";
    s.section_id = 7;
    s.plan_item = 3;
    CHECK(s.clamped() == s);
    s.section_id = -4;
    s.plan_item = -1;
    SessionState c = s.clamped();
    CHECK(c.section_id == 0);
    CHECK(c.plan_item == 0);
    s.section_id = 2'100'000'000;
    s.plan_item = 1000;
    c = s.clamped();
    CHECK(c.section_id == kMaxSessionSectionId);
    CHECK(c.plan_item == kMaxSessionPlanItems);
    CHECK(c.clamped() == c);
}

TEST_CASE("clamped() without a song forgets the section and the plan item") {
    SessionState s;
    s.section_id = 7;
    s.plan_item = 3;
    SessionState c = s.clamped();
    CHECK(c.section_id == 0);
    CHECK(c.plan_item == 0);
}

TEST_CASE("the section and the plan item are read from and written to the session") {
    SessionState s = read("[session]\nsong = a.mid\nsection_id = 12\nplan_item = 2\n");
    CHECK(s.section_id == 12);
    CHECK(s.plan_item == 2);
    std::vector<SettingsIssue> issues;
    s = read("[session]\nsong = a.mid\nsection_id = x\nplan_item = 99\n", &issues);
    CHECK(s.section_id == 0);
    CHECK(s.plan_item == kMaxSessionPlanItems);
    CHECK(has_issue(issues, Kind::BadValue, "section_id"));
    CHECK(has_issue(issues, Kind::OutOfRange, "plan_item"));
}

TEST_CASE("clamped() snaps speeds to steps of five like the transport does") {
    SessionState s;
    s.speed_percent = 62;
    s.auto_speed_start_percent = 58;
    s.auto_speed_max_percent = 143;
    s.auto_speed_step_percent = 7;
    SessionState c = s.clamped();
    CHECK(c.speed_percent == snap_speed_percent(62));
    CHECK(c.speed_percent == 60);
    CHECK(c.auto_speed_start_percent == 60);
    CHECK(c.auto_speed_max_percent == 145);
    CHECK(c.auto_speed_step_percent == 5);
    s.speed_percent = 63;
    CHECK(s.clamped().speed_percent == 65);
}

TEST_CASE("clamped() turns NaN and infinity into valid times") {
    SessionState s;
    s.song_path = "a.mid";
    s.position_s = kNaN;
    s.loop_start_s = kNaN;
    s.loop_end_s = kNaN;
    s.loop_enabled = true;
    SessionState c = s.clamped();
    CHECK(c.position_s == 0.0);
    CHECK_FALSE(c.has_loop());
    CHECK_FALSE(c.loop_enabled);
    s.position_s = kInf;
    CHECK(s.clamped().position_s == kMaxSessionSeconds);
    s.position_s = -kInf;
    CHECK(s.clamped().position_s == 0.0);
}

TEST_CASE(
    "clamped() orders the loop, drops one that is too short, and switches off a missing one") {
    SessionState s;
    s.song_path = "a.mid";
    s.loop_start_s = 20.0;
    s.loop_end_s = 10.0;
    s.loop_enabled = true;
    SessionState c = s.clamped();
    CHECK(c.loop_start_s == 10.0);
    CHECK(c.loop_end_s == 20.0);
    CHECK(c.loop_enabled);

    s.loop_start_s = 10.0;
    s.loop_end_s = 10.05; // under kMinLoopSeconds
    c = s.clamped();
    CHECK(c.loop_start_s == 0.0);
    CHECK(c.loop_end_s == 0.0);
    CHECK_FALSE(c.loop_enabled);

    s.loop_start_s = 10.0;
    s.loop_end_s = 10.5;
    c = s.clamped();
    CHECK(c.has_loop());

    s.loop_start_s = 0.0;
    s.loop_end_s = 0.0;
    s.loop_enabled = true;
    CHECK_FALSE(s.clamped().loop_enabled);

    s.loop_start_s = 5.0;
    s.loop_end_s = 9.0;
    s.loop_enabled = false;
    c = s.clamped();
    CHECK(c.has_loop()); // a loop that is set but switched off stays
    CHECK_FALSE(c.loop_enabled);
}

TEST_CASE("clamped() keeps the auto speed-up maximum at or above its start") {
    SessionState s;
    s.auto_speed_start_percent = 90;
    s.auto_speed_max_percent = 70;
    SessionState c = s.clamped();
    CHECK(c.auto_speed_start_percent == 90);
    CHECK(c.auto_speed_max_percent == 90);
    AutoSpeedUp policy = c.auto_speed_up();
    CHECK(policy.start_percent == 90);
    CHECK(policy.max_percent == 90);
}

TEST_CASE("clamped() accepts only library-relative song paths") {
    auto path = [](std::string p) {
        SessionState s;
        s.song_path = std::move(p);
        s.song_hash = 5;
        return s.clamped();
    };
    CHECK(path("Songs/a.mid").song_path == "Songs/a.mid");
    CHECK(path("a b/c d.mid").song_path == "a b/c d.mid");
    CHECK(path("a..b/c.mid").song_path == "a..b/c.mid"); // dots inside a name are fine
    CHECK(path("\xF0\x9F\x8E\xB9.mid").song_path == "\xF0\x9F\x8E\xB9.mid"); // 4-byte character
    CHECK(path("Songs/F\xC3\xBCr Elise.mid").song_path == "Songs/F\xC3\xBCr Elise.mid");
    for (const char* bad : {"/etc/passwd", "../x.mid", "a/../../x.mid", "a/..", "..", "C:/x.mid",
                            "c:\\x.mid", "a\nb.mid",
                            "a\x01"
                            "b.mid",
                            "\x7f", "\xFF\xFE.mid", "\xC3", "a\\..\\b.mid", "\xC0\x80",
                            "\xED\xA0\x80", "\xF4\x90\x80\x80"}) {
        CAPTURE(bad);
        SessionState c = path(bad);
        CHECK(c.song_path.empty());
        CHECK(c.song_hash == 0); // the hash went with the path
    }
    CHECK(path(std::string(kMaxSongPathBytes, 'a')).song_path.size() == kMaxSongPathBytes);
    CHECK(path(std::string(kMaxSongPathBytes + 1, 'a')).song_path.empty());
}

TEST_CASE("clamped() without a song clears the song-bound position and loop") {
    SessionState s;
    s.position_s = 40.0;
    s.loop_start_s = 1.0;
    s.loop_end_s = 5.0;
    s.loop_enabled = true;
    s.song_hash = 77;
    SessionState c = s.clamped();
    CHECK(c.song_hash == 0);
    CHECK(c.position_s == 0.0);
    CHECK_FALSE(c.has_loop());
    CHECK_FALSE(c.loop_enabled);
}

TEST_CASE("clamped() repairs the manual keyboard range or falls back to Auto") {
    SessionState s;
    s.keyboard = KeyboardChoice::Manual;
    s.keyboard_lowest = 36;
    s.keyboard_highest = 96;
    CHECK(s.clamped() == s);

    s.keyboard_lowest = 37;  // C#: widens down to C
    s.keyboard_highest = 97; // C#: widens up to D
    SessionState c = s.clamped();
    CHECK(c.keyboard == KeyboardChoice::Manual);
    CHECK(c.keyboard_lowest == 36);
    CHECK(c.keyboard_highest == 98);

    s.keyboard_lowest = 96;
    s.keyboard_highest = 36; // reversed
    c = s.clamped();
    CHECK(c.keyboard_lowest == 36);
    CHECK(c.keyboard_highest == 96);

    s.keyboard_lowest = -5;
    s.keyboard_highest = 400;
    c = s.clamped();
    CHECK(c.keyboard_lowest == 0);
    CHECK(c.keyboard_highest == 127);
    CHECK(KeyRange{c.keyboard_lowest, c.keyboard_highest}.valid());

    s.keyboard_lowest = 60;
    s.keyboard_highest = 64; // five keys: not a keyboard
    c = s.clamped();
    CHECK(c.keyboard == KeyboardChoice::Auto);
    CHECK(c.keyboard_lowest == 36);
    CHECK(c.keyboard_highest == 96);
}

TEST_CASE("clamped() makes a Profile choice without a name Auto, and cleans the names") {
    SessionState s;
    s.keyboard = KeyboardChoice::Profile;
    CHECK(s.clamped().keyboard == KeyboardChoice::Auto);
    s.keyboard_name = "Some Future Board";
    CHECK(s.clamped().keyboard == KeyboardChoice::Profile); // this build need not know the name
    s.keyboard_name = std::string(kMaxNameBytes + 1, 'k');
    CHECK(s.clamped().keyboard_name.empty());
    s.keyboard_name = "bad\xFF";
    CHECK(s.clamped().keyboard_name.empty());

    SessionState bad_enum;
    bad_enum.practice_hands = static_cast<HandFilter>(77);
    bad_enum.piano_plays = static_cast<PianoPlays>(77);
    bad_enum.mode = static_cast<PracticeMode>(77);
    bad_enum.keyboard = static_cast<KeyboardChoice>(77);
    SessionState c = bad_enum.clamped();
    CHECK(c.practice_hands == HandFilter::Both);
    CHECK(c.piano_plays == PianoPlays::Nothing);
    CHECK(c.mode == PracticeMode::FreePlay);
    CHECK(c.keyboard == KeyboardChoice::Auto);
}

TEST_CASE("AppSettings::clamped() holds each field to its range") {
    AppSettings a;
    a.hit_window_ms = 0;
    a.latency_correction_ms = -9000;
    a.daily_goal_minutes = 5000;
    a.resume_countdown_s = 0;
    a.skip_back_seconds = 0;
    a.skip_forward_seconds = -7;
    a.midi_in_port = std::string(kMaxNameBytes + 1, 'p');
    a.midi_out_port = "ok port";
    AppSettings c = a.clamped();
    CHECK(c.hit_window_ms == 20);
    CHECK(c.latency_correction_ms == -250);
    CHECK(c.daily_goal_minutes == 720);
    CHECK(c.resume_countdown_s == 1);
    CHECK(c.skip_back_seconds == 1);
    CHECK(c.skip_forward_seconds == 1);
    CHECK(c.midi_in_port.empty());
    CHECK(c.midi_out_port == "ok port");
    a.hit_window_ms = 9999;
    a.latency_correction_ms = 9000;
    a.daily_goal_minutes = -1;
    a.resume_countdown_s = 60;
    a.skip_back_seconds = 31;
    a.skip_forward_seconds = 900;
    c = a.clamped();
    CHECK(c.hit_window_ms == 250);
    CHECK(c.latency_correction_ms == 250);
    CHECK(c.daily_goal_minutes == 0);
    CHECK(c.resume_countdown_s == 6);
    CHECK(c.skip_back_seconds == 30);
    CHECK(c.skip_forward_seconds == 30);
}

TEST_CASE("skip back and skip forward seconds round-trip separately and clamp to 1..30") {
    for (int back = 1; back <= 30; back += 7) {
        AppSettings a;
        a.skip_back_seconds = back;
        a.skip_forward_seconds = 31 - back;
        SettingsDocument doc = SettingsDocument::parse("");
        write_app_settings(doc, a);
        const AppSettings b = read_app_settings(SettingsDocument::parse(doc.serialize()));
        CHECK(b.skip_back_seconds == back);
        CHECK(b.skip_forward_seconds == 31 - back);
    }
    const AppSettings c = read_app_settings(SettingsDocument::parse(
        "[app]\nskip_back_seconds = 0\nskip_forward_seconds = 45\n"));
    CHECK(c.skip_back_seconds == 1);
    CHECK(c.skip_forward_seconds == 30);
    const AppSettings d = read_app_settings(
        SettingsDocument::parse("[app]\nskip_back_seconds = far\nskip_forward_seconds = 2.5\n"));
    CHECK(d.skip_back_seconds == 5);
    CHECK(d.skip_forward_seconds == 5);
}

TEST_CASE("the resume countdown round-trips through the file and falls back when unreadable") {
    for (int seconds = 1; seconds <= 6; ++seconds) {
        AppSettings a;
        a.resume_countdown_s = seconds;
        SettingsDocument doc = SettingsDocument::parse("");
        write_app_settings(doc, a);
        CHECK(read_app_settings(SettingsDocument::parse(doc.serialize())).resume_countdown_s ==
              seconds);
    }
    std::vector<SettingsIssue> issues;
    CHECK(read_app_settings(SettingsDocument::parse("[app]\nresume_countdown_s = soon\n"), &issues)
              .resume_countdown_s == 3);
    CHECK_FALSE(issues.empty());
    CHECK(read_app_settings(SettingsDocument::parse("[app]\nresume_countdown_s = 9\n"))
              .resume_countdown_s == 6);
}

TEST_CASE("the resume countdown, skip steps and bar style have their defaults and ranges") {
    const AppSettings d;
    CHECK(d.resume_countdown_s == 3);
    CHECK(d.skip_back_seconds == 5);
    CHECK(d.skip_forward_seconds == 5);
    CHECK(d.bar_style == BarStyle::Strip);
    AppSettings a;
    a.resume_countdown_s = 0;
    a.skip_back_seconds = 0;
    a.skip_forward_seconds = -4;
    a.bar_style = static_cast<BarStyle>(9);
    AppSettings c = a.clamped();
    CHECK(c.resume_countdown_s == 1);
    CHECK(c.skip_back_seconds == 1);
    CHECK(c.skip_forward_seconds == 1);
    CHECK(c.bar_style == BarStyle::Strip);
    a.resume_countdown_s = 7;
    a.skip_back_seconds = 31;
    a.skip_forward_seconds = 300;
    c = a.clamped();
    CHECK(c.resume_countdown_s == 6);
    CHECK(c.skip_back_seconds == 30);
    CHECK(c.skip_forward_seconds == 30);
}

TEST_CASE("the skip steps and bar style are saved under their own keys") {
    AppSettings a;
    a.skip_back_seconds = 1;
    a.skip_forward_seconds = 30;
    a.resume_countdown_s = 6;
    a.bar_style = BarStyle::Dock;
    SettingsDocument doc = SettingsDocument::parse("");
    write_app_settings(doc, a);
    const std::string text = doc.serialize();
    CHECK(text.find("skip_back_seconds = 1\n") != std::string::npos);
    CHECK(text.find("skip_forward_seconds = 30\n") != std::string::npos);
    CHECK(text.find("resume_countdown_s = 6\n") != std::string::npos);
    CHECK(text.find("bar_style = dock\n") != std::string::npos);
    std::vector<SettingsIssue> issues;
    CHECK(read_app_settings(SettingsDocument::parse(text), &issues) == a);
    CHECK(issues.empty());
    a.bar_style = BarStyle::Groups;
    write_app_settings(doc, a);
    CHECK(read_app_settings(SettingsDocument::parse(doc.serialize())).bar_style ==
          BarStyle::Groups);
}

TEST_CASE("an unknown bar style in the file keeps the strip and reports the line") {
    std::vector<SettingsIssue> issues;
    AppSettings got = read_app_settings(
        SettingsDocument::parse("[app]\nbar_style = floating\nskip_back_seconds = 9\n"), &issues);
    CHECK(got.bar_style == BarStyle::Strip);
    CHECK(got.skip_back_seconds == 9);
    CHECK_FALSE(issues.empty());
}

TEST_CASE("a default state writes this exact document") {
    SettingsDocument doc = SettingsDocument::parse("");
    doc.ensure_format();
    write_app_settings(doc, AppSettings{});
    write_session(doc, SessionState{});
    CHECK(doc.serialize() == "# toonval settings\n"
                             "format = 1.0\n"
                             "\n"
                             "[app]\n"
                             "hit_window_ms = 80\n"
                             "latency_correction_ms = 0\n"
                             "midi_in_port =\n"
                             "midi_out_port =\n"
                             "daily_goal_minutes = 15\n"
                             "resume_countdown_s = 3\n"
                             "skip_back_seconds = 5\n"
                             "skip_forward_seconds = 5\n"
                             "bar_style = strip\n"
                             "\n"
                             "[session]\n"
                             "song =\n"
                             "song_hash = 0000000000000000\n"
                             "position_s = 0\n"
                             "speed_percent = 100\n"
                             "hands = both\n"
                             "piano_plays = nothing\n"
                             "loop_start_s = 0\n"
                             "loop_end_s = 0\n"
                             "loop_enabled = false\n"
                             "auto_speed_enabled = false\n"
                             "auto_speed_start_percent = 60\n"
                             "auto_speed_step_percent = 5\n"
                             "auto_speed_max_percent = 100\n"
                             "count_in_beats = 0\n"
                             "metronome = false\n"
                             "mode = free_play\n"
                             "transpose_semitones = 0\n"
                             "section_id = 0\n"
                             "plan_item = 0\n"
                             "keyboard = auto\n"
                             "keyboard_name =\n"
                             "keyboard_lowest = 36\n"
                             "keyboard_highest = 96\n");
}

TEST_CASE("a busy state writes this exact session section") {
    SettingsDocument doc = SettingsDocument::parse("");
    write_session(doc, busy_session());
    CHECK(doc.serialize() == "[session]\n"
                             "song = Songs/F\xC3\xBCr Elise #2.mid\n"
                             "song_hash = 00000000deadbeef\n"
                             "position_s = 12.5\n"
                             "speed_percent = 75\n"
                             "hands = right\n"
                             "piano_plays = other_hand\n"
                             "loop_start_s = 8\n"
                             "loop_end_s = 16.25\n"
                             "loop_enabled = true\n"
                             "auto_speed_enabled = true\n"
                             "auto_speed_start_percent = 60\n"
                             "auto_speed_step_percent = 5\n"
                             "auto_speed_max_percent = 100\n"
                             "count_in_beats = 4\n"
                             "metronome = true\n"
                             "mode = wait\n"
                             "transpose_semitones = -2\n"
                             "section_id = 3\n"
                             "plan_item = 2\n"
                             "keyboard = profile\n"
                             "keyboard_name = PSR-E383\n"
                             "keyboard_lowest = 36\n"
                             "keyboard_highest = 96\n");
}

TEST_CASE("every field survives write then read") {
    SessionState s = busy_session();
    s.song_hash = 0xffffffffffffffffULL;
    s.piano_plays = PianoPlays::Nothing;
    s.keyboard = KeyboardChoice::Manual;
    s.keyboard_name = "  padded \"name\"  ";
    s.keyboard_lowest = 28;
    s.keyboard_highest = 103;
    s.position_s = 0.1 + 0.2; // not exactly representable in decimal
    s.loop_start_s = 1.0 / 3.0;
    s.loop_end_s = 100.0 / 7.0;
    s.auto_speed_start_percent = 40;
    s.auto_speed_step_percent = 10;
    s.auto_speed_max_percent = 120;
    s = s.clamped();
    REQUIRE(s.piano_plays == PianoPlays::Nothing);

    SettingsDocument doc = SettingsDocument::parse("");
    write_session(doc, s);
    std::vector<SettingsIssue> issues;
    SessionState back = read_session(SettingsDocument::parse(doc.serialize()), &issues);
    CHECK(issues.empty());
    CHECK(back == s);

    for (HandFilter h : {HandFilter::Both, HandFilter::Left, HandFilter::Right}) {
        for (PianoPlays p : {PianoPlays::OtherHand, PianoPlays::Left, PianoPlays::Right,
                             PianoPlays::Both, PianoPlays::Nothing}) {
            for (PracticeMode m :
                 {PracticeMode::FreePlay, PracticeMode::Wait, PracticeMode::Flow}) {
                SessionState e = s;
                e.practice_hands = h;
                e.piano_plays = p;
                e.mode = m;
                SettingsDocument d = SettingsDocument::parse("");
                write_session(d, e);
                CHECK(read_session(SettingsDocument::parse(d.serialize())) == e);
            }
        }
    }
}

TEST_CASE("app settings survive write then read") {
    AppSettings a;
    a.hit_window_ms = 120;
    a.latency_correction_ms = -35;
    a.midi_in_port = "PSR-E383 MIDI 1";
    a.midi_out_port = " Port with \"quotes\" # and = signs ";
    a.daily_goal_minutes = 0;
    a.resume_countdown_s = 2;
    a.skip_back_seconds = 12;
    a.skip_forward_seconds = 7;
    a.bar_style = BarStyle::Groups;
    SettingsDocument doc = SettingsDocument::parse("");
    write_app_settings(doc, a);
    std::vector<SettingsIssue> issues;
    CHECK(read_app_settings(SettingsDocument::parse(doc.serialize()), &issues) == a);
    CHECK(issues.empty());
}

TEST_CASE("missing sections and keys keep the defaults without any issue") {
    std::vector<SettingsIssue> issues;
    CHECK(read("", &issues) == SessionState{});
    CHECK(read_app("format = 1.0\n", &issues) == AppSettings{});
    CHECK(read("[session]\nspeed_percent = 80\n", &issues).speed_percent == 80);
    CHECK(read("[session]\nspeed_percent = 80\n", &issues).mode == PracticeMode::FreePlay);
    CHECK(issues.empty());
}

TEST_CASE("a value that does not parse keeps the default and is reported") {
    std::vector<SettingsIssue> issues;
    SessionState s = read("[session]\n"
                          "speed_percent = fast\n"
                          "position_s = 1.5x\n"
                          "loop_enabled = maybe\n"
                          "hands = both hands\n"
                          "song_hash = xyz\n"
                          "count_in_beats = 99999999999999999999\n"
                          "song = \"unterminated\n"
                          "position_s = nan\n"
                          "transpose_semitones = 3\n",
                          &issues);
    CHECK(s.speed_percent == 100);
    CHECK(s.position_s == 0.0);
    CHECK_FALSE(s.loop_enabled);
    CHECK(s.practice_hands == HandFilter::Both);
    CHECK(s.song_hash == 0);
    CHECK(s.count_in_beats == 0);
    CHECK(s.song_path.empty());
    CHECK(s.transpose_semitones == 3); // good lines still count
    for (const char* key : {"speed_percent", "position_s", "loop_enabled", "hands", "song_hash",
                            "count_in_beats", "song"}) {
        CAPTURE(key);
        CHECK(has_issue(issues, Kind::BadValue, key));
    }
}

TEST_CASE("a value outside its range is clamped and reported") {
    std::vector<SettingsIssue> issues;
    SessionState s =
        read("[session]\ntranspose_semitones = 30\nspeed_percent = 7\nposition_s = -4\n", &issues);
    CHECK(s.transpose_semitones == 12);
    CHECK(s.speed_percent == 25);
    CHECK(s.position_s == 0.0);
    CHECK(has_issue(issues, Kind::OutOfRange, "transpose_semitones"));
    CHECK(has_issue(issues, Kind::OutOfRange, "speed_percent"));
    CHECK(has_issue(issues, Kind::OutOfRange, "position_s"));

    issues.clear();
    AppSettings a = read_app("[app]\nhit_window_ms = 5000\n", &issues);
    CHECK(a.hit_window_ms == 250);
    CHECK(has_issue(issues, Kind::OutOfRange, "hit_window_ms"));
}

TEST_CASE("an off-step speed is snapped and reported") {
    std::vector<SettingsIssue> issues;
    CHECK(read("[session]\nspeed_percent = 62\n", &issues).speed_percent == 60);
    CHECK(has_issue(issues, Kind::OutOfRange, "speed_percent"));
}

TEST_CASE("fields that clash with each other are fixed and the clash is reported") {
    std::vector<SettingsIssue> issues;
    SessionState s =
        read("[session]\nsong = a.mid\nloop_start_s = 20\nloop_end_s = 10\nloop_enabled = true\n"
             "keyboard = manual\nkeyboard_lowest = 60\nkeyboard_highest = 62\n",
             &issues);
    CHECK(s.loop_start_s == 10.0);
    CHECK(s.loop_end_s == 20.0);
    CHECK(s.keyboard == KeyboardChoice::Auto);
    CHECK(has_issue(issues, Kind::OutOfRange));
}

TEST_CASE("an absolute or climbing song path in a file is refused") {
    std::vector<SettingsIssue> issues;
    SessionState s =
        read("[session]\nsong = ../../etc/hosts\nsong_hash = 00000000000000ff\n", &issues);
    CHECK(s.song_path.empty());
    CHECK(s.song_hash == 0);
    CHECK(has_issue(issues, Kind::OutOfRange, "song"));
}

TEST_CASE("a repeated key reports DuplicateKey and the last one counts") {
    std::vector<SettingsIssue> issues;
    SessionState s = read("[session]\nspeed_percent = 80\nspeed_percent = 90\n", &issues);
    CHECK(s.speed_percent == 90);
    CHECK(has_issue(issues, Kind::DuplicateKey, "speed_percent"));
}

TEST_CASE("unknown keys and malformed lines are reported and survive a save") {
    const std::string text = "format = 1.0\n"
                             "[session]\n"
                             "speed_percent = 80\n"
                             "future_key = something   odd\n"
                             "# a comment\n"
                             "garbage line\n";
    std::vector<SettingsIssue> issues;
    SettingsDocument doc = SettingsDocument::parse(text);
    SessionState s = read_session(doc, &issues);
    CHECK(s.speed_percent == 80);
    CHECK(has_issue(issues, Kind::UnknownKey, "future_key"));
    CHECK(has_issue(issues, Kind::MalformedLine));
    for (const SettingsIssue& i : issues) {
        if (i.kind == Kind::UnknownKey) {
            CHECK(i.section == "session");
            CHECK(i.line == 4);
        }
    }
    s.speed_percent = 85;
    write_session(doc, s);
    std::string out = doc.serialize();
    CHECK(out.find("future_key = something   odd\n") != std::string::npos);
    CHECK(out.find("# a comment\n") != std::string::npos);
    CHECK(out.find("garbage line\n") != std::string::npos);
    CHECK(out.find("speed_percent = 85\n") != std::string::npos);
}

TEST_CASE("reading a document and writing the same values back changes no byte") {
    const std::string text = "# my settings\r\n"
                             "format=1.0\r\n"
                             "\r\n"
                             "[app]\r\n"
                             "hit_window_ms   =   80\r\n"
                             "\r\n"
                             "[session]\r\n"
                             "speed_percent=100\r\n"
                             "song = Songs/a.mid\r\n"
                             "future = 1\r\n";
    LoadedSettings loaded = load_settings(text);
    CHECK(loaded.serialize().find("hit_window_ms   =   80\r\n") != std::string::npos);
    SettingsDocument doc = SettingsDocument::parse(text);
    write_app_settings(doc, read_app_settings(doc));
    write_session(doc, read_session(doc));
    // Only keys that were missing are added; the lines that were there stay as typed.
    std::string out = doc.serialize();
    CHECK(out.rfind("# my settings\r\nformat=1.0\r\n\r\n[app]\r\nhit_window_ms   =   80\r\n", 0) ==
          0);
    CHECK(out.find("speed_percent=100\r\nsong = Songs/a.mid\r\nfuture = 1\r\n") !=
          std::string::npos);
}

TEST_CASE("load_settings reads both sections and reports sections nobody owns") {
    const std::string text = "format = 1.0\n"
                             "[app]\nhit_window_ms = 100\n"
                             "[session]\nspeed_percent = 50\n"
                             "[mystery]\nx = 1\n";
    LoadedSettings loaded = load_settings(text);
    CHECK(loaded.app.hit_window_ms == 100);
    CHECK(loaded.session.speed_percent == 50);
    CHECK_FALSE(loaded.read_only_recommended());
    int unknown = 0;
    for (const SettingsIssue& i : loaded.issues) {
        if (i.kind == Kind::UnknownSection) {
            ++unknown;
            CHECK(i.section == "mystery");
            CHECK(i.line == 6);
        }
    }
    CHECK(unknown == 1);
}

TEST_CASE("sections owned by another reader are not reported, and every unknown one survives") {
    const std::string future =
        "[from_the_future]\n# written by a newer build\nweird   =  1\n\nno equals\n";
    const std::string look = "[look]\nversion = 1\nstyle = neon\n";
    const std::string text =
        "format = 1.0\n" + look + "\n" + future + "\n[session]\nspeed_percent = 50\n";
    const std::array<std::string_view, 2> owned{"look", "user_presets"};
    LoadedSettings loaded = load_settings(text, owned);
    for (const SettingsIssue& i : loaded.issues) {
        CHECK(i.section != "look");
    }
    CHECK(loaded.document.section_text("look") == "version = 1\nstyle = neon\n");

    loaded.session.speed_percent = 70;
    loaded.app.hit_window_ms = 90;
    loaded.document.set_section_text("look", "version = 1\nstyle = glass\n");
    std::string out = loaded.serialize();
    CHECK(out.find(future) != std::string::npos); // byte for byte
    CHECK(out.find("[look]\nversion = 1\nstyle = glass\n") != std::string::npos);
    CHECK(out.find("speed_percent = 70\n") != std::string::npos);
    CHECK(out.find("[app]\nhit_window_ms = 90\n") != std::string::npos);

    // And the result loads again to the same values.
    LoadedSettings again = load_settings(out, owned);
    CHECK(again.session.speed_percent == 70);
    CHECK(again.app.hit_window_ms == 90);
    CHECK(again.serialize() == out);
}

TEST_CASE("a newer major version loads what it can and recommends read-only") {
    LoadedSettings loaded = load_settings(
        "format = 2.0\n[app]\nhit_window_ms = 100\nnew_thing = 1\n[session]\nspeed_percent = 50\n");
    CHECK(loaded.read_only_recommended());
    CHECK(loaded.app.hit_window_ms == 100);
    CHECK(loaded.session.speed_percent == 50);
    CHECK(has_issue(loaded.issues, Kind::NewerVersion));
}

TEST_CASE(
    "a hash, enum and bool are read in the forms the writer produces and a few friendlier ones") {
    SessionState s =
        read("[session]\nsong = a.mid\nsong_hash = ABC\nmetronome = yes\nloop_enabled = 1\n"
             "hands = left\nmode = flow\npiano_plays = nothing\nkeyboard = manual\n");
    CHECK(s.song_hash == 0xabc);
    CHECK(s.metronome);
    CHECK(s.mode == PracticeMode::Flow);
    CHECK(s.practice_hands == HandFilter::Left);
    CHECK(s.piano_plays == PianoPlays::Nothing);
    CHECK(s.keyboard == KeyboardChoice::Manual);
}

TEST_CASE("a song path with quotes, blanks and hashes round trips through the file") {
    for (const char* name :
         {"  lead.mid", "trail.mid  ", "\"quoted\".mid", "a # b = c.mid", "dir/\xE6\x9B\xB2.mid"}) {
        CAPTURE(name);
        SessionState s;
        s.song_path = name;
        s.song_hash = 9;
        SettingsDocument doc = SettingsDocument::parse("");
        write_session(doc, s);
        CHECK(read_session(SettingsDocument::parse(doc.serialize())) == s);
    }
}

TEST_CASE("a default LoadedSettings can be edited and saved") {
    LoadedSettings loaded;
    loaded.session.speed_percent = 80;
    std::string text = loaded.serialize();
    CHECK(text.rfind("# toonval settings\nformat = 1.0\n", 0) == 0);
    CHECK(load_settings(text).session.speed_percent == 80);
}
