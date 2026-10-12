// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/midi_event.hpp"
#include "toonval/spsc_queue.hpp"

#include <cstdint>
#include <thread>

#include <doctest/doctest.h>

using namespace toonval;

TEST_CASE("an empty queue pops nothing") {
    SpscQueue<int, 4> q;
    int out = -1;
    CHECK_FALSE(q.pop(out));
    CHECK(out == -1);
    CHECK(q.dropped() == 0);
}

TEST_CASE("items pop in the order they were pushed") {
    SpscQueue<int, 8> q;
    for (int i = 0; i < 5; ++i) {
        CHECK(q.push(i));
    }
    for (int i = 0; i < 5; ++i) {
        int out = -1;
        REQUIRE(q.pop(out));
        CHECK(out == i);
    }
    int out = -1;
    CHECK_FALSE(q.pop(out));
}

TEST_CASE("a full queue drops new items and counts them") {
    SpscQueue<int, 4> q;
    for (int i = 0; i < 4; ++i) {
        CHECK(q.push(i));
    }
    CHECK_FALSE(q.push(100));
    CHECK_FALSE(q.push(101));
    CHECK(q.dropped() == 2);
    // The items already queued are intact; the dropped ones never appear.
    for (int i = 0; i < 4; ++i) {
        int out = -1;
        REQUIRE(q.pop(out));
        CHECK(out == i);
    }
    int out = -1;
    CHECK_FALSE(q.pop(out));
}

TEST_CASE("the queue keeps working as indices wrap around the buffer many times") {
    SpscQueue<int, 4> q;
    for (int i = 0; i < 1000; ++i) {
        CHECK(q.push(i));
        CHECK(q.push(i + 1));
        int a = -1;
        int b = -1;
        REQUIRE(q.pop(a));
        REQUIRE(q.pop(b));
        CHECK(a == i);
        CHECK(b == i + 1);
    }
    CHECK(q.dropped() == 0);
}

TEST_CASE("MidiEvent passes through the queue unchanged") {
    SpscQueue<MidiEvent, 16> q;
    const MidiEvent in{123456789, MidiEventType::NoteOn, 3, 60, 100};
    CHECK(q.push(in));
    MidiEvent out;
    REQUIRE(q.pop(out));
    CHECK(out.time_ns == in.time_ns);
    CHECK(out.type == in.type);
    CHECK(out.channel == in.channel);
    CHECK(out.data1 == in.data1);
    CHECK(out.data2 == in.data2);
}

TEST_CASE("two threads: every item arrives once and in order, or is counted as dropped") {
    // A small queue so the producer overruns the consumer and both paths run under contention.
    SpscQueue<MidiEvent, 64> q;
    constexpr std::int64_t kItems = 200000;

    std::thread producer([&q] {
        for (std::int64_t i = 0; i < kItems; ++i) {
            // Every field derives from i, so a torn read shows up as a mismatch below.
            const auto low = static_cast<std::uint8_t>(i & 0x7F);
            q.push(MidiEvent{i, MidiEventType::NoteOn, static_cast<std::uint8_t>(i & 0x0F), low,
                             static_cast<std::uint8_t>(127 - low)});
        }
    });

    std::int64_t received = 0;
    std::int64_t last = -1;
    bool ordered = true;
    bool intact = true;
    // Spin until everything pushed is accounted for as received or dropped.
    while (true) {
        MidiEvent ev;
        if (q.pop(ev)) {
            ordered = ordered && ev.time_ns > last;
            const auto low = static_cast<std::uint8_t>(ev.time_ns & 0x7F);
            intact = intact && ev.data1 == low && ev.data2 == 127 - low &&
                     ev.channel == (ev.time_ns & 0x0F);
            last = ev.time_ns;
            ++received;
        } else if (received + static_cast<std::int64_t>(q.dropped()) == kItems) {
            break;
        }
    }
    producer.join();

    CHECK(ordered);
    CHECK(intact);
    CHECK(received + static_cast<std::int64_t>(q.dropped()) == kItems);
    CHECK(received > 0);
    MidiEvent ev;
    CHECK_FALSE(q.pop(ev));
}
