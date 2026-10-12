// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/sections_store.hpp"

#include "toonval/content_hash.hpp"
#include "toonval/session_state.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>

namespace toonval {

// The session keeps section ids and playlist places as ints within these bounds.
static_assert(kMaxSectionId == static_cast<SectionId>(kMaxSessionSectionId));
static_assert(kMaxPlanItems == static_cast<std::size_t>(kMaxSessionPlanItems));

namespace {

constexpr std::string_view kVersionKey = "version";
constexpr std::string_view kNextIdKey = "next_id";
constexpr std::string_view kUsedKey = "used";
constexpr std::string_view kSectionKey = "section.";
constexpr std::string_view kPlanKey = "plan.";

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

// `section.<digits>` / `plan.<digits>`: the keys this layer owns and rewrites. Other keys, like a
// newer build's `section.3.color`, are kept as they are.
bool numbered(std::string_view key, std::string_view prefix, std::uint32_t* number = nullptr) {
    if (key.substr(0, prefix.size()) != prefix) {
        return false;
    }
    std::string_view digits = key.substr(prefix.size());
    if (digits.empty() ||
        !std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        return false;
    }
    if (number != nullptr) {
        std::uint32_t v = 0;
        auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), v);
        *number = ec == std::errc() && ptr == digits.data() + digits.size() ? v : 0;
    }
    return true;
}

bool owned_key(std::string_view key) {
    return key == kVersionKey || key == kNextIdKey || key == kUsedKey ||
           numbered(key, kSectionKey) || numbered(key, kPlanKey);
}

template <class T> bool parse_number(std::string_view text, T& out) {
    T v{};
    auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
    if (ec != std::errc() || ptr != text.data() + text.size()) {
        return false;
    }
    out = v;
    return true;
}

// The next blank-separated word; `rest` moves past it and the blanks after it.
std::string_view next_word(std::string_view& rest) {
    std::size_t end = rest.find(' ');
    std::string_view word = rest.substr(0, end);
    rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end);
    while (!rest.empty() && rest.front() == ' ') {
        rest.remove_prefix(1);
    }
    return word;
}

bool parse_section(std::string_view value, std::uint32_t id, Section& out) {
    std::string_view rest = value;
    double start = 0.0;
    double end = 0.0;
    if (!parse_number(next_word(rest), start) || !parse_number(next_word(rest), end) ||
        !std::isfinite(start) || !std::isfinite(end)) {
        return false;
    }
    out = Section{};
    out.id = id;
    out.start_s = start;
    out.end_s = end;
    out.name = std::string(rest); // cleaned by normalize()
    out.custom_name = !out.name.empty();
    return true;
}

bool parse_plan_item(std::string_view value, PlanItem& out) {
    std::string_view rest = value;
    std::uint32_t section = 0;
    int clean = 0;
    int enabled = 0;
    PlanItem item;
    if (!parse_number(next_word(rest), section) || !parse_number(next_word(rest), item.repeats) ||
        !parse_number(next_word(rest), clean) || !parse_number(next_word(rest), enabled) ||
        !parse_number(next_word(rest), item.speed_up.start_percent) ||
        !parse_number(next_word(rest), item.speed_up.step_percent) ||
        !parse_number(next_word(rest), item.speed_up.max_percent) || !rest.empty() ||
        (clean != 0 && clean != 1) || (enabled != 0 && enabled != 1)) {
        return false;
    }
    item.section = section;
    item.clean_passes_only = clean == 1;
    item.speed_up.enabled = enabled == 1;
    out = item;
    return true;
}

std::string number_text(double v) {
    char buf[64];
    auto r = std::to_chars(buf, buf + sizeof buf, v);
    return std::string(buf, static_cast<std::size_t>(r.ptr - buf));
}

std::string section_name_of(std::uint64_t hash) {
    return std::string(kSectionsSectionPrefix) + content_hash_hex(hash);
}

// 16 lowercase hex digits, not all zero.
bool entry_hash(std::string_view name, std::uint64_t& hash) {
    std::string_view digits = name.substr(kSectionsSectionPrefix.size());
    if (digits.size() != 16 || !std::all_of(digits.begin(), digits.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        })) {
        return false;
    }
    return parse_content_hash_hex(digits, hash) && hash != 0;
}

// The lines of an entry's text that this layer does not own: unknown keys, comments, malformed
// lines. `has_keys` is set when one of them is a key = value pair.
std::string foreign_lines(const std::string& existing, bool& has_keys) {
    std::string out;
    std::size_t pos = 0;
    while (pos < existing.size()) {
        std::size_t eol = existing.find('\n', pos);
        std::string_view line = std::string_view(existing).substr(
            pos, eol == std::string::npos ? std::string::npos : eol - pos);
        pos = eol == std::string::npos ? existing.size() : eol + 1;
        std::string_view stripped = trim(line);
        std::size_t eq = stripped.find('=');
        bool is_pair = !stripped.empty() && stripped.front() != '#' && eq != std::string_view::npos;
        if (is_pair && owned_key(trim(stripped.substr(0, eq)))) {
            continue;
        }
        has_keys = has_keys || is_pair;
        out.append(line);
        out += "\n";
    }
    return out;
}

class Issues {
  public:
    Issues(std::vector<SettingsIssue>* out, const std::string& section)
        : out_(out), section_(section) {}
    void add(SettingsIssueKind kind, int line, std::string key, std::string detail) {
        if (out_ != nullptr) {
            out_->push_back({kind, line, section_, std::move(key), std::move(detail)});
        }
    }

  private:
    std::vector<SettingsIssue>* out_;
    const std::string& section_;
};

} // namespace

std::vector<std::string> sections_section_names(const SettingsDocument& doc) {
    std::vector<std::string> out;
    for (const std::string& name : doc.section_names()) {
        if (name.compare(0, kSectionsSectionPrefix.size(), kSectionsSectionPrefix) == 0) {
            out.push_back(name);
        }
    }
    return out;
}

SectionLibrary read_sections(const SettingsDocument& doc, std::vector<SettingsIssue>* issues) {
    SectionLibrary library;
    for (const std::string& name : sections_section_names(doc)) {
        const SettingsSection body = doc.section(name);
        Issues report(issues, name);
        std::uint64_t hash = 0;
        if (!entry_hash(name, hash)) {
            report.add(SettingsIssueKind::UnknownSection, body.header_line, "",
                       "not sections.<16 lowercase hex digits>, dropped");
            library.stale_names().push_back(name);
            continue;
        }

        // The last line of each key counts.
        std::map<std::string, const SettingsEntry*> last;
        std::map<std::string, int> seen;
        for (const SettingsEntry& e : body.entries) {
            last[e.key] = &e;
            if (++seen[e.key] == 2) {
                report.add(SettingsIssueKind::DuplicateKey, e.line, e.key,
                           "repeated key, the last one counts");
            }
        }
        for (int line : body.malformed_lines) {
            report.add(SettingsIssueKind::MalformedLine, line, "", "not a key = value line");
        }

        if (auto it = last.find(std::string(kVersionKey)); it != last.end()) {
            int version = 0;
            if (!parse_number(it->second->value, version) || version < 1) {
                report.add(SettingsIssueKind::BadValue, it->second->line, it->first,
                           "expected a version number, reading it as 1");
            } else if (version > kSectionsFormatVersion) {
                report.add(SettingsIssueKind::NewerVersion, it->second->line, it->first,
                           "written by a newer build, not loaded and left as it is");
                continue;
            }
        }

        SongSections entry;
        entry.song_hash = hash;
        bool had_data = false;
        bool had_unknown = false;
        std::map<std::uint32_t, const SettingsEntry*> plan_lines;
        for (const auto& [key, e] : last) {
            std::uint32_t number = 0;
            if (key == kVersionKey) {
                continue;
            }
            if (key == kNextIdKey) {
                std::uint32_t v = 0;
                if (parse_number(e->value, v)) {
                    entry.next_id = v;
                } else {
                    report.add(SettingsIssueKind::BadValue, e->line, key,
                               "expected a whole number");
                }
            } else if (key == kUsedKey) {
                std::uint32_t v = 0;
                if (parse_number(e->value, v)) {
                    entry.last_used = v;
                } else {
                    report.add(SettingsIssueKind::BadValue, e->line, key,
                               "expected a whole number");
                }
            } else if (numbered(key, kSectionKey, &number)) {
                Section s;
                if (number != 0 && parse_section(e->value, number, s)) {
                    entry.sections.push_back(std::move(s));
                    had_data = true;
                } else {
                    report.add(SettingsIssueKind::BadValue, e->line, key,
                               "expected <id>: <start> <end> [name], dropped");
                }
            } else if (numbered(key, kPlanKey, &number)) {
                if (number != 0) {
                    plan_lines[number] = e;
                } else {
                    report.add(SettingsIssueKind::BadValue, e->line, key,
                               "bad playlist number, dropped");
                }
            } else {
                had_unknown = true;
                report.add(SettingsIssueKind::UnknownKey, e->line, key,
                           "unknown key, kept as it is");
            }
        }
        for (const auto& [number, e] : plan_lines) {
            PlanItem item;
            if (parse_plan_item(e->value, item)) {
                entry.plan.push_back(item);
                had_data = true;
            } else {
                report.add(SettingsIssueKind::BadValue, e->line, e->key,
                           "expected <section> <repeats> <clean> <speed-up> <start> <step> <max>, "
                           "dropped");
            }
        }
        // Lines come in key order; the sections are sorted by time inside normalize().
        entry.normalize();
        if (entry.sections.empty() && entry.plan.empty()) {
            if (had_data) {
                report.add(SettingsIssueKind::OutOfRange, body.header_line, "",
                           "no usable section, entry dropped");
            }
            // Keys this build does not know belong to a newer one: leave the entry as it is.
            if (!had_unknown) {
                library.stale_names().push_back(name);
            }
            continue;
        }
        library.put(std::move(entry));
    }
    return library;
}

void write_sections(SettingsDocument& doc, const SectionLibrary& library) {
    std::vector<std::string> current;
    for (const SongSections& s : library.songs()) {
        if (s.song_hash != 0) {
            current.push_back(section_name_of(s.song_hash));
        }
    }
    for (const std::string& name : library.stale_names()) {
        if (std::find(current.begin(), current.end(), name) == current.end()) {
            doc.remove_section(name);
        }
    }

    for (const SongSections& s : library.songs()) {
        if (s.song_hash == 0) {
            continue;
        }
        const std::string name = section_name_of(s.song_hash);
        if (s.sections.empty() && s.plan.empty()) {
            // Nothing to keep, but unknown keys of a newer build stay.
            bool has_keys = false;
            std::string foreign = foreign_lines(doc.section_text(name), has_keys);
            if (has_keys) {
                doc.set_section_text(name, foreign);
            } else {
                doc.remove_section(name);
            }
            continue;
        }
        SongSections clean = s;
        clean.normalize();

        std::string text;
        text += "version = " + std::to_string(kSectionsFormatVersion) + "\n";
        text += "next_id = " + std::to_string(clean.next_id) + "\n";
        text += "used = " + std::to_string(clean.last_used) + "\n";
        for (const Section& sec : clean.sections) {
            text += "section." + std::to_string(sec.id) + " = " + number_text(sec.start_s) + " " +
                    number_text(sec.end_s);
            if (sec.custom_name) {
                text += " " + sec.name;
            }
            text += "\n";
        }
        for (std::size_t i = 0; i < clean.plan.size(); ++i) {
            const PlanItem& p = clean.plan[i];
            text += "plan." + std::to_string(i + 1) + " = " + std::to_string(p.section) + " " +
                    std::to_string(p.repeats) + " " + (p.clean_passes_only ? "1" : "0") + " " +
                    (p.speed_up.enabled ? "1" : "0") + " " +
                    std::to_string(p.speed_up.start_percent) + " " +
                    std::to_string(p.speed_up.step_percent) + " " +
                    std::to_string(p.speed_up.max_percent) + "\n";
        }

        // What the file already has under this entry and this layer does not own stays.
        bool unused = false;
        text += foreign_lines(doc.section_text(name), unused);
        doc.set_section_text(name, text);
    }
}

} // namespace toonval
