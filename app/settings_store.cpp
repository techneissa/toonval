// SPDX-License-Identifier: GPL-3.0-or-later
#include "settings_store.hpp"

#include "toonval/settings_doc.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace toonval::app {

namespace fs = std::filesystem;

bool settings_text_usable(std::string_view text) {
    if (text.empty() || text.size() > kMaxSettingsBytes ||
        text.find('\0') != std::string_view::npos) {
        return false;
    }
    const SettingsDocument doc = SettingsDocument::parse(text);
    std::vector<std::string> names = doc.section_names();
    bool structured = !names.empty();           // at least one [section] header
    names.insert(names.begin(), std::string()); // "" is the preamble
    std::size_t good = 0;
    std::size_t bad = 0;
    for (const std::string& name : names) {
        const SettingsSection section = doc.section(name);
        good += section.entries.size();
        bad += section.malformed_lines.size();
        if (name.empty()) {
            structured =
                structured || std::any_of(section.entries.begin(), section.entries.end(),
                                          [](const SettingsEntry& e) { return e.key == "format"; });
        }
    }
    return structured && good > 0 && bad <= good;
}

namespace {

enum class ReadStatus { Ok, Missing, Unreadable, TooLarge };

ReadStatus read_file(const fs::path& path, std::string& out) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        return fs::exists(path, ec) ? ReadStatus::Unreadable : ReadStatus::Missing;
    }
    const std::uintmax_t size = fs::file_size(path, ec);
    if (ec) {
        return ReadStatus::Unreadable;
    }
    if (size > kMaxSettingsBytes) {
        return ReadStatus::TooLarge;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return ReadStatus::Unreadable;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return in.bad() ? ReadStatus::Unreadable : ReadStatus::Ok;
}

// Writes `text` to `path` and forces it to disk. False on any failure.
bool write_synced(const fs::path& path, std::string_view text) {
#if defined(_WIN32)
    std::FILE* f = std::fopen(path.string().c_str(), "wb");
    if (f == nullptr) {
        return false;
    }
    bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size() && std::fflush(f) == 0;
    ok = ok && _commit(_fileno(f)) == 0;
    return std::fclose(f) == 0 && ok;
#else
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return false;
    }
    bool ok = true;
    std::size_t done = 0;
    while (ok && done < text.size()) {
        const ssize_t n = ::write(fd, text.data() + done, text.size() - done);
        if (n <= 0) {
            ok = false;
        } else {
            done += static_cast<std::size_t>(n);
        }
    }
    ok = ok && ::fsync(fd) == 0;
    return ::close(fd) == 0 && ok;
#endif
}

// Makes the renames durable. Best effort: some filesystems refuse to fsync a folder.
void sync_directory(const fs::path& dir) {
#if !defined(_WIN32)
    const int fd = ::open(dir.c_str(), O_RDONLY);
    if (fd >= 0) {
        (void)::fsync(fd);
        ::close(fd);
    }
#else
    (void)dir;
#endif
}

} // namespace

SettingsStore::SettingsStore(fs::path directory) : directory_(std::move(directory)) {}

LoadResult SettingsStore::load() {
    LoadResult result;
    std::string text;
    const ReadStatus main_status = read_file(main_path(), text);
    if (main_status == ReadStatus::Ok && settings_text_usable(text)) {
        result.source = LoadSource::Main;
        result.text = std::move(text);
        last_text_ = result.text;
        have_last_ = true;
        return result;
    }
    result.main_damaged = main_status != ReadStatus::Missing;

    std::string backup;
    const ReadStatus backup_status = read_file(backup_path(), backup);
    const bool backup_ok = backup_status == ReadStatus::Ok && settings_text_usable(backup);
    if (backup_ok) {
        result.source = LoadSource::Backup;
        result.text = std::move(backup);
        result.message = result.main_damaged
                             ? "Settings file was damaged. Restored the previous version."
                             : "Settings file was missing. Restored the previous version.";
        have_last_ = false; // main is not what we loaded: the next save must not skip
        return result;
    }
    if (result.main_damaged || backup_status != ReadStatus::Missing) {
        result.message = backup_status == ReadStatus::Missing
                             ? "Settings file was damaged. Using the defaults."
                             : "Settings file and its backup were damaged. Using the defaults.";
    }
    have_last_ = false;
    return result;
}

SaveResult SettingsStore::save(std::string_view text) {
    SaveResult result;
    if (text.size() > kMaxSettingsBytes) {
        result.error = "settings are too large to save";
        return result;
    }
    if (have_last_ && text == last_text_) {
        result.ok = true;
        return result;
    }
    std::error_code ec;
    fs::create_directories(directory_, ec);
    if (ec) {
        result.error = "cannot create the settings folder";
        return result;
    }
    const fs::path tmp = temp_path();
    if (!write_synced(tmp, text)) {
        fs::remove(tmp, ec);
        result.error = "cannot write the settings file";
        return result;
    }

    // The current file becomes the backup, but only when it is usable: a damaged one must not
    // push out a good backup. The backup is made under a temporary name and renamed, so it is
    // never half written; settings.conf itself is untouched until the final rename.
    std::string current;
    const bool current_usable =
        read_file(main_path(), current) == ReadStatus::Ok && settings_text_usable(current);
    if (current_usable) {
        const fs::path bak_tmp = directory_ / (std::string(kSettingsBackupName) + ".tmp");
        if (write_synced(bak_tmp, current)) {
            fs::rename(bak_tmp, backup_path(), ec);
            if (ec) {
                fs::remove(bak_tmp, ec);
            }
        } else {
            fs::remove(bak_tmp, ec);
        }
    }

    // A damaged settings.conf is about to be replaced: keep it as settings.conf.damaged (one copy)
    // for whoever wants to look at what went wrong.
    if (!current_usable && fs::exists(main_path(), ec)) {
        fs::rename(main_path(), damaged_path(), ec);
    }
    fs::rename(tmp, main_path(), ec);
    if (ec) {
        fs::remove(tmp, ec);
        result.error = "cannot replace the settings file";
        return result;
    }
    sync_directory(directory_);
    last_text_.assign(text);
    have_last_ = true;
    result.ok = true;
    return result;
}

} // namespace toonval::app
