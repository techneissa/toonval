// SPDX-License-Identifier: GPL-3.0-or-later
// Random and mutated documents: reading never crashes, always gives a valid (clamped) state, never
// changes a byte it is not asked to change, and writing what was read is stable. The generators use
// a fixed seed, so a failure repeats; the printed iteration number and input are the repro.
#include "toonval/session_restore.hpp"
#include "toonval/session_state.hpp"
#include "toonval/settings_doc.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

using namespace toonval;

namespace {

// ---- An oracle written from the documented ranges, not from clamped().

bool utf8_ok(const std::string& s) {
    std::size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        int n = c < 0x80           ? 0
                : (c >> 5) == 0x6  ? 1
                : (c >> 4) == 0xE  ? 2
                : (c >> 3) == 0x1E ? 3
                                   : -1;
        if (n < 0 || i + static_cast<std::size_t>(n) >= s.size() + (n == 0 ? 1 : 0)) {
            return false;
        }
        std::uint32_t cp = n == 0 ? c : (n == 1 ? c & 0x1F : n == 2 ? c & 0x0F : c & 0x07);
        for (int k = 1; k <= n; ++k) {
            unsigned char b = static_cast<unsigned char>(s[i + static_cast<std::size_t>(k)]);
            if ((b & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (b & 0x3F);
        }
        const std::uint32_t floor[] = {0, 0x80, 0x800, 0x10000};
        if (cp < floor[n] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            return false;
        }
        i += static_cast<std::size_t>(n) + 1;
    }
    return true;
}

bool text_ok(const std::string& s, std::size_t max) {
    if (s.size() > max || !utf8_ok(s)) {
        return false;
    }
    for (char c : s) {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) {
            return false;
        }
    }
    return true;
}

bool path_ok(const std::string& s) {
    if (!text_ok(s, 4096)) {
        return false;
    }
    if (s.empty()) {
        return true;
    }
    if (s[0] == '/' || s[0] == '\\' || (s.size() >= 2 && s[1] == ':')) {
        return false;
    }
    std::string seg;
    for (std::size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == '/' || s[i] == '\\') {
            if (seg == "..") {
                return false;
            }
            seg.clear();
        } else {
            seg += s[i];
        }
    }
    return true;
}

bool white_key(int note) {
    int pc = note % 12;
    return pc != 1 && pc != 3 && pc != 6 && pc != 8 && pc != 10;
}

bool num_in(double v, double lo, double hi) { return std::isfinite(v) && v >= lo && v <= hi; }

bool valid_session(const SessionState& s) {
    bool ok = path_ok(s.song_path);
    ok = ok && num_in(s.position_s, 0, 86400) && s.speed_percent >= 25 && s.speed_percent <= 150 &&
         s.speed_percent % 5 == 0;
    ok = ok && static_cast<int>(s.practice_hands) <= 2 && static_cast<int>(s.piano_plays) <= 4 &&
         static_cast<int>(s.mode) <= 2 && static_cast<int>(s.keyboard) <= 2;
    ok = ok && num_in(s.loop_start_s, 0, 86400) && num_in(s.loop_end_s, 0, 86400);
    bool no_loop = s.loop_start_s == 0.0 && s.loop_end_s == 0.0 && !s.loop_enabled;
    bool real_loop = s.loop_end_s - s.loop_start_s >= 0.1;
    ok = ok && (no_loop || real_loop);
    ok = ok && s.auto_speed_start_percent >= 25 && s.auto_speed_start_percent <= 150 &&
         s.auto_speed_start_percent % 5 == 0;
    ok = ok && s.auto_speed_step_percent >= 5 && s.auto_speed_step_percent <= 25 &&
         s.auto_speed_step_percent % 5 == 0;
    ok = ok && s.auto_speed_max_percent >= s.auto_speed_start_percent &&
         s.auto_speed_max_percent <= 150 && s.auto_speed_max_percent % 5 == 0;
    ok = ok && s.count_in_beats >= 0 && s.count_in_beats <= 16;
    ok = ok && s.transpose_semitones >= -12 && s.transpose_semitones <= 12;
    ok = ok && s.section_id >= 0 && s.section_id <= 2'000'000'000 && s.plan_item >= 0 &&
         s.plan_item <= 64;
    ok = ok && text_ok(s.keyboard_name, 256);
    ok = ok && s.keyboard_lowest >= 0 && s.keyboard_highest <= 127 &&
         s.keyboard_lowest <= s.keyboard_highest && white_key(s.keyboard_lowest) &&
         white_key(s.keyboard_highest) && s.keyboard_highest - s.keyboard_lowest + 1 >= 12;
    ok = ok && !(s.keyboard == KeyboardChoice::Profile && s.keyboard_name.empty());
    if (s.song_path.empty()) {
        ok = ok && s.song_hash == 0 && s.position_s == 0.0 && no_loop && s.section_id == 0 &&
             s.plan_item == 0;
    }
    return ok;
}

bool valid_app(const AppSettings& a) {
    return a.hit_window_ms >= 20 && a.hit_window_ms <= 250 && a.latency_correction_ms >= -250 &&
           a.latency_correction_ms <= 250 && a.daily_goal_minutes >= 0 &&
           a.daily_goal_minutes <= 720 && text_ok(a.midi_in_port, 256) &&
           text_ok(a.midi_out_port, 256);
}

// ---- Generators.

using Rng = std::mt19937_64;

std::string random_bytes(Rng& rng, std::size_t max_len) {
    std::string s(rng() % (max_len + 1), '\0');
    for (char& c : s) {
        c = static_cast<char>(rng() & 0xff);
    }
    return s;
}

const std::array<const char*, 28> kKeys = {"song",
                                           "song_hash",
                                           "position_s",
                                           "speed_percent",
                                           "hands",
                                           "piano_plays",
                                           "loop_start_s",
                                           "loop_end_s",
                                           "loop_enabled",
                                           "auto_speed_enabled",
                                           "auto_speed_start_percent",
                                           "auto_speed_step_percent",
                                           "auto_speed_max_percent",
                                           "count_in_beats",
                                           "metronome",
                                           "mode",
                                           "transpose_semitones",
                                           "section_id",
                                           "plan_item",
                                           "keyboard",
                                           "keyboard_name",
                                           "keyboard_lowest",
                                           "keyboard_highest",
                                           "hit_window_ms",
                                           "latency_correction_ms",
                                           "midi_in_port",
                                           "midi_out_port",
                                           "daily_goal_minutes"};

const std::array<const char*, 40> kValues = {"0",
                                             "1",
                                             "-1",
                                             "100",
                                             "60",
                                             "62",
                                             "150",
                                             "151",
                                             "-12",
                                             "13",
                                             "99999999999999999999",
                                             "1e308",
                                             "-1e308",
                                             "nan",
                                             "inf",
                                             "-inf",
                                             "0.1",
                                             "12.5",
                                             "true",
                                             "false",
                                             "yes",
                                             "maybe",
                                             "both",
                                             "left",
                                             "right",
                                             "nothing",
                                             "wait",
                                             "flow",
                                             "profile",
                                             "manual",
                                             "auto",
                                             "",
                                             "\"",
                                             "\"quoted\"",
                                             "\"\\x\"",
                                             "../../etc/passwd",
                                             "/abs.mid",
                                             "Songs/a.mid",
                                             "PSR-E383",
                                             "00000000deadbeef"};

const std::array<const char*, 8> kSections = {"app", "session", "look", "user_presets",
                                              "zz",  "",        "[bad", "app"};

std::string line_soup(Rng& rng) {
    std::string out;
    int lines = static_cast<int>(rng() % 40);
    for (int i = 0; i < lines; ++i) {
        switch (rng() % 8) {
        case 0:
            out += std::string("[") + kSections[rng() % kSections.size()] + "]";
            break;
        case 1:
            out += "# " + random_bytes(rng, 12);
            break;
        case 2:
            out += random_bytes(rng, 30);
            break;
        case 3:
            out += "format = " + std::string(kValues[rng() % kValues.size()]);
            break;
        default:
            out += std::string(kKeys[rng() % kKeys.size()]) + (rng() % 5 ? " = " : "=") +
                   kValues[rng() % kValues.size()];
            break;
        }
        out += rng() % 7 ? "\n" : "\r\n";
    }
    if (rng() % 4 == 0 && !out.empty()) {
        out.pop_back(); // no final newline
    }
    return out;
}

std::string valid_document(Rng& rng) {
    SessionState s;
    s.song_path = "Songs/a.mid";
    s.song_hash = rng();
    s.position_s = static_cast<double>(rng() % 1000) / 8.0;
    s.speed_percent = 25 + 5 * static_cast<int>(rng() % 26);
    s.practice_hands = static_cast<HandFilter>(rng() % 3);
    s.piano_plays = static_cast<PianoPlays>(rng() % 5);
    s.loop_start_s = 1.0;
    s.loop_end_s = 1.0 + static_cast<double>(rng() % 100 + 1) / 4.0;
    s.loop_enabled = rng() % 2;
    s.count_in_beats = static_cast<int>(rng() % 17);
    s.mode = static_cast<PracticeMode>(rng() % 3);
    s.transpose_semitones = static_cast<int>(rng() % 25) - 12;
    s.keyboard = static_cast<KeyboardChoice>(rng() % 3);
    s.keyboard_name = "PSR-E383";
    LoadedSettings loaded;
    loaded.session = s.clamped();
    loaded.app.hit_window_ms = 20 + static_cast<int>(rng() % 231);
    return loaded.serialize();
}

std::string mutate(Rng& rng, std::string text) {
    int edits = 1 + static_cast<int>(rng() % 6);
    for (int i = 0; i < edits; ++i) {
        std::size_t pos = text.empty() ? 0 : rng() % text.size();
        switch (rng() % 6) {
        case 0: // flip a bit
            if (!text.empty()) {
                text[pos] = static_cast<char>(text[pos] ^ (1 << (rng() % 8)));
            }
            break;
        case 1: // delete a run
            text.erase(pos, rng() % 20);
            break;
        case 2: // insert bytes
            text.insert(pos, random_bytes(rng, 10));
            break;
        case 3: // truncate
            text.resize(pos);
            break;
        case 4: { // repeat a chunk
            std::size_t len = rng() % 60;
            text.insert(pos, text.substr(pos, len));
            break;
        }
        default: // swap in a vocabulary word
            text.insert(pos, kValues[rng() % kValues.size()]);
            break;
        }
    }
    return text;
}

constexpr int kIterations = 6000;

std::string next_input(Rng& rng, int i) {
    switch (i % 3) {
    case 0:
        return random_bytes(rng, 600);
    case 1:
        return line_soup(rng);
    default:
        return mutate(rng, valid_document(rng));
    }
}

} // namespace

TEST_CASE("fuzz: any text parses, round trips byte for byte, and reads as a valid state") {
    Rng rng(0x70011ea5);
    for (int i = 0; i < kIterations; ++i) {
        const std::string input = next_input(rng, i);
        CAPTURE(i);
        CAPTURE(input);

        std::vector<SettingsIssue> issues;
        SettingsDocument doc = SettingsDocument::parse(input, &issues);
        REQUIRE(doc.serialize() == input);

        LoadedSettings loaded = load_settings(input);
        REQUIRE(valid_session(loaded.session));
        REQUIRE(valid_app(loaded.app));
        REQUIRE(loaded.session == loaded.session.clamped());
        REQUIRE(loaded.app == loaded.app.clamped());
    }
}

TEST_CASE("fuzz: saving what was read is stable and loses no known value") {
    Rng rng(0xfeedbeef);
    for (int i = 0; i < kIterations; ++i) {
        const std::string input = next_input(rng, i);
        CAPTURE(i);
        CAPTURE(input);

        LoadedSettings first = load_settings(input);
        const std::string saved = first.serialize();
        LoadedSettings second = load_settings(saved);
        REQUIRE(second.session == first.session);
        REQUIRE(second.app == first.app);
        // What we wrote has nothing wrong with its known keys.
        for (const SettingsIssue& issue : second.issues) {
            if (issue.kind == SettingsIssueKind::BadValue ||
                issue.kind == SettingsIssueKind::OutOfRange) {
                // A format line we did not write (a newer or bad one) is not ours to fix.
                REQUIRE(issue.section != "app");
                REQUIRE(issue.section != "session");
            }
        }
        // Writing again changes nothing.
        REQUIRE(second.serialize() == saved);
    }
}

TEST_CASE("fuzz: unknown sections and unknown keys survive a save byte for byte") {
    Rng rng(0xabad1dea);
    for (int i = 0; i < kIterations / 2; ++i) {
        // A payload with no line that starts a section, so it stays one block.
        std::string payload = "[zz_future]\n";
        int lines = 1 + static_cast<int>(rng() % 8);
        for (int l = 0; l < lines; ++l) {
            std::string line = random_bytes(rng, 25);
            for (char& c : line) {
                if (c == '\n') {
                    c = ' ';
                }
            }
            if (!line.empty() && line.find_first_not_of(" \t\r\f\v") != std::string::npos &&
                line[line.find_first_not_of(" \t\r\f\v")] == '[') {
                line = "# " + line;
            }
            payload += line + "\n";
        }
        std::string head = rng() % 2 ? valid_document(rng) : std::string();
        // Keep the payload's section unique: drop a generated duplicate of its name.
        const std::string input = head + "\n" + payload;
        CAPTURE(i);
        CAPTURE(input);

        LoadedSettings loaded = load_settings(input);
        loaded.session.speed_percent = 35;
        loaded.app.hit_window_ms = 111;
        const std::string saved = loaded.serialize();
        REQUIRE(saved.find(payload) != std::string::npos);
        REQUIRE(load_settings(saved).session.speed_percent == 35);
    }
}

TEST_CASE("fuzz: set() changes its own key and no other section") {
    Rng rng(0x5e7c0de);
    for (int i = 0; i < kIterations / 2; ++i) {
        const std::string input = line_soup(rng);
        CAPTURE(i);
        CAPTURE(input);
        SettingsDocument doc = SettingsDocument::parse(input);
        std::vector<std::string> names = doc.section_names();
        std::vector<std::string> before;
        for (const std::string& n : names) {
            before.push_back(doc.section_text(n));
        }
        const std::string pre_before = doc.section_text("");

        const std::string value = std::to_string(rng() % 1000);
        doc.set("target", "k", value);

        SettingsDocument again = SettingsDocument::parse(doc.serialize());
        SettingsSection s = again.section("target");
        REQUIRE(s.present);
        REQUIRE(!s.entries.empty());
        REQUIRE(s.entries.back().key == "k");
        REQUIRE(s.entries.back().value == value);
        // The one change allowed to the section before a new one: its last line gets a line ending.
        auto unterminated = [](std::string t) {
            if (!t.empty() && t.back() == '\n') {
                t.pop_back();
            }
            return t;
        };
        for (std::size_t n = 0; n < names.size(); ++n) {
            if (names[n] != "target") {
                REQUIRE(unterminated(again.section_text(names[n])) == unterminated(before[n]));
            }
        }
        REQUIRE(unterminated(again.section_text("")) == unterminated(pre_before));
    }
}

TEST_CASE("fuzz: quoting round trips any bytes") {
    Rng rng(0x9071e);
    for (int i = 0; i < kIterations; ++i) {
        std::string text = random_bytes(rng, 40);
        std::string quoted = settings_quote(text);
        std::string back = "untouched";
        // A quoted form is a value on one line, so it has no raw newline.
        REQUIRE(quoted.find('\n') == std::string::npos);
        REQUIRE(settings_unquote(quoted, back));
        REQUIRE(back == text);

        // And no input crashes the unquoter.
        std::string junk = "\"" + random_bytes(rng, 20);
        std::string out;
        (void)settings_unquote(junk, out);
    }
}

TEST_CASE("fuzz: a restore plan from any state and any song is valid and inside the song") {
    Rng rng(0x2e57012e);
    for (int i = 0; i < kIterations; ++i) {
        const std::string input = next_input(rng, i);
        SessionState saved = load_settings(input).session;
        if (rng() % 2) { // sometimes skip the clamp pass: the plan must not trust its input
            saved.position_s = static_cast<double>(rng() % 200000) - 50000.0;
            saved.loop_start_s = static_cast<double>(rng() % 100000) - 1000.0;
            saved.loop_end_s = static_cast<double>(rng() % 100000) - 1000.0;
            saved.loop_enabled = rng() % 2;
            saved.speed_percent = static_cast<int>(rng() % 400) - 100;
            if (rng() % 2) {
                saved.song_path = "a.mid";
            }
        }
        Song song;
        song.duration_s = static_cast<double>(rng() % 4000) / 8.0;
        bool found = rng() % 4 != 0;
        bool matches = rng() % 3 != 0;
        const Song* song_ptr = rng() % 5 == 0 ? nullptr : &song;
        CAPTURE(i);
        CAPTURE(input);

        RestorePlan plan = plan_restore(saved, found, matches, song_ptr);
        REQUIRE(valid_session(plan.state));
        if (plan.outcome == RestoreOutcome::Restored) {
            REQUIRE(plan.load_song);
            REQUIRE(plan.state.position_s <= song.duration_s);
            REQUIRE(plan.state.loop_end_s <= song.duration_s);
        }
        if (!plan.load_song) {
            REQUIRE(plan.outcome != RestoreOutcome::Restored);
        }
        if (plan.outcome == RestoreOutcome::SongMissing ||
            plan.outcome == RestoreOutcome::SongUnreadable) {
            REQUIRE(plan.state.song_path.empty());
        }
    }
}
