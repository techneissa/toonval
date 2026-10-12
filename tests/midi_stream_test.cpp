// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/midi_event.hpp"

#include <cstdint>
#include <vector>

#include <doctest/doctest.h>

using namespace toonval;

namespace {
using Bytes = std::vector<std::uint8_t>;

std::vector<Bytes> split(const Bytes& chunk) {
    std::vector<Bytes> out;
    MidiStreamSplitter splitter(chunk);
    ChannelMessage msg;
    while (splitter.next(msg)) {
        out.emplace_back(msg.bytes.begin(), msg.bytes.begin() + static_cast<long>(msg.size));
    }
    return out;
}
} // namespace

TEST_CASE("splitter yields a single complete message") {
    CHECK(split({0x90, 60, 100}) == std::vector<Bytes>{{0x90, 60, 100}});
}

TEST_CASE("splitter yields several messages packed in one chunk") {
    CHECK(split({0x90, 60, 100, 0x90, 64, 90, 0xB0, 64, 127}) ==
          std::vector<Bytes>{{0x90, 60, 100}, {0x90, 64, 90}, {0xB0, 64, 127}});
}

TEST_CASE("splitter expands running status into full messages") {
    // A chord sent with running status, then a velocity-0 note-off the same way.
    CHECK(split({0x90, 60, 100, 64, 90, 67, 80, 60, 0}) ==
          std::vector<Bytes>{{0x90, 60, 100}, {0x90, 64, 90}, {0x90, 67, 80}, {0x90, 60, 0}});
}

TEST_CASE("splitter keeps two-byte messages two bytes long") {
    CHECK(split({0xC0, 5, 0xD2, 40}) == std::vector<Bytes>{{0xC0, 5}, {0xD2, 40}});
}

TEST_CASE("splitter skips clock and active sensing, even inside a message") {
    CHECK(split({0xF8, 0x90, 60, 0xFE, 100, 0xF8}) == std::vector<Bytes>{{0x90, 60, 100}});
}

TEST_CASE("splitter skips SysEx even when its bytes look like a note") {
    CHECK(split({0xF0, 0x43, 0x10, 0x4C, 0xF7, 0x90, 60, 100}) ==
          std::vector<Bytes>{{0x90, 60, 100}});
}

TEST_CASE("splitter skips a SysEx continuation chunk with no status byte") {
    CHECK(split({0x10, 0x4C, 0x00, 0xF7}).empty());
}

TEST_CASE("splitter drops a message cut off by a new status byte") {
    CHECK(split({0x90, 60, 0x80, 60, 0}) == std::vector<Bytes>{{0x80, 60, 0}});
}

TEST_CASE("splitter drops a message cut off by the end of the chunk") {
    CHECK(split({0x90, 60, 100, 0x90, 64}) == std::vector<Bytes>{{0x90, 60, 100}});
}

TEST_CASE("system common messages cancel running status") {
    CHECK(split({0x90, 60, 100, 0xF2, 1, 2, 64, 90}) == std::vector<Bytes>{{0x90, 60, 100}});
}

TEST_CASE("splitter handles empty and null input") {
    CHECK(split({}).empty());
    MidiStreamSplitter splitter(nullptr, 3);
    ChannelMessage msg;
    CHECK_FALSE(splitter.next(msg));
}

// --- State across chunks ---------------------------------------------------------------------
// A port's splitter lives as long as the port, so a message split across reads (raw ALSA hands
// over arbitrary byte counts) is reassembled instead of dropped, and never misread.

namespace {
// Feeds `chunks` one after another into one splitter and collects every message.
std::vector<Bytes> split_chunks(const std::vector<Bytes>& chunks) {
    std::vector<Bytes> out;
    MidiStreamSplitter splitter;
    ChannelMessage msg;
    for (const auto& chunk : chunks) {
        splitter.feed(chunk);
        while (splitter.next(msg)) {
            out.emplace_back(msg.bytes.begin(), msg.bytes.begin() + static_cast<long>(msg.size));
        }
    }
    return out;
}

// Every way of cutting `stream` into two and into three chunks gives the unsplit result.
void check_every_split(const Bytes& stream) {
    const auto whole = split_chunks({stream});
    for (std::size_t i = 0; i <= stream.size(); ++i) {
        const Bytes a(stream.begin(), stream.begin() + static_cast<long>(i));
        const Bytes b(stream.begin() + static_cast<long>(i), stream.end());
        CHECK(split_chunks({a, b}) == whole);
        for (std::size_t j = i; j <= stream.size(); ++j) {
            const Bytes m(stream.begin() + static_cast<long>(i),
                          stream.begin() + static_cast<long>(j));
            const Bytes c(stream.begin() + static_cast<long>(j), stream.end());
            CHECK(split_chunks({a, m, c}) == whole);
        }
    }
    // One byte per chunk, the worst case.
    std::vector<Bytes> singles;
    for (auto byte : stream) {
        singles.push_back({byte});
    }
    CHECK(split_chunks(singles) == whole);
}
} // namespace

TEST_CASE("a note-on split across two chunks is reassembled") {
    CHECK(split_chunks({{0x90, 60}, {100}}) == std::vector<Bytes>{{0x90, 60, 100}});
    CHECK(split_chunks({{0x80}, {60, 0}}) == std::vector<Bytes>{{0x80, 60, 0}});
}

TEST_CASE("running status carries across chunks") {
    CHECK(split_chunks({{0x90, 60, 100}, {64, 90}}) ==
          std::vector<Bytes>{{0x90, 60, 100}, {0x90, 64, 90}});
    // Velocity-0 note-off with running status, cut between its two data bytes.
    CHECK(split_chunks({{0x90, 60, 100, 60}, {0}}) ==
          std::vector<Bytes>{{0x90, 60, 100}, {0x90, 60, 0}});
}

TEST_CASE("realtime bytes inside a split message are skipped without breaking it") {
    CHECK(split_chunks({{0x90, 0xF8}, {60, 0xFE}, {0xF8, 100}}) ==
          std::vector<Bytes>{{0x90, 60, 100}});
    // Realtime between running-status messages does not cancel running status.
    CHECK(split({0x90, 60, 100, 0xF8, 64, 90}) ==
          std::vector<Bytes>{{0x90, 60, 100}, {0x90, 64, 90}});
    CHECK(split({0x90, 60, 0xFF, 100}) == std::vector<Bytes>{{0x90, 60, 100}});
    CHECK(split({0x90, 60, 100, 0xFE, 64, 0xFE, 90}) ==
          std::vector<Bytes>{{0x90, 60, 100}, {0x90, 64, 90}});
}

TEST_CASE("SysEx split across chunks is skipped, and its data never becomes a note") {
    // A note-on running status is set before the SysEx; the SysEx continuation bytes in the
    // second chunk look like note data but must not be read as notes.
    CHECK(split_chunks({{0x90, 60, 100, 0xF0, 0x43, 0x10},
                        {0x4C, 0x00, 0x7F},
                        {0x10, 0xF7},
                        {0x90, 62, 80}}) == std::vector<Bytes>{{0x90, 60, 100}, {0x90, 62, 80}});
    // The data after F7 has no status: dropped, not read with the old running status.
    CHECK(split_chunks({{0x90, 60, 100, 0xF0, 1}, {2, 0xF7}, {64, 90}}) ==
          std::vector<Bytes>{{0x90, 60, 100}});
}

TEST_CASE("a stray F7 before a note does not hide the note") {
    CHECK(split({0xF7, 0x90, 60, 100}) == std::vector<Bytes>{{0x90, 60, 100}});
}

TEST_CASE("a SysEx aborted by a channel status yields that channel message") {
    CHECK(split({0xF0, 0x43, 0x10, 0x90, 60, 100}) == std::vector<Bytes>{{0x90, 60, 100}});
}

TEST_CASE("a system common F1 cancels running status") {
    CHECK(split({0x90, 60, 100, 0xF1, 0x10, 64, 90}) == std::vector<Bytes>{{0x90, 60, 100}});
}

TEST_CASE("a message cut by a status byte in the next chunk is dropped, not misread") {
    CHECK(split_chunks({{0x90, 60}, {0x80, 60, 0}}) == std::vector<Bytes>{{0x80, 60, 0}});
}

TEST_CASE("every split point of a stream gives the unsplit messages") {
    check_every_split({0x90, 60, 100, 64, 90, 0xF8, 67, 80, 0x80, 60, 0, 0xB0, 64, 127});
    check_every_split({0xC0, 5, 0xF8, 6, 0xE0, 0, 64, 0xD0, 30});
    check_every_split({0x90, 60,  100, 0xF0, 0x43, 0x10, 0x4C, 0xF8, 0xF7, 0x90, 62, 0,
                       0xB0, 123, 0,   0xF2, 1,    2,    64,   90,   0x90, 64,   0});
    check_every_split({0x22, 0x90, 0xFE, 60, 0xF8, 100, 0xF1, 3, 0x80, 0x90, 61, 0xFF, 1});
}

TEST_CASE("every split point of random streams matches the unsplit result") {
    // Fixed-seed generator: status, data, realtime, SysEx and system common bytes mixed.
    std::uint32_t state = 12345;
    auto rnd = [&state] {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    };
    for (int round = 0; round < 60; ++round) {
        Bytes stream;
        const int length = 4 + static_cast<int>(rnd() % 16);
        for (int i = 0; i < length; ++i) {
            const auto kind = rnd() % 10;
            if (kind < 5) {
                stream.push_back(static_cast<std::uint8_t>(rnd() % 0x80));
            } else if (kind < 8) {
                stream.push_back(static_cast<std::uint8_t>(0x80 + rnd() % 0x70));
            } else if (kind == 8) {
                stream.push_back(static_cast<std::uint8_t>(0xF8 + rnd() % 8));
            } else {
                stream.push_back(static_cast<std::uint8_t>(0xF0 + rnd() % 8));
            }
        }
        check_every_split(stream);
    }
}

TEST_CASE("reset forgets a partial message and running status") {
    MidiStreamSplitter splitter;
    ChannelMessage msg;
    const Bytes first{0x90, 60};
    splitter.feed(first);
    CHECK_FALSE(splitter.next(msg));
    splitter.reset();
    const Bytes second{100, 64, 90};
    splitter.feed(second);
    CHECK_FALSE(splitter.next(msg));
}
