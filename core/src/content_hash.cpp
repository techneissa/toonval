// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/content_hash.hpp"

namespace toonval {

std::uint64_t content_hash(const void* data, std::size_t size) {
    constexpr std::uint64_t kOffsetBasis = 0xcbf29ce484222325ULL;
    constexpr std::uint64_t kPrime = 0x100000001b3ULL;
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::uint64_t hash = kOffsetBasis;
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= kPrime;
    }
    return hash;
}

std::string content_hash_hex(std::uint64_t hash) {
    constexpr char kDigits[] = "0123456789abcdef";
    std::string text(16, '0');
    for (int i = 15; i >= 0; --i) {
        text[static_cast<std::size_t>(i)] = kDigits[hash & 0xf];
        hash >>= 4;
    }
    return text;
}

bool parse_content_hash_hex(std::string_view text, std::uint64_t& out) {
    if (text.empty() || text.size() > 16) {
        return false;
    }
    std::uint64_t value = 0;
    for (char c : text) {
        int digit;
        if (c >= '0' && c <= '9') {
            digit = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            digit = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            digit = c - 'A' + 10;
        } else {
            return false;
        }
        value = (value << 4) | static_cast<std::uint64_t>(digit);
    }
    out = value;
    return true;
}

} // namespace toonval
