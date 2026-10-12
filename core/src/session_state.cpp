// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/session_state.hpp"

#include "toonval/content_hash.hpp"
#include "toonval/pitch.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>

namespace toonval {

namespace {

using Names = std::span<const std::string_view>;

constexpr std::array<std::string_view, 3> kHandNames{"both", "left", "right"};
constexpr std::array<std::string_view, 5> kPianoNames{"nothing", "other_hand", "left", "right",
                                                      "both"};
constexpr std::array<std::string_view, 3> kModeNames{"free_play", "wait", "flow"};
constexpr std::array<std::string_view, 3> kKeyboardNames{"auto", "profile", "manual"};
constexpr std::array<std::string_view, 3> kBarStyleNames{"strip", "groups", "dock"};

constexpr int kMinHitWindowMs = 20;
constexpr int kMaxHitWindowMs = 250;
constexpr int kMaxLatencyCorrectionMs = 250;
constexpr int kMaxDailyGoalMinutes = 720;
constexpr int kMinResumeCountdownS = 1;
constexpr int kMaxResumeCountdownS = 6;
constexpr int kMinSkipSeconds = 1;
constexpr int kMaxSkipSeconds = 30;
constexpr int kDefaultKeyboardLowest = 36;
constexpr int kDefaultKeyboardHighest = 96;

// Strict UTF-8: no overlong forms, no surrogates, nothing above U+10FFFF.
bool valid_utf8(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size()) {
        auto b0 = static_cast<unsigned char>(s[i]);
        std::size_t extra = 0;
        std::uint32_t cp = 0;
        std::uint32_t min = 0;
        if (b0 < 0x80) {
            ++i;
            continue;
        } else if (b0 >= 0xC2 && b0 <= 0xDF) {
            extra = 1;
            cp = b0 & 0x1F;
            min = 0x80;
        } else if (b0 >= 0xE0 && b0 <= 0xEF) {
            extra = 2;
            cp = b0 & 0x0F;
            min = 0x800;
        } else if (b0 >= 0xF0 && b0 <= 0xF4) {
            extra = 3;
            cp = b0 & 0x07;
            min = 0x10000;
        } else {
            return false;
        }
        if (i + extra >= s.size()) {
            return false; // cut off in the middle of a character
        }
        for (std::size_t k = 1; k <= extra; ++k) {
            auto b = static_cast<unsigned char>(s[i + k]);
            if ((b & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (b & 0x3F);
        }
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            return false;
        }
        i += extra + 1;
    }
    return true;
}

bool has_control(std::string_view s) {
    return std::any_of(s.begin(), s.end(), [](char c) {
        auto u = static_cast<unsigned char>(c);
        return u < 0x20 || u == 0x7f;
    });
}

bool valid_name(std::string_view s, std::size_t max_bytes) {
    return s.size() <= max_bytes && valid_utf8(s) && !has_control(s);
}

// Library-relative: not rooted, no drive letter, no ".." segment (either slash).
bool valid_song_path(std::string_view s) {
    if (!valid_name(s, kMaxSongPathBytes)) {
        return false;
    }
    if (s.empty()) {
        return true;
    }
    if (s.front() == '/' || s.front() == '\\' || (s.size() >= 2 && s[1] == ':')) {
        return false;
    }
    std::size_t start = 0;
    while (start <= s.size()) {
        std::size_t end = s.find_first_of("/\\", start);
        if (end == std::string_view::npos) {
            end = s.size();
        }
        if (s.substr(start, end - start) == "..") {
            return false;
        }
        start = end + 1;
    }
    return true;
}

enum class TextKind { Name, SongPath };

bool valid_text(std::string_view s, std::size_t max_bytes, TextKind kind) {
    return kind == TextKind::SongPath ? valid_song_path(s) : valid_name(s, max_bytes);
}

int clamp_int(int v, int lo, int hi, int step) {
    v = std::clamp(v, lo, hi);
    if (step > 1) {
        v = std::clamp((v + step / 2) / step * step, lo, hi);
    }
    return v;
}

double clamp_real(double v, double lo, double hi) {
    if (std::isnan(v)) {
        return lo;
    }
    v = std::clamp(v, lo, hi);
    return v == 0.0 ? 0.0 : v; // no negative zero
}

// ---- The three visitors over the field list: clamp in place, read from a document, write to one.
// One list per struct, so the key names, ranges and order cannot differ between reading, writing
// and clamping.

struct Clamp {
    void integer(std::string_view, int& v, int lo, int hi, int step = 1) const {
        v = clamp_int(v, lo, hi, step);
    }
    void real(std::string_view, double& v, double lo, double hi) const {
        v = clamp_real(v, lo, hi);
    }
    void boolean(std::string_view, bool&) const {}
    void text(std::string_view, std::string& v, std::size_t max, TextKind kind) const {
        if (!valid_text(v, max, kind)) {
            v.clear();
        }
    }
    void hash(std::string_view, std::uint64_t&) const {}
    template <class E> void choice(std::string_view, E& v, Names names) const {
        if (static_cast<std::size_t>(v) >= names.size()) {
            v = E{};
        }
    }
};

class Reader {
  public:
    Reader(const SettingsDocument& doc, std::string_view section,
           std::vector<SettingsIssue>* issues)
        : section_name_(section), issues_(issues), section_(doc.section(section)) {}

    void integer(std::string_view key, int& v, int lo, int hi, int step = 1) {
        const SettingsEntry* e = find(key);
        if (e == nullptr) {
            return;
        }
        int parsed = 0;
        auto [ptr, ec] =
            std::from_chars(e->value.data(), e->value.data() + e->value.size(), parsed);
        if (ec != std::errc() || ptr != e->value.data() + e->value.size()) {
            bad(*e, "expected a whole number");
            return;
        }
        int fixed = clamp_int(parsed, lo, hi, step);
        if (fixed != parsed) {
            report(SettingsIssueKind::OutOfRange, *e,
                   "outside " + std::to_string(lo) + ".." + std::to_string(hi) + ", using " +
                       std::to_string(fixed));
        }
        v = fixed;
    }

    void real(std::string_view key, double& v, double lo, double hi) {
        const SettingsEntry* e = find(key);
        if (e == nullptr) {
            return;
        }
        double parsed = 0.0;
        auto [ptr, ec] =
            std::from_chars(e->value.data(), e->value.data() + e->value.size(), parsed);
        if (ec != std::errc() || ptr != e->value.data() + e->value.size() ||
            !std::isfinite(parsed)) {
            bad(*e, "expected a number");
            return;
        }
        double fixed = clamp_real(parsed, lo, hi);
        if (fixed != parsed) {
            report(SettingsIssueKind::OutOfRange, *e, "outside the valid range, clamped");
        }
        v = fixed;
    }

    void boolean(std::string_view key, bool& v) {
        const SettingsEntry* e = find(key);
        if (e == nullptr) {
            return;
        }
        std::string lower;
        for (char c : e->value) {
            lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (lower == "true" || lower == "yes" || lower == "on" || lower == "1") {
            v = true;
        } else if (lower == "false" || lower == "no" || lower == "off" || lower == "0") {
            v = false;
        } else {
            bad(*e, "expected true or false");
        }
    }

    void text(std::string_view key, std::string& v, std::size_t max, TextKind kind) {
        const SettingsEntry* e = find(key);
        if (e == nullptr) {
            return;
        }
        std::string parsed;
        if (!settings_unquote(e->value, parsed)) {
            bad(*e, "bad quoting");
            return;
        }
        if (!valid_text(parsed, max, kind)) {
            report(SettingsIssueKind::OutOfRange, *e,
                   kind == TextKind::SongPath ? "not a library-relative path, ignored"
                                              : "too long or not UTF-8 text, ignored");
            v.clear();
            return;
        }
        v = std::move(parsed);
    }

    void hash(std::string_view key, std::uint64_t& v) {
        const SettingsEntry* e = find(key);
        if (e == nullptr) {
            return;
        }
        std::uint64_t parsed = 0;
        if (!parse_content_hash_hex(e->value, parsed)) {
            bad(*e, "expected up to 16 hex digits");
            return;
        }
        v = parsed;
    }

    template <class E> void choice(std::string_view key, E& v, Names names) {
        const SettingsEntry* e = find(key);
        if (e == nullptr) {
            return;
        }
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (names[i] == e->value) {
                v = static_cast<E>(i);
                return;
            }
        }
        bad(*e, "not one of the known names");
    }

    // After the fields: report keys and lines nothing claimed.
    void finish() {
        for (const SettingsEntry& e : section_.entries) {
            if (std::find(known_.begin(), known_.end(), e.key) == known_.end()) {
                report(SettingsIssueKind::UnknownKey, e, "unknown key, kept as it is");
            }
        }
        for (int line : section_.malformed_lines) {
            if (issues_ != nullptr) {
                issues_->push_back({SettingsIssueKind::MalformedLine, line,
                                    std::string(section_name_), "", "not a key = value line"});
            }
        }
    }

  private:
    // The last entry with this key; a repeated key is reported once.
    const SettingsEntry* find(std::string_view key) {
        known_.emplace_back(key);
        const SettingsEntry* found = nullptr;
        int count = 0;
        for (const SettingsEntry& e : section_.entries) {
            if (e.key == key) {
                found = &e;
                ++count;
            }
        }
        if (count > 1) {
            report(SettingsIssueKind::DuplicateKey, *found, "repeated key, the last one counts");
        }
        return found;
    }
    void bad(const SettingsEntry& e, const std::string& what) {
        report(SettingsIssueKind::BadValue, e, what + ", keeping the default");
    }
    void report(SettingsIssueKind kind, const SettingsEntry& e, std::string detail) {
        if (issues_ != nullptr) {
            issues_->push_back(
                {kind, e.line, std::string(section_name_), e.key, std::move(detail)});
        }
    }

    std::string_view section_name_;
    std::vector<SettingsIssue>* issues_;
    SettingsSection section_;
    std::vector<std::string> known_;
};

class Writer {
  public:
    Writer(SettingsDocument& doc, std::string_view section) : doc_(doc), section_(section) {}

    void integer(std::string_view key, const int& v, int, int, int = 1) {
        doc_.set(section_, key, std::to_string(v));
    }
    void real(std::string_view key, const double& v, double, double) {
        char buf[64];
        auto r = std::to_chars(buf, buf + sizeof buf, v);
        doc_.set(section_, key, std::string_view(buf, static_cast<std::size_t>(r.ptr - buf)));
    }
    void boolean(std::string_view key, const bool& v) {
        doc_.set(section_, key, v ? "true" : "false");
    }
    void text(std::string_view key, const std::string& v, std::size_t, TextKind) {
        doc_.set(section_, key, settings_quote(v));
    }
    void hash(std::string_view key, const std::uint64_t& v) {
        doc_.set(section_, key, content_hash_hex(v));
    }
    template <class E> void choice(std::string_view key, const E& v, Names names) {
        doc_.set(section_, key, names[static_cast<std::size_t>(v)]);
    }

  private:
    SettingsDocument& doc_;
    std::string_view section_;
};

template <class V, class S> void visit_session(V& v, S& s) {
    v.text("song", s.song_path, kMaxSongPathBytes, TextKind::SongPath);
    v.hash("song_hash", s.song_hash);
    v.real("position_s", s.position_s, 0.0, kMaxSessionSeconds);
    v.integer("speed_percent", s.speed_percent, kMinSpeedPercent, kMaxSpeedPercent,
              kSpeedStepPercent);
    v.choice("hands", s.practice_hands, Names(kHandNames));
    v.choice("piano_plays", s.piano_plays, Names(kPianoNames));
    v.real("loop_start_s", s.loop_start_s, 0.0, kMaxSessionSeconds);
    v.real("loop_end_s", s.loop_end_s, 0.0, kMaxSessionSeconds);
    v.boolean("loop_enabled", s.loop_enabled);
    v.boolean("auto_speed_enabled", s.auto_speed_enabled);
    v.integer("auto_speed_start_percent", s.auto_speed_start_percent, kMinSpeedPercent,
              kMaxSpeedPercent, kSpeedStepPercent);
    v.integer("auto_speed_step_percent", s.auto_speed_step_percent, kSpeedStepPercent, 25,
              kSpeedStepPercent);
    v.integer("auto_speed_max_percent", s.auto_speed_max_percent, kMinSpeedPercent,
              kMaxSpeedPercent, kSpeedStepPercent);
    v.integer("count_in_beats", s.count_in_beats, 0, kMaxCountInBeats);
    v.boolean("metronome", s.metronome);
    v.choice("mode", s.mode, Names(kModeNames));
    v.integer("transpose_semitones", s.transpose_semitones, -12, 12);
    v.integer("section_id", s.section_id, 0, kMaxSessionSectionId);
    v.integer("plan_item", s.plan_item, 0, kMaxSessionPlanItems);
    v.choice("keyboard", s.keyboard, Names(kKeyboardNames));
    v.text("keyboard_name", s.keyboard_name, kMaxNameBytes, TextKind::Name);
    v.integer("keyboard_lowest", s.keyboard_lowest, 0, 127);
    v.integer("keyboard_highest", s.keyboard_highest, 0, 127);
}

template <class V, class S> void visit_app(V& v, S& s) {
    v.integer("hit_window_ms", s.hit_window_ms, kMinHitWindowMs, kMaxHitWindowMs);
    v.integer("latency_correction_ms", s.latency_correction_ms, -kMaxLatencyCorrectionMs,
              kMaxLatencyCorrectionMs);
    v.text("midi_in_port", s.midi_in_port, kMaxNameBytes, TextKind::Name);
    v.text("midi_out_port", s.midi_out_port, kMaxNameBytes, TextKind::Name);
    v.integer("daily_goal_minutes", s.daily_goal_minutes, 0, kMaxDailyGoalMinutes);
    v.integer("resume_countdown_s", s.resume_countdown_s, kMinResumeCountdownS,
              kMaxResumeCountdownS);
    v.integer("skip_back_seconds", s.skip_back_seconds, kMinSkipSeconds, kMaxSkipSeconds);
    v.integer("skip_forward_seconds", s.skip_forward_seconds, kMinSkipSeconds, kMaxSkipSeconds);
    v.choice("bar_style", s.bar_style, Names(kBarStyleNames));
}

} // namespace

SessionState SessionState::clamped() const {
    SessionState s = *this;
    Clamp clamp;
    visit_session(clamp, s);

    if (!s.has_song()) {
        s.song_hash = 0;
        s.position_s = 0.0;
        s.loop_start_s = s.loop_end_s = 0.0;
        s.section_id = 0;
        s.plan_item = 0;
    }
    if (s.loop_start_s > s.loop_end_s) {
        std::swap(s.loop_start_s, s.loop_end_s);
    }
    if (!s.has_loop()) {
        s.loop_start_s = s.loop_end_s = 0.0;
        s.loop_enabled = false;
    }
    s.auto_speed_max_percent = std::max(s.auto_speed_max_percent, s.auto_speed_start_percent);

    int lowest = s.keyboard_lowest;
    int highest = s.keyboard_highest;
    if (lowest > highest) {
        std::swap(lowest, highest);
    }
    if (is_black_key(lowest)) {
        --lowest; // notes 0 and 127 are white, so a black key has a white neighbour inside 0..127
    }
    if (is_black_key(highest)) {
        ++highest;
    }
    if (highest - lowest + 1 < kMinKeyboardKeys) {
        lowest = kDefaultKeyboardLowest;
        highest = kDefaultKeyboardHighest;
        if (s.keyboard == KeyboardChoice::Manual) {
            s.keyboard = KeyboardChoice::Auto;
        }
    }
    s.keyboard_lowest = lowest;
    s.keyboard_highest = highest;
    if (s.keyboard == KeyboardChoice::Profile && s.keyboard_name.empty()) {
        s.keyboard = KeyboardChoice::Auto;
    }
    return s;
}

AppSettings AppSettings::clamped() const {
    AppSettings s = *this;
    Clamp clamp;
    visit_app(clamp, s);
    return s;
}

SessionState read_session(const SettingsDocument& doc, std::vector<SettingsIssue>* issues) {
    SessionState s;
    Reader reader(doc, kSessionSection, issues);
    visit_session(reader, s);
    reader.finish();
    SessionState fixed = s.clamped();
    if (!(fixed == s) && issues != nullptr && doc.has_section(kSessionSection)) {
        issues->push_back({SettingsIssueKind::OutOfRange, 0, std::string(kSessionSection), "",
                           "some values clash with each other and were adjusted"});
    }
    return fixed;
}

AppSettings read_app_settings(const SettingsDocument& doc, std::vector<SettingsIssue>* issues) {
    AppSettings s;
    Reader reader(doc, kAppSection, issues);
    visit_app(reader, s);
    reader.finish();
    return s.clamped();
}

void write_session(SettingsDocument& doc, const SessionState& state) {
    const SessionState s = state.clamped();
    Writer writer(doc, kSessionSection);
    visit_session(writer, s);
}

void write_app_settings(SettingsDocument& doc, const AppSettings& settings) {
    const AppSettings s = settings.clamped();
    Writer writer(doc, kAppSection);
    visit_app(writer, s);
}

std::string LoadedSettings::serialize() const {
    SettingsDocument out = document;
    out.ensure_format();
    write_app_settings(out, app);
    write_session(out, session);
    return out.serialize();
}

LoadedSettings load_settings(std::string_view text,
                             std::span<const std::string_view> other_known_sections) {
    LoadedSettings loaded;
    loaded.document = SettingsDocument::parse(text, &loaded.issues);
    loaded.app = read_app_settings(loaded.document, &loaded.issues);
    loaded.session = read_session(loaded.document, &loaded.issues);
    for (const std::string& name : loaded.document.section_names()) {
        bool known = name == kAppSection || name == kSessionSection ||
                     std::find(other_known_sections.begin(), other_known_sections.end(), name) !=
                         other_known_sections.end();
        if (!known) {
            loaded.issues.push_back({SettingsIssueKind::UnknownSection,
                                     loaded.document.section(name).header_line, name, "",
                                     "unknown section, kept as it is"});
        }
    }
    return loaded;
}

} // namespace toonval
