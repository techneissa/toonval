// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/key_state.hpp"
#include "toonval/midi_event.hpp"

#include <cstdint>
#include <thread>
#include <vector>

#include <doctest/doctest.h>

using namespace toonval;

namespace {
MidiEvent note_on(std::int64_t t, std::uint8_t note) {
    return {t, MidiEventType::NoteOn, 0, note, 100};
}
MidiEvent note_off(std::int64_t t, std::uint8_t note) {
    return {t, MidiEventType::NoteOff, 0, note, 0};
}
MidiEvent cc(std::int64_t t, std::uint8_t number, std::uint8_t value) {
    return {t, MidiEventType::ControlChange, 0, number, value};
}

template <std::size_t N> std::vector<MidiEvent> pop_all(MidiEventQueue<N>& q) {
    std::vector<MidiEvent> out;
    MidiEvent ev;
    while (q.pop(ev)) {
        out.push_back(ev);
    }
    return out;
}
} // namespace

// --- Trainer event filter ----------------------------------------------------------------------

TEST_CASE("the input filter keeps notes and the controllers the trainer uses") {
    CHECK(is_trainer_event(note_on(0, 60)));
    CHECK(is_trainer_event(note_off(0, 60)));
    CHECK(is_trainer_event(cc(0, kCcSustain, 127)));
    CHECK(is_trainer_event(cc(0, kCcAllSoundOff, 0)));
    CHECK(is_trainer_event(cc(0, kCcAllNotesOff, 0)));
}

TEST_CASE("the input filter drops controllers the trainer ignores and Other") {
    for (int number : {0, 1, 7, 10, 11, 63, 65, 66, 67, 121, 122, 124, 127}) {
        CHECK_FALSE(is_trainer_event(cc(0, static_cast<std::uint8_t>(number), 64)));
    }
    CHECK_FALSE(is_trainer_event(MidiEvent{}));
}

// --- Overflow resync ---------------------------------------------------------------------------

TEST_CASE("a queue with room passes events through unchanged") {
    MidiEventQueue<8> q;
    q.push(note_on(1, 60));
    q.push(note_off(2, 60));
    const auto out = pop_all(q);
    REQUIRE(out.size() == 2);
    CHECK(out[0].type == MidiEventType::NoteOn);
    CHECK(out[1].type == MidiEventType::NoteOff);
    CHECK_FALSE(out[0].synthetic);
    CHECK(q.dropped() == 0);
    CHECK(q.resyncs() == 0);
}

TEST_CASE("overflow inserts one All Notes Off where the events were dropped") {
    MidiEventQueue<8> q; // 7 events fit, the last slot is kept for the resync
    for (int i = 0; i < 7; ++i) {
        q.push(note_on(i, static_cast<std::uint8_t>(60 + i)));
    }
    q.push(note_off(100, 60)); // dropped: the queue holds 7 of 8
    q.push(note_off(101, 61)); // dropped too, no second resync in the same gap
    CHECK(q.dropped() == 2);
    CHECK(q.resyncs() == 1);

    const auto out = pop_all(q);
    REQUIRE(out.size() == 8);
    const MidiEvent& resync = out[7];
    CHECK(resync.type == MidiEventType::ControlChange);
    CHECK(resync.data1 == kCcAllNotesOff);
    CHECK(resync.data2 == 0);
    CHECK(resync.synthetic);
    CHECK(resync.time_ns == 100); // stamped with the first dropped event
}

TEST_CASE("a dropped note-off never leaves a key lit") {
    MidiEventQueue<4> q;
    KeyState keys;
    q.push(note_on(1, 60));
    q.push(note_on(2, 62));
    q.push(note_on(3, 64));
    q.push(note_off(4, 60)); // dropped
    q.push(note_off(5, 62)); // dropped
    for (const auto& ev : pop_all(q)) {
        keys.apply(ev);
    }
    CHECK(keys.held_count() == 0);
    // Events after the gap apply normally once there is room again.
    q.push(note_on(6, 67));
    q.push(note_off(7, 64)); // the release of a key the resync already let go: no effect
    for (const auto& ev : pop_all(q)) {
        keys.apply(ev);
    }
    CHECK(keys.held_notes() == std::vector<std::uint8_t>{67});
}

TEST_CASE("each separate overflow gets its own resync") {
    MidiEventQueue<4> q;
    for (int round = 0; round < 3; ++round) {
        for (int i = 0; i < 5; ++i) {
            q.push(note_on(round * 10 + i, 60));
        }
        const auto out = pop_all(q);
        REQUIRE(out.size() == 4);
        CHECK(out.back().synthetic);
        CHECK(out.back().time_ns == round * 10 + 3);
    }
    CHECK(q.resyncs() == 3);
    CHECK(q.dropped() == 6);
}

TEST_CASE("the resync slot is never used by a normal event") {
    MidiEventQueue<4> q;
    q.push(note_on(1, 60));
    q.push(note_on(2, 61));
    q.push(note_on(3, 62));
    MidiEvent ev;
    REQUIRE(q.pop(ev)); // room for one more normal event
    q.push(note_on(4, 63));
    q.push(note_on(5, 64)); // dropped, resync takes the reserved slot
    const auto out = pop_all(q);
    REQUIRE(out.size() == 4);
    CHECK(out[2].data1 == 63);
    CHECK(out[3].synthetic);
    CHECK(q.dropped() == 1);
}

TEST_CASE("an overflowing producer thread never leaves a key held at the consumer") {
    // The producer presses and releases notes far faster than the consumer pops, so the queue
    // overflows many times. Whatever is dropped, the consumer ends with no key held, and every
    // gap was announced by exactly one resync.
    MidiEventQueue<64> q;
    constexpr int kPairs = 200000;
    std::thread producer([&q] {
        for (int i = 0; i < kPairs; ++i) {
            const auto note = static_cast<std::uint8_t>(36 + i % 61);
            q.push(note_on(2 * i, note));
            q.push(note_off(2 * i + 1, note));
        }
    });
    KeyState keys;
    std::uint64_t popped = 0;
    std::uint64_t resyncs_seen = 0;
    std::int64_t last_time = -1;
    bool in_order = true;
    MidiEvent ev;
    auto consume = [&] {
        while (q.pop(ev)) {
            ++popped;
            resyncs_seen += ev.synthetic ? 1 : 0;
            in_order = in_order && ev.time_ns >= last_time;
            last_time = ev.time_ns;
            keys.apply(ev);
        }
    };
    for (int i = 0; i < 2000; ++i) {
        consume();
        std::this_thread::yield();
    }
    producer.join();
    consume();
    CHECK(in_order);
    CHECK(keys.held_count() == 0);
    CHECK(resyncs_seen == q.resyncs());
    CHECK(popped == 2u * kPairs - q.dropped() + q.resyncs());
}

// --- Merge across ports ------------------------------------------------------------------------

namespace {
// Pops from per-source vectors, front first, for merge_by_time.
struct VectorSources {
    std::vector<std::vector<MidiEvent>> sources;
    std::vector<std::size_t> next = std::vector<std::size_t>(8, 0);

    bool pop(std::size_t i, MidiEvent& out) {
        if (i >= sources.size() || next[i] >= sources[i].size()) {
            return false;
        }
        out = sources[i][next[i]++];
        return true;
    }
};

std::vector<MidiEvent> merged(VectorSources& src, std::size_t max_per_source = 1000) {
    std::vector<MidiEvent> out;
    merge_by_time<8>([&src](std::size_t i, MidiEvent& ev) { return src.pop(i, ev); },
                     [&out](const MidiEvent& ev) { out.push_back(ev); }, max_per_source);
    return out;
}
} // namespace

TEST_CASE("events from several ports come out in time order") {
    VectorSources src;
    src.sources = {{note_on(10, 60), note_off(40, 60)},
                   {note_on(5, 62), note_off(30, 62)},
                   {},
                   {note_on(20, 64)}};
    const auto out = merged(src);
    REQUIRE(out.size() == 5);
    const std::int64_t expected[] = {5, 10, 20, 30, 40};
    for (std::size_t i = 0; i < out.size(); ++i) {
        CHECK(out[i].time_ns == expected[i]);
    }
}

TEST_CASE("events with equal times keep port order, then each port's own order") {
    VectorSources src;
    src.sources = {{note_on(10, 60), note_off(10, 60)}, {note_on(10, 62)}};
    const auto out = merged(src);
    REQUIRE(out.size() == 3);
    CHECK(out[0].data1 == 60);
    CHECK(out[0].type == MidiEventType::NoteOn);
    CHECK(out[1].data1 == 60);
    CHECK(out[1].type == MidiEventType::NoteOff);
    CHECK(out[2].data1 == 62);
}

TEST_CASE("a source with no events and no sources at all are fine") {
    VectorSources src;
    CHECK(merged(src).empty());
    src.sources = {{}, {}, {note_on(1, 60)}};
    CHECK(merged(src).size() == 1);
}

TEST_CASE("a flooding source is cut off after its limit so drain always returns") {
    VectorSources src;
    src.sources = {std::vector<MidiEvent>(50, note_on(1, 60)), {note_on(2, 62)}};
    const auto out = merged(src, 10);
    CHECK(out.size() == 11);
    CHECK(out.back().data1 == 62);
    // The rest stays for the next drain.
    CHECK(merged(src, 10).size() == 10);
}
