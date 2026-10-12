// SPDX-License-Identifier: GPL-3.0-or-later
#include "song_library.hpp"

#include "toonval/content_hash.hpp"
#include "toonval/hand_split.hpp"
#include "toonval/midi_file.hpp"
#include "toonval/musicxml.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <system_error>

namespace toonval::app {

namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

// The path's text with '/' separators.
std::string generic(const fs::path& p) { return p.generic_string(); }

// `file` inside `root` as a relative '/' path, or empty.
std::string relative_to(const fs::path& file, const fs::path& root) {
    std::error_code ec;
    const fs::path f = fs::weakly_canonical(file, ec);
    if (ec) {
        return {};
    }
    const fs::path r = fs::weakly_canonical(root, ec);
    if (ec) {
        return {};
    }
    const fs::path rel = f.lexically_relative(r);
    if (rel.empty() || rel.is_absolute()) {
        return {};
    }
    const std::string text = generic(rel);
    if (text == "." || text.rfind("..", 0) == 0) {
        return {};
    }
    return text;
}

} // namespace

bool is_song_file_name(const fs::path& path) {
    static constexpr std::string_view kExtensions[] = {".mid", ".midi",     ".kar", ".smf",
                                                       ".rmi", ".musicxml", ".xml", ".mxl",
                                                       ".mscz", ".mscx"};
    const std::string ext = lower(path.extension().string());
    return std::find(std::begin(kExtensions), std::end(kExtensions), ext) != std::end(kExtensions);
}

bool read_song_bytes(const fs::path& path, std::vector<std::uint8_t>& out, std::string* error) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        if (error != nullptr) {
            *error = fs::exists(path, ec) ? "not a file" : "file not found";
        }
        return false;
    }
    const std::uintmax_t size = fs::file_size(path, ec);
    if (ec || size > kMaxSongFileBytes) {
        if (error != nullptr) {
            *error = ec ? "unreadable" : "too large for a song";
        }
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error != nullptr) {
            *error = "unreadable";
        }
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (in.bad()) {
        if (error != nullptr) {
            *error = "unreadable";
        }
        return false;
    }
    return true;
}

SongFileLoad load_song_bytes(std::span<const std::uint8_t> bytes, std::string_view file_name) {
    SongFileLoad out;
    const SongFileFormat format = sniff_song_format(bytes);
    Song song;
    if (format == SongFileFormat::Xml || format == SongFileFormat::Zip) {
        ScoreLoadResult r = load_score(bytes);
        if (!r.ok) {
            out.error = std::string(to_string(r.error));
            if (!r.message.empty()) {
                out.error += ": " + r.message;
            }
            return out;
        }
        out.entry = make_song_entry(r.song, bytes, file_name, r.title);
        song = std::move(r.song);
    } else {
        MidiLoadResult r = load_midi(bytes);
        if (!r.ok) {
            out.error = r.error == MidiFileError::NotMidi ? std::string("not a song file")
                                                          : std::string(to_string(r.error));
            return out;
        }
        out.entry = make_song_entry(r.song, bytes, file_name);
        song = std::move(r.song);
    }
    if (needs_hand_split(song)) {
        song = with_split_hands(song);
        out.song.hands_split = true;
    }
    out.song.song = std::move(song);
    out.song.title = out.entry.title;
    out.song.hash = content_hash(bytes.data(), bytes.size());
    out.song.display_path = std::string(file_name);
    out.ok = true;
    return out;
}

SongFileLoad load_song_file(const fs::path& path) {
    std::vector<std::uint8_t> bytes;
    std::string error;
    const std::string name = path.filename().string();
    if (!read_song_bytes(path, bytes, &error)) {
        SongFileLoad out;
        out.error = "Can't open " + name + ": " + error;
        return out;
    }
    SongFileLoad out = load_song_bytes(bytes, name);
    if (!out.ok) {
        out.error = "Can't open " + name + ": " + out.error;
    }
    return out;
}

SongLibrary::SongLibrary(std::vector<fs::path> roots) : roots_(std::move(roots)) {}

std::size_t SongLibrary::scan() {
    songs_.clear();
    unreadable_ = 0;
    std::vector<fs::path> seen;
    for (const fs::path& root : roots_) {
        std::error_code ec;
        if (!fs::is_directory(root, ec)) {
            continue;
        }
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied,
                                            ec);
        for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            const fs::path& p = it->path();
            if (!it->is_regular_file(ec) || !is_song_file_name(p)) {
                continue;
            }
            const std::string rel = relative_path(p);
            if (rel.empty() || resolve(rel) != std::optional<fs::path>(p)) {
                continue; // the same relative path in an earlier folder wins
            }
            std::vector<std::uint8_t> bytes;
            if (!read_song_bytes(p, bytes, nullptr)) {
                ++unreadable_;
                continue;
            }
            const LibraryLoad load = load_library_entry(bytes, p.filename().string());
            if (!load.ok) {
                ++unreadable_;
                continue;
            }
            songs_.push_back({p, rel, load.entry});
        }
    }
    std::vector<SongEntry> entries;
    entries.reserve(songs_.size());
    for (const LibrarySong& s : songs_) {
        entries.push_back(s.entry);
    }
    std::vector<LibrarySong> sorted;
    sorted.reserve(songs_.size());
    for (const std::size_t i : browse_library(entries)) {
        sorted.push_back(std::move(songs_[i]));
    }
    songs_ = std::move(sorted);
    return songs_.size();
}

std::string SongLibrary::relative_path(const fs::path& file) const {
    for (const fs::path& root : roots_) {
        std::string rel = relative_to(file, root);
        if (!rel.empty()) {
            return rel;
        }
    }
    return {};
}

std::optional<fs::path> SongLibrary::resolve(std::string_view relative) const {
    if (relative.empty()) {
        return std::nullopt;
    }
    const fs::path rel = fs::path(std::string(relative));
    if (rel.is_absolute()) {
        return std::nullopt;
    }
    for (const fs::path& part : rel) {
        if (part == "..") {
            return std::nullopt;
        }
    }
    for (const fs::path& root : roots_) {
        std::error_code ec;
        const fs::path candidate = root / rel;
        if (fs::is_regular_file(candidate, ec)) {
            return candidate;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> SongLibrary::index_of(const fs::path& file) const {
    std::error_code ec;
    const fs::path want = fs::weakly_canonical(file, ec);
    for (std::size_t i = 0; i < songs_.size(); ++i) {
        std::error_code ec2;
        if (fs::weakly_canonical(songs_[i].path, ec2) == want) {
            return i;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> SongLibrary::step(std::optional<std::size_t> current, int step) const {
    if (songs_.empty()) {
        return std::nullopt;
    }
    const auto n = static_cast<long>(songs_.size());
    if (!current || *current >= songs_.size()) {
        return std::size_t{0};
    }
    const long next = ((static_cast<long>(*current) + step) % n + n) % n;
    return static_cast<std::size_t>(next);
}

} // namespace toonval::app
