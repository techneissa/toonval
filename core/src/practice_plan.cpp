// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/practice_plan.hpp"

#include <algorithm>
#include <limits>

namespace toonval {

bool PracticePlan::resolves(const SongSections& sections, std::size_t index) const {
    if (index >= items_.size()) {
        return false;
    }
    LoopSpan span = sections.span_of(items_[index].section);
    return span.ok && span.end_s - span.start_s >= kMinLoopSeconds;
}

PlanStep PracticePlan::start(const SongSections& sections, std::span<const PlanItem> items,
                             int base_speed_percent, std::size_t first_item) {
    reset();
    const std::size_t count = std::min(items.size(), kMaxPlanItems);
    for (std::size_t i = 0; i < count; ++i) {
        items_.push_back(sanitized(items[i]));
    }
    base_speed_ = snap_speed_percent(base_speed_percent);
    active_ = true;
    PlanStep step = advance(sections, first_item);
    if (step.kind == PlanStep::Kind::Finished) {
        // Nothing to play: no plan was started, so there is nothing to end either.
        reset();
        return PlanStep{};
    }
    return step;
}

PlanStep PracticePlan::enter(const SongSections& sections, std::size_t index) {
    const PlanItem& item = items_[index];
    LoopSpan span = sections.span_of(item.section);
    index_ = index;
    passes_ = 0;
    loop_start_s_ = span.start_s;
    loop_end_s_ = span.end_s;

    PlanStep step;
    step.kind = PlanStep::Kind::SetLoop;
    step.item_index = static_cast<int>(index);
    step.section = item.section;
    step.loop_start_s = span.start_s;
    step.loop_end_s = span.end_s;
    step.seek = true;
    step.new_item = true;
    step.set_speed_up = true;
    step.speed_up = item.speed_up;
    step.speed_percent = item.speed_up.enabled ? 0 : base_speed_;
    return step;
}

PlanStep PracticePlan::finish(PlanStep::Kind kind) {
    PlanStep step;
    step.kind = kind;
    step.item_index = static_cast<int>(index_);
    step.section = index_ < items_.size() ? items_[index_].section : kNoSection;
    step.set_speed_up = true; // a default policy is disabled
    step.speed_percent = base_speed_;
    active_ = false;
    passes_ = 0;
    return step;
}

PlanStep PracticePlan::advance(const SongSections& sections, std::size_t from) {
    for (std::size_t i = from; i < items_.size(); ++i) {
        if (resolves(sections, i)) {
            return enter(sections, i);
        }
    }
    if (!items_.empty()) {
        index_ = std::min(index_, items_.size() - 1);
    }
    return finish(PlanStep::Kind::Finished);
}

// The current item against the sections as they are now.
PlanStep PracticePlan::check(const SongSections& sections) {
    if (!resolves(sections, index_)) {
        return advance(sections, index_ + 1);
    }
    LoopSpan span = sections.span_of(items_[index_].section);
    if (span.start_s == loop_start_s_ && span.end_s == loop_end_s_) {
        return PlanStep{};
    }
    loop_start_s_ = span.start_s;
    loop_end_s_ = span.end_s;
    PlanStep step;
    step.kind = PlanStep::Kind::SetLoop;
    step.item_index = static_cast<int>(index_);
    step.section = items_[index_].section;
    step.loop_start_s = span.start_s;
    step.loop_end_s = span.end_s;
    step.seek = true;
    return step;
}

PlanStep PracticePlan::on_pass(const SongSections& sections, bool clean) {
    if (!active_) {
        return PlanStep{};
    }
    if (!resolves(sections, index_)) {
        return advance(sections, index_ + 1);
    }
    const PlanItem& item = items_[index_];
    if ((clean || !item.clean_passes_only) && passes_ < std::numeric_limits<int>::max()) {
        ++passes_;
    }
    if (item.repeats > 0 && passes_ >= item.repeats) {
        return advance(sections, index_ + 1);
    }
    return check(sections);
}

PlanStep PracticePlan::refresh(const SongSections& sections) {
    return active_ ? check(sections) : PlanStep{};
}

PlanStep PracticePlan::skip_next(const SongSections& sections) {
    return active_ ? advance(sections, index_ + 1) : PlanStep{};
}

PlanStep PracticePlan::skip_previous(const SongSections& sections) {
    if (!active_) {
        return PlanStep{};
    }
    for (std::size_t i = index_; i-- > 0;) {
        if (resolves(sections, i)) {
            return enter(sections, i);
        }
    }
    return advance(sections, index_); // the first item: start it again
}

PlanStep PracticePlan::jump_to_item(const SongSections& sections, std::size_t index) {
    if (!active_ || index >= items_.size()) {
        return PlanStep{};
    }
    return advance(sections, index);
}

PlanStep PracticePlan::jump_to_section(const SongSections& sections, SectionId section) {
    if (!active_) {
        return PlanStep{};
    }
    for (std::size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].section == section && resolves(sections, i)) {
            return enter(sections, i);
        }
    }
    return PlanStep{};
}

PlanStep PracticePlan::sections_merged(const SongSections& sections, SectionId survivor,
                                       std::span<const SectionId> absorbed) {
    if (!active_) {
        return PlanStep{};
    }
    for (PlanItem& item : items_) {
        if (std::find(absorbed.begin(), absorbed.end(), item.section) != absorbed.end()) {
            item.section = survivor;
        }
    }
    return check(sections);
}

PlanStep PracticePlan::stop() { return active_ ? finish(PlanStep::Kind::Stopped) : PlanStep{}; }

void PracticePlan::reset() {
    items_.clear();
    active_ = false;
    index_ = 0;
    passes_ = 0;
    loop_start_s_ = loop_end_s_ = 0.0;
}

void PracticePlan::set_base_speed_percent(int percent) {
    base_speed_ = snap_speed_percent(percent);
}

bool apply_plan_step(Transport& transport, std::int64_t now_ns, const PlanStep& step) {
    using Kind = PlanStep::Kind;
    if (step.kind == Kind::None) {
        return true;
    }
    if (step.set_speed_up) {
        transport.set_auto_speed_up(now_ns, step.speed_up);
    }
    if (step.speed_percent > 0) {
        transport.set_speed_percent(now_ns, step.speed_percent);
    }
    if (step.kind == Kind::SetLoop) {
        if (!transport.set_loop(now_ns, step.loop_start_s, step.loop_end_s)) {
            return false;
        }
        if (step.seek) {
            transport.seek(now_ns, step.loop_start_s);
        }
        return true;
    }
    transport.clear_loop(now_ns);
    if (step.kind == Kind::Finished) {
        transport.pause(now_ns);
    }
    return true;
}

} // namespace toonval
