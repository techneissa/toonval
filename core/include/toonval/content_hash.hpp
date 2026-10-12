// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace toonval {

// FNV-1a, 64 bit, over the bytes of a song file. It tells the app that a saved session's song was
// moved, edited or replaced; it is not a security hash. The same bytes give the same value on
// every platform.
std::uint64_t content_hash(const void* data, std::size_t size);
inline std::uint64_t content_hash(std::string_view bytes) {
    return content_hash(bytes.data(), bytes.size());
}

// The text form used in saved files: 16 lowercase hex digits.
std::string content_hash_hex(std::uint64_t hash);
// Accepts 1 to 16 hex digits of either case and nothing else. `out` is untouched on failure.
bool parse_content_hash_hex(std::string_view text, std::uint64_t& out);

} // namespace toonval
