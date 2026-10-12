// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/content_hash.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

using namespace toonval;

TEST_CASE("content_hash matches the published FNV-1a 64 test vectors") {
    CHECK(content_hash("") == 0xcbf29ce484222325ULL);
    CHECK(content_hash("a") == 0xaf63dc4c8601ec8cULL);
    CHECK(content_hash("foobar") == 0x85944171f73967e8ULL);
}

TEST_CASE("content_hash of bytes equals the hash of the same text") {
    const std::vector<std::uint8_t> bytes{'f', 'o', 'o', 'b', 'a', 'r'};
    CHECK(content_hash(bytes.data(), bytes.size()) == content_hash("foobar"));
    CHECK(content_hash(nullptr, 0) == content_hash(""));
}

TEST_CASE("content_hash changes with one flipped bit, an appended byte, or a swap") {
    std::string song("MThd\x00\x00\x00\x06 song bytes", 20);
    REQUIRE(song.size() == 20);
    const std::uint64_t base = content_hash(song);
    std::string flipped = song;
    flipped[7] = static_cast<char>(flipped[7] ^ 0x01);
    CHECK(content_hash(flipped) != base);
    CHECK(content_hash(song + '\0') != base);
    CHECK(content_hash("ab") != content_hash("ba"));
}

TEST_CASE("content_hash_hex is 16 lowercase digits, zero padded, and round trips") {
    CHECK(content_hash_hex(0x1) == "0000000000000001");
    CHECK(content_hash_hex(0xcbf29ce484222325ULL) == "cbf29ce484222325");
    std::uint64_t back = 0;
    CHECK(parse_content_hash_hex("cbf29ce484222325", back));
    CHECK(back == 0xcbf29ce484222325ULL);
    CHECK(parse_content_hash_hex("CBF29CE484222325", back));
    CHECK(back == 0xcbf29ce484222325ULL);
}

TEST_CASE("parse_content_hash_hex refuses anything that is not 1 to 16 hex digits") {
    std::uint64_t out = 7;
    CHECK_FALSE(parse_content_hash_hex("", out));
    CHECK_FALSE(parse_content_hash_hex("0123456789abcdef0", out)); // 17 digits
    CHECK_FALSE(parse_content_hash_hex("xyz", out));
    CHECK_FALSE(parse_content_hash_hex("0x10", out));
    CHECK_FALSE(parse_content_hash_hex("-1", out));
    CHECK_FALSE(parse_content_hash_hex(" 10", out));
    CHECK(out == 7); // untouched on failure
}
