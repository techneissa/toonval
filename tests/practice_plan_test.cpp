// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/practice_plan.hpp"

#include "section_fixtures.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

using namespace toonval;
using namespace toonval::testing;
using Kind = PlanStep::Kind;

namespace {

// 16 bars of 4/4 at 120 BPM: 32 s. Sections: A = bars 1-4 [0,8), B = bars 5-8 [8,16),
// C = bars 9-16 [16,32).
struct Fixture {
    Song song = bars_song(16);
    SongSections sections;
    SectionId a = 0;
    SectionId b = 0;
    SectionId c = 0;

    Fixture() {
        sections.song_hash = 1;
        REQUIRE(sections.add_marker(song, 8.0).ok);
        REQUIRE(sections.add_marker(song, 16.0).ok);
        a = sections.sections[0].id;
        b = sections.sections[1].id;
        c = sections.sections[2].id;
    }
};

PlanItem item(SectionId id, int repeats = 1) {
    PlanItem p;
    p.section = id;
    p.repeats = repeats;
    return p;
}

constexpr std::int64_t sec(double s) { return static_cast<std::int64_t>(s * 1e9); }

} // namespace

TEST_CASE("plan: starting enters the first section with a seek and its speed policy") {
    Fixture f;
    PlanItem first = item(f.a, 2);
    first.speed_up = AutoSpeedUp{true, 60, 5, 100};
    std::array<PlanItem, 2> items{first, item(f.b)};
    PracticePlan plan;
    PlanStep s = plan.start(f.sections, items, 90);
    CHECK(s.kind == Kind::SetLoop);
    CHECK(s.item_index == 0);
    CHECK(s.section == f.a);
    CHECK(s.loop_start_s == 0.0);
    CHECK(s.loop_end_s == 8.0);
    CHECK(s.seek);
    CHECK(s.new_item);
    CHECK(s.set_speed_up);
    CHECK(s.speed_up.enabled);
    CHECK(s.speed_up.start_percent == 60);
    CHECK(s.speed_percent == 0); // the policy sets its own start speed
    CHECK(plan.active());
    CHECK(plan.item_index() == 0);
}

TEST_CASE("plan: an item without a speed-up goes back to the starting speed") {
    Fixture f;
    std::array<PlanItem, 1> items{item(f.b)};
    PracticePlan plan;
    PlanStep s = plan.start(f.sections, items, 85);
    CHECK(s.set_speed_up);
    CHECK_FALSE(s.speed_up.enabled);
    CHECK(s.speed_percent == 85);
}

TEST_CASE("plan: sections play in order, each for its repeat count") {
    Fixture f;
    std::array<PlanItem, 3> items{item(f.c, 2), item(f.a, 1), item(f.b, 3)};
    PracticePlan plan;
    REQUIRE(plan.start(f.sections, items, 100).section == f.c);

    CHECK(plan.on_pass(f.sections, true).kind == Kind::None); // pass 1 of 2
    CHECK(plan.passes_done() == 1);
    PlanStep s = plan.on_pass(f.sections, true); // pass 2 of 2: move on
    CHECK(s.kind == Kind::SetLoop);
    CHECK(s.section == f.a);
    CHECK(s.item_index == 1);
    CHECK(s.loop_start_s == 0.0);
    CHECK(s.loop_end_s == 8.0);
    CHECK(s.seek);
    CHECK(s.new_item);
    CHECK(plan.passes_done() == 0);

    s = plan.on_pass(f.sections, true); // a: one pass
    CHECK(s.kind == Kind::SetLoop);
    CHECK(s.section == f.b);
    CHECK(plan.on_pass(f.sections, true).kind == Kind::None);
    CHECK(plan.on_pass(f.sections, true).kind == Kind::None);
    s = plan.on_pass(f.sections, true); // the third pass of the last item ends the plan
    CHECK(s.kind == Kind::Finished);
    CHECK_FALSE(plan.active());
    CHECK(plan.on_pass(f.sections, true).kind == Kind::None);
}

TEST_CASE("plan: the last section ending restores the speed and switches the speed-up off") {
    Fixture f;
    PlanItem p = item(f.a);
    p.speed_up = AutoSpeedUp{true, 60, 5, 100};
    std::array<PlanItem, 1> items{p};
    PracticePlan plan;
    plan.start(f.sections, items, 80);
    PlanStep s = plan.on_pass(f.sections, true);
    CHECK(s.kind == Kind::Finished);
    CHECK(s.set_speed_up);
    CHECK_FALSE(s.speed_up.enabled);
    CHECK(s.speed_percent == 80);
}

TEST_CASE("plan: a section on repeat stays until it is skipped") {
    Fixture f;
    std::array<PlanItem, 1> items{item(f.b, 0)};
    PracticePlan plan;
    plan.start(f.sections, items, 100);
    for (int i = 0; i < 500; ++i) {
        REQUIRE(plan.on_pass(f.sections, i % 2 == 0).kind == Kind::None);
    }
    CHECK(plan.active());
    CHECK(plan.skip_next(f.sections).kind == Kind::Finished);
}

TEST_CASE("plan: an item can count only clean passes") {
    Fixture f;
    PlanItem p = item(f.a, 2);
    p.clean_passes_only = true;
    std::array<PlanItem, 2> items{p, item(f.b)};
    PracticePlan plan;
    plan.start(f.sections, items, 100);
    CHECK(plan.on_pass(f.sections, false).kind == Kind::None);
    CHECK(plan.on_pass(f.sections, false).kind == Kind::None);
    CHECK(plan.passes_done() == 0);
    CHECK(plan.on_pass(f.sections, true).kind == Kind::None);
    PlanStep s = plan.on_pass(f.sections, true);
    CHECK(s.kind == Kind::SetLoop);
    CHECK(s.section == f.b);
}

TEST_CASE("plan: each item brings its own auto speed-up and drops the last one's") {
    Fixture f;
    PlanItem first = item(f.a);
    first.speed_up = AutoSpeedUp{true, 60, 5, 100};
    PlanItem second = item(f.b);
    PlanItem third = item(f.c);
    third.speed_up = AutoSpeedUp{true, 70, 10, 90};
    std::array<PlanItem, 3> items{first, second, third};
    PracticePlan plan;
    plan.start(f.sections, items, 100);

    PlanStep s = plan.on_pass(f.sections, true);
    CHECK(s.set_speed_up);
    CHECK_FALSE(s.speed_up.enabled);
    CHECK(s.speed_percent == 100);

    s = plan.on_pass(f.sections, true);
    CHECK(s.speed_up.enabled);
    CHECK(s.speed_up.start_percent == 70);
    CHECK(s.speed_up.step_percent == 10);
    CHECK(s.speed_up.max_percent == 90);
    CHECK(s.speed_percent == 0);
}

TEST_CASE("plan: starting speed is the one the user last set") {
    Fixture f;
    std::array<PlanItem, 2> items{item(f.a), item(f.b)};
    PracticePlan plan;
    plan.start(f.sections, items, 100);
    plan.set_base_speed_percent(73); // snaps to the grid
    PlanStep s = plan.on_pass(f.sections, true);
    CHECK(s.speed_percent == 75);
}

TEST_CASE("plan: skipping moves to the next item, and past the last one ends the plan") {
    Fixture f;
    std::array<PlanItem, 3> items{item(f.a, 0), item(f.b, 0), item(f.c, 0)};
    PracticePlan plan;
    plan.start(f.sections, items, 100);
    PlanStep s = plan.skip_next(f.sections);
    CHECK(s.section == f.b);
    CHECK(s.new_item);
    s = plan.skip_next(f.sections);
    CHECK(s.section == f.c);
    s = plan.skip_next(f.sections);
    CHECK(s.kind == Kind::Finished);
    CHECK_FALSE(plan.active());
}

TEST_CASE("plan: skipping back goes to the previous item, or restarts the first") {
    Fixture f;
    std::array<PlanItem, 3> items{item(f.a, 0), item(f.b, 0), item(f.c, 0)};
    PracticePlan plan;
    plan.start(f.sections, items, 100);
    plan.skip_next(f.sections);
    plan.skip_next(f.sections);
    PlanStep s = plan.skip_previous(f.sections);
    CHECK(s.section == f.b);
    s = plan.skip_previous(f.sections);
    CHECK(s.section == f.a);
    s = plan.skip_previous(f.sections); // first item: start it again
    CHECK(s.kind == Kind::SetLoop);
    CHECK(s.section == f.a);
    CHECK(s.seek);
    CHECK(plan.item_index() == 0);
}

TEST_CASE("plan: skipping resets the pass count of the item") {
    Fixture f;
    std::array<PlanItem, 2> items{item(f.a, 5), item(f.b, 5)};
    PracticePlan plan;
    plan.start(f.sections, items, 100);
    plan.on_pass(f.sections, true);
    plan.on_pass(f.sections, true);
    CHECK(plan.passes_done() == 2);
    plan.skip_next(f.sections);
    CHECK(plan.passes_done() == 0);
    plan.skip_previous(f.sections);
    CHECK(plan.passes_done() == 0);
}

TEST_CASE("plan: jumping goes to an item or to a section") {
    Fixture f;
    std::array<PlanItem, 3> items{item(f.a), item(f.b), item(f.c)};
    PracticePlan plan;
    plan.start(f.sections, items, 100);
    PlanStep s = plan.jump_to_item(f.sections, 2);
    CHECK(s.section == f.c);
    CHECK(s.item_index == 2);
    CHECK(plan.jump_to_item(f.sections, 3).kind == Kind::None);
    CHECK(plan.item_index() == 2);
    s = plan.jump_to_section(f.sections, f.b);
    CHECK(s.section == f.b);
    CHECK(s.item_index == 1);
    CHECK(plan.jump_to_section(f.sections, 99).kind == Kind::None);
    CHECK(plan.item_index() == 1);
}

TEST_CASE("plan: jumping to a section picks its first item") {
    Fixture f;
    std::array<PlanItem, 3> items{item(f.a), item(f.b), item(f.a)};
    PracticePlan plan;
    plan.start(f.sections, items, 100);
    plan.jump_to_item(f.sections, 2);
    CHECK(plan.jump_to_section(f.sections, f.a).item_index == 0);
}

TEST_CASE("plan: starting at a later item") {
    Fixture f;
    std::array<PlanItem, 3> items{item(f.a), item(f.b), item(f.c)};
    PracticePlan plan;
    PlanStep s = plan.start(f.sections, items, 100, 1);
    CHECK(s.section == f.b);
    CHECK(plan.item_index() == 1);
    PracticePlan beyond;
    CHECK(beyond.start(f.sections, items, 100, 7).kind == Kind::None); // nothing at or after it
    CHECK_FALSE(beyond.active());
}

TEST_CASE("plan: stopping restores the speed and keeps playing") {
    Fixture f;
    PlanItem p = item(f.a, 0);
    p.speed_up = AutoSpeedUp{true, 60, 5, 100};
    std::array<PlanItem, 1> items{p};
    PracticePlan plan;
    plan.start(f.sections, items, 90);
    PlanStep s = plan.stop();
    CHECK(s.kind == Kind::Stopped);
    CHECK(s.set_speed_up);
    CHECK_FALSE(s.speed_up.enabled);
    CHECK(s.speed_percent == 90);
    CHECK_FALSE(plan.active());
    CHECK(plan.stop().kind == Kind::None);
}

TEST_CASE("plan: reset forgets the plan with no step") {
    Fixture f;
    std::array<PlanItem, 1> items{item(f.a)};
    PracticePlan plan;
    plan.start(f.sections, items, 100);
    plan.reset();
    CHECK_FALSE(plan.active());
    CHECK(plan.on_pass(f.sections, true).kind == Kind::None);
    CHECK(plan.items().empty());
}

TEST_CASE("plan: items naming sections that do not exist are skipped from the start") {
    Fixture f;
    std::array<PlanItem, 3> items{item(99), item(f.b), item(77)};
    PracticePlan plan;
    PlanStep s = plan.start(f.sections, items, 100);
    CHECK(s.section == f.b);
    CHECK(s.item_index == 1);
    s = plan.on_pass(f.sections, true);
    CHECK(s.kind == Kind::Finished); // item 2 names nothing
    PracticePlan none;
    std::array<PlanItem, 1> ghost{item(99)};
    CHECK(none.start(f.sections, ghost, 100).kind == Kind::None);
    CHECK_FALSE(none.active());
}

TEST_CASE("plan: items are sanitized and capped") {
    Fixture f;
    std::vector<PlanItem> items(kMaxPlanItems + 10, item(f.a, 500));
    PracticePlan plan;
    plan.start(f.sections, items, 100);
    CHECK(plan.items().size() == kMaxPlanItems);
    CHECK(plan.items()[0].repeats == kMaxPlanRepeats);
}

// ---- Sections edited while the plan runs

TEST_CASE("plan edits: renaming a section changes nothing") {
    Fixture f;
    std::array<PlanItem, 1> items{item(f.b, 0)};
    PracticePlan plan;
    plan.start(f.sections, items, 100);
    REQUIRE(f.sections.rename(f.b, "Bridge").ok);
    CHECK(plan.refresh(f.sections).kind == Kind::None);
    CHECK(plan.on_pass(f.sections, true).kind == Kind::None);
}

TEST_CASE("plan edits: splitting the current section sets the shorter loop at once") {
    Fixture f;
    std::array<PlanItem, 2> items{item(f.c, 3), item(f.a)};
    PracticePlan plan;
    plan.start(f.sections, items, 100);
    plan.on_pass(f.sections, true);
    REQUIRE(f.sections.split(f.song, f.c, 24.0).ok);
    PlanStep s = plan.refresh(f.sections);
    CHECK(s.kind == Kind::SetLoop);
    CHECK(s.section == f.c);
    CHECK(s.loop_start_s == 16.0);
    CHECK(s.loop_end_s == 24.0);
    CHECK(s.seek);
    CHECK_FALSE(s.new_item);
    CHECK_FALSE(s.set_speed_up);
    CHECK(plan.passes_done() == 1);                     // the count stays
    CHECK(plan.refresh(f.sections).kind == Kind::None); // told once
}

TEST_CASE("plan edits: a change is reported at the next pass if nobody called refresh") {
    Fixture f;
    std::array<PlanItem, 1> items{item(f.c, 0)};
    PracticePlan plan;
    plan.start(f.sections, items, 100);
    REQUIRE(f.sections.split(f.song, f.c, 24.0).ok);
    PlanStep s = plan.on_pass(f.sections, true);
    CHECK(s.kind == Kind::SetLoop);
    CHECK(s.loop_end_s == 24.0);
    CHECK_FALSE(s.new_item);
}

TEST_CASE("plan edits: deleting the current section moves to the next item") {
    Fixture f;
    std::array<PlanItem, 3> items{item(f.a, 0), item(f.b, 0), item(f.c, 0)};
    PracticePlan plan;
    plan.start(f.sections, items, 100);
    plan.skip_next(f.sections); // now on b
    REQUIRE(f.sections.erase(f.song, f.b).ok);
    PlanStep s = plan.refresh(f.sections);
    CHECK(s.kind == Kind::SetLoop);
    CHECK(s.section == f.c);
    CHECK(s.new_item);
    CHECK(s.item_index == 2);
}

TEST_CASE("plan edits: deleting a later section skips it on arrival") {
    Fixture f;
    std::array<PlanItem, 3> items{item(f.a), item(f.b), item(f.c)};
    PracticePlan plan;
    plan.start(f.sections, items, 100);
    REQUIRE(f.sections.erase(f.song, f.b).ok); // a takes over b's time
    PlanStep s = plan.on_pass(f.sections, true);
    CHECK(s.section == f.c);
    CHECK(s.item_index == 2);
}

TEST_CASE("plan edits: deleting every remaining section ends the plan cleanly") {
    Fixture f;
    std::array<PlanItem, 2> items{item(f.a, 0), item(f.b, 0)};
    PracticePlan plan;
    plan.start(f.sections, items, 95);
    REQUIRE(f.sections.erase(f.song, f.b).ok);
    REQUIRE(f.sections.erase(f.song, f.a).ok);
    PlanStep s = plan.refresh(f.sections);
    CHECK(s.kind == Kind::Finished);
    CHECK(s.speed_percent == 95);
    CHECK_FALSE(plan.active());
}

TEST_CASE("plan edits: merging sections keeps the survivor's plan item and drops the rest") {
    Fixture f;
    std::array<PlanItem, 3> items{item(f.a, 0), item(f.b, 0), item(f.c, 0)};
    PracticePlan plan;
    plan.start(f.sections, items, 100);
    plan.skip_next(f.sections); // on b
    std::array<SectionId, 2> pick{f.a, f.b};
    REQUIRE(f.sections.merge(f.song, pick).ok); // a survives, b is gone
    PlanStep s = plan.refresh(f.sections);
    CHECK(s.kind == Kind::SetLoop);
    CHECK(s.section == f.c);
    CHECK(s.new_item);
    plan.jump_to_item(f.sections, 0);
    CHECK(plan.item_index() == 0);
    CHECK(plan.jump_to_section(f.sections, f.a).loop_end_s == 16.0);
}

TEST_CASE("plan edits: moving a marker changes the loop of both neighbours") {
    Fixture f;
    std::array<PlanItem, 2> items{item(f.a, 0), item(f.b, 0)};
    PracticePlan plan;
    plan.start(f.sections, items, 100);
    // Remove the marker at 8 and put it at 12: a grows, b shrinks.
    REQUIRE(f.sections.remove_marker(f.song, 8.0).ok);
    REQUIRE(f.sections.add_marker(f.song, 12.0).ok);
    PlanStep s = plan.refresh(f.sections);
    CHECK(s.kind == Kind::SetLoop);
    CHECK(s.section == f.a);
    CHECK(s.loop_end_s == 12.0);
}

TEST_CASE("plan edits: sections that vanish and return under the same id are not confused") {
    Fixture f;
    std::array<PlanItem, 2> items{item(f.a, 0), item(f.b, 0)};
    PracticePlan plan;
    plan.start(f.sections, items, 100);
    SectionId old_b = f.b;
    REQUIRE(f.sections.erase(f.song, f.b).ok);
    REQUIRE(f.sections.add_marker(f.song, 8.0).ok); // a new section is made where b was
    CHECK(f.sections.find(old_b) == nullptr);       // its id is not b's
    PlanStep s = plan.skip_next(f.sections);
    CHECK(s.kind == Kind::Finished); // b is gone for good
}

TEST_CASE("plan edits: a song switch with a fresh SongSections ends the plan on the next call") {
    Fixture f;
    std::array<PlanItem, 1> items{item(f.a, 0)};
    PracticePlan plan;
    plan.start(f.sections, items, 100);
    SongSections other;
    PlanStep s = plan.on_pass(other, true);
    CHECK(s.kind == Kind::Finished);
    CHECK_FALSE(plan.active());
}

// ---- Driving a real Transport

namespace {

struct Drive {
    Fixture f;
    Transport transport;
    PracticePlan plan;
    std::int64_t now = 0;
    std::vector<TransportOp> seen;

    Drive() { transport.set_song(f.song); }

    void apply(const PlanStep& s) { CHECK(apply_plan_step(transport, now, s)); }

    // Runs the clock for `seconds` of wall time in small frames, feeding every wrap to the plan as
    // a clean pass.
    void run(double seconds) {
        const std::int64_t end = now + sec(seconds);
        while (now < end) {
            now += sec(0.01);
            transport.update(now);
            std::vector<TransportOp> ops;
            transport.take_ops(ops);
            for (const TransportOp& op : ops) {
                seen.push_back(op);
                if (op.kind == TransportOp::Kind::Wrap) {
                    transport.report_pass(now, true);
                    apply(plan.on_pass(f.sections, true));
                }
            }
        }
    }
};

} // namespace

TEST_CASE("plan transport: a plan loops one section, then the next, then pauses") {
    Drive d;
    std::array<PlanItem, 2> items{item(d.f.a, 2), item(d.f.b, 1)};
    d.apply(d.plan.start(d.f.sections, items, d.transport.speed_percent()));
    CHECK(d.transport.loop_enabled());
    CHECK(d.transport.loop_start_s() == 0.0);
    CHECK(d.transport.loop_end_s() == 8.0);
    d.transport.play(d.now);

    d.run(8.5); // the first pass of a ends at 8 s
    CHECK(d.plan.item_index() == 0);
    CHECK(d.transport.loop_end_s() == 8.0);
    d.run(8.0); // the second pass ends: the plan moves to b
    CHECK(d.plan.item_index() == 1);
    CHECK(d.transport.loop_start_s() == 8.0);
    CHECK(d.transport.loop_end_s() == 16.0);
    CHECK(d.transport.position_s() >= 8.0);
    CHECK(d.transport.position_s() < 16.0);
    d.run(8.0); // b's only pass ends: the plan is over
    CHECK_FALSE(d.plan.active());
    CHECK_FALSE(d.transport.loop_enabled());
    CHECK_FALSE(d.transport.playing());
    CHECK(d.transport.speed_percent() == 100);
}

TEST_CASE("plan transport: a section's auto speed-up ramps and the next one starts from the base") {
    Drive d;
    PlanItem first = item(d.f.a, 3);
    first.speed_up = AutoSpeedUp{true, 60, 10, 100};
    std::array<PlanItem, 2> items{first, item(d.f.b, 0)};
    d.apply(d.plan.start(d.f.sections, items, 100));
    CHECK(d.transport.speed_percent() == 60);
    d.transport.play(d.now);
    d.run(8.0 / 0.6 + 0.2); // one pass at 60%
    CHECK(d.transport.speed_percent() == 70);
    d.run(8.0 / 0.7 + 0.2); // a second at 70%
    CHECK(d.transport.speed_percent() == 80);
    d.run(8.0 / 0.8 + 0.2); // the third: the plan moves to b, back at the base speed
    CHECK(d.plan.item_index() == 1);
    CHECK(d.transport.speed_percent() == 100);
    CHECK_FALSE(d.transport.auto_speed_up().enabled);
}

TEST_CASE("plan transport: editing the section while looping updates the loop") {
    Drive d;
    std::array<PlanItem, 1> items{item(d.f.c, 0)};
    d.apply(d.plan.start(d.f.sections, items, 100));
    d.transport.play(d.now);
    d.run(1.0);
    REQUIRE(d.f.sections.split(d.f.song, d.f.c, 24.0).ok);
    d.apply(d.plan.refresh(d.f.sections));
    CHECK(d.transport.loop_start_s() == 16.0);
    CHECK(d.transport.loop_end_s() == 24.0);
    CHECK(d.transport.position_s() == 16.0);
}

// ---- Random edits and plan calls

TEST_CASE("plan property: any mix of edits and plan calls leaves a consistent plan") {
    std::mt19937_64 rng(99);
    for (int iteration = 0; iteration < 150; ++iteration) {
        Song song = bars_song(8 + static_cast<int>(rng() % 40));
        SongSections sections;
        sections.song_hash = 1;
        for (int i = 0; i < 6; ++i) {
            sections.add_marker(song, song.duration_s * static_cast<double>(rng() % 1001) / 1000.0);
        }
        std::vector<PlanItem> items;
        for (std::size_t i = 0; i < 1 + rng() % 6 && !sections.sections.empty(); ++i) {
            PlanItem p;
            p.section = sections.sections[rng() % sections.sections.size()].id;
            p.repeats = static_cast<int>(rng() % 4);
            p.clean_passes_only = rng() % 2 == 0;
            items.push_back(p);
        }
        PracticePlan plan;
        plan.start(sections, items, 100);
        for (int step_no = 0; step_no < 60; ++step_no) {
            CAPTURE(iteration);
            CAPTURE(step_no);
            PlanStep s;
            switch (rng() % 9) {
            case 0:
                sections.add_marker(song,
                                    song.duration_s * static_cast<double>(rng() % 1001) / 1000.0);
                s = plan.refresh(sections);
                break;
            case 1:
                if (!sections.sections.empty()) {
                    sections.erase(song, sections.sections[rng() % sections.sections.size()].id);
                }
                s = plan.refresh(sections);
                break;
            case 2:
                sections.remove_marker(song, song.duration_s * static_cast<double>(rng() % 1001) /
                                                 1000.0);
                s = plan.refresh(sections);
                break;
            case 3:
                s = plan.skip_next(sections);
                break;
            case 4:
                s = plan.skip_previous(sections);
                break;
            case 5:
                s = plan.jump_to_item(sections, rng() % 8);
                break;
            case 6:
                s = plan.stop();
                break;
            default:
                s = plan.on_pass(sections, rng() % 2 == 0);
                break;
            }
            if (s.kind == PlanStep::Kind::SetLoop) {
                LoopSpan span = sections.span_of(s.section);
                REQUIRE(span.ok);
                CHECK(s.loop_start_s == span.start_s);
                CHECK(s.loop_end_s == span.end_s);
                CHECK(s.loop_end_s - s.loop_start_s >= kMinLoopSeconds);
                CHECK(plan.active());
                CHECK(plan.items()[static_cast<std::size_t>(s.item_index)].section == s.section);
            }
            if (s.kind == PlanStep::Kind::Finished || s.kind == PlanStep::Kind::Stopped) {
                CHECK_FALSE(plan.active());
                CHECK(s.speed_percent == 100);
                CHECK_FALSE(s.speed_up.enabled);
            }
            if (plan.active()) {
                CHECK(plan.item_index() < plan.items().size());
            }
            // Whenever the plan is active right after a step that looked at the sections, its
            // current section exists.
            if (plan.active() && s.kind != PlanStep::Kind::None) {
                CHECK(sections.find(plan.items()[plan.item_index()].section) != nullptr);
            }
        }
    }
}

// ---- Audit follow-ups (issue #32)

TEST_CASE("plan: a merge during a run sends items of the absorbed section to the survivor") {
    Fixture f;
    std::array<PlanItem, 3> items{item(f.a, 1), item(f.b, 2), item(f.c, 1)};
    PracticePlan plan;
    plan.start(f.sections, items, 100);
    plan.on_pass(f.sections, true); // a is done, now on b
    REQUIRE(plan.item_index() == 1);

    std::array<SectionId, 2> pick{f.a, f.b};
    std::vector<SectionId> absorbed;
    REQUIRE(f.sections.merge(f.song, pick, &absorbed).ok);
    PlanStep step = plan.sections_merged(f.sections, f.a, absorbed);
    // The current item (b) now plays the merged section a+b: a new loop at once.
    CHECK(step.kind == Kind::SetLoop);
    CHECK_FALSE(step.new_item);
    CHECK(step.section == f.a);
    CHECK(step.loop_start_s == 0.0);
    CHECK(step.loop_end_s == 16.0);
    CHECK(step.seek);
    CHECK(plan.active());
    CHECK(plan.item_index() == 1);
    CHECK(plan.items()[1].section == f.a);
    // The passes it had stay (none yet), the plan carries on to c after its two repeats.
    CHECK(plan.on_pass(f.sections, true).kind == Kind::None);
    PlanStep next = plan.on_pass(f.sections, true);
    CHECK(next.kind == Kind::SetLoop);
    CHECK(next.section == f.c);
}

TEST_CASE("plan: a merge of a later section does not skip its item") {
    Fixture f;
    std::array<PlanItem, 2> items{item(f.a, 1), item(f.c, 1)};
    PracticePlan plan;
    plan.start(f.sections, items, 100);
    // b and c merge: the plan's item on c now points at b's survivor, which is b.
    std::array<SectionId, 2> pick{f.b, f.c};
    std::vector<SectionId> absorbed;
    REQUIRE(f.sections.merge(f.song, pick, &absorbed).ok);
    CHECK(plan.sections_merged(f.sections, f.b, absorbed).kind == Kind::None);
    PlanStep step = plan.on_pass(f.sections, true);
    CHECK(step.kind == Kind::SetLoop); // not Finished
    CHECK(step.section == f.b);
    CHECK(step.loop_start_s == 8.0);
    CHECK(step.loop_end_s == 32.0);
}

TEST_CASE("plan: sections_merged does nothing when no plan runs") {
    Fixture f;
    PracticePlan plan;
    std::array<SectionId, 1> gone{f.b};
    CHECK(plan.sections_merged(f.sections, f.a, gone).kind == Kind::None);
    CHECK_FALSE(plan.active());
}

TEST_CASE("plan: set_base_speed_percent snaps to the grid and clamps") {
    Fixture f;
    PracticePlan plan;
    plan.set_base_speed_percent(73);
    CHECK(plan.base_speed_percent() == 75);
    plan.set_base_speed_percent(1);
    CHECK(plan.base_speed_percent() == 25);
    plan.set_base_speed_percent(100000);
    CHECK(plan.base_speed_percent() == 150);
    plan.set_base_speed_percent(std::numeric_limits<int>::max());
    CHECK(plan.base_speed_percent() == 150);
    plan.set_base_speed_percent(std::numeric_limits<int>::min());
    CHECK(plan.base_speed_percent() == 25);
}

TEST_CASE("plan: an item with a speed-up near INT_MAX starts on the grid") {
    Fixture f;
    PlanItem p = item(f.a);
    p.speed_up = AutoSpeedUp{true, std::numeric_limits<int>::max(), std::numeric_limits<int>::max(),
                             std::numeric_limits<int>::max()};
    std::array<PlanItem, 1> items{p};
    PracticePlan plan;
    plan.start(f.sections, items, std::numeric_limits<int>::max());
    REQUIRE(plan.active());
    CHECK(plan.base_speed_percent() == 150);
    CHECK(plan.items()[0].speed_up.step_percent == 25);
    CHECK(plan.items()[0].speed_up.start_percent == 150);
}

TEST_CASE("apply_plan_step: each kind does the Transport calls it asks for") {
    Fixture f;
    Transport t;
    t.set_song(f.song);
    std::int64_t now = 0;

    CHECK(apply_plan_step(t, now, PlanStep{})); // None: nothing changes
    CHECK_FALSE(t.loop_enabled());

    PlanStep set;
    set.kind = Kind::SetLoop;
    set.loop_start_s = 8.0;
    set.loop_end_s = 16.0;
    set.seek = true;
    set.set_speed_up = true;
    set.speed_up = AutoSpeedUp{true, 50, 5, 100};
    CHECK(apply_plan_step(t, now, set));
    CHECK(t.loop_enabled());
    CHECK(t.loop_start_s() == 8.0);
    CHECK(t.loop_end_s() == 16.0);
    CHECK(t.speed_percent() == 50); // an enabled policy sets its own start speed

    PlanStep plain;
    plain.kind = Kind::SetLoop;
    plain.loop_start_s = 16.0;
    plain.loop_end_s = 32.0;
    plain.set_speed_up = true; // disabled policy
    plain.speed_percent = 80;
    CHECK(apply_plan_step(t, now, plain));
    CHECK(t.loop_start_s() == 16.0);
    CHECK(t.speed_percent() == 80);

    PlanStep too_short = set;
    too_short.loop_start_s = 8.0;
    too_short.loop_end_s = 8.0;
    CHECK_FALSE(apply_plan_step(t, now, too_short)); // Transport refuses the loop

    PlanStep stopped;
    stopped.kind = Kind::Stopped;
    stopped.set_speed_up = true;
    stopped.speed_percent = 100;
    CHECK(apply_plan_step(t, now, stopped));
    CHECK_FALSE(t.loop_enabled());
    CHECK(t.speed_percent() == 100);

    CHECK(apply_plan_step(t, now, set));
    PlanStep finished;
    finished.kind = Kind::Finished;
    finished.set_speed_up = true;
    finished.speed_percent = 100;
    CHECK(apply_plan_step(t, now, finished));
    CHECK_FALSE(t.loop_enabled());
    CHECK_FALSE(t.playing());
}
