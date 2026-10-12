// SPDX-License-Identifier: GPL-3.0-or-later
// Builds small zip files in memory for the score loader tests: stored entries, a deflated run of
// zeros (a zip bomb in miniature), and entries whose headers lie about their size.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace toonval::test {

struct ZipEntry {
    std::string name;
    std::uint16_t method = 0; // 0 stored, 8 deflate
    std::uint32_t crc = 0;
    std::uint32_t declared_size = 0; // what the headers claim the entry unpacks to
    std::vector<std::uint8_t> payload;
};

inline std::uint32_t crc32_of(const std::uint8_t* data, std::size_t size) {
    static const auto table = [] {
        std::vector<std::uint32_t> t(256);
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i) {
        c = table[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

inline ZipEntry stored_entry(std::string name, std::string_view text) {
    ZipEntry e;
    e.name = std::move(name);
    e.payload.assign(text.begin(), text.end());
    e.crc = crc32_of(e.payload.data(), e.payload.size());
    e.declared_size = static_cast<std::uint32_t>(text.size());
    return e;
}

// Deflate stream (one fixed-Huffman block) of `1 + 258 * runs` zero bytes: a literal 0, then
// `runs` copies of length 258 at distance 1. A few hundred KB of this unpack to hundreds of MB.
inline ZipEntry zeros_entry(std::string name, std::size_t runs, std::uint32_t declared_size) {
    std::vector<std::uint8_t> out;
    std::uint32_t acc = 0;
    int bits = 0;
    auto put = [&](std::uint32_t value, int count) { // least significant bit first
        for (int i = 0; i < count; ++i) {
            acc |= ((value >> i) & 1u) << bits;
            if (++bits == 8) {
                out.push_back(static_cast<std::uint8_t>(acc));
                acc = 0;
                bits = 0;
            }
        }
    };
    auto put_code = [&](std::uint32_t code, int count) { // Huffman codes go most significant first
        for (int i = count - 1; i >= 0; --i) {
            put((code >> i) & 1u, 1);
        }
    };
    put(1, 1);         // final block
    put(1, 2);         // fixed Huffman codes
    put_code(0x30, 8); // literal 0
    for (std::size_t i = 0; i < runs; ++i) {
        put_code(0xC5, 8); // length code 285 = 258
        put_code(0, 5);    // distance code 0 = 1
    }
    put_code(0, 7); // end of block
    if (bits > 0) {
        out.push_back(static_cast<std::uint8_t>(acc));
    }
    ZipEntry e;
    e.name = std::move(name);
    e.method = 8;
    e.declared_size = declared_size;
    e.payload = std::move(out);
    return e;
}

inline std::vector<std::uint8_t> zip_of(const std::vector<ZipEntry>& entries) {
    std::vector<std::uint8_t> out;
    auto u16 = [&](std::uint16_t v) {
        out.push_back(static_cast<std::uint8_t>(v));
        out.push_back(static_cast<std::uint8_t>(v >> 8));
    };
    auto u32 = [&](std::uint32_t v) {
        u16(static_cast<std::uint16_t>(v));
        u16(static_cast<std::uint16_t>(v >> 16));
    };
    std::vector<std::uint32_t> offsets;
    for (const ZipEntry& e : entries) {
        offsets.push_back(static_cast<std::uint32_t>(out.size()));
        u32(0x04034b50);
        u16(20);
        u16(0);
        u16(e.method);
        u16(0);
        u16(0x21); // 1980-01-01
        u32(e.crc);
        u32(static_cast<std::uint32_t>(e.payload.size()));
        u32(e.declared_size);
        u16(static_cast<std::uint16_t>(e.name.size()));
        u16(0);
        out.insert(out.end(), e.name.begin(), e.name.end());
        out.insert(out.end(), e.payload.begin(), e.payload.end());
    }
    const auto directory_at = static_cast<std::uint32_t>(out.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const ZipEntry& e = entries[i];
        u32(0x02014b50);
        u16(20);
        u16(20);
        u16(0);
        u16(e.method);
        u16(0);
        u16(0x21);
        u32(e.crc);
        u32(static_cast<std::uint32_t>(e.payload.size()));
        u32(e.declared_size);
        u16(static_cast<std::uint16_t>(e.name.size()));
        u16(0);
        u16(0);
        u16(0);
        u16(0);
        u32(0);
        u32(offsets[i]);
        out.insert(out.end(), e.name.begin(), e.name.end());
    }
    const auto directory_size = static_cast<std::uint32_t>(out.size()) - directory_at;
    u32(0x06054b50);
    u16(0);
    u16(0);
    u16(static_cast<std::uint16_t>(entries.size()));
    u16(static_cast<std::uint16_t>(entries.size()));
    u32(directory_size);
    u32(directory_at);
    u16(0);
    return out;
}

} // namespace toonval::test
