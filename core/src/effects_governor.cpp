// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/effects_governor.hpp"

#include <algorithm>

namespace toonval {

EffectsBudget effects_budget(EffectsLevel level) {
    const int n = static_cast<int>(level);
    EffectsBudget b;
    b.particles = n < static_cast<int>(EffectsLevel::NoParticles);
    b.motion_background = n < static_cast<int>(EffectsLevel::NoMotion);
    b.glow_width = n < static_cast<int>(EffectsLevel::NarrowGlow) ? 1.0f : 0.5f;
    b.style_extras = n < static_cast<int>(EffectsLevel::NoStyleExtras);
    return b;
}

std::string_view effects_level_name(EffectsLevel level) {
    switch (level) {
    case EffectsLevel::Full:
        return "full";
    case EffectsLevel::NoParticles:
        return "no particles";
    case EffectsLevel::NoMotion:
        return "no motion";
    case EffectsLevel::NarrowGlow:
        return "narrow glow";
    case EffectsLevel::NoStyleExtras:
        return "no style extras";
    }
    return "full";
}

std::int64_t frame_cost_ns(std::int64_t work_ns, std::int64_t interval_ns, std::int64_t period_ns) {
    if (period_ns > 0 && interval_ns > period_ns + period_ns / 2) {
        return std::max(interval_ns, work_ns);
    }
    return work_ns;
}

EffectsGovernor::EffectsGovernor(GovernorConfig config) : config_(config) {
    config_.down_frames = std::max(1, config_.down_frames);
    config_.up_frames = std::max(1, config_.up_frames);
    config_.max_up_backoff = std::max(1, config_.max_up_backoff);
    config_.smoothing = std::clamp(config_.smoothing, 0.001, 1.0);
}

void EffectsGovernor::reset() {
    const GovernorConfig config = config_;
    *this = EffectsGovernor(config);
}

EffectsLevel EffectsGovernor::add_frame(std::int64_t frame_ns) {
    if (frame_ns <= 0) {
        return level_;
    }
    const auto t = static_cast<double>(frame_ns);
    average_ns_ = has_average_ ? average_ns_ + config_.smoothing * (t - average_ns_) : t;
    has_average_ = true;
    ++frames_since_up_;

    const auto budget = static_cast<double>(config_.budget_ns);
    if (average_ns_ > budget * config_.over_ratio) {
        ++over_run_;
        headroom_run_ = 0;
    } else if (average_ns_ < budget * config_.headroom_ratio) {
        ++headroom_run_;
        over_run_ = 0;
    } else {
        over_run_ = 0;
        headroom_run_ = 0;
    }

    if (over_run_ >= config_.down_frames && level_ != kLowestEffectsLevel) {
        level_ = static_cast<EffectsLevel>(static_cast<int>(level_) + 1);
        lowest_ = std::max(lowest_, level_);
        if (frames_since_up_ < config_.up_frames) {
            backoff_ = std::min(backoff_ * 2, config_.max_up_backoff);
        }
        over_run_ = 0;
        headroom_run_ = 0;
    } else if (headroom_run_ >= config_.up_frames * backoff_ && level_ != EffectsLevel::Full) {
        level_ = static_cast<EffectsLevel>(static_cast<int>(level_) - 1);
        frames_since_up_ = 0;
        over_run_ = 0;
        headroom_run_ = 0;
    }
    // Long runs at either end stop counting, so the counters never overflow.
    over_run_ = std::min(over_run_, config_.down_frames);
    headroom_run_ = std::min(headroom_run_, config_.up_frames * config_.max_up_backoff);
    return level_;
}

} // namespace toonval
