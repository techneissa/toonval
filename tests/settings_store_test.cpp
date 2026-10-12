// SPDX-License-Identifier: GPL-3.0-or-later
#include "settings_store.hpp"

#include "toonval/settings_doc.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>

using toonval::app::LoadSource;
using toonval::app::SettingsStore;
namespace fs = std::filesystem;

namespace {

// A fresh folder under the system temp directory, removed at the end of the test.
struct TempDir {
    fs::path path;
    TempDir() {
        std::random_device rd;
        path = fs::temp_directory_path() / ("toonval-store-test-" + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

void write(const fs::path& p, const std::string& text) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

std::string read(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

const std::string kV1 = "format = 1.0\n\n[app]\nhit_window_ms = 80\n";
const std::string kV2 = "format = 1.0\n\n[app]\nhit_window_ms = 90\n";
const std::string kV3 = "format = 1.0\n\n[app]\nhit_window_ms = 100\n";

} // namespace

TEST_CASE("a first start has no file and says nothing") {
    TempDir dir;
    SettingsStore store(dir.path);
    const auto r = store.load();
    CHECK(r.source == LoadSource::None);
    CHECK(r.text.empty());
    CHECK(r.message.empty());
    CHECK_FALSE(r.main_damaged);
}

TEST_CASE("a missing folder is created by the first save") {
    TempDir dir;
    SettingsStore store(dir.path / "a" / "b");
    CHECK(store.save(kV1).ok);
    CHECK(read(store.main_path()) == kV1);
}

TEST_CASE("what was saved loads back, and the temp file is gone") {
    TempDir dir;
    SettingsStore store(dir.path);
    REQUIRE(store.save(kV1).ok);
    CHECK_FALSE(fs::exists(store.temp_path()));
    SettingsStore again(dir.path);
    const auto r = again.load();
    CHECK(r.source == LoadSource::Main);
    CHECK(r.text == kV1);
    CHECK(r.message.empty());
}

TEST_CASE("the previous version is kept as the backup") {
    TempDir dir;
    SettingsStore store(dir.path);
    REQUIRE(store.save(kV1).ok);
    CHECK_FALSE(fs::exists(store.backup_path())); // nothing to keep yet
    REQUIRE(store.save(kV2).ok);
    CHECK(read(store.main_path()) == kV2);
    CHECK(read(store.backup_path()) == kV1);
    REQUIRE(store.save(kV3).ok);
    CHECK(read(store.main_path()) == kV3);
    CHECK(read(store.backup_path()) == kV2);
}

TEST_CASE("saving the same text again leaves the backup alone") {
    TempDir dir;
    SettingsStore store(dir.path);
    REQUIRE(store.save(kV1).ok);
    REQUIRE(store.save(kV2).ok);
    REQUIRE(store.save(kV2).ok);
    CHECK(read(store.backup_path()) == kV1);
}

TEST_CASE("a missing main file falls back to the backup and says so") {
    TempDir dir;
    SettingsStore store(dir.path);
    REQUIRE(store.save(kV1).ok);
    REQUIRE(store.save(kV2).ok);
    fs::remove(store.main_path());
    SettingsStore again(dir.path);
    const auto r = again.load();
    CHECK(r.source == LoadSource::Backup);
    CHECK(r.text == kV1);
    CHECK_FALSE(r.message.empty());
    CHECK_FALSE(r.main_damaged);
}

TEST_CASE("damaged main files fall back to the backup") {
    TempDir dir;
    SettingsStore store(dir.path);
    REQUIRE(store.save(kV1).ok);
    REQUIRE(store.save(kV2).ok);
    const std::string garbage[] = {
        "",                             // zero bytes, as after a crash mid-write
        std::string("\0\0\0\0\0\0", 6), // NULs
        "this is not a settings file\nneither is this\nor this one\n",
        "\x89PNG\r\n\x1a\n\x01\x02 binary stuff",
        "[app\nhit_window_ms", // cut off
    };
    for (const std::string& g : garbage) {
        write(store.main_path(), g);
        SettingsStore again(dir.path);
        const auto r = again.load();
        INFO("garbage of " << g.size() << " bytes");
        CHECK(r.source == LoadSource::Backup);
        CHECK(r.text == kV1);
        CHECK(r.main_damaged);
        CHECK_FALSE(r.message.empty());
    }
}

TEST_CASE("a main file over the size limit is damaged, not read") {
    TempDir dir;
    SettingsStore store(dir.path);
    REQUIRE(store.save(kV1).ok);
    REQUIRE(store.save(kV2).ok);
    write(store.main_path(), kV3 + std::string(toonval::kMaxSettingsBytes, '#'));
    const auto r = SettingsStore(dir.path).load();
    CHECK(r.source == LoadSource::Backup);
    CHECK(r.text == kV1);
    CHECK(r.main_damaged);
}

TEST_CASE("main and backup both damaged: defaults, with a message") {
    TempDir dir;
    SettingsStore store(dir.path);
    write(store.main_path(), "garbage garbage\nmore garbage\n");
    write(store.backup_path(), "");
    const auto r = store.load();
    CHECK(r.source == LoadSource::None);
    CHECK(r.text.empty());
    CHECK(r.main_damaged);
    CHECK_FALSE(r.message.empty());
}

TEST_CASE("main damaged and no backup: defaults, with a message") {
    TempDir dir;
    SettingsStore store(dir.path);
    write(store.main_path(), "garbage garbage\nmore garbage\n");
    const auto r = store.load();
    CHECK(r.source == LoadSource::None);
    CHECK(r.main_damaged);
    CHECK_FALSE(r.message.empty());
}

TEST_CASE("a damaged backup is not used when the main file is fine") {
    TempDir dir;
    SettingsStore store(dir.path);
    write(store.main_path(), kV2);
    write(store.backup_path(), "garbage\n");
    const auto r = store.load();
    CHECK(r.source == LoadSource::Main);
    CHECK(r.message.empty());
}

TEST_CASE("a leftover temp file from an interrupted save is ignored and replaced") {
    TempDir dir;
    SettingsStore store(dir.path);
    REQUIRE(store.save(kV1).ok);
    write(store.temp_path(), "format = 1.0\n[app]\nhit_");
    const auto r = SettingsStore(dir.path).load();
    CHECK(r.source == LoadSource::Main);
    CHECK(r.text == kV1);
    REQUIRE(store.save(kV2).ok);
    CHECK_FALSE(fs::exists(store.temp_path()));
    CHECK(read(store.main_path()) == kV2);
}

TEST_CASE("saving over a damaged main file keeps the good backup") {
    TempDir dir;
    SettingsStore store(dir.path);
    REQUIRE(store.save(kV1).ok);
    REQUIRE(store.save(kV2).ok); // main V2, backup V1
    write(store.main_path(), "garbage\ngarbage\n");

    SettingsStore again(dir.path);
    const auto loaded = again.load();
    REQUIRE(loaded.source == LoadSource::Backup);
    REQUIRE(again.save(kV3).ok);
    CHECK(read(again.main_path()) == kV3);
    CHECK(read(again.backup_path()) == kV1); // not replaced by the garbage
    CHECK(SettingsStore(dir.path).load().source == LoadSource::Main);
}

TEST_CASE("saving after loading the backup writes even when the text equals it") {
    TempDir dir;
    SettingsStore store(dir.path);
    REQUIRE(store.save(kV1).ok);
    REQUIRE(store.save(kV2).ok);
    fs::remove(store.main_path());
    SettingsStore again(dir.path);
    REQUIRE(again.load().source == LoadSource::Backup);
    REQUIRE(again.save(kV1).ok); // the app rewrites what it restored
    CHECK(read(again.main_path()) == kV1);
}

TEST_CASE("a save that cannot be written reports why and leaves the old file") {
    TempDir dir;
    SettingsStore store(dir.path);
    REQUIRE(store.save(kV1).ok);
    // A folder where the temp file should go makes the write fail on every platform.
    fs::create_directory(store.temp_path());
    const auto r = store.save(kV2);
    CHECK_FALSE(r.ok);
    CHECK_FALSE(r.error.empty());
    CHECK(read(store.main_path()) == kV1);
}

TEST_CASE("a folder path that is a file cannot be saved to") {
    TempDir dir;
    write(dir.path / "occupied", "x");
    SettingsStore store(dir.path / "occupied");
    const auto r = store.save(kV1);
    CHECK_FALSE(r.ok);
    CHECK_FALSE(r.error.empty());
}

TEST_CASE("text that is too large is refused") {
    TempDir dir;
    SettingsStore store(dir.path);
    CHECK_FALSE(store.save(std::string(toonval::kMaxSettingsBytes + 1, 'x')).ok);
    CHECK_FALSE(fs::exists(store.main_path()));
}

TEST_CASE("settings_text_usable accepts real files and rejects junk") {
    using toonval::app::settings_text_usable;
    CHECK(settings_text_usable(kV1));
    CHECK(settings_text_usable("format = 1.0\n"));
    CHECK(settings_text_usable("[app]\nhit_window_ms = 80\n"));
    // A newer build's file with sections and keys this build does not know.
    CHECK(settings_text_usable("format = 2.0\n[future]\nthing = 1\n[app]\nx = 2\nsurprise\n"));
    CHECK_FALSE(settings_text_usable(""));
    CHECK_FALSE(settings_text_usable("# only a comment\n"));
    CHECK_FALSE(settings_text_usable("a = 1\nb = 2\n")); // no format, no section
    CHECK_FALSE(settings_text_usable(std::string("format = 1.0\n\0[app]\n", 20)));
    CHECK_FALSE(settings_text_usable("[app]\nnot a setting\nnor this\nnor this\nx = 1\n"));
}

TEST_CASE("a damaged main file is set aside, not lost, when it is replaced") {
    TempDir dir;
    SettingsStore store(dir.path);
    REQUIRE(store.save(kV1).ok);
    REQUIRE(store.save(kV2).ok);
    write(store.main_path(), "garbage\ngarbage\n");
    REQUIRE(store.save(kV3).ok);
    CHECK(read(store.damaged_path()) == "garbage\ngarbage\n");
    CHECK(read(store.main_path()) == kV3);
}
