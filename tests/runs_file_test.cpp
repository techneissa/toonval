// SPDX-License-Identifier: GPL-3.0-or-later
// The practice history file: round trip, atomic save, and reading what survives damage.
#include "runs_file.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

using namespace toonval;
using namespace toonval::app;
namespace fs = std::filesystem;

namespace {

RunRecord sample_run(int n) {
    RunRecord r;
    r.song_hash = 0xabc0 + static_cast<std::uint64_t>(n);
    r.song_title = "Song " + std::to_string(n);
    r.mode = n % 2 == 0 ? RunMode::Flow : RunMode::Wait;
    r.hands = HandFilter::Right;
    r.speed_percent = 80;
    r.start_utc_s = 1'760'000'000 + n * 100;
    r.end_utc_s = r.start_utc_s + 60;
    r.local_day = 20373;
    r.duration_ms = 55'000;
    r.counts.perfect = 10 + n;
    r.counts.missed = 2;
    r.right = {12 + n, 10 + n};
    r.all = r.right;
    r.longest_combo = 7;
    r.measures = {{1, 4, 1, 0}, {2, 8, 1, 1}};
    return sanitized(r);
}

struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() /
               ("toonval-runs-" + std::to_string(std::random_device{}()));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

std::string read_all(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void write_all(const fs::path& p, const std::string& bytes) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << bytes;
}

} // namespace

TEST_CASE("runs file: appended runs come back in order, equal to what was saved") {
    TempDir dir;
    {
        RunsFile file(dir.path);
        CHECK(file.load().empty()); // no file yet: an empty history, no message
        CHECK(file.runs().empty());
        for (int i = 0; i < 5; ++i) {
            std::string error;
            REQUIRE(file.append(sample_run(i), &error));
        }
    }
    RunsFile again(dir.path);
    CHECK(again.load().empty());
    REQUIRE(again.runs().size() == 5);
    for (int i = 0; i < 5; ++i) {
        CHECK(again.runs()[static_cast<std::size_t>(i)] == sample_run(i));
    }
    CHECK_FALSE(fs::exists(dir.path / "runs.dat.tmp"));
    CHECK(read_all(again.path()).rfind(std::string(kRunsHeader), 0) == 0);
}

TEST_CASE("runs file: a file cut off inside a record keeps every whole record before it") {
    std::vector<std::string> raw;
    for (int i = 0; i < 4; ++i) {
        raw.push_back(serialize_run_record(sample_run(i)));
    }
    const std::string bytes = encode_runs(raw);
    for (std::size_t cut = kRunsHeader.size(); cut < bytes.size(); ++cut) {
        const RunsDecode d = decode_runs(std::string_view(bytes).substr(0, cut));
        CHECK_FALSE(d.bad_header);
        CHECK(d.runs.size() <= 4);
        for (std::size_t i = 0; i < d.runs.size(); ++i) {
            CHECK(d.runs[i] == sample_run(static_cast<int>(i)));
        }
        // Exactly at a record boundary nothing is cut; anywhere else the tail is.
        std::size_t boundary = kRunsHeader.size();
        bool at_boundary = cut == boundary;
        for (const std::string& r : raw) {
            boundary += 8 + r.size();
            at_boundary = at_boundary || cut == boundary;
        }
        CHECK(d.truncated == !at_boundary);
    }
}

TEST_CASE("runs file: a damaged record is skipped, the rest read, and the next save keeps a copy") {
    TempDir dir;
    std::vector<std::string> raw;
    for (int i = 0; i < 3; ++i) {
        raw.push_back(serialize_run_record(sample_run(i)));
    }
    std::string bytes = encode_runs(raw);
    // Flip a byte inside the second record's body: its check fails.
    const std::size_t second = kRunsHeader.size() + 8 + raw[0].size() + 8 + 5;
    bytes[second] = static_cast<char>(bytes[second] ^ 0x5A);
    write_all(dir.path / "runs.dat", bytes);

    RunsFile file(dir.path);
    const std::string message = file.load();
    CHECK(message.find("1 run damaged") != std::string::npos);
    REQUIRE(file.runs().size() == 2);
    CHECK(file.runs()[0] == sample_run(0));
    CHECK(file.runs()[1] == sample_run(2));
    std::string error;
    REQUIRE(file.append(sample_run(9), &error));
    CHECK(read_all(dir.path / "runs.dat.damaged") == bytes);
    RunsFile again(dir.path);
    CHECK(again.load().empty());
    CHECK(again.runs().size() == 3);
}

TEST_CASE("runs file: a file that is not a runs file starts a new history and is set aside") {
    TempDir dir;
    write_all(dir.path / "runs.dat", "garbage, not runs");
    RunsFile file(dir.path);
    CHECK_FALSE(file.load().empty());
    CHECK(file.runs().empty());
    std::string error;
    REQUIRE(file.append(sample_run(1), &error));
    CHECK(read_all(dir.path / "runs.dat.damaged") == "garbage, not runs");
    RunsFile again(dir.path);
    CHECK(again.load().empty());
    CHECK(again.runs().size() == 1);
}

TEST_CASE("runs file: records from a newer build are kept byte for byte") {
    std::string future = serialize_run_record(sample_run(0));
    future[2] = 9; // version byte
    const std::vector<std::string> raw{serialize_run_record(sample_run(1)), future};
    const RunsDecode d = decode_runs(encode_runs(raw));
    CHECK(d.runs.size() == 1);
    CHECK(d.newer == 1);
    CHECK(d.damaged == 0);
    CHECK(d.kept == raw);
}

TEST_CASE("runs file: random damage never crashes and never yields a record that was not saved") {
    std::vector<std::string> raw;
    for (int i = 0; i < 6; ++i) {
        raw.push_back(serialize_run_record(sample_run(i)));
    }
    const std::string good = encode_runs(raw);
    std::mt19937 rng(7);
    for (int trial = 0; trial < 2000; ++trial) {
        std::string bytes = good;
        const int flips = 1 + static_cast<int>(rng() % 4);
        for (int f = 0; f < flips; ++f) {
            const std::size_t at = rng() % bytes.size();
            bytes[at] = static_cast<char>(rng() & 0xFF);
        }
        if (rng() % 3 == 0) {
            bytes.resize(rng() % bytes.size());
        }
        const RunsDecode d = decode_runs(bytes);
        for (const RunRecord& r : d.runs) {
            bool known = false;
            for (int i = 0; i < 6; ++i) {
                known = known || r == sample_run(i);
            }
            CHECK(known);
        }
    }
}

namespace {

// A runs file with one damaged record, as bytes.
std::string damaged_bytes() {
    std::vector<std::string> raw{serialize_run_record(sample_run(0)), serialize_run_record(sample_run(1))};
    std::string bytes = encode_runs(raw);
    bytes[kRunsHeader.size() + 8 + 5] = static_cast<char>(bytes[kRunsHeader.size() + 8 + 5] ^ 0x5A);
    return bytes;
}

} // namespace

TEST_CASE("runs file: a second damage keeps the first copy and writes the next name") {
    TempDir dir;
    const std::string first = damaged_bytes();
    write_all(dir.path / "runs.dat", first);
    {
        RunsFile file(dir.path);
        file.load();
        std::string error;
        REQUIRE(file.append(sample_run(5), &error));
    }
    CHECK(read_all(dir.path / "runs.dat.damaged") == first);

    const std::string second = "not a runs file at all";
    write_all(dir.path / "runs.dat", second);
    {
        RunsFile file(dir.path);
        CHECK_FALSE(file.load().empty());
        std::string error;
        REQUIRE(file.append(sample_run(6), &error));
    }
    CHECK(read_all(dir.path / "runs.dat.damaged") == first);
    CHECK(read_all(dir.path / "runs.dat.damaged.1") == second);
    RunsFile again(dir.path);
    CHECK(again.load().empty());
    CHECK(again.runs().size() == 1);
}

TEST_CASE("runs file: with every copy name taken the original is still never overwritten by a copy") {
    TempDir dir;
    write_all(dir.path / "runs.dat.damaged", "old 0");
    for (int i = 1; i < kMaxDamagedCopies; ++i) {
        write_all(dir.path / ("runs.dat.damaged." + std::to_string(i)), "old " + std::to_string(i));
    }
    write_all(dir.path / "runs.dat", "garbage");
    RunsFile file(dir.path);
    file.load();
    std::string error;
    REQUIRE(file.append(sample_run(1), &error)); // copy skipped, history starts again
    CHECK(read_all(dir.path / "runs.dat.damaged") == "old 0");
    CHECK(read_all(dir.path / ("runs.dat.damaged." + std::to_string(kMaxDamagedCopies - 1))) ==
          "old " + std::to_string(kMaxDamagedCopies - 1));
    CHECK_FALSE(fs::exists(dir.path / ("runs.dat.damaged." + std::to_string(kMaxDamagedCopies))));
    RunsFile again(dir.path);
    CHECK(again.load().empty());
    CHECK(again.runs().size() == 1);
}

#if !defined(_WIN32)
TEST_CASE("runs file: when the damaged copy cannot be made, runs.dat is left alone and the run kept") {
    TempDir dir;
    const std::string bytes = damaged_bytes();
    write_all(dir.path / "runs.dat", bytes);
    RunsFile file(dir.path);
    file.load();
    // A read-only folder: no copy (or temp file) can be created in it.
    fs::permissions(dir.path, fs::perms::owner_read | fs::perms::owner_exec, fs::perm_options::replace);
    std::error_code probe;
    const bool really_blocked = !std::ofstream(dir.path / "probe").is_open();
    std::string error;
    const bool ok = file.append(sample_run(7), &error);
    fs::permissions(dir.path, fs::perms::owner_all, fs::perm_options::replace);
    if (!really_blocked) {
        MESSAGE("skipped: the folder stays writable (running as root)");
        return;
    }
    CHECK_FALSE(ok);
    CHECK_FALSE(error.empty());
    CHECK(read_all(dir.path / "runs.dat") == bytes);
    CHECK_FALSE(fs::exists(dir.path / "runs.dat.damaged"));
    CHECK_FALSE(fs::exists(dir.path / "runs.dat.tmp"));
    CHECK(file.runs().size() == 2); // 1 good + the new one stay in memory
}
#endif
