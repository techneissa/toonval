// SPDX-License-Identifier: GPL-3.0-or-later
// The settings file on disk: where it lives, how it is written so a power cut cannot lose it, and
// how it is read back when it is missing or damaged. Only text goes through here; what the text
// means is core's job (toonval::load_settings and the look serializer). The folder is a
// constructor argument, so the tests point it at a temporary directory.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace toonval::app {

inline constexpr std::string_view kSettingsFileName = "settings.conf";
inline constexpr std::string_view kSettingsTempName = "settings.conf.tmp";
inline constexpr std::string_view kSettingsBackupName = "settings.conf.bak";
inline constexpr std::string_view kSettingsDamagedName = "settings.conf.damaged";

// True when `text` is plausibly a settings file: not empty, no NUL bytes, no more than
// kMaxSettingsBytes, and it has `key = value` lines (a `format` key or a section header) with
// at most as many malformed lines as good ones. Truncated and garbled files fail this; a file from
// a newer build with unknown keys passes. Wrong *values* are not this function's business: core
// clamps them field by field.
bool settings_text_usable(std::string_view text);

enum class LoadSource : std::uint8_t {
    Main,   // settings.conf was read
    Backup, // settings.conf was missing or damaged, settings.conf.bak was read
    None,   // nothing usable: the caller starts from the defaults
};

struct LoadResult {
    LoadSource source = LoadSource::None;
    std::string text; // empty for None
    // Short English sentence for the status line; empty when nothing needs saying (a normal load,
    // or a first start with no file at all).
    std::string message;
    bool main_damaged = false; // settings.conf exists but could not be used
};

struct SaveResult {
    bool ok = false;
    std::string error; // short reason when !ok
};

class SettingsStore {
  public:
    explicit SettingsStore(std::filesystem::path directory);

    const std::filesystem::path& directory() const { return directory_; }
    std::filesystem::path main_path() const { return directory_ / kSettingsFileName; }
    std::filesystem::path temp_path() const { return directory_ / kSettingsTempName; }
    std::filesystem::path backup_path() const { return directory_ / kSettingsBackupName; }
    std::filesystem::path damaged_path() const { return directory_ / kSettingsDamagedName; }

    // Reads settings.conf; when that is missing or unusable, settings.conf.bak; else None. Never
    // throws and never writes. A stray settings.conf.tmp (a save a power cut interrupted) is
    // ignored: it never replaced the main file, so it is not trusted.
    LoadResult load();

    // Saves `text` atomically: write settings.conf.tmp, fsync it, keep the current usable
    // settings.conf as settings.conf.bak (itself replaced atomically), rename the temp file over
    // settings.conf and fsync the folder. At every instant a complete file is on disk. A
    // damaged settings.conf is not copied over a good backup; it is set aside as
    // settings.conf.damaged before the new file takes its place. Text identical to what the last
    // load() or save() saw is not written again, so the backup keeps the previous *different*
    // version. Never throws.
    SaveResult save(std::string_view text);

  private:
    std::filesystem::path directory_;
    std::string last_text_; // what main holds as far as this object knows
    bool have_last_ = false;
};

} // namespace toonval::app
