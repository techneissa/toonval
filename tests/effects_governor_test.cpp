// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/effects_governor.hpp"

#include <doctest/doctest.h>

using namespace toonval;

namespace {

constexpr std::int64_t kBudget = 16'666'667;
constexpr std::int64_t kSlow = 25'000'000;  // well over budget
constexpr std::int64_t kFast = 6'000'000;   // well inside the headroom
constexpr std::int64_t kNear = 14'000'000;  // under budget but not headroom

void feed(EffectsGovernor& g, std::int64_t ns, int frames) {
    for (int i = 0; i < frames; ++i) {
        g.add_frame(ns);
    }
}

// Frames of `ns` until the level changes, or -1 within `limit`.
int frames_until_change(EffectsGovernor& g, std::int64_t ns, int limit) {
    const EffectsLevel start = g.level();
    for (int i = 1; i <= limit; ++i) {
        if (g.add_frame(ns) != start) {
            return i;
        }
    }
    return -1;
}

} // namespace

TEST_CASE("levels drop effects in the fixed order: particles, motion, glow width, extras") {
    const EffectsBudget full = effects_budget(EffectsLevel::Full);
    CHECK(full.particles);
    CHECK(full.motion_background);
    CHECK(full.glow_width == 1.0f);
    CHECK(full.style_extras);

    const EffectsBudget l1 = effects_budget(EffectsLevel::NoParticles);
    CHECK_FALSE(l1.particles);
    CHECK(l1.motion_background);
    CHECK(l1.glow_width == 1.0f);
    CHECK(l1.style_extras);

    const EffectsBudget l2 = effects_budget(EffectsLevel::NoMotion);
    CHECK_FALSE(l2.particles);
    CHECK_FALSE(l2.motion_background);
    CHECK(l2.glow_width == 1.0f);
    CHECK(l2.style_extras);

    const EffectsBudget l3 = effects_budget(EffectsLevel::NarrowGlow);
    CHECK_FALSE(l3.motion_background);
    CHECK(l3.glow_width < 1.0f);
    CHECK(l3.glow_width > 0.0f);
    CHECK(l3.style_extras);

    const EffectsBudget l4 = effects_budget(EffectsLevel::NoStyleExtras);
    CHECK_FALSE(l4.particles);
    CHECK_FALSE(l4.motion_background);
    CHECK(l4.glow_width < 1.0f);
    CHECK_FALSE(l4.style_extras);
}

TEST_CASE("every level has a name") {
    for (EffectsLevel l : kAllEffectsLevels) {
        CHECK_FALSE(effects_level_name(l).empty());
    }
    CHECK(effects_level_name(EffectsLevel::Full) == "full");
}

TEST_CASE("frames within budget keep full effects") {
    EffectsGovernor g;
    feed(g, kNear, 2000);
    CHECK(g.level() == EffectsLevel::Full);
    CHECK(g.lowest() == EffectsLevel::Full);
}

TEST_CASE("sustained overrun steps down one level at a time, in order, to the floor") {
    EffectsGovernor g;
    const int down = g.config().down_frames;
    EffectsLevel expected = EffectsLevel::Full;
    for (int step = 1; step < static_cast<int>(kAllEffectsLevels.size()); ++step) {
        const int n = frames_until_change(g, kSlow, 10 * down);
        REQUIRE(n > 0);
        CHECK(n >= down);
        expected = static_cast<EffectsLevel>(step);
        CHECK(g.level() == expected);
    }
    // The floor still draws notes and keys; nothing goes below it.
    feed(g, kSlow * 4, 10'000);
    CHECK(g.level() == kLowestEffectsLevel);
    CHECK(g.lowest() == kLowestEffectsLevel);
}

TEST_CASE("one slow frame does not degrade") {
    EffectsGovernor g;
    feed(g, kNear, 100);
    g.add_frame(100'000'000); // a 100 ms hitch
    feed(g, kNear, 200);
    CHECK(g.level() == EffectsLevel::Full);
}

TEST_CASE("a short burst under the down threshold does not degrade") {
    EffectsGovernor g;
    feed(g, kNear, 100);
    feed(g, kSlow, g.config().down_frames / 2);
    feed(g, kFast, 200);
    CHECK(g.level() == EffectsLevel::Full);
}

TEST_CASE("headroom steps back up only after a long run, slower than stepping down") {
    EffectsGovernor g;
    feed(g, kSlow, 200);
    REQUIRE(g.level() != EffectsLevel::Full);
    const EffectsLevel degraded = g.level();
    // Frames just under budget are not headroom: the level holds (hysteresis band).
    feed(g, kNear, 3000);
    CHECK(g.level() == degraded);
    // Real headroom brings it back one step after up_frames.
    const int n = frames_until_change(g, kFast, 10 * g.config().up_frames);
    CHECK(n >= g.config().up_frames);
    CHECK(static_cast<int>(g.level()) == static_cast<int>(degraded) - 1);
    CHECK(g.config().up_frames > g.config().down_frames);
}

TEST_CASE("sustained headroom recovers full effects and lowest() remembers the worst") {
    EffectsGovernor g;
    feed(g, kSlow, 1000);
    REQUIRE(g.level() == kLowestEffectsLevel);
    feed(g, kFast, 100'000);
    CHECK(g.level() == EffectsLevel::Full);
    CHECK(g.lowest() == kLowestEffectsLevel);
    g.reset();
    CHECK(g.lowest() == EffectsLevel::Full);
}

TEST_CASE("flapping backs off: a step up that overruns again doubles the wait") {
    GovernorConfig c;
    c.down_frames = 10;
    c.up_frames = 100;
    EffectsGovernor g(c);
    feed(g, kSlow, 60);
    REQUIRE(g.level() != EffectsLevel::Full);
    CHECK(g.up_backoff() == 1);
    REQUIRE(frames_until_change(g, kFast, 1000) > 0); // steps up
    REQUIRE(frames_until_change(g, kSlow, 1000) > 0); // overruns right away: steps down
    CHECK(g.up_backoff() == 2);
    const int n = frames_until_change(g, kFast, 10'000);
    CHECK(n >= 2 * c.up_frames);
}

TEST_CASE("bad frame times are ignored") {
    EffectsGovernor g;
    feed(g, 0, 1000);
    feed(g, -5, 1000);
    CHECK(g.level() == EffectsLevel::Full);
    CHECK(g.average_ns() == 0.0);
}

TEST_CASE("budget follows the config") {
    GovernorConfig c;
    c.budget_ns = 8'333'333; // 120 Hz
    EffectsGovernor g(c);
    feed(g, 12'000'000, 200); // fine at 60 Hz, over at 120 Hz
    CHECK(g.level() != EffectsLevel::Full);
    CHECK(kBudget > c.budget_ns);
}

TEST_CASE("frame cost is the work, or the interval when a refresh was missed") {
    constexpr std::int64_t period = 16'666'667;
    CHECK(frame_cost_ns(4'000'000, period, period) == 4'000'000);
    CHECK(frame_cost_ns(4'000'000, period + 2'000'000, period) == 4'000'000); // jitter
    CHECK(frame_cost_ns(4'000'000, 2 * period, period) == 2 * period);       // missed one
    CHECK(frame_cost_ns(40'000'000, 2 * period, period) == 40'000'000);
    CHECK(frame_cost_ns(4'000'000, 3 * period, 0) == 4'000'000); // no known period
}
