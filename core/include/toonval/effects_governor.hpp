// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace toonval {

// How much of the look the renderer can afford. Each level drops
// one more effect, in this fixed order: particles first, then the motion background, then half the
// glow width, then the style extras (frost grain, specular streaks, gloss bands, rims). Notes and
// keys are never dropped: no level has a switch for them.
enum class EffectsLevel : std::uint8_t { Full, NoParticles, NoMotion, NarrowGlow, NoStyleExtras };
inline constexpr std::array<EffectsLevel, 5> kAllEffectsLevels{
    EffectsLevel::Full, EffectsLevel::NoParticles, EffectsLevel::NoMotion, EffectsLevel::NarrowGlow,
    EffectsLevel::NoStyleExtras};
inline constexpr EffectsLevel kLowestEffectsLevel = EffectsLevel::NoStyleExtras;

// What a level leaves switched on. The look's own switches still apply on top (a level never turns
// an effect on that the look has off).
struct EffectsBudget {
    bool particles = true;
    bool motion_background = true;
    float glow_width = 1.0f; // multiplier on every glow's width
    bool style_extras = true;
};

EffectsBudget effects_budget(EffectsLevel level);
// "full", "no particles", "no motion", "narrow glow", "no style extras".
std::string_view effects_level_name(EffectsLevel level);

struct GovernorConfig {
    std::int64_t budget_ns = 16'666'667; // one frame at 60 Hz
    // The moving average of frame times counts as over budget above `budget_ns * over_ratio`, and
    // as headroom below `budget_ns * headroom_ratio`. The gap between them is the hysteresis.
    double over_ratio = 1.0;
    double headroom_ratio = 0.7;
    double smoothing = 0.1; // weight of the newest frame in the moving average
    int down_frames = 30;   // frames over budget in a row before stepping down (0.5 s at 60 Hz)
    int up_frames = 300;    // frames of headroom in a row before stepping up (5 s at 60 Hz)
    // A step up that runs over budget again within `up_frames` frames doubles the headroom needed
    // for the next step up, up to this factor, so the level does not flap.
    int max_up_backoff = 8;
};

// What one frame cost, for EffectsGovernor::add_frame. With vsync the time between frames is the
// refresh period however light the frame was, so the cost is the frame's own work (`work_ns`,
// from the start of drawing to after the present), unless the frame missed a refresh: then the
// time since the last frame (`interval_ns`, more than 1.5 periods) is what it really cost.
std::int64_t frame_cost_ns(std::int64_t work_ns, std::int64_t interval_ns, std::int64_t period_ns);

// Picks the effects level from measured frame times. Pure logic: the caller measures each frame
// and applies the level it returns.
class EffectsGovernor {
  public:
    explicit EffectsGovernor(GovernorConfig config = {});

    // Adds one frame's time and returns the level to draw the next frame at. Times that are not
    // positive are ignored.
    EffectsLevel add_frame(std::int64_t frame_ns);

    EffectsLevel level() const { return level_; }
    // The most degraded level reached since construction or reset().
    EffectsLevel lowest() const { return lowest_; }
    double average_ns() const { return average_ns_; }
    int up_backoff() const { return backoff_; }
    const GovernorConfig& config() const { return config_; }

    void reset();

  private:
    GovernorConfig config_;
    EffectsLevel level_ = EffectsLevel::Full;
    EffectsLevel lowest_ = EffectsLevel::Full;
    double average_ns_ = 0.0;
    bool has_average_ = false;
    int over_run_ = 0;
    int headroom_run_ = 0;
    int backoff_ = 1;
    std::int64_t frames_since_up_ = INT64_MAX / 2;
};

} // namespace toonval
