// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/note_trail.hpp"

#include <doctest/doctest.h>

using namespace toonval;

namespace {
constexpr std::int64_t kMs = 1'000'000;
constexpr std::int64_t kSec = 1'000'000'000;

KeyTransition pressed(int note, int velocity = 80) {
    return {KeyTransitionType::Pressed, static_cast<std::uint8_t>(note),
            static_cast<std::uint8_t>(velocity), 0};
}
KeyTransition released(int note) {
    return {KeyTransitionType::Released, static_cast<std::uint8_t>(note), 0, 0};
}
KeyTransition all_released(int count) {
    return {KeyTransitionType::AllReleased, 0, 0, static_cast<std::uint8_t>(count)};
}
} // namespace

TEST_CASE("a new NoteTrail is empty") {
    NoteTrail trail;
    CHECK(trail.size() == 0);
    CHECK(trail.visible(0, kSec).empty());
}

TEST_CASE("a press opens a block with its note, velocity and start time") {
    NoteTrail trail;
    trail.apply(pressed(60, 91), 5 * kMs);
    REQUIRE(trail.size() == 1);
    const TrailBlock& b = trail.blocks()[0];
    CHECK(b.note == 60);
    CHECK(b.velocity == 91);
    CHECK(b.start_ns == 5 * kMs);
    CHECK(b.open);
}

TEST_CASE("a release closes the block at the release time") {
    NoteTrail trail;
    trail.apply(pressed(60), 0);
    trail.apply(released(60), 300 * kMs);
    REQUIRE(trail.size() == 1);
    CHECK_FALSE(trail.blocks()[0].open);
    CHECK(trail.blocks()[0].end_ns == 300 * kMs);
}

TEST_CASE("a release only closes the block of its own note") {
    NoteTrail trail;
    trail.apply(pressed(60), 0);
    trail.apply(pressed(64), 10 * kMs);
    trail.apply(released(64), 50 * kMs);
    CHECK(trail.blocks()[0].open);
    CHECK_FALSE(trail.blocks()[1].open);
}

TEST_CASE("a release with no open block is ignored") {
    NoteTrail trail;
    trail.apply(released(60), 10 * kMs);
    CHECK(trail.size() == 0);
}

TEST_CASE("a second release of the same note does not move the end time") {
    NoteTrail trail;
    trail.apply(pressed(60), 0);
    trail.apply(released(60), 100 * kMs);
    trail.apply(released(60), 900 * kMs);
    CHECK(trail.blocks()[0].end_ns == 100 * kMs);
}

TEST_CASE("AllReleased closes every open block at that time and leaves closed ones alone") {
    NoteTrail trail;
    trail.apply(pressed(60), 0);
    trail.apply(released(60), 100 * kMs);
    trail.apply(pressed(60), 200 * kMs);
    trail.apply(pressed(64), 210 * kMs);
    trail.apply(all_released(2), 500 * kMs);
    REQUIRE(trail.size() == 3);
    CHECK(trail.blocks()[0].end_ns == 100 * kMs);
    CHECK_FALSE(trail.blocks()[1].open);
    CHECK(trail.blocks()[1].end_ns == 500 * kMs);
    CHECK_FALSE(trail.blocks()[2].open);
    CHECK(trail.blocks()[2].end_ns == 500 * kMs);
}

TEST_CASE("a press for a note that already has an open block closes the old block first") {
    NoteTrail trail;
    trail.apply(pressed(60, 50), 0);
    trail.apply(pressed(60, 70), 200 * kMs);
    REQUIRE(trail.size() == 2);
    CHECK_FALSE(trail.blocks()[0].open);
    CHECK(trail.blocks()[0].end_ns == 200 * kMs);
    CHECK(trail.blocks()[1].open);
    CHECK(trail.blocks()[1].velocity == 70);
    CHECK(trail.blocks()[1].start_ns == 200 * kMs);
}

TEST_CASE("a transition of type None or with an invalid note changes nothing") {
    NoteTrail trail;
    trail.apply({}, 0);
    trail.apply(pressed(128), 0);
    trail.apply(pressed(255), 0);
    CHECK(trail.size() == 0);
}

TEST_CASE("a release stamped before the press ends the block at its start") {
    NoteTrail trail;
    trail.apply(pressed(60), 100 * kMs);
    trail.apply(released(60), 90 * kMs);
    CHECK(trail.blocks()[0].end_ns == 100 * kMs);
}

TEST_CASE("a held block grows from the keyboard") {
    NoteTrail trail;
    trail.apply(pressed(60, 88), 0);
    const std::int64_t span = 4 * kSec;
    const auto bars = trail.visible(1 * kSec, span);
    REQUIRE(bars.size() == 1);
    CHECK(bars[0].note == 60);
    CHECK(bars[0].velocity == 88);
    CHECK(bars[0].held);
    CHECK(bars[0].bottom == 0.0);
    CHECK(bars[0].top == doctest::Approx(0.25));
}

TEST_CASE("a held block stops at the top of the screen") {
    NoteTrail trail;
    trail.apply(pressed(60), 0);
    const auto bars = trail.visible(10 * kSec, 4 * kSec);
    REQUIRE(bars.size() == 1);
    CHECK(bars[0].bottom == 0.0);
    CHECK(bars[0].top == 1.0);
}

TEST_CASE("a released block lifts off the keyboard and rises") {
    NoteTrail trail;
    trail.apply(pressed(60), 0);
    trail.apply(released(60), 1 * kSec);
    const auto bars = trail.visible(2 * kSec, 4 * kSec);
    REQUIRE(bars.size() == 1);
    CHECK_FALSE(bars[0].held);
    CHECK(bars[0].bottom == doctest::Approx(0.25));
    CHECK(bars[0].top == doctest::Approx(0.5));
}

TEST_CASE("a block is partly visible while its top is off the screen") {
    NoteTrail trail;
    trail.apply(pressed(60), 0);
    trail.apply(released(60), 1 * kSec);
    const auto bars = trail.visible(4 * kSec + 500 * kMs, 4 * kSec);
    REQUIRE(bars.size() == 1);
    CHECK(bars[0].bottom == doctest::Approx(0.875));
    CHECK(bars[0].top == 1.0);
}

TEST_CASE("a block whose bottom reaches the top of the screen is not visible") {
    NoteTrail trail;
    trail.apply(pressed(60), 0);
    trail.apply(released(60), 1 * kSec);
    CHECK(trail.visible(5 * kSec - 1, 4 * kSec).size() == 1);
    CHECK(trail.visible(5 * kSec, 4 * kSec).empty());
}

TEST_CASE("visible returns bars oldest first") {
    NoteTrail trail;
    trail.apply(pressed(72), 0);
    trail.apply(pressed(60), 100 * kMs);
    const auto bars = trail.visible(1 * kSec, 4 * kSec);
    REQUIRE(bars.size() == 2);
    CHECK(bars[0].note == 72);
    CHECK(bars[1].note == 60);
}

TEST_CASE("a block that starts in the future of now never has a negative height") {
    NoteTrail trail;
    trail.apply(pressed(60), 100 * kMs);
    const auto bars = trail.visible(90 * kMs, 4 * kSec);
    REQUIRE(bars.size() == 1);
    CHECK(bars[0].held);
    CHECK(bars[0].bottom == 0.0);
    CHECK(bars[0].top == 0.0);
}

TEST_CASE("a closed block that ends in the future of now is clamped to the keyboard") {
    NoteTrail trail;
    trail.apply(pressed(60), 0);
    trail.apply(released(60), 200 * kMs);
    const auto bars = trail.visible(100 * kMs, 4 * kSec);
    REQUIRE(bars.size() == 1);
    CHECK(bars[0].bottom == 0.0);
    CHECK(bars[0].top == doctest::Approx(0.025));
}

TEST_CASE("a closed block entirely in the future of now is left out") {
    NoteTrail trail;
    trail.apply(pressed(60), 100 * kMs);
    trail.apply(released(60), 200 * kMs);
    CHECK(trail.visible(50 * kMs, 4 * kSec).empty());
}

TEST_CASE("a closed block with no duration is left out") {
    NoteTrail trail;
    trail.apply(pressed(60), 0);
    trail.apply(released(60), 0);
    CHECK(trail.visible(1 * kSec, 4 * kSec).empty());
}

TEST_CASE("every bar stays within 0..1 with bottom <= top, whatever the clock skew") {
    NoteTrail trail;
    trail.apply(pressed(60), 1 * kSec);
    trail.apply(released(60), 2 * kSec);
    trail.apply(pressed(62), 3 * kSec);
    for (std::int64_t now = -2 * kSec; now <= 12 * kSec; now += 250 * kMs) {
        for (const TrailBar& bar : trail.visible(now, 4 * kSec)) {
            CHECK(bar.bottom >= 0.0);
            CHECK(bar.top <= 1.0);
            CHECK(bar.bottom <= bar.top);
        }
    }
}

TEST_CASE("a span of zero or less shows nothing") {
    NoteTrail trail;
    trail.apply(pressed(60), 0);
    CHECK(trail.visible(1 * kSec, 0).empty());
    CHECK(trail.visible(1 * kSec, -kSec).empty());
}

TEST_CASE("prune drops closed blocks ended longer ago than the horizon") {
    NoteTrail trail;
    trail.apply(pressed(60), 0);
    trail.apply(released(60), 1 * kSec);
    trail.apply(pressed(62), 0);
    trail.apply(released(62), 2 * kSec);
    // Note 60 ended 4 s before now, note 62 ended 3 s before now.
    trail.prune(5 * kSec, 3 * kSec + 500 * kMs);
    REQUIRE(trail.size() == 1);
    CHECK(trail.blocks()[0].note == 62);
}

TEST_CASE("prune keeps a block that ended exactly one horizon ago") {
    NoteTrail trail;
    trail.apply(pressed(60), 0);
    trail.apply(released(60), 1 * kSec);
    trail.prune(5 * kSec, 4 * kSec);
    CHECK(trail.size() == 1);
    trail.prune(5 * kSec + 1, 4 * kSec);
    CHECK(trail.size() == 0);
}

TEST_CASE("prune never drops an open block") {
    NoteTrail trail;
    trail.apply(pressed(60), 0);
    trail.prune(1000 * kSec, 1 * kSec);
    REQUIRE(trail.size() == 1);
    CHECK(trail.blocks()[0].open);
}

TEST_CASE("prune keeps the order of the surviving blocks") {
    NoteTrail trail;
    for (int i = 0; i < 5; ++i) {
        trail.apply(pressed(60 + i), i * kSec);
        trail.apply(released(60 + i), i * kSec + 100 * kMs);
    }
    trail.prune(10 * kSec, 8 * kSec);
    REQUIRE(trail.size() == 3);
    CHECK(trail.blocks()[0].note == 62);
    CHECK(trail.blocks()[1].note == 63);
    CHECK(trail.blocks()[2].note == 64);
}

TEST_CASE("the block cap drops the closed block with the oldest end first") {
    NoteTrail trail(NoteTrail::kMinMaxBlocks);
    for (int i = 0; i < 200; ++i) {
        trail.apply(pressed(60), i * 10 * kMs);
        trail.apply(released(60), i * 10 * kMs + 5 * kMs);
    }
    CHECK(trail.size() == NoteTrail::kMinMaxBlocks);
    // The 72 oldest are gone, so the oldest left started at press number 72.
    CHECK(trail.blocks().front().start_ns == 72 * 10 * kMs);
    CHECK(trail.blocks().back().start_ns == 199 * 10 * kMs);
}

TEST_CASE("the block cap never drops an open block") {
    NoteTrail trail(NoteTrail::kMinMaxBlocks);
    trail.apply(pressed(10), 0); // Stuck key: never released.
    for (int i = 0; i < 500; ++i) {
        trail.apply(pressed(60), (i + 1) * 10 * kMs);
        trail.apply(released(60), (i + 1) * 10 * kMs + 5 * kMs);
    }
    CHECK(trail.size() == NoteTrail::kMinMaxBlocks);
    CHECK(trail.blocks().front().note == 10);
    CHECK(trail.blocks().front().open);
}

TEST_CASE("a cap below the minimum is raised to it") {
    NoteTrail trail(3);
    for (int note = 0; note < 128; ++note) {
        trail.apply(pressed(note), note * kMs);
    }
    CHECK(trail.size() == 128);
    for (const TrailBlock& b : trail.blocks()) {
        CHECK(b.open);
    }
}

TEST_CASE("clear removes every block") {
    NoteTrail trail;
    trail.apply(pressed(60), 0);
    trail.clear();
    CHECK(trail.size() == 0);
}

TEST_CASE("visible into a buffer matches the returning overload and replaces old contents") {
    NoteTrail trail;
    trail.apply(pressed(60), 0);
    trail.apply(released(60), 100 * kMs);
    trail.apply(pressed(64), 200 * kMs);
    std::vector<TrailBar> out{TrailBar{1, 1, false, 0.0, 0.5}};
    trail.visible(300 * kMs, kSec, out);
    const auto expected = trail.visible(300 * kMs, kSec);
    REQUIRE(out.size() == expected.size());
    REQUIRE(out.size() == 2);
    for (std::size_t i = 0; i < out.size(); ++i) {
        CHECK(out[i].note == expected[i].note);
        CHECK(out[i].held == expected[i].held);
        CHECK(out[i].bottom == doctest::Approx(expected[i].bottom));
        CHECK(out[i].top == doctest::Approx(expected[i].top));
    }
    trail.visible(300 * kMs, 0, out);
    CHECK(out.empty());
}

TEST_CASE("a visible bar carries its block's press time") {
    NoteTrail trail;
    trail.apply(pressed(60), 10 * kMs);
    trail.apply(released(60), 20 * kMs);
    trail.apply(pressed(60), 30 * kMs);
    const auto bars = trail.visible(40 * kMs, kSec);
    REQUIRE(bars.size() == 2);
    CHECK(bars[0].start_ns == 10 * kMs);
    CHECK(bars[1].start_ns == 30 * kMs);
}
