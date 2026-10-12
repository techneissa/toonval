// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace toonval {

// The text format of every saved file (settings, session, look, user presets): UTF-8, one
// `key = value` per line, grouped under `[section]` headers.
//
//   # toonval settings
//   format = 1.0
//
//   [app]
//   hit_window_ms = 80
//
//   [session]
//   song = Songs/Fur Elise.mid
//
// - Lines before the first header form the preamble. It carries `format = <major>.<minor>`.
// - A header is a whole line `[name]`; spaces around the name are ignored. Names and keys are
//   case-sensitive.
// - A line whose first non-blank character is `#` is a comment. There are no end-of-line comments,
//   so a value may hold `#`.
// - A line is split at its first `=`; key and value lose the blanks around them. A value that
//   starts with `"` is quoted (see settings_quote).
// - Blank lines are free. A line that is none of the above is malformed.
//
// The document keeps every line exactly as it was read, so reading and saving never loses a
// comment, a section or a key that this build does not know about (a newer build may have written
// it). A typed layer (session_state.hpp, and later the look) reads and sets the keys it knows and
// leaves everything else alone. Nothing here throws.

struct FormatVersion {
    int major = 1;
    int minor = 0;
    friend bool operator==(const FormatVersion&, const FormatVersion&) = default;
};

// What this build writes. A file with a higher major version loads what it can and
// `read_only_recommended()` says to leave it alone; a higher minor version only adds keys.
inline constexpr FormatVersion kSettingsFormat{1, 0};

enum class SettingsIssueKind : std::uint8_t {
    UnknownSection,   // a section no reader claimed; kept as it is
    UnknownKey,       // a key its section reader doesn't know; kept as it is
    BadValue,         // the value doesn't parse; the field keeps its default
    OutOfRange,       // the value parses but is outside the valid range; it was clamped
    MalformedLine,    // not blank, not a comment, not `key = value`; kept as it is, ignored
    DuplicateKey,     // a key twice in one section; the last one counts
    DuplicateSection, // a section twice; only the first counts, the rest are kept untouched
    BadVersion,       // the `format` value isn't `<major>.<minor>`; read as the current version
    NewerVersion,     // a newer major version: read what is understood, don't save over it
};

struct SettingsIssue {
    SettingsIssueKind kind = SettingsIssueKind::UnknownKey;
    int line = 0; // 1-based line of the text that was parsed, 0 when not tied to a line
    std::string section;
    std::string key;
    std::string detail; // short English text for a log, not for translation
};

struct SettingsEntry {
    std::string key;
    std::string value; // as written, minus the blanks around it; still quoted if it was
    int line = 0;      // 1-based line of the text that was parsed
};

// The `key = value` lines of one section in file order (a repeated key appears each time), plus
// the lines that are not comments, blank or `key = value`.
struct SettingsSection {
    bool present = false;
    int header_line = 0; // 1-based line of the `[name]` header when parsed; 0 for the preamble
    std::vector<SettingsEntry> entries;
    std::vector<int> malformed_lines;
};

class SettingsDocument {
  public:
    // Splits text into sections. Always succeeds; an empty or garbage text is a valid document.
    // Structure problems go to `issues` (when not null): DuplicateSection, BadVersion,
    // NewerVersion. Callers should refuse to read files over kMaxSettingsBytes.
    static SettingsDocument parse(std::string_view text,
                                  std::vector<SettingsIssue>* issues = nullptr);

    // Exactly the text that was parsed, with the edits since.
    std::string serialize() const;

    FormatVersion format() const { return format_; }
    // True for a newer major version: saving over it could drop what it means.
    bool read_only_recommended() const { return format_.major > kSettingsFormat.major; }

    // Section names in file order, once each, without the preamble.
    std::vector<std::string> section_names() const;
    bool has_section(std::string_view name) const;
    // "" is the preamble. A missing section comes back with present == false.
    SettingsSection section(std::string_view name) const;

    // Sets `key` to `value` (already formatted) in the section, creating the section when missing.
    // The last line with that key is rewritten as `key = value` in place; a line whose value
    // already reads `value` is left as it was, byte for byte. A missing key is appended after the
    // section's last non-blank line.
    void set(std::string_view section, std::string_view key, std::string_view value);

    // The lines of a section as one text (without the header, and without the blank lines that
    // separate it from the next section), for readers with their own format, like the look.
    // set_section_text replaces them and keeps the separating blank lines, or creates the section
    // at the end.
    std::string section_text(std::string_view name) const;
    void set_section_text(std::string_view name, std::string_view text);
    void remove_section(std::string_view name);

    // Adds `format = <current>` to the preamble when it has none (and a comment line when the
    // document is empty). Leaves an existing format line, including a newer one, alone.
    void ensure_format();

  private:
    struct Part {
        std::string name;        // "" for the preamble
        std::string header;      // the header line with its line ending; empty for the preamble
        std::string body;        // every line after the header, verbatim
        int first_body_line = 1; // 1-based line number of the body's first line when parsed
        bool duplicate = false;  // a second section of the same name
    };
    // Makes the part end in a line ending, so the next line starts on its own line: the body's last
    // line, or the header when the body is empty.
    static void end_line(Part& part);
    Part* find(std::string_view name);
    const Part* find(std::string_view name) const;
    Part& find_or_add(std::string_view name);

    bool bom_ = false;
    FormatVersion format_{};
    std::vector<Part> parts_ = std::vector<Part>(1); // parts_[0] is always the preamble
};

inline constexpr std::size_t kMaxSettingsBytes = 1 << 20;

// Value syntax. Plain values are written as they are. A value with blanks at either end, one
// starting with `"`, or one with a control character is written quoted: `"`, the text with `\` and
// `"` escaped by `\`, control characters as \n, \r, \t or \xHH, and `"`. Reading takes the same
// escapes.
std::string settings_quote(std::string_view text); // quotes only when needed
// Reverses settings_quote on a raw value. False for a quoted value that is cut off or has a bad
// escape or trailing text after the closing quote; `out` is untouched then.
bool settings_unquote(std::string_view raw, std::string& out);

} // namespace toonval
