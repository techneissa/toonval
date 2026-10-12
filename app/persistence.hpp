// SPDX-License-Identifier: GPL-3.0-or-later
// Ties the UI's model to the settings file: reads it at startup, notices changes and saves them
// a moment after the last one, and once more on quit. The sections are [app], [session], [look]
// and [user_presets]; any other section or key in the file is written back untouched.
#pragma once

#include "settings_store.hpp"
#include "ui_model.hpp"

#include "toonval/session_state.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace toonval::app {

// Wait this long after the last change before writing, so dragging a slider is one write.
inline constexpr std::int64_t kSaveDebounceNs = 500'000'000;
inline constexpr std::int64_t kRetryDelayNs = 5'000'000'000; // after a failed write

class Persistence {
  public:
    explicit Persistence(std::filesystem::path directory);

    // Reads the settings file into `model` (the defaults for anything missing or damaged) and
    // remembers it as the saved state. Returns a short sentence for the status line, empty when all
    // was well. Never throws and never blocks startup.
    std::string load(UiModel& model);

    // Takes `model` as the saved state without writing: for startup overrides that are not meant
    // to reach the file by themselves.
    void adopt(const UiModel& model) {
        saved_ = model;
        dirty_ = false;
    }

    // Call every frame. Notices a change to `model` and writes it once `kSaveDebounceNs` passed
    // without another. Returns a message when a save failed, else empty.
    std::string update(const UiModel& model, std::int64_t now_ns);

    // Writes at once if anything is unsaved (on quit). Returns a message on failure.
    std::string flush(const UiModel& model);

    const SettingsStore& store() const { return store_; }

  private:
    std::string save_now(const UiModel& model, std::int64_t now_ns);

    SettingsStore store_;
    LoadedSettings loaded_; // the document as read, so unknown sections survive
    UiModel saved_;         // what the file holds as far as we know
    UiModel pending_;       // the unsaved state the debounce is waiting on
    bool dirty_ = false;
    std::int64_t changed_at_ns_ = 0;
    bool heal_ = false;      // the main file was missing or damaged and the backup was used
    bool read_only_ = false; // a newer major version: never write over it
    bool warned_failure_ = false;
};

// The text of the whole file for `model`, written into `base`: [app], [session], [look],
// [user_presets] replaced, everything else kept. Exposed for the tests.
std::string serialize_model(const LoadedSettings& base, const UiModel& model);
// The model a file holds; the defaults for what is missing.
UiModel model_from(const LoadedSettings& loaded);

} // namespace toonval::app
