// SPDX-License-Identifier: GPL-3.0-or-later
// The practice history on disk until history moves to a database: one file of run records, each one
// the bytes serialize_run_record() makes, framed so a damaged record is skipped and the rest read.
//
// Layout of runs.dat:
//   "toonval runs 1\n"                      the header (15 bytes)
//   per record:
//     length    4 bytes, little-endian      the record's size, 1 .. kMaxRunBytes
//     check     4 bytes, little-endian      the low 32 bits of content_hash() of the record
//     record    `length` bytes              serialize_run_record()
//
// Appending writes the whole file to runs.dat.tmp, forces it to disk and renames it over runs.dat,
// so at every moment a complete file is on disk and a power cut loses at most the run being
// saved. Records a newer build wrote are kept byte for byte. Reading never fails: a missing file
// is an empty history; a record whose check or contents are wrong is skipped and counted; a file
// that ends inside a record (cut off) keeps everything before it; a file with a wrong header is set
// aside as runs.dat.damaged on the next save and the history starts again.
#pragma once

#include "toonval/run_record.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace toonval::app {

inline constexpr std::string_view kRunsFileName = "runs.dat";
inline constexpr std::string_view kRunsHeader = "toonval runs 1\n";
inline constexpr std::size_t kMaxRunBytes = 1u << 20;
inline constexpr int kMaxDamagedCopies = 8; // runs.dat.damaged and .1 .. .7

struct RunsDecode {
    std::vector<RunRecord> runs;    // oldest first
    std::vector<std::string> kept;  // every record to write back (good and newer), raw, in order
    int damaged = 0;                // records skipped: bad check, bad contents
    int newer = 0;                  // records a newer build wrote: kept, not read
    bool truncated = false;         // the file ends inside a record
    bool bad_header = false;        // not a runs file at all
};

// The file's bytes for these raw records.
std::string encode_runs(const std::vector<std::string>& records);
RunsDecode decode_runs(std::string_view bytes);

class RunsFile {
  public:
    explicit RunsFile(std::filesystem::path directory);

    // Reads runs.dat. Returns a short sentence for the status row when something was damaged,
    // else empty.
    std::string load();
    const std::vector<RunRecord>& runs() const { return runs_; }

    // Adds a run and saves atomically. False with a reason on failure; the run is still in runs().
    // When the file on disk is damaged, a copy is kept first as runs.dat.damaged (then
    // .damaged.1, .2, ... up to kMaxDamagedCopies; an existing copy is never overwritten). If
    // the copy fails, runs.dat is left as it is and append() fails; with every name taken the
    // copy is skipped and the file is replaced.
    bool append(const RunRecord& record, std::string* error);

    std::filesystem::path path() const { return directory_ / kRunsFileName; }

  private:
    std::filesystem::path directory_;
    std::vector<RunRecord> runs_;
    std::vector<std::string> kept_;
    bool set_aside_ = false; // the file on disk is damaged: keep a copy before replacing it
};

} // namespace toonval::app
