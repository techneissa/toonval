// SPDX-License-Identifier: GPL-3.0-or-later
#include "runs_file.hpp"

#include "toonval/content_hash.hpp"

#include <fstream>
#include <iterator>
#include <system_error>

#if defined(_WIN32)
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace toonval::app {

namespace fs = std::filesystem;

namespace {

void put_u32(std::string& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xFFu));
    }
}

std::uint32_t get_u32(std::string_view bytes, std::size_t at) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        v |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + static_cast<std::size_t>(i)]))
             << (8 * i);
    }
    return v;
}

std::uint32_t check_of(std::string_view record) {
    return static_cast<std::uint32_t>(content_hash(record) & 0xFFFFFFFFu);
}

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

std::string encode_runs(const std::vector<std::string>& records) {
    std::string out(kRunsHeader);
    for (const std::string& r : records) {
        put_u32(out, static_cast<std::uint32_t>(r.size()));
        put_u32(out, check_of(r));
        out += r;
    }
    return out;
}

RunsDecode decode_runs(std::string_view bytes) {
    RunsDecode out;
    if (bytes.empty()) {
        return out;
    }
    if (bytes.substr(0, kRunsHeader.size()) != kRunsHeader) {
        out.bad_header = true;
        return out;
    }
    std::size_t at = kRunsHeader.size();
    while (at < bytes.size()) {
        if (bytes.size() - at < 8) {
            out.truncated = true;
            break;
        }
        const std::uint32_t length = get_u32(bytes, at);
        const std::uint32_t check = get_u32(bytes, at + 4);
        if (length == 0 || length > kMaxRunBytes) {
            // The framing itself is damaged: nothing after it can be trusted to line up.
            ++out.damaged;
            break;
        }
        if (bytes.size() - at - 8 < length) {
            out.truncated = true;
            break;
        }
        const std::string_view record = bytes.substr(at + 8, length);
        at += 8 + length;
        if (check_of(record) != check) {
            ++out.damaged;
            continue;
        }
        RunRecord run;
        switch (parse_run_record(record, run)) {
        case RunParseStatus::Ok:
            out.runs.push_back(std::move(run));
            out.kept.emplace_back(record);
            break;
        case RunParseStatus::NewerVersion:
            ++out.newer;
            out.kept.emplace_back(record);
            break;
        case RunParseStatus::Truncated:
        case RunParseStatus::BadMagic:
        case RunParseStatus::Corrupt:
            ++out.damaged;
            break;
        }
    }
    return out;
}

RunsFile::RunsFile(fs::path directory) : directory_(std::move(directory)) {}

std::string RunsFile::load() {
    runs_.clear();
    kept_.clear();
    set_aside_ = false;
    std::error_code ec;
    if (!fs::exists(path(), ec)) {
        return {};
    }
    std::ifstream in(path(), std::ios::binary);
    if (!in) {
        set_aside_ = true;
        return "Practice history unreadable; starting a new one";
    }
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    RunsDecode d = decode_runs(bytes);
    runs_ = std::move(d.runs);
    kept_ = std::move(d.kept);
    if (d.bad_header) {
        set_aside_ = true;
        return "Practice history damaged; kept as runs.dat.damaged, starting a new one";
    }
    if (d.damaged > 0 || d.truncated) {
        set_aside_ = true;
        const int lost = d.damaged + (d.truncated ? 1 : 0);
        return "Practice history: " + std::to_string(lost) + (lost == 1 ? " run" : " runs") +
               " damaged and skipped";
    }
    return {};
}

bool RunsFile::append(const RunRecord& record, std::string* error) {
    const RunRecord clean = sanitized(record);
    runs_.push_back(clean);
    kept_.push_back(serialize_run_record(clean));
    std::error_code ec;
    fs::create_directories(directory_, ec);
    if (set_aside_ && fs::exists(path(), ec)) {
        // Keep the damaged file under a name not taken yet; never overwrite an earlier copy.
        fs::path copy;
        for (int i = 0; i < kMaxDamagedCopies; ++i) {
            fs::path candidate = directory_ / "runs.dat.damaged";
            if (i > 0) {
                candidate += "." + std::to_string(i);
            }
            if (!fs::exists(candidate, ec) && !fs::is_symlink(candidate, ec)) {
                copy = candidate;
                break;
            }
        }
        if (!copy.empty()) {
            ec.clear();
            if (!fs::copy_file(path(), copy, fs::copy_options::none, ec) || ec) {
                // Keep the damaged original: the history file is not replaced.
                std::error_code ignore;
                fs::remove(copy, ignore);
                if (error != nullptr) {
                    *error = "could not keep a copy of the damaged " + path().string() + " (" +
                             ec.message() + "); left as it is";
                }
                return false;
            }
        }
        set_aside_ = false;
    }
    const fs::path tmp = directory_ / "runs.dat.tmp";
    if (!write_synced(tmp, encode_runs(kept_))) {
        if (error != nullptr) {
            *error = "could not write " + tmp.string();
        }
        return false;
    }
    fs::rename(tmp, path(), ec);
    if (ec) {
        if (error != nullptr) {
            *error = "could not replace " + path().string() + ": " + ec.message();
        }
        return false;
    }
    sync_directory(directory_);
    return true;
}

} // namespace toonval::app
