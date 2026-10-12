// SPDX-License-Identifier: GPL-3.0-or-later
// Song files on disk: reading one into a LoadedSong (MIDI, MusicXML, .mxl, .mscz, told apart by
// their bytes), and a minimal library of the songs in a few folders, for next/previous song and
// for the session's song path, which is kept relative to the library. A future song browser
// would replace the minimal part. No SDL: std::filesystem only, so the tests use it.
#pragma once

#include "song_controller.hpp"

#include "toonval/library.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace toonval::app {

// Larger files are not songs (the zip loaders have their own, larger, limits inside).
inline constexpr std::uintmax_t kMaxSongFileBytes = 64u * 1024 * 1024;

struct SongFileLoad {
    bool ok = false;
    LoadedSong song;   // library_path empty: the caller sets it
    SongEntry entry;   // title, length, range, difficulty
    std::string error; // one line for the status row when !ok
};

// Loads a song from its bytes; `file_name` gives the title when the file has none. Notes with no
// hand get the smart hand split. Never throws.
SongFileLoad load_song_bytes(std::span<const std::uint8_t> bytes, std::string_view file_name);
// Reads the file and loads it. Never throws.
SongFileLoad load_song_file(const std::filesystem::path& path);
// The whole file, or false (missing, unreadable, too large) with a short reason.
bool read_song_bytes(const std::filesystem::path& path, std::vector<std::uint8_t>& out,
                     std::string* error);

// True for the extensions a song file may have (.mid .midi .kar .smf .musicxml .xml .mxl .mscz
// .mscx, any case).
bool is_song_file_name(const std::filesystem::path& path);

struct LibrarySong {
    std::filesystem::path path; // absolute
    std::string relative;       // to its library folder, '/' separated
    SongEntry entry;
};

class SongLibrary {
  public:
    // The folders, first first: a relative path is looked up in each in this order.
    explicit SongLibrary(std::vector<std::filesystem::path> roots);

    // Reads every song file in the folders and their subfolders (missing folders are skipped),
    // sorted by title. Unreadable files are left out and counted. Returns the songs found.
    std::size_t scan();
    std::span<const LibrarySong> songs() const { return songs_; }
    int unreadable() const { return unreadable_; }
    const std::vector<std::filesystem::path>& roots() const { return roots_; }

    // `file` relative to the first folder that holds it, '/' separated; empty when none does.
    std::string relative_path(const std::filesystem::path& file) const;
    // The file a saved relative path names, in the first folder that has it.
    std::optional<std::filesystem::path> resolve(std::string_view relative) const;
    // Position of a file in songs(), by path.
    std::optional<std::size_t> index_of(const std::filesystem::path& file) const;
    // The song after (step 1) or before (step -1) `current`, wrapping; the first song when
    // `current` is not in the library. Empty when the library is.
    std::optional<std::size_t> step(std::optional<std::size_t> current, int step) const;

  private:
    std::vector<std::filesystem::path> roots_;
    std::vector<LibrarySong> songs_;
    int unreadable_ = 0;
};

} // namespace toonval::app
