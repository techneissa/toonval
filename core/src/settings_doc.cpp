// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/settings_doc.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>

namespace toonval {

namespace {

constexpr std::string_view kBlanks = " \t\r\f\v";
constexpr std::string_view kBom = "\xEF\xBB\xBF";

std::string_view trim(std::string_view s) {
    std::size_t begin = s.find_first_not_of(kBlanks);
    if (begin == std::string_view::npos) {
        return {};
    }
    std::size_t end = s.find_last_not_of(kBlanks);
    return s.substr(begin, end - begin + 1);
}

// One line of a body: `content` without its line ending, `eol` the "\n" (or empty on a last line
// that has none), `begin` the offset of the line in the body.
struct Line {
    std::size_t begin = 0;
    std::string_view content;
    std::string_view eol;
    std::size_t end() const { return begin + content.size() + eol.size(); }
};

template <class Fn> void for_each_line(std::string_view body, Fn&& fn) {
    std::size_t pos = 0;
    while (pos < body.size()) {
        std::size_t nl = body.find('\n', pos);
        Line line;
        line.begin = pos;
        if (nl == std::string_view::npos) {
            line.content = body.substr(pos);
            pos = body.size();
        } else {
            line.content = body.substr(pos, nl - pos);
            line.eol = body.substr(nl, 1);
            pos = nl + 1;
        }
        if (!fn(line)) {
            return;
        }
    }
}

enum class LineKind { Skip, Pair, Malformed };

LineKind classify(std::string_view content, std::string_view& key, std::string_view& value) {
    std::string_view t = trim(content);
    if (t.empty() || t.front() == '#') {
        return LineKind::Skip;
    }
    std::size_t eq = t.find('=');
    if (eq == std::string_view::npos) {
        return LineKind::Malformed;
    }
    key = trim(t.substr(0, eq));
    if (key.empty()) {
        return LineKind::Malformed;
    }
    value = trim(t.substr(eq + 1));
    return LineKind::Pair;
}

// "[name]" as a whole line, with blanks allowed around and inside. Returns false for anything else.
bool header_name(std::string_view content, std::string_view& name) {
    std::string_view t = trim(content);
    if (t.size() < 2 || t.front() != '[' || t.back() != ']') {
        return false;
    }
    std::string_view inner = trim(t.substr(1, t.size() - 2));
    if (inner.empty() || inner.find_first_of("[]") != std::string_view::npos) {
        return false;
    }
    name = inner;
    return true;
}

bool parse_number(std::string_view s, int& out) {
    if (s.empty() || s.size() > 9) {
        return false;
    }
    int value = 0;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (ec != std::errc() || ptr != s.data() + s.size()) {
        return false;
    }
    out = value;
    return true;
}

bool parse_version(std::string_view text, FormatVersion& out) {
    std::string_view major = text;
    std::string_view minor;
    std::size_t dot = text.find('.');
    if (dot != std::string_view::npos) {
        major = text.substr(0, dot);
        minor = text.substr(dot + 1);
    }
    FormatVersion v;
    if (!parse_number(major, v.major) || v.major < 1) {
        return false;
    }
    if (dot != std::string_view::npos && !parse_number(minor, v.minor)) {
        return false;
    }
    out = v;
    return true;
}

// True when the last line of the body, which has a line ending, holds only blanks.
bool ends_with_blank_line(std::string_view body) {
    bool blank = false;
    for_each_line(body, [&](const Line& line) {
        blank = !line.eol.empty() && trim(line.content).empty();
        return true;
    });
    return blank;
}

// Where the blank lines at the end of a body start (the body's size when it has none): they
// separate the section from the next header and belong to neither.
std::size_t trailing_blank_start(std::string_view body) {
    std::size_t start = body.size();
    for_each_line(body, [&](const Line& line) {
        if (!trim(line.content).empty()) {
            start = line.end();
        }
        return true;
    });
    return start;
}

bool is_control(unsigned char c) { return c < 0x20 || c == 0x7f; }

int hex_digit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

} // namespace

SettingsDocument SettingsDocument::parse(std::string_view text,
                                         std::vector<SettingsIssue>* issues) {
    SettingsDocument doc;
    if (text.substr(0, kBom.size()) == kBom) {
        doc.bom_ = true;
        text.remove_prefix(kBom.size());
    }
    int line_no = 1;
    for_each_line(text, [&](const Line& line) {
        std::string_view name;
        if (header_name(line.content, name)) {
            Part part;
            part.name = std::string(name);
            part.header = std::string(line.content) + std::string(line.eol);
            part.first_body_line = line_no + 1;
            part.duplicate = std::any_of(doc.parts_.begin(), doc.parts_.end(), [&](const Part& p) {
                return !p.duplicate && p.name == part.name && !p.header.empty();
            });
            if (part.duplicate && issues != nullptr) {
                issues->push_back({SettingsIssueKind::DuplicateSection, line_no, part.name, "",
                                   "section appears twice; only the first one is used"});
            }
            doc.parts_.push_back(std::move(part));
        } else {
            doc.parts_.back().body.append(line.content);
            doc.parts_.back().body.append(line.eol);
        }
        ++line_no;
        return true;
    });

    // The format line is the last `format` key of the preamble.
    SettingsSection pre = doc.section("");
    for (auto it = pre.entries.rbegin(); it != pre.entries.rend(); ++it) {
        if (it->key != "format") {
            continue;
        }
        std::string value;
        FormatVersion v;
        if (!settings_unquote(it->value, value) || !parse_version(value, v)) {
            if (issues != nullptr) {
                issues->push_back({SettingsIssueKind::BadVersion, it->line, "", "format",
                                   "expected <major>.<minor>, reading it as " +
                                       std::to_string(kSettingsFormat.major) + "." +
                                       std::to_string(kSettingsFormat.minor)});
            }
            break;
        }
        doc.format_ = v;
        if (v.major > kSettingsFormat.major && issues != nullptr) {
            issues->push_back({SettingsIssueKind::NewerVersion, it->line, "", "format",
                               "written by a newer major version; not saving over it is safest"});
        }
        break;
    }
    return doc;
}

std::string SettingsDocument::serialize() const {
    std::string out;
    if (bom_) {
        out.append(kBom);
    }
    for (const Part& p : parts_) {
        out.append(p.header);
        out.append(p.body);
    }
    return out;
}

SettingsDocument::Part* SettingsDocument::find(std::string_view name) {
    if (name.empty()) {
        return &parts_.front();
    }
    for (std::size_t i = 1; i < parts_.size(); ++i) {
        if (parts_[i].name == name) {
            return &parts_[i];
        }
    }
    return nullptr;
}

const SettingsDocument::Part* SettingsDocument::find(std::string_view name) const {
    return const_cast<SettingsDocument*>(this)->find(name);
}

std::vector<std::string> SettingsDocument::section_names() const {
    std::vector<std::string> names;
    for (std::size_t i = 1; i < parts_.size(); ++i) {
        if (!parts_[i].duplicate) {
            names.push_back(parts_[i].name);
        }
    }
    return names;
}

bool SettingsDocument::has_section(std::string_view name) const { return find(name) != nullptr; }

SettingsSection SettingsDocument::section(std::string_view name) const {
    SettingsSection out;
    const Part* part = find(name);
    if (part == nullptr) {
        return out;
    }
    out.present = true;
    out.header_line = part->header.empty() ? 0 : part->first_body_line - 1;
    int line_no = part->first_body_line;
    for_each_line(part->body, [&](const Line& line) {
        std::string_view key;
        std::string_view value;
        switch (classify(line.content, key, value)) {
        case LineKind::Pair:
            out.entries.push_back({std::string(key), std::string(value), line_no});
            break;
        case LineKind::Malformed:
            out.malformed_lines.push_back(line_no);
            break;
        case LineKind::Skip:
            break;
        }
        ++line_no;
        return true;
    });
    return out;
}

void SettingsDocument::end_line(Part& part) {
    std::string& text = part.body.empty() ? part.header : part.body;
    if (!text.empty() && text.back() != '\n') {
        text.push_back('\n');
    }
}

SettingsDocument::Part& SettingsDocument::find_or_add(std::string_view name) {
    if (Part* existing = find(name)) {
        return *existing;
    }
    // One blank line between the last section's content and the new header.
    Part& last = parts_.back();
    end_line(last);
    if (!last.body.empty() && !ends_with_blank_line(last.body)) {
        last.body.push_back('\n');
    }
    Part part;
    part.name = std::string(name);
    part.header = "[" + part.name + "]\n";
    parts_.push_back(std::move(part));
    return parts_.back();
}

void SettingsDocument::set(std::string_view section, std::string_view key, std::string_view value) {
    Part& part = find_or_add(section);
    std::string new_line = std::string(key) + (value.empty() ? " =" : " = ") + std::string(value);

    // The last line with this key, and where a new key would go: after the last line with content.
    bool found = false;
    bool unchanged = false;
    std::size_t match_begin = 0;
    std::size_t match_length = 0; // the line's content, without its "\n"
    bool match_has_cr = false;
    bool any_content = false;
    std::size_t anchor_end = 0; // end of the last line with content, including its "\n"
    bool anchor_has_eol = true;
    bool anchor_is_crlf = false;
    for_each_line(part.body, [&](const Line& line) {
        std::string_view k;
        std::string_view v;
        if (classify(line.content, k, v) == LineKind::Pair && k == key) {
            found = true;
            unchanged = v == value;
            match_begin = line.begin;
            match_length = line.content.size();
            match_has_cr = line.content.ends_with('\r');
        }
        if (!trim(line.content).empty()) {
            any_content = true;
            anchor_end = line.end();
            anchor_has_eol = !line.eol.empty();
            anchor_is_crlf = line.content.ends_with('\r');
        }
        return true;
    });

    if (found) {
        if (!unchanged) {
            part.body.replace(match_begin, match_length, match_has_cr ? new_line + '\r' : new_line);
        }
        return;
    }
    const std::string eol = anchor_is_crlf ? "\r\n" : "\n";
    if (!any_content) {
        end_line(part);
        part.body.insert(0, new_line + "\n");
        return;
    }
    if (!anchor_has_eol) {
        part.body.insert(anchor_end, eol);
        anchor_end += eol.size();
    }
    part.body.insert(anchor_end, new_line + eol);
}

std::string SettingsDocument::section_text(std::string_view name) const {
    const Part* part = find(name);
    if (part == nullptr) {
        return {};
    }
    return part->body.substr(0, trailing_blank_start(part->body));
}

void SettingsDocument::set_section_text(std::string_view name, std::string_view text) {
    Part& part = find_or_add(name);
    if (!text.empty()) {
        end_line(part);
    }
    std::string separator = part.body.substr(trailing_blank_start(part.body));
    part.body.assign(text);
    if (!part.body.empty() && part.body.back() != '\n') {
        part.body.push_back('\n');
    }
    part.body += separator;
}

void SettingsDocument::remove_section(std::string_view name) {
    if (name.empty()) {
        return;
    }
    parts_.erase(std::remove_if(parts_.begin() + 1, parts_.end(),
                                [&](const Part& p) { return p.name == name; }),
                 parts_.end());
}

void SettingsDocument::ensure_format() {
    for (const SettingsEntry& e : section("").entries) {
        if (e.key == "format") {
            return;
        }
    }
    bool empty = true;
    for (const Part& p : parts_) {
        empty = empty && p.header.empty() && p.body.empty();
    }
    if (empty) {
        parts_[0].body = "# toonval settings\n";
    }
    set("", "format",
        std::to_string(kSettingsFormat.major) + "." + std::to_string(kSettingsFormat.minor));
    format_ = kSettingsFormat;
}

std::string settings_quote(std::string_view text) {
    bool quote = false;
    if (!text.empty()) {
        std::string_view blanks = kBlanks;
        quote = blanks.find(text.front()) != std::string_view::npos ||
                blanks.find(text.back()) != std::string_view::npos || text.front() == '"';
    }
    for (char c : text) {
        quote = quote || is_control(static_cast<unsigned char>(c));
    }
    if (!quote) {
        return std::string(text);
    }
    constexpr char kHex[] = "0123456789abcdef";
    std::string out = "\"";
    for (char c : text) {
        auto u = static_cast<unsigned char>(c);
        if (c == '\\' || c == '"') {
            out += '\\';
            out += c;
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c == '\t') {
            out += "\\t";
        } else if (is_control(u)) {
            out += "\\x";
            out += kHex[u >> 4];
            out += kHex[u & 0xf];
        } else {
            out += c;
        }
    }
    out += '"';
    return out;
}

bool settings_unquote(std::string_view raw, std::string& out) {
    if (raw.empty() || raw.front() != '"') {
        out.assign(raw);
        return true;
    }
    std::string text;
    std::size_t i = 1;
    for (; i < raw.size(); ++i) {
        char c = raw[i];
        if (c == '"') {
            if (i + 1 != raw.size()) {
                return false; // text after the closing quote
            }
            out = std::move(text);
            return true;
        }
        if (c != '\\') {
            text += c;
            continue;
        }
        if (++i >= raw.size()) {
            return false;
        }
        switch (raw[i]) {
        case '\\':
        case '"':
            text += raw[i];
            break;
        case 'n':
            text += '\n';
            break;
        case 'r':
            text += '\r';
            break;
        case 't':
            text += '\t';
            break;
        case 'x': {
            if (i + 2 >= raw.size()) {
                return false;
            }
            int hi = hex_digit(raw[i + 1]);
            int lo = hex_digit(raw[i + 2]);
            if (hi < 0 || lo < 0) {
                return false;
            }
            text += static_cast<char>(hi * 16 + lo);
            i += 2;
            break;
        }
        default:
            return false;
        }
    }
    return false; // no closing quote
}

} // namespace toonval
