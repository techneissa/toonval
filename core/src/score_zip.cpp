// SPDX-License-Identifier: GPL-3.0-or-later
// Score files in zips (.mxl, .mscz) and the content sniffing that picks a reader. The unzipper
// works on memory only (miniz built without stdio) and enforces the limits of musicxml.hpp before
// it unpacks anything.
#include "toonval/musicxml.hpp"

#include "score_raw.hpp"

#include "miniz.h"
#include "pugixml.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace toonval {

using namespace score_detail;

namespace {

constexpr const char* kExportHint = "export the score to MusicXML from MuseScore (File > Export) "
                                    "and import that file instead";

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

// "./a\b" -> "a/b": how zips written on Windows name things.
std::string normalized_path(std::string_view path) {
    std::string out(path);
    std::replace(out.begin(), out.end(), '\\', '/');
    while (out.rfind("./", 0) == 0) {
        out.erase(0, 2);
    }
    while (!out.empty() && out.front() == '/') {
        out.erase(0, 1);
    }
    return out;
}

// miniz's own memory reader adds an unchecked offset to the base pointer (flagged by UBSan on
// damaged archives), so reads go through this bounds-checked one.
struct MemorySource {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
};

std::size_t read_from_memory(void* opaque, mz_uint64 offset, void* buffer, std::size_t count) {
    const auto* source = static_cast<const MemorySource*>(opaque);
    if (offset >= source->size) {
        return 0;
    }
    const std::size_t n =
        std::min<std::size_t>(count, source->size - static_cast<std::size_t>(offset));
    std::memcpy(buffer, source->data + offset, n);
    return n;
}

class Zip {
  public:
    Zip() = default;
    Zip(const Zip&) = delete;
    Zip& operator=(const Zip&) = delete;
    ~Zip() {
        if (open_) {
            mz_zip_reader_end(&zip_);
        }
    }

    // Opens `data` and checks the entry count and the total declared size. False with `error` set.
    bool open(const std::uint8_t* data, std::size_t size, ScoreLoadResult& error) {
        if (data == nullptr || size == 0) {
            error = make_error(ScoreFileError::NotZip, "the file is empty");
            return false;
        }
        std::memset(&zip_, 0, sizeof(zip_));
        source_ = {data, size};
        zip_.m_pRead = read_from_memory;
        zip_.m_pIO_opaque = &source_;
        if (mz_zip_reader_init(&zip_, size, 0) == 0) {
            error = make_error(ScoreFileError::NotZip, "not a readable zip archive");
            return false;
        }
        open_ = true;
        const mz_uint count = mz_zip_reader_get_num_files(&zip_);
        if (count > kMaxZipEntries) {
            error = make_error(ScoreFileError::TooLarge, "the zip has too many entries");
            return false;
        }
        std::uint64_t total = 0;
        for (mz_uint i = 0; i < count; ++i) {
            mz_zip_archive_file_stat st;
            if (mz_zip_reader_file_stat(&zip_, i, &st) == 0) {
                error = make_error(ScoreFileError::NotZip, "the zip directory is damaged");
                return false;
            }
            total += st.m_uncomp_size;
            if (total > kMaxZipTotalBytes) {
                error = make_error(ScoreFileError::TooLarge,
                                   "the zip would unpack to more than 256 MiB");
                return false;
            }
            names_.push_back(normalized_path(st.m_filename));
            directory_.push_back(mz_zip_reader_is_file_a_directory(&zip_, i) != 0);
        }
        return true;
    }

    std::size_t count() const { return names_.size(); }
    const std::string& name(std::size_t i) const { return names_[i]; }

    // Index of the entry named `path` (exact, after normalizing), or -1.
    int find(std::string_view path) const {
        const std::string want = normalized_path(path);
        for (std::size_t i = 0; i < names_.size(); ++i) {
            if (!directory_[i] && names_[i] == want) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    // Index of the first file whose lower-case name ends with one of `suffixes`, outside META-INF.
    int find_by_suffix(std::initializer_list<std::string_view> suffixes) const {
        for (std::size_t i = 0; i < names_.size(); ++i) {
            if (directory_[i] || lower(names_[i]).rfind("meta-inf/", 0) == 0) {
                continue;
            }
            const std::string low = lower(names_[i]);
            for (std::string_view suffix : suffixes) {
                if (ends_with(low, suffix)) {
                    return static_cast<int>(i);
                }
            }
        }
        return -1;
    }

    bool extract(int index, std::vector<std::uint8_t>& out, ScoreLoadResult& error) {
        mz_zip_archive_file_stat st;
        if (index < 0 || mz_zip_reader_file_stat(&zip_, static_cast<mz_uint>(index), &st) == 0) {
            error = make_error(ScoreFileError::NotZip, "the zip directory is damaged");
            return false;
        }
        if (st.m_is_encrypted != 0 || st.m_is_supported == 0) {
            error = make_error(ScoreFileError::Unsupported,
                               "the zip uses encryption or a compression this reader lacks");
            return false;
        }
        if (st.m_uncomp_size > kMaxZipEntryBytes) {
            error = make_error(ScoreFileError::TooLarge, "a file in the zip is larger than 64 MiB");
            return false;
        }
        out.assign(static_cast<std::size_t>(st.m_uncomp_size), 0);
        if (mz_zip_reader_extract_to_mem(&zip_, static_cast<mz_uint>(index), out.data(), out.size(),
                                         0) == 0) {
            out.clear();
            error = make_error(ScoreFileError::Corrupt, "a file in the zip is damaged");
            return false;
        }
        return true;
    }

  private:
    mz_zip_archive zip_{};
    MemorySource source_;
    bool open_ = false;
    std::vector<std::string> names_;
    std::vector<bool> directory_;
};

// The rootfile paths of META-INF/container.xml, in order; empty when there is none or it is
// unreadable. MusicXML lists the score first; MuseScore 4 lists its style file and more first.
std::vector<std::string> container_roots(Zip& zip) {
    std::vector<std::string> roots;
    const int at = zip.find("META-INF/container.xml");
    ScoreLoadResult ignored;
    std::vector<std::uint8_t> bytes;
    if (at < 0 || !zip.extract(at, bytes, ignored)) {
        return roots;
    }
    pugi::xml_document doc;
    if (!doc.load_buffer(bytes.data(), bytes.size(), pugi::parse_default)) {
        return roots;
    }
    for (const pugi::xml_node& r : doc.document_element().child("rootfiles").children("rootfile")) {
        roots.emplace_back(r.attribute("full-path").as_string());
    }
    return roots;
}

bool is_musicxml_name(std::string_view name) {
    const std::string low = lower(name);
    return ends_with(low, ".musicxml") || ends_with(low, ".xml");
}

// The MusicXML entry: the first container rootfile named like a MusicXML file, else the first such
// entry outside META-INF. -1 when there is none.
int find_musicxml_entry(Zip& zip) {
    for (const std::string& root : container_roots(zip)) {
        const int at = zip.find(root);
        if (at >= 0 && is_musicxml_name(root)) {
            return at;
        }
    }
    return zip.find_by_suffix({".musicxml", ".xml"});
}

// The MuseScore entry: a container rootfile ending in .mscx, else the first .mscx entry.
int find_mscx_entry(Zip& zip) {
    for (const std::string& root : container_roots(zip)) {
        const int at = zip.find(root);
        if (at >= 0 && ends_with(lower(root), ".mscx")) {
            return at;
        }
    }
    for (std::size_t i = 0; i < zip.count(); ++i) {
        if (ends_with(lower(zip.name(i)), ".mscx")) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

ScoreLoadResult mscx_not_found() {
    return make_error(ScoreFileError::Unsupported,
                      std::string("this MuseScore file holds no score this reader knows; ") +
                          kExportHint);
}

} // namespace

SongFileFormat sniff_song_format(const std::uint8_t* data, std::size_t size) {
    if (data == nullptr || size < 4) {
        return SongFileFormat::Unknown;
    }
    if (std::memcmp(data, "MThd", 4) == 0) {
        return SongFileFormat::Midi;
    }
    if (data[0] == 'P' && data[1] == 'K' &&
        ((data[2] == 3 && data[3] == 4) || (data[2] == 5 && data[3] == 6))) {
        return SongFileFormat::Zip;
    }
    std::size_t i = 0;
    if (size >= 2 &&
        ((data[0] == 0xFF && data[1] == 0xFE) || (data[0] == 0xFE && data[1] == 0xFF))) {
        return SongFileFormat::Xml; // UTF-16 byte order mark: pugixml reads both orders
    }
    if (size >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
        i = 3;
    }
    while (i < size && (data[i] == ' ' || data[i] == '\t' || data[i] == '\n' || data[i] == '\r')) {
        ++i;
    }
    return i < size && data[i] == '<' ? SongFileFormat::Xml : SongFileFormat::Unknown;
}

ScoreLoadResult load_mxl(const std::uint8_t* data, std::size_t size) {
    Zip zip;
    ScoreLoadResult error;
    if (!zip.open(data, size, error)) {
        return error;
    }
    const int at = find_musicxml_entry(zip);
    if (at < 0) {
        if (find_mscx_entry(zip) >= 0) {
            return make_error(ScoreFileError::NotScore,
                              "this zip is a MuseScore file (.mscz), not a compressed MusicXML "
                              "file");
        }
        return make_error(ScoreFileError::NotScore,
                          "the zip holds no MusicXML score (no container.xml rootfile, .musicxml "
                          "or .xml file)");
    }
    std::vector<std::uint8_t> bytes;
    if (!zip.extract(at, bytes, error)) {
        return error;
    }
    return load_musicxml(bytes.data(), bytes.size());
}

ScoreLoadResult load_mscz(const std::uint8_t* data, std::size_t size) {
    Zip zip;
    ScoreLoadResult error;
    if (!zip.open(data, size, error)) {
        return error;
    }
    const int at = find_mscx_entry(zip);
    if (at < 0) {
        return mscx_not_found();
    }
    std::vector<std::uint8_t> bytes;
    if (!zip.extract(at, bytes, error)) {
        return error;
    }
    return load_mscx(bytes.data(), bytes.size());
}

ScoreLoadResult load_mscx(const std::uint8_t* data, std::size_t size) {
    pugi::xml_document doc;
    ScoreLoadResult error;
    if (!parse_xml_bytes(data, size, doc, error)) {
        return error;
    }
    return score_from_mscx(doc);
}

ScoreLoadResult load_score(const std::uint8_t* data, std::size_t size) {
    switch (sniff_song_format(data, size)) {
    case SongFileFormat::Xml: {
        pugi::xml_document doc;
        ScoreLoadResult error;
        if (!parse_xml_bytes(data, size, doc, error)) {
            return error;
        }
        if (std::strcmp(doc.document_element().name(), "museScore") == 0) {
            return score_from_mscx(doc);
        }
        return score_from_musicxml(doc);
    }
    case SongFileFormat::Zip: {
        Zip zip;
        ScoreLoadResult error;
        if (!zip.open(data, size, error)) {
            return error;
        }
        const int music_xml = find_musicxml_entry(zip);
        std::vector<std::uint8_t> bytes;
        if (music_xml >= 0 && find_mscx_entry(zip) < 0) {
            if (!zip.extract(music_xml, bytes, error)) {
                return error;
            }
            return load_musicxml(bytes.data(), bytes.size());
        }
        const int mscx = find_mscx_entry(zip);
        if (mscx < 0) {
            return make_error(ScoreFileError::NotScore,
                              "the zip holds no score (no MusicXML or MuseScore file)");
        }
        if (!zip.extract(mscx, bytes, error)) {
            return error;
        }
        return load_mscx(bytes.data(), bytes.size());
    }
    case SongFileFormat::Midi:
        return make_error(ScoreFileError::NotScore, "this is a MIDI file, not a score file");
    case SongFileFormat::Unknown:
        break;
    }
    return make_error(ScoreFileError::NotScore, "the file is not MusicXML, MuseScore or MIDI");
}

} // namespace toonval
