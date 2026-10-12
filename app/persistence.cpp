// SPDX-License-Identifier: GPL-3.0-or-later
#include "persistence.hpp"

#include "toonval/sections_store.hpp"

#include "toonval/look.hpp"
#include "toonval/settings_doc.hpp"

#include <array>
#include <string_view>

namespace toonval::app {

namespace {

constexpr std::array<std::string_view, 2> kOwnedSections{"look", "user_presets"};

// The `slot_N_...` lines of a look document; the look's own lines live in [look].
std::string slot_lines(const UserPresets& presets) {
    LookDocument doc;
    doc.user_presets = presets;
    const std::string text = serialize_document(doc);
    std::string out;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        const std::string_view line(text.data() + start, end - start);
        if (line.substr(0, 5) == "slot_") {
            out.append(line);
            out.push_back('\n');
        }
        start = end + 1;
    }
    return out;
}

} // namespace

UiModel model_from(const LoadedSettings& loaded) {
    UiModel model;
    model.app = loaded.app;
    model.session = loaded.session;
    model.look = parse_look(loaded.document.section_text("look"));
    model.user_presets =
        parse_look_document(loaded.document.section_text("user_presets")).user_presets;
    return model;
}

std::string serialize_model(const LoadedSettings& base, const UiModel& model) {
    LoadedSettings out = base;
    out.app = model.app;
    out.session = model.session;
    out.document.set_section_text("look", serialize(model.look));
    out.document.set_section_text("user_presets", slot_lines(model.user_presets));
    return out.serialize();
}

Persistence::Persistence(std::filesystem::path directory) : store_(std::move(directory)) {}

std::string Persistence::load(UiModel& model) {
    const LoadResult result = store_.load();
    std::string message = result.message;
    if (result.source == LoadSource::None) {
        loaded_ = load_settings("", kOwnedSections);
    } else {
        // The song sections (`sections.<hash>`) belong to the sections layer: known, not ours.
        std::vector<std::string> names =
            sections_section_names(SettingsDocument::parse(result.text));
        std::vector<std::string_view> known(std::begin(kOwnedSections), std::end(kOwnedSections));
        known.insert(known.end(), names.begin(), names.end());
        loaded_ = load_settings(result.text, known);
        if (loaded_.read_only_recommended()) {
            read_only_ = true;
            message = "Settings are from a newer version: changes are not saved";
        }
    }
    model = model_from(loaded_);
    model.app = model.app.clamped();
    model.session = model.session.clamped();
    model.look = model.look.clamped();
    saved_ = model;
    dirty_ = false;
    // Restored from the backup: write the main file again on the first frame, so the next start
    // does not depend on the backup. When nothing usable was found the defaults are not written
    // until the first change, and whatever damaged file is there stays for now.
    heal_ = result.source == LoadSource::Backup && !read_only_;
    return message;
}

std::string Persistence::save_now(const UiModel& model, std::int64_t now_ns) {
    dirty_ = false;
    if (read_only_) {
        return {};
    }
    const SaveResult result = store_.save(serialize_model(loaded_, model));
    if (!result.ok) {
        // Try again in five seconds, and say so once.
        dirty_ = true;
        pending_ = model;
        changed_at_ns_ = now_ns + kRetryDelayNs - kSaveDebounceNs;
        if (!warned_failure_) {
            warned_failure_ = true;
            return "Could not save settings: " + result.error;
        }
        return {};
    }
    warned_failure_ = false;
    saved_ = model;
    return {};
}

std::string Persistence::update(const UiModel& model, std::int64_t now_ns) {
    if (heal_) {
        heal_ = false;
        return save_now(model, now_ns);
    }
    if (model == saved_) {
        dirty_ = false; // nothing to write, or changed and changed back
        return {};
    }
    if (!dirty_ || !(model == pending_)) {
        pending_ = model;
        changed_at_ns_ = now_ns;
        dirty_ = true;
    }
    if (now_ns - changed_at_ns_ >= kSaveDebounceNs) {
        return save_now(model, now_ns);
    }
    return {};
}

std::string Persistence::flush(const UiModel& model) {
    if (model == saved_) {
        return {};
    }
    return save_now(model, changed_at_ns_);
}

} // namespace toonval::app
