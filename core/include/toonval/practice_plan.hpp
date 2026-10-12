// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "toonval/sections.hpp"
#include "toonval/transport.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace toonval {

// A practice plan plays chosen sections of a song in order, each looped a number of times and each
// with its own auto speed-up, or one section on repeat. The plan owns no clock: it drives the
// Transport through set_loop and seek by telling the caller what to do after each pass.
//
// How the app drives it (and apply_plan_step does the Transport calls):
//
//   step = plan.start(sections, items, transport.speed_percent());   apply_plan_step(...)
//   for each loop pass (a TransportOp::Wrap, with the evaluator's PassResult::clean):
//       transport.report_pass(now, clean);       // the speed-up the transport keeps
//       step = plan.on_pass(sections, clean);    // move on, or stay
//       apply_plan_step(transport, now, step);
//   after the user edits the sections while the plan runs: plan.refresh(sections)
//
// The plan keeps its own copy of the items, so editing the saved playlist (SongSections::plan)
// while a plan runs changes nothing until the next start. It reads the section times from the
// SongSections it is given on every call, so edits made mid-plan take effect at once and the
// plan stays valid:
//
// - a section that no longer exists is skipped (when it is the current one, the plan moves on;
//   when the plan has nothing left, it ends);
// - a section whose time changed (split, merged, a neighbour deleted) gets the new loop at once,
//   with a seek to its start, and keeps its pass count;
// - after a merge, sections_merged() points items of the absorbed sections at the survivor;
// - renaming changes nothing.
//
// Ids are stable (see sections.hpp), so a plan item never points at a different section.

struct PlanStep {
    enum class Kind : std::uint8_t {
        None,     // keep looping as before
        SetLoop,  // set this loop (and seek to its start when `seek`), and the speed fields
        Finished, // the last section is done: clear the loop, restore the speed, pause
        Stopped,  // the user stopped the plan: clear the loop, restore the speed, keep playing
    };
    Kind kind = Kind::None;
    int item_index = -1; // the plan item now playing (SetLoop), the last one (Finished)
    SectionId section = kNoSection;
    double loop_start_s = 0.0; // SetLoop
    double loop_end_s = 0.0;
    bool seek = false;     // SetLoop: jump to loop_start_s
    bool new_item = false; // SetLoop: the plan moved to another item (else the same
                           // section's time changed)
    // SetLoop on a new item, Finished and Stopped: call Transport::set_auto_speed_up(speed_up)
    // (a disabled policy switches the speed-up off), then set the speed to `speed_percent` when it
    // is not 0 (the speed the plan started with, for an item without a speed-up). An enabled
    // policy sets its own start speed.
    bool set_speed_up = false;
    AutoSpeedUp speed_up;
    int speed_percent = 0;
};

class PracticePlan {
  public:
    // Starts at `first_item` (or the first item after it whose section exists). `items` are copied,
    // sanitized and capped at kMaxPlanItems. `base_speed_percent` is the speed to go back to when
    // an item has no speed-up and when the plan ends. Returns None, and the plan stays inactive,
    // when no item names an existing section.
    PlanStep start(const SongSections& sections, std::span<const PlanItem> items,
                   int base_speed_percent, std::size_t first_item = 0);

    // One loop pass finished; `clean` is the evaluator's verdict. Counts it toward the item's
    // repeats and moves on when they are done: SetLoop for the next section, Finished after the
    // last one. Otherwise None, or SetLoop when the current section's time changed.
    PlanStep on_pass(const SongSections& sections, bool clean);

    // Checks the current item against edited sections, without a pass.
    PlanStep refresh(const SongSections& sections);

    // Navigation. Each skips items whose section is gone. All return None when the plan is not
    // active (and jump_to_item / jump_to_section when the target does not exist).
    PlanStep skip_next(const SongSections& sections);
    // To the previous item; on the first one, back to the start of it.
    PlanStep skip_previous(const SongSections& sections);
    PlanStep jump_to_item(const SongSections& sections, std::size_t index);
    // The first item that plays this section.
    PlanStep jump_to_section(const SongSections& sections, SectionId section);

    // The saved sections were merged (SongSections::merge or remove_marker, which report the
    // absorbed ids): running items on an absorbed section now play the survivor, as the saved
    // playlist does, and the current item gets the survivor's new loop. The running items are
    // remapped, not collapsed, so item indices and the pass count stay. None when not active.
    PlanStep sections_merged(const SongSections& sections, SectionId survivor,
                             std::span<const SectionId> absorbed);

    // The user ends the plan: Stopped, with the speed restored. None when not active.
    PlanStep stop();
    // Forgets the plan without a step (the song changed).
    void reset();
    // The speed to restore: call it when the user changes the speed by hand.
    void set_base_speed_percent(int percent);

    bool active() const { return active_; }
    std::size_t item_index() const { return index_; }
    int passes_done() const { return passes_; } // counted passes of the current item
    int base_speed_percent() const { return base_speed_; }
    const std::vector<PlanItem>& items() const { return items_; }

  private:
    bool resolves(const SongSections& sections, std::size_t index) const;
    // Plays the first item at or after `from` that exists, or finishes.
    PlanStep advance(const SongSections& sections, std::size_t from);
    PlanStep enter(const SongSections& sections, std::size_t index);
    PlanStep finish(PlanStep::Kind kind);
    PlanStep check(const SongSections& sections);

    std::vector<PlanItem> items_;
    bool active_ = false;
    std::size_t index_ = 0;
    int passes_ = 0;
    int base_speed_ = 100;
    double loop_start_s_ = 0.0; // the loop in force for the current item
    double loop_end_s_ = 0.0;
};

// The Transport calls a step asks for, in order: set_auto_speed_up and the speed, then set_loop and
// seek for SetLoop, or clear_loop (and pause, for Finished) for the endings. Returns false when
// Transport::set_loop refused the loop (it never does for a section of a valid SongSections).
bool apply_plan_step(Transport& transport, std::int64_t now_ns, const PlanStep& step);

} // namespace toonval
