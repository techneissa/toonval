// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/settings_doc.hpp"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace toonval;
using Kind = SettingsIssueKind;

namespace {

SettingsDocument parse(const std::string& text, std::vector<SettingsIssue>* issues = nullptr) {
    return SettingsDocument::parse(text, issues);
}

std::string roundtrip(const std::string& text) { return parse(text).serialize(); }

bool has_issue(const std::vector<SettingsIssue>& issues, Kind kind) {
    for (const SettingsIssue& i : issues) {
        if (i.kind == kind) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("an empty text is a valid document with the current format and no sections") {
    std::vector<SettingsIssue> issues;
    SettingsDocument doc = parse("", &issues);
    CHECK(issues.empty());
    CHECK(doc.format() == kSettingsFormat);
    CHECK_FALSE(doc.read_only_recommended());
    CHECK(doc.section_names().empty());
    CHECK(doc.serialize().empty());
}

TEST_CASE("a canonical document serializes back to the same text") {
    const std::string text = "# toonval settings\n"
                             "format = 1.0\n"
                             "\n"
                             "[app]\n"
                             "hit_window_ms = 80\n"
                             "\n"
                             "[session]\n"
                             "song = Songs/Fur Elise.mid\n";
    CHECK(roundtrip(text) == text);
    CHECK(parse(text).section_names() == std::vector<std::string>{"app", "session"});
}

TEST_CASE("odd but legal text survives a round trip byte for byte") {
    // CRLF, no final newline, tabs, spaces around =, comments in sections, blank runs, a BOM,
    // junk lines, a header with blanks inside.
    const std::vector<std::string> texts = {
        "\xEF\xBB\xBF"
        "format = 1.0\r\n[app]\r\nhit_window_ms=80\r\n",
        "[app]\n\thit_window_ms\t=\t80  \n# c\n\n\n\nnot a pair\n[ session ]\nx = y",
        "no section at all\n= no key\nkey only\n",
        "[a]\n[b]\n[a]\n",
        "\n\n\n",
        "[unterminated\n[]\n[ ]\n]\n",
        std::string("[a]\nk = v\0w\n", 12),
    };
    for (const std::string& text : texts) {
        CAPTURE(text);
        CHECK(roundtrip(text) == text);
    }
}

TEST_CASE("a byte-order mark is skipped when reading and kept when saving") {
    SettingsDocument doc = parse("\xEF\xBB\xBF"
                                 "format = 1.2\n[app]\nk = v\n");
    CHECK(doc.format() == FormatVersion{1, 2});
    CHECK(doc.section("app").entries.size() == 1);
    CHECK(doc.serialize().rfind("\xEF\xBB\xBF", 0) == 0);
}

TEST_CASE("format versions: major.minor, a bare major, newer major, and garbage") {
    std::vector<SettingsIssue> issues;
    CHECK(parse("format = 1.0\n", &issues).format() == FormatVersion{1, 0});
    CHECK(issues.empty());
    CHECK(parse("format = 1\n", &issues).format() == FormatVersion{1, 0});
    CHECK(issues.empty());

    SettingsDocument newer_minor = parse("format = 1.7\n", &issues);
    CHECK(newer_minor.format() == FormatVersion{1, 7});
    CHECK_FALSE(newer_minor.read_only_recommended());
    CHECK(issues.empty());

    issues.clear();
    SettingsDocument newer = parse("format = 2.0\n[app]\nk = v\n", &issues);
    CHECK(newer.format() == FormatVersion{2, 0});
    CHECK(newer.read_only_recommended());
    CHECK(has_issue(issues, Kind::NewerVersion));
    CHECK(newer.section("app").entries.size() == 1); // still readable

    for (const char* bad : {"format = banana\n", "format = 1.x\n", "format = \n", "format = 0.9\n",
                            "format = -1.0\n", "format = 1.2.3\n", "format = 99999999999.0\n"}) {
        CAPTURE(bad);
        issues.clear();
        SettingsDocument doc = parse(bad, &issues);
        CHECK(has_issue(issues, Kind::BadVersion));
        CHECK(doc.format() == kSettingsFormat);
        CHECK_FALSE(doc.read_only_recommended());
    }
}

TEST_CASE("a format line inside a section is just a key") {
    std::vector<SettingsIssue> issues;
    SettingsDocument doc = parse("[app]\nformat = 9.9\n", &issues);
    CHECK(doc.format() == kSettingsFormat);
    CHECK(issues.empty());
}

TEST_CASE("section() lists keys and values with the blanks trimmed and the lines numbered") {
    SettingsDocument doc = parse("# top\n"
                                 "format = 1.0\n"
                                 "[ app ]\n"
                                 "  hit_window_ms\t=  80 \r\n"
                                 "# comment = skipped\n"
                                 "\n"
                                 "port = a = b # not a comment\n"
                                 "empty =\n");
    SettingsSection s = doc.section("app");
    REQUIRE(s.present);
    REQUIRE(s.entries.size() == 3);
    CHECK(s.entries[0].key == "hit_window_ms");
    CHECK(s.entries[0].value == "80");
    CHECK(s.entries[0].line == 4);
    CHECK(s.entries[1].key == "port");
    CHECK(s.entries[1].value == "a = b # not a comment");
    CHECK(s.entries[2].key == "empty");
    CHECK(s.entries[2].value.empty());
    CHECK(s.malformed_lines.empty());
    CHECK_FALSE(doc.section("nope").present);
}

TEST_CASE("the preamble is the section named empty") {
    SettingsDocument doc = parse("format = 1.0\nstray = 1\n[app]\nk = v\n");
    SettingsSection pre = doc.section("");
    REQUIRE(pre.present);
    CHECK(pre.entries.size() == 2);
}

TEST_CASE("keys and section names are case sensitive") {
    SettingsDocument doc = parse("[App]\nKey = 1\n");
    CHECK_FALSE(doc.has_section("app"));
    CHECK(doc.has_section("App"));
    CHECK(doc.section("App").entries[0].key == "Key");
}

TEST_CASE("lines that are not pairs are reported as malformed and kept") {
    const std::string text = "[app]\nnonsense\n= value without key\nok = 1\n[bad\n";
    SettingsDocument doc = parse(text);
    SettingsSection s = doc.section("app");
    CHECK(s.entries.size() == 1);
    CHECK(s.malformed_lines == std::vector<int>{2, 3, 5});
    CHECK(doc.serialize() == text);
}

TEST_CASE("a repeated key is listed each time") {
    SettingsDocument doc = parse("[app]\nk = 1\nk = 2\n");
    CHECK(doc.section("app").entries.size() == 2);
}

TEST_CASE("a second section of the same name is reported and left untouched") {
    std::vector<SettingsIssue> issues;
    const std::string text = "[app]\nk = 1\n[other]\nz = 1\n[app]\nk = 2\n";
    SettingsDocument doc = parse(text, &issues);
    CHECK(has_issue(issues, Kind::DuplicateSection));
    CHECK(doc.section_names() == std::vector<std::string>{"app", "other"});
    CHECK(doc.section("app").entries.size() == 1);
    doc.set("app", "k", "3");
    CHECK(doc.serialize() == "[app]\nk = 3\n[other]\nz = 1\n[app]\nk = 2\n");
}

TEST_CASE("set() rewrites the last line with that key in place, keeping the line ending") {
    SettingsDocument doc = parse("[app]\r\na = 1\r\nb = 2\r\na = 3\r\nc = 4\r\n");
    doc.set("app", "a", "9");
    CHECK(doc.serialize() == "[app]\r\na = 1\r\nb = 2\r\na = 9\r\nc = 4\r\n");
}

TEST_CASE("set() with the value already there leaves the line as typed") {
    const std::string text = "[app]\nhit_window_ms=80\nother =   x  \n";
    SettingsDocument doc = parse(text);
    doc.set("app", "hit_window_ms", "80");
    doc.set("app", "other", "x");
    CHECK(doc.serialize() == text);
}

TEST_CASE("set() appends a new key after the section's last content line") {
    SettingsDocument doc = parse("[app]\na = 1\n# tail comment\n\n\n[session]\nb = 2\n");
    doc.set("app", "c", "3");
    CHECK(doc.serialize() == "[app]\na = 1\n# tail comment\nc = 3\n\n\n[session]\nb = 2\n");
}

TEST_CASE("set() on a section whose last line has no newline adds one first") {
    SettingsDocument doc = parse("[app]\na = 1");
    doc.set("app", "b", "2");
    CHECK(doc.serialize() == "[app]\na = 1\nb = 2\n");
}

TEST_CASE("set() creates a missing section at the end, after a blank line") {
    SettingsDocument doc = parse("format = 1.0\n[app]\na = 1\n");
    doc.set("session", "x", "y");
    CHECK(doc.serialize() == "format = 1.0\n[app]\na = 1\n\n[session]\nx = y\n");
    SettingsDocument empty = parse("");
    empty.set("app", "a", "1");
    CHECK(empty.serialize() == "[app]\na = 1\n");
}

TEST_CASE("set() into the preamble goes before the first header") {
    SettingsDocument doc = parse("[app]\na = 1\n");
    doc.set("", "format", "1.0");
    CHECK(doc.serialize() == "format = 1.0\n[app]\na = 1\n");
}

TEST_CASE("an unknown section is not touched when other sections are edited") {
    const std::string unknown =
        "[from_the_future]\n# written by a newer build\nweird   =  1\n\nline without equals\n";
    SettingsDocument doc = parse("[app]\na = 1\n\n" + unknown + "\n[session]\nb = 2\n");
    doc.set("app", "a", "2");
    doc.set("session", "b", "3");
    doc.set("session", "c", "4");
    CHECK(doc.serialize() == "[app]\na = 2\n\n" + unknown + "\n[session]\nb = 3\nc = 4\n");
}

TEST_CASE("section_text returns the body and set_section_text replaces it") {
    SettingsDocument doc = parse("[look]\nversion = 1\nstyle = neon\n[app]\na = 1\n");
    CHECK(doc.section_text("look") == "version = 1\nstyle = neon\n");
    CHECK(doc.section_text("missing").empty());
    doc.set_section_text("look", "version = 1\nstyle = glass\n");
    CHECK(doc.serialize() == "[look]\nversion = 1\nstyle = glass\n[app]\na = 1\n");
    doc.set_section_text("user_presets", "slot_1_used = false");
    CHECK(doc.serialize() == "[look]\nversion = 1\nstyle = glass\n[app]\na = "
                             "1\n\n[user_presets]\nslot_1_used = false\n");
}

TEST_CASE("remove_section drops the header and body and nothing else") {
    SettingsDocument doc = parse("[a]\nx = 1\n[b]\ny = 2\n[c]\nz = 3\n");
    doc.remove_section("b");
    CHECK(doc.serialize() == "[a]\nx = 1\n[c]\nz = 3\n");
    doc.remove_section("nope");
    CHECK(doc.serialize() == "[a]\nx = 1\n[c]\nz = 3\n");
}

TEST_CASE("ensure_format adds the format line once and leaves an existing one alone") {
    SettingsDocument fresh = parse("");
    fresh.ensure_format();
    CHECK(fresh.serialize() == "# toonval settings\nformat = 1.0\n");
    fresh.ensure_format();
    CHECK(fresh.serialize() == "# toonval settings\nformat = 1.0\n");

    SettingsDocument no_format = parse("[app]\na = 1\n");
    no_format.ensure_format();
    CHECK(no_format.serialize() == "format = 1.0\n[app]\na = 1\n");

    SettingsDocument newer = parse("format = 2.3\n[app]\na = 1\n");
    newer.ensure_format();
    CHECK(newer.serialize() == "format = 2.3\n[app]\na = 1\n");
}

TEST_CASE("values are quoted only when blanks or a leading quote would be lost") {
    CHECK(settings_quote("Songs/Fur Elise.mid") == "Songs/Fur Elise.mid");
    CHECK(settings_quote("a # b = c") == "a # b = c");
    CHECK(settings_quote("C:\\music") == "C:\\music");
    CHECK(settings_quote("").empty());
    CHECK(settings_quote(" lead") == "\" lead\"");
    CHECK(settings_quote("trail ") == "\"trail \"");
    CHECK(settings_quote("\"quoted\"") == "\"\\\"quoted\\\"\"");
    CHECK(settings_quote("\ttab") == "\"\\ttab\"");
    CHECK(settings_quote("a\nb") == "\"a\\nb\"");
    CHECK(settings_quote(std::string("a\0b", 3)) == "\"a\\x00b\"");
}

TEST_CASE("settings_unquote reads plain and quoted values, and what quote wrote comes back") {
    std::string out;
    CHECK(settings_unquote("plain value", out));
    CHECK(out == "plain value");
    CHECK(settings_unquote("\"  spaced  \"", out));
    CHECK(out == "  spaced  ");
    CHECK(settings_unquote("\"a\\\\b\\\"c\\n\\r\\t\\x41\"", out));
    CHECK(out == "a\\b\"c\n\r\tA");
    CHECK(settings_unquote("\"\"", out));
    CHECK(out.empty());
    for (const char* s :
         {" lead", "trail ", "\"q\"", "\\", "a\"b", "\"", "plain", "\t", "a\nb", "\x01\x7f"}) {
        CAPTURE(s);
        std::string back;
        CHECK(settings_unquote(settings_quote(s), back));
        CHECK(back == s);
    }
}

TEST_CASE("settings_unquote refuses cut-off, badly escaped and trailing-garbage quotes") {
    std::string out = "keep";
    CHECK_FALSE(settings_unquote("\"open", out));
    CHECK_FALSE(settings_unquote("\"", out));
    CHECK_FALSE(settings_unquote("\"bad \\q escape\"", out));
    CHECK_FALSE(settings_unquote("\"bad \\x4\"", out));
    CHECK_FALSE(settings_unquote("\"bad \\xZZ\"", out));
    CHECK_FALSE(settings_unquote("\"done\" extra", out));
    CHECK_FALSE(settings_unquote("\"ends with backslash\\", out));
    CHECK(out == "keep");
}

TEST_CASE(
    "set_section_text keeps the blank lines between sections, and section_text leaves them out") {
    SettingsDocument doc = parse("[look]\nstyle = neon\n\n\n[app]\na = 1\n");
    CHECK(doc.section_text("look") == "style = neon\n");
    doc.set_section_text("look", "style = glass\nglow = 3");
    CHECK(doc.serialize() == "[look]\nstyle = glass\nglow = 3\n\n\n[app]\na = 1\n");
    // Reading the text and writing it back is a no-op.
    doc.set_section_text("look", doc.section_text("look"));
    CHECK(doc.serialize() == "[look]\nstyle = glass\nglow = 3\n\n\n[app]\na = 1\n");
}

TEST_CASE("a default-constructed document is an empty, fully usable document") {
    SettingsDocument doc;
    CHECK(doc.serialize().empty());
    CHECK(doc.section_names().empty());
    CHECK(doc.section_text("").empty());
    doc.set("app", "a", "1");
    CHECK(doc.serialize() == "[app]\na = 1\n");
    SettingsDocument fresh;
    fresh.ensure_format();
    CHECK(fresh.serialize() == "# toonval settings\nformat = 1.0\n");
}

TEST_CASE("a header at the very end with no line ending still gets its keys on their own lines") {
    SettingsDocument doc = parse("[app]\na = 1\n[session]");
    doc.set("session", "x", "y");
    CHECK(doc.serialize() == "[app]\na = 1\n[session]\nx = y\n");
    CHECK(parse(doc.serialize()).section("session").entries.size() == 1);

    SettingsDocument text_doc = parse("[a]\nk = 1\n[look]");
    text_doc.set_section_text("look", "style = neon");
    CHECK(text_doc.serialize() == "[a]\nk = 1\n[look]\nstyle = neon\n");

    SettingsDocument added = parse("[a]\nk = 1\n[look]");
    added.set("other", "z", "1");
    CHECK(added.serialize() == "[a]\nk = 1\n[look]\n[other]\nz = 1\n");
}
