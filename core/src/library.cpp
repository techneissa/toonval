// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/library.hpp"

#include "toonval/content_hash.hpp"
#include "toonval/hand_split.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace toonval {

namespace {

constexpr char32_t kReplacement = 0xFFFD;

// ---- UTF-8

// Decodes one code point at `i`; returns its length in bytes, or 0 when the bytes there are not
// well-formed UTF-8.
std::size_t decode_utf8(std::string_view s, std::size_t i, char32_t& out) {
    auto byte = [&](std::size_t k) { return static_cast<unsigned char>(s[k]); };
    unsigned char b0 = byte(i);
    if (b0 < 0x80) {
        out = b0;
        return 1;
    }
    std::size_t length = 0;
    char32_t cp = 0;
    char32_t min = 0;
    if (b0 >= 0xC2 && b0 <= 0xDF) {
        length = 2;
        cp = b0 & 0x1Fu;
        min = 0x80;
    } else if (b0 >= 0xE0 && b0 <= 0xEF) {
        length = 3;
        cp = b0 & 0x0Fu;
        min = 0x800;
    } else if (b0 >= 0xF0 && b0 <= 0xF4) {
        length = 4;
        cp = b0 & 0x07u;
        min = 0x10000;
    } else {
        return 0;
    }
    if (i + length > s.size()) {
        return 0;
    }
    for (std::size_t k = 1; k < length; ++k) {
        unsigned char b = byte(i + k);
        if ((b & 0xC0) != 0x80) {
            return 0;
        }
        cp = (cp << 6) | (b & 0x3Fu);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        return 0;
    }
    out = cp;
    return length;
}

void append_utf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// Code points of well-formed UTF-8 text.
std::u32string code_points(std::string_view valid_utf8) {
    std::u32string out;
    for (std::size_t i = 0; i < valid_utf8.size();) {
        char32_t cp = kReplacement;
        std::size_t n = decode_utf8(valid_utf8, i, cp);
        out.push_back(n == 0 ? kReplacement : cp);
        i += n == 0 ? 1 : n;
    }
    return out;
}

// ---- Legacy encodings

// Windows-1252 for 0x80..0x9F; 0 where the encoding leaves the byte undefined.
constexpr std::array<char32_t, 32> kCp1252High = {
    0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
    0x2039, 0x0152, 0,      0x017D, 0,      0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178};

std::string from_windows_1252(std::string_view bytes) {
    std::string out;
    for (char c : bytes) {
        unsigned char b = static_cast<unsigned char>(c);
        char32_t cp = b;
        if (b >= 0x80 && b <= 0x9F) {
            cp = kCp1252High[b - 0x80];
            if (cp == 0) {
                cp = kReplacement;
            }
        }
        append_utf8(out, cp);
    }
    return out;
}

bool sjis_lead(unsigned char b) { return (b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC); }
bool sjis_trail(unsigned char b) { return (b >= 0x40 && b <= 0x7E) || (b >= 0x80 && b <= 0xFC); }

// What a Shift-JIS pair is, when it is one of the groups core can decode without a kanji table.
char32_t decode_sjis_pair(unsigned char lead, unsigned char trail) {
    unsigned code = (static_cast<unsigned>(lead) << 8) | trail;
    if (code == 0x8140) {
        return 0x3000; // ideographic space
    }
    if (code == 0x8141) {
        return 0x3001;
    }
    if (code == 0x8142) {
        return 0x3002;
    }
    if (code == 0x815B) {
        return 0x30FC; // prolonged sound mark
    }
    if (code >= 0x824F && code <= 0x8258) {
        return 0xFF10 + (code - 0x824F); // full-width digits
    }
    if (code >= 0x8260 && code <= 0x8279) {
        return 0xFF21 + (code - 0x8260); // full-width capitals
    }
    if (code >= 0x8281 && code <= 0x829A) {
        return 0xFF41 + (code - 0x8281); // full-width small letters
    }
    if (code >= 0x829F && code <= 0x82F1) {
        return 0x3041 + (code - 0x829F); // hiragana
    }
    if (code >= 0x8340 && code <= 0x8396 && trail != 0x7F) {
        return 0x30A1 + (code - 0x8340) - (trail > 0x7F ? 1 : 0); // katakana
    }
    return 0;
}

// Decodes `bytes` as Shift-JIS when every high byte is part of it and at least one is decodable
// kana or full-width Latin; false otherwise.
bool decode_shift_jis(std::string_view bytes, std::string& out) {
    std::string result;
    bool evidence = false;
    for (std::size_t i = 0; i < bytes.size();) {
        unsigned char b = static_cast<unsigned char>(bytes[i]);
        if (b < 0x80) {
            result.push_back(static_cast<char>(b));
            ++i;
        } else if (b >= 0xA1 && b <= 0xDF) {
            append_utf8(result, 0xFF61 + (b - 0xA1u)); // half-width katakana
            evidence = true;
            ++i;
        } else if (sjis_lead(b) && i + 1 < bytes.size() &&
                   sjis_trail(static_cast<unsigned char>(bytes[i + 1]))) {
            char32_t cp = decode_sjis_pair(b, static_cast<unsigned char>(bytes[i + 1]));
            evidence = evidence || cp != 0;
            append_utf8(result, cp != 0 ? cp : kReplacement);
            i += 2;
        } else {
            return false;
        }
    }
    if (!evidence) {
        return false;
    }
    out = std::move(result);
    return true;
}

// ---- Search folding

// For U+00C0..U+00FF: the folded letter; '0' keeps the character, '1' is "ae", '2' "th", '3' "ss".
constexpr std::string_view kLatin1Fold = "aaaaaa1ceeeeiiiidnooooo0ouuuuy23"
                                         "aaaaaa1ceeeeiiiidnooooo0ouuuuy2y";
// For U+0100..U+017F; '4' is "oe", '5' is "ij".
constexpr std::string_view kLatinAFold = "aaaaaa"       // 0100-0105
                                         "cccccccc"     // 0106-010D
                                         "dddd"         // 010E-0111
                                         "eeeeeeeeee"   // 0112-011B
                                         "gggggggg"     // 011C-0123
                                         "hhhh"         // 0124-0127
                                         "iiiiiiiiii"   // 0128-0131
                                         "55"           // 0132-0133
                                         "jj"           // 0134-0135
                                         "kkk"          // 0136-0138
                                         "llllllllll"   // 0139-0142
                                         "nnnnnnnnn"    // 0143-014B
                                         "oooooo"       // 014C-0151
                                         "44"           // 0152-0153
                                         "rrrrrr"       // 0154-0159
                                         "ssssssss"     // 015A-0161
                                         "tttttt"       // 0162-0167
                                         "uuuuuuuuuuuu" // 0168-0173
                                         "ww"           // 0174-0175
                                         "yyy"          // 0176-0178
                                         "zzzzzz"       // 0179-017E
                                         "s";           // 017F
static_assert(kLatin1Fold.size() == 64);
static_assert(kLatinAFold.size() == 128);

void append_folded_letter(std::string& out, char code, char32_t original) {
    switch (code) {
    case '0':
        append_utf8(out, original);
        break;
    case '1':
        out += "ae";
        break;
    case '2':
        out += "th";
        break;
    case '3':
        out += "ss";
        break;
    case '4':
        out += "oe";
        break;
    case '5':
        out += "ij";
        break;
    default:
        out.push_back(code);
        break;
    }
}

void append_folded(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out.push_back(cp >= 'A' && cp <= 'Z' ? static_cast<char>(cp + 32) : static_cast<char>(cp));
    } else if (cp >= 0xC0 && cp <= 0xFF) {
        append_folded_letter(out, kLatin1Fold[cp - 0xC0], cp);
    } else if (cp >= 0x100 && cp <= 0x17F) {
        append_folded_letter(out, kLatinAFold[cp - 0x100], cp);
    } else if (cp >= 0x300 && cp <= 0x36F) {
        // combining mark: dropped
    } else if (cp >= 0x391 && cp <= 0x3A9 && cp != 0x3A2) {
        append_utf8(out, cp + 0x20); // Greek capitals
    } else if (cp >= 0x410 && cp <= 0x42F) {
        append_utf8(out, cp + 0x20); // Cyrillic capitals
    } else if (cp >= 0x400 && cp <= 0x40F) {
        append_utf8(out, cp + 0x50);
    } else if (cp >= 0xFF21 && cp <= 0xFF3A) {
        out.push_back(static_cast<char>('a' + (cp - 0xFF21)));
    } else if (cp >= 0xFF41 && cp <= 0xFF5A) {
        out.push_back(static_cast<char>('a' + (cp - 0xFF41)));
    } else if (cp >= 0xFF10 && cp <= 0xFF19) {
        out.push_back(static_cast<char>('0' + (cp - 0xFF10)));
    } else if (cp == kReplacement) {
        // dropped
    } else {
        append_utf8(out, cp);
    }
}

// ---- Titles

bool is_space_or_control(unsigned char c) { return c <= 0x20 || c == 0x7F; }

// White space and control characters collapse to one space; the ends are trimmed.
std::string collapse_spaces(std::string_view s) {
    std::string out;
    bool pending_space = false;
    for (char ch : s) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (is_space_or_control(c)) {
            pending_space = !out.empty();
        } else {
            if (pending_space) {
                out.push_back(' ');
                pending_space = false;
            }
            out.push_back(ch);
        }
    }
    return out;
}

std::string cap_utf8(std::string s, std::size_t max_bytes) {
    if (s.size() <= max_bytes) {
        return s;
    }
    std::size_t cut = max_bytes;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    s.resize(cut);
    while (!s.empty() && s.back() == ' ') {
        s.pop_back();
    }
    return s;
}

std::string clean_text(std::string_view valid_utf8) {
    std::string underscores(valid_utf8);
    std::replace(underscores.begin(), underscores.end(), '_', ' ');
    return cap_utf8(collapse_spaces(underscores), kMaxLibraryTitleBytes);
}

bool has_known_extension(std::string_view name, std::size_t& dot) {
    static constexpr std::array<std::string_view, 10> kExtensions = {
        "mid", "midi", "kar", "smf", "rmi", "musicxml", "xml", "mxl", "mscz", "mscx"};
    dot = name.rfind('.');
    if (dot == std::string_view::npos) {
        return false;
    }
    std::string ext;
    for (char c : name.substr(dot + 1)) {
        ext.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c);
    }
    return std::find(kExtensions.begin(), kExtensions.end(), ext) != kExtensions.end();
}

// A title that has a letter or digit of some script (not only punctuation or U+FFFD).
bool usable_title(std::string_view title) {
    for (char32_t cp : code_points(title)) {
        if ((cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') ||
            (cp >= 0xC0 && cp != kReplacement && !(cp >= 0x2000 && cp <= 0x206F) &&
             !(cp >= 0x3000 && cp <= 0x303F) && !(cp >= 0xFF00 && cp <= 0xFF0F))) {
            return true;
        }
    }
    return false;
}

bool generic_title(std::string_view title) {
    std::string key = fold_for_search(title);
    static constexpr std::array<std::string_view, 15> kGeneric = {
        "untitled",      "unnamed",  "noname",  "no name", "no title",
        "new song",      "newsong",  "song",    "midi",    "track",
        "control track", "sequence", "default", "music",   "piano"};
    if (std::find(kGeneric.begin(), kGeneric.end(), key) != kGeneric.end()) {
        return true;
    }
    auto all_digits = [](std::string_view s) {
        return !s.empty() &&
               std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
    };
    if (all_digits(key)) {
        return true;
    }
    for (std::string_view prefix : {"track ", "midi ", "song "}) {
        if (key.starts_with(prefix) && all_digits(std::string_view(key).substr(prefix.size()))) {
            return true;
        }
    }
    return false;
}

// ---- MIDI sequence name

class Reader {
  public:
    explicit Reader(std::span<const std::uint8_t> data) : data_(data) {}
    std::size_t pos() const { return pos_; }
    bool at_end() const { return pos_ >= data_.size(); }
    bool skip(std::size_t n) {
        if (n > data_.size() - pos_) {
            return false;
        }
        pos_ += n;
        return true;
    }
    bool u8(std::uint8_t& v) {
        if (at_end()) {
            return false;
        }
        v = data_[pos_++];
        return true;
    }
    bool be32(std::uint32_t& v) {
        if (data_.size() - pos_ < 4) {
            return false;
        }
        v = (std::uint32_t{data_[pos_]} << 24) | (std::uint32_t{data_[pos_ + 1]} << 16) |
            (std::uint32_t{data_[pos_ + 2]} << 8) | data_[pos_ + 3];
        pos_ += 4;
        return true;
    }
    bool vlq(std::uint32_t& v) {
        v = 0;
        for (int i = 0; i < 4; ++i) {
            std::uint8_t b = 0;
            if (!u8(b)) {
                return false;
            }
            v = (v << 7) | (b & 0x7Fu);
            if ((b & 0x80) == 0) {
                return true;
            }
        }
        return false;
    }
    std::span<const std::uint8_t> take(std::size_t n) {
        if (n > data_.size() - pos_) {
            return {};
        }
        auto out = data_.subspan(pos_, n);
        pos_ += n;
        return out;
    }
    std::span<const std::uint8_t> rest() const { return data_.subspan(pos_); }

  private:
    std::span<const std::uint8_t> data_;
    std::size_t pos_ = 0;
};

} // namespace

// ---- Public text functions

bool is_valid_utf8(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        char32_t cp = 0;
        std::size_t n = decode_utf8(text, i, cp);
        if (n == 0) {
            return false;
        }
        i += n;
    }
    return true;
}

std::string utf8_from_midi_text(std::string_view bytes) {
    if (is_valid_utf8(bytes)) {
        return std::string(bytes);
    }
    std::string decoded;
    if (decode_shift_jis(bytes, decoded)) {
        return decoded;
    }
    return from_windows_1252(bytes);
}

std::string title_from_file_name(std::string_view file_name) {
    std::string text = utf8_from_midi_text(file_name);
    std::string_view name = text;
    std::size_t slash = name.find_last_of("/\\");
    if (slash != std::string_view::npos) {
        name.remove_prefix(slash + 1);
    }
    std::size_t dot = 0;
    if (has_known_extension(name, dot)) {
        name = name.substr(0, dot);
    }
    return clean_text(name);
}

std::string midi_sequence_name(std::span<const std::uint8_t> bytes) {
    Reader r(bytes);
    // Header chunk.
    if (r.rest().size() < 4 ||
        std::string_view(reinterpret_cast<const char*>(r.rest().data()), 4) != "MThd") {
        return {};
    }
    r.skip(4);
    std::uint32_t header_length = 0;
    if (!r.be32(header_length) || !r.skip(header_length)) {
        return {};
    }
    // First track chunk (unknown chunks are skipped).
    for (;;) {
        if (r.rest().size() < 8) {
            return {};
        }
        std::string_view id(reinterpret_cast<const char*>(r.rest().data()), 4);
        r.skip(4);
        std::uint32_t length = 0;
        if (!r.be32(length)) {
            return {};
        }
        std::span<const std::uint8_t> body = r.take(std::min<std::size_t>(length, r.rest().size()));
        if (id != "MTrk") {
            if (body.size() < length) {
                return {};
            }
            continue;
        }
        Reader t(body);
        std::uint8_t running = 0;
        while (!t.at_end()) {
            std::uint32_t delta = 0;
            std::uint8_t status = 0;
            if (!t.vlq(delta) || !t.u8(status)) {
                return {};
            }
            if (status < 0x80) { // running status: the byte just read is data
                if (running == 0) {
                    return {};
                }
                bool one_data_byte = (running & 0xF0) == 0xC0 || (running & 0xF0) == 0xD0;
                if (!one_data_byte && !t.skip(1)) {
                    return {};
                }
            } else if (status == 0xFF) {
                running = 0;
                std::uint8_t type = 0;
                std::uint32_t meta_length = 0;
                if (!t.u8(type) || !t.vlq(meta_length)) {
                    return {};
                }
                std::span<const std::uint8_t> payload = t.take(meta_length);
                if (payload.size() < meta_length) {
                    return {};
                }
                if (type == 0x03) {
                    return std::string(reinterpret_cast<const char*>(payload.data()),
                                       payload.size());
                }
                if (type == 0x2F) {
                    return {};
                }
            } else if (status == 0xF0 || status == 0xF7) {
                running = 0;
                std::uint32_t sysex_length = 0;
                if (!t.vlq(sysex_length) || !t.skip(sysex_length)) {
                    return {};
                }
            } else if (status < 0xF0) {
                running = status;
                bool one_data_byte = (status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0;
                if (!t.skip(one_data_byte ? 1 : 2)) {
                    return {};
                }
            } else {
                return {};
            }
        }
        return {};
    }
}

std::string choose_title(std::string_view file_name, std::string_view sequence_name) {
    std::string from_file = title_from_file_name(file_name);
    std::string from_sequence = clean_text(utf8_from_midi_text(sequence_name));
    bool file_ok = usable_title(from_file);
    bool sequence_ok = usable_title(from_sequence);
    if (file_ok && !generic_title(from_file)) {
        return from_file;
    }
    if (sequence_ok && !generic_title(from_sequence)) {
        return from_sequence;
    }
    if (file_ok) {
        return from_file;
    }
    if (sequence_ok) {
        return from_sequence;
    }
    return "Untitled";
}

std::string fold_for_search(std::string_view text) {
    std::string valid = utf8_from_midi_text(text);
    std::string out;
    for (char32_t cp : code_points(valid)) {
        append_folded(out, cp);
    }
    return out;
}

bool matches_search(std::string_view folded_title, std::string_view query) {
    std::string folded_query = fold_for_search(query);
    std::string_view rest = folded_query;
    while (!rest.empty()) {
        while (!rest.empty() && is_space_or_control(static_cast<unsigned char>(rest.front()))) {
            rest.remove_prefix(1);
        }
        std::size_t end = 0;
        while (end < rest.size() && !is_space_or_control(static_cast<unsigned char>(rest[end]))) {
            ++end;
        }
        if (end > 0 && folded_title.find(rest.substr(0, end)) == std::string_view::npos) {
            return false;
        }
        rest.remove_prefix(end);
    }
    return true;
}

// ---- Difficulty

namespace {

double clamp01(double v) { return std::clamp(v, 0.0, 1.0); }

DifficultyFactors compute_factors(const Song& song) {
    DifficultyFactors f;
    int onsets = 0;
    double first_onset = 0.0;
    double last_onset = 0.0;
    int groups = 0;
    int notes = 0;
    int chord_groups = 0;
    double span_sum = 0.0;
    int lowest = 128;
    int highest = -1;

    struct HandGroup {
        int count = 0;
        int low = 128;
        int high = -1;
    };
    std::array<HandGroup, 3> window{};
    auto close_window = [&]() {
        for (HandGroup& g : window) {
            if (g.count > 0) {
                ++groups;
                notes += g.count;
                if (g.count >= 2) {
                    ++chord_groups;
                    span_sum += g.high - g.low;
                }
            }
            g = HandGroup{};
        }
    };
    double window_start = 0.0;
    bool open = false;
    for (const Note& n : song.notes) {
        if (n.drum) {
            continue;
        }
        if (!open || n.start_s - window_start > kChordWindowSeconds) {
            close_window();
            window_start = n.start_s;
            open = true;
            if (onsets == 0) {
                first_onset = n.start_s;
            }
            ++onsets;
            last_onset = n.start_s;
        }
        HandGroup& g = window[static_cast<std::size_t>(n.hand)];
        ++g.count;
        g.low = std::min<int>(g.low, n.pitch);
        g.high = std::max<int>(g.high, n.pitch);
        lowest = std::min<int>(lowest, n.pitch);
        highest = std::max<int>(highest, n.pitch);
    }
    close_window();

    double span = last_onset - first_onset;
    if (onsets >= 2 && span > 1e-3) {
        f.onsets_per_second = (onsets - 1) / span;
    }
    if (groups > 0) {
        f.notes_per_onset = static_cast<double>(notes) / groups;
    }
    if (chord_groups > 0) {
        f.mean_chord_span = span_sum / chord_groups;
    }
    if (highest >= 0) {
        f.pitch_range = highest - lowest;
    }
    return f;
}

} // namespace

int difficulty_level(const DifficultyFactors& f) {
    double score = 0.40 * clamp01((f.onsets_per_second - 0.5) / 6.5) +
                   0.25 * clamp01((f.notes_per_onset - 1.0) / 2.5) +
                   0.20 * clamp01(f.mean_chord_span / 14.0) +
                   0.15 * clamp01((f.pitch_range - 12.0) / 48.0);
    return 1 + std::min(4, static_cast<int>(std::floor(5.0 * score)));
}

// ---- Keyboard fit

KeyboardFit fit_to_keyboard(int lowest, int highest, KeyRange range) {
    KeyboardFit out;
    if (lowest < 0 || highest < lowest) {
        return out;
    }
    // A shift s fits when lowest + s >= range.lowest and highest + s <= range.highest.
    int low = std::max(range.lowest - lowest, -kMaxTransposeSemitones);
    int high = std::min(range.highest - highest, kMaxTransposeSemitones);
    if (low > high) {
        out.fit = KeyFit::TooWide;
    } else if (low > 0) {
        out.fit = KeyFit::FitsWithShift;
        out.semitones = low;
    } else if (high < 0) {
        out.fit = KeyFit::FitsWithShift;
        out.semitones = high;
    }
    return out;
}

// ---- Entries

namespace {

SongEntry build_entry(const Song& song, std::span<const std::uint8_t> bytes,
                      std::string_view file_name, std::string_view sequence_name) {
    SongEntry e;
    e.hash = content_hash(bytes.data(), bytes.size());
    e.title = choose_title(file_name, sequence_name);
    e.title_key = fold_for_search(e.title);
    e.duration_s = song.duration_s;
    e.bar_count = static_cast<int>(song.measures.size());
    int lowest = 128;
    int highest = -1;
    for (const Note& n : song.notes) {
        if (n.drum) {
            continue;
        }
        ++e.note_count;
        (n.hand == Hand::Left    ? e.left_notes
         : n.hand == Hand::Right ? e.right_notes
                                 : e.unknown_notes) += 1;
        lowest = std::min<int>(lowest, n.pitch);
        highest = std::max<int>(highest, n.pitch);
    }
    if (highest >= 0) {
        e.lowest_pitch = lowest;
        e.highest_pitch = highest;
    }
    e.factors = compute_factors(song);
    e.difficulty = difficulty_level(e.factors);
    return e;
}

} // namespace

SongEntry make_song_entry(const Song& song, std::span<const std::uint8_t> bytes,
                          std::string_view file_name) {
    return build_entry(song, bytes, file_name, midi_sequence_name(bytes));
}

SongEntry make_song_entry(const Song& song, std::span<const std::uint8_t> bytes,
                          std::string_view file_name, std::string_view score_title) {
    return build_entry(song, bytes, file_name, score_title);
}

LibraryLoad load_library_entry(std::span<const std::uint8_t> bytes, std::string_view file_name) {
    LibraryLoad out;
    out.format = sniff_song_format(bytes);
    if (out.format == SongFileFormat::Xml || out.format == SongFileFormat::Zip) {
        ScoreLoadResult r = load_score(bytes);
        out.error = MidiFileError::NotMidi;
        out.message = r.message;
        out.score_error = r.error;
        if (!r.ok) {
            return out;
        }
        out.error = MidiFileError::None;
        out.ok = true;
        out.entry = build_entry(r.song, bytes, file_name, r.title);
        return out;
    }
    MidiLoadResult r = load_midi(bytes);
    out.error = r.error;
    out.message = r.message;
    if (!r.ok) {
        return out;
    }
    out.ok = true;
    out.entry = make_song_entry(r.song, bytes, file_name);
    return out;
}

void attach_history(SongEntry& entry, std::span<const RunRecord> runs) {
    entry.has_best = false;
    entry.best = BestEntry{};
    entry.run_count = 0;
    entry.last_run_index = -1;
    entry.last_run_day = 0;
    if (entry.hash == 0) {
        return;
    }
    for (std::size_t i : unique_run_indices(runs)) {
        if (runs[i].song_hash == entry.hash) {
            ++entry.run_count;
            entry.last_run_index = static_cast<int>(i);
            entry.last_run_day = runs[i].local_day;
        }
    }
    for (const BestEntry& b : song_bests(runs, entry.hash)) {
        if (!entry.has_best || b.accuracy > entry.best.accuracy ||
            (b.accuracy == entry.best.accuracy && b.speed_percent > entry.best.speed_percent)) {
            entry.best = b;
            entry.has_best = true;
        }
    }
}

// ---- Browsing

bool matches_filter(const SongEntry& entry, const LibraryFilter& filter) {
    if (entry.difficulty < filter.min_difficulty || entry.difficulty > filter.max_difficulty) {
        return false;
    }
    if (filter.played == PlayedFilter::PlayedOnly && !entry.played()) {
        return false;
    }
    if (filter.played == PlayedFilter::UnplayedOnly && entry.played()) {
        return false;
    }
    if (filter.keyboard != KeyboardFilter::Any) {
        KeyFit fit = fit_to_keyboard(entry, filter.range).fit;
        if (fit == KeyFit::TooWide ||
            (filter.keyboard == KeyboardFilter::FitsAsIs && fit != KeyFit::Fits)) {
            return false;
        }
    }
    return matches_search(entry.title_key, filter.text);
}

std::vector<std::size_t> browse_library(std::span<const SongEntry> entries,
                                        const LibraryFilter& filter, const LibrarySort& sort) {
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (matches_filter(entries[i], filter)) {
            order.push_back(i);
        }
    }
    auto by_title = [&](std::size_t a, std::size_t b) {
        const SongEntry& x = entries[a];
        const SongEntry& y = entries[b];
        if (x.title_key != y.title_key) {
            return x.title_key < y.title_key;
        }
        if (x.hash != y.hash) {
            return x.hash < y.hash;
        }
        return a < b;
    };
    auto primary = [&](std::size_t a, std::size_t b) {
        const SongEntry& x = entries[a];
        const SongEntry& y = entries[b];
        switch (sort.key) {
        case SortKey::Title:
            return 0;
        case SortKey::Difficulty:
            return x.difficulty < y.difficulty ? -1 : x.difficulty > y.difficulty ? 1 : 0;
        case SortKey::Length:
            return x.duration_s < y.duration_s ? -1 : x.duration_s > y.duration_s ? 1 : 0;
        case SortKey::RecentlyPlayed:
            return x.last_run_index < y.last_run_index   ? -1
                   : x.last_run_index > y.last_run_index ? 1
                                                         : 0;
        }
        return 0;
    };
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        if (sort.key == SortKey::Title) {
            return sort.descending ? by_title(b, a) : by_title(a, b);
        }
        if (sort.key == SortKey::RecentlyPlayed) {
            bool a_never = entries[a].last_run_index < 0;
            bool b_never = entries[b].last_run_index < 0;
            if (a_never != b_never) {
                return b_never; // played songs first
            }
            if (a_never) {
                return by_title(a, b);
            }
        }
        int c = primary(a, b);
        if (c != 0) {
            return sort.descending ? c > 0 : c < 0;
        }
        return by_title(a, b);
    });
    return order;
}

} // namespace toonval
