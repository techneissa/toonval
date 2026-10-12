// SPDX-License-Identifier: GPL-3.0-or-later
#include "persistence.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>

using namespace toonval;
using namespace toonval::app;
using toonval::app::LoadSource;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        std::random_device rd;
        path = fs::temp_directory_path() / ("toonval-persist-test-" + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

std::string read(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

UiModel changed_model() {
    UiModel m;
    m.look = preset(PresetId::NeonNight);
    m.look.octave_numbering = OctaveNumbering::Yamaha;
    m.look.pitch_colors[3] = RGBA{1, 2, 3, 255};
    m.user_presets.save_slot(1, "Night owl", m.look);
    m.app.hit_window_ms = 120;
    m.app.latency_correction_ms = -35;
    m.session.keyboard = KeyboardChoice::Manual;
    m.session.keyboard_lowest = 28;
    m.session.keyboard_highest = 103;
    return m;
}

} // namespace

TEST_CASE("a model survives the trip through the file text") {
    const UiModel m = changed_model();
    const std::string text = serialize_model(load_settings(""), m);
    CHECK(text.find("[look]") != std::string::npos);
    CHECK(text.find("[user_presets]") != std::string::npos);
    CHECK(text.find("[app]") != std::string::npos);
    CHECK(text.find("[session]") != std::string::npos);
    const UiModel back =
        model_from(load_settings(text, std::array<std::string_view, 2>{"look", "user_presets"}));
    CHECK(back == m);
}

TEST_CASE("sections and keys this build does not know survive a save") {
    const std::string original =
        "format = 1.0\n\n[from_the_future]\nweird = 1\n\n[app]\nhit_window_ms = 90\nnew_key = x\n";
    const std::array<std::string_view, 2> owned{"look", "user_presets"};
    LoadedSettings loaded = load_settings(original, owned);
    UiModel m = model_from(loaded);
    m.look.note_style = NoteStyle::Neon;
    const std::string text = serialize_model(loaded, m);
    CHECK(text.find("[from_the_future]\nweird = 1\n") != std::string::npos);
    CHECK(text.find("new_key = x") != std::string::npos);
    CHECK(model_from(load_settings(text, owned)).look.note_style == NoteStyle::Neon);
}

TEST_CASE("the first start gives the defaults and says nothing") {
    TempDir dir;
    Persistence p(dir.path);
    UiModel m;
    CHECK(p.load(m).empty());
    CHECK(m == UiModel{});
}

TEST_CASE("a change is written only after the debounce, and survives a restart") {
    TempDir dir;
    UiModel m;
    {
        Persistence p(dir.path);
        p.load(m);
        m = changed_model();
        std::int64_t t = 1'000'000'000;
        CHECK(p.update(m, t).empty());
        CHECK_FALSE(fs::exists(p.store().main_path())); // not yet
        m.look.visible_seconds = 6.0f;                  // another change restarts the wait
        t += kSaveDebounceNs - 1;
        p.update(m, t);
        CHECK_FALSE(fs::exists(p.store().main_path()));
        t += kSaveDebounceNs;
        CHECK(p.update(m, t).empty());
        CHECK(fs::exists(p.store().main_path()));
    }
    Persistence again(dir.path);
    UiModel back;
    CHECK(again.load(back).empty());
    CHECK(back == m);
}

TEST_CASE("flush writes an unsaved change at once, and a clean model writes nothing") {
    TempDir dir;
    Persistence p(dir.path);
    UiModel m;
    p.load(m);
    CHECK(p.flush(m).empty());
    CHECK_FALSE(fs::exists(p.store().main_path()));
    m.look.c_markers = false;
    CHECK(p.flush(m).empty());
    UiModel back;
    Persistence again(dir.path);
    again.load(back);
    CHECK_FALSE(back.look.c_markers);
}

TEST_CASE("a startup override that is adopted is not written until something else changes") {
    TempDir dir;
    Persistence p(dir.path);
    UiModel m;
    p.load(m);
    m.look = preset(PresetId::Glass);
    p.adopt(m);
    CHECK(p.update(m, 0).empty());
    CHECK(p.update(m, 10 * kSaveDebounceNs).empty());
    CHECK_FALSE(fs::exists(p.store().main_path()));
}

TEST_CASE("a damaged file falls back to the backup, then the defaults, with a message") {
    TempDir dir;
    UiModel version_a = changed_model();
    UiModel version_b = version_a;
    version_b.look.visible_seconds = 7.0f;
    {
        Persistence p(dir.path);
        UiModel m;
        p.load(m);
        p.flush(version_a);
        p.flush(version_b); // main holds B, the backup holds A
    }
    const fs::path main_file = dir.path / "settings.conf";
    {
        std::ofstream(main_file, std::ios::trunc) << "garbage\nmore garbage\n";
    }
    {
        Persistence p(dir.path);
        UiModel m;
        CHECK_FALSE(p.load(m).empty());
        CHECK(m == version_a);
    }
    {
        std::ofstream(main_file, std::ios::trunc) << "garbage\nmore garbage\n";
        std::ofstream(dir.path / "settings.conf.bak", std::ios::trunc) << "";
    }
    {
        Persistence p(dir.path);
        UiModel m;
        CHECK_FALSE(p.load(m).empty());
        CHECK(m == UiModel{});
    }
}

TEST_CASE("a file from a newer major version is read but never written over") {
    TempDir dir;
    const std::string newer = "format = 2.0\n\n[app]\nhit_window_ms = 100\n";
    {
        std::ofstream(dir.path / "settings.conf") << newer;
    }
    Persistence p(dir.path);
    UiModel m;
    CHECK_FALSE(p.load(m).empty());
    CHECK(m.app.hit_window_ms == 100);
    m.look.c_markers = false;
    CHECK(p.flush(m).empty());
    CHECK(read(dir.path / "settings.conf") == newer);
}

TEST_CASE("a save that fails is reported once and retried later") {
    TempDir dir;
    // The folder path is a file, so nothing can be written there.
    const fs::path blocked = dir.path / "blocked";
    {
        std::ofstream(blocked) << "x";
    }
    Persistence p(blocked);
    UiModel m;
    p.load(m);
    m.look.c_markers = false;
    std::int64_t t = 0;
    p.update(m, t);
    t += kSaveDebounceNs;
    CHECK_FALSE(p.update(m, t).empty()); // reported
    t += kSaveDebounceNs;
    CHECK(p.update(m, t).empty());                 // not again right away
    CHECK(p.update(m, t + kRetryDelayNs).empty()); // the retry fails quietly
}

TEST_CASE("after restoring from the backup the main file is written again at once") {
    TempDir dir;
    const UiModel a = changed_model();
    UiModel b = a;
    b.look.visible_seconds = 7.0f;
    {
        Persistence p(dir.path);
        UiModel m;
        p.load(m);
        p.flush(a);
        p.flush(b);
    }
    {
        std::ofstream(dir.path / "settings.conf", std::ios::trunc) << "garbage\ngarbage\n";
    }
    Persistence p(dir.path);
    UiModel m;
    REQUIRE_FALSE(p.load(m).empty());
    CHECK(m == a); // the backup
    CHECK(p.update(m, 0).empty());
    // The main file is good again, so a plain load finds it.
    SettingsStore check(dir.path);
    CHECK(check.load().source == LoadSource::Main);
    Persistence again(dir.path);
    UiModel m2;
    CHECK(again.load(m2).empty());
    CHECK(m2 == a);
    // The damaged file was kept aside, and the good backup was not replaced by it.
    CHECK(fs::exists(dir.path / "settings.conf.damaged"));
    CHECK(settings_text_usable(read(dir.path / "settings.conf.bak")));
}
