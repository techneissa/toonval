// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace toonval {

// One reporting window of latency samples. All zero when `count` is 0.
struct LatencySummary {
    std::size_t count = 0;
    std::size_t dropped = 0; // Samples refused because the window was full.
    std::int64_t min_ns = 0;
    std::int64_t median_ns = 0; // Mean of the two middle samples for an even count, rounded down.
    std::int64_t max_ns = 0;
};

// Collects latency samples over a window and summarises them, e.g. key press to presented frame.
// Storage is reserved up front, so add() never allocates.
class LatencyStats {
  public:
    static constexpr std::size_t kDefaultCapacity = 4096;

    // A capacity of 0 is raised to 1.
    explicit LatencyStats(std::size_t capacity = kDefaultCapacity);

    // Records one sample. When the window is full the sample is counted as dropped instead.
    void add(std::int64_t ns);
    // Summarises the window and starts a new one.
    LatencySummary take_summary();
    std::size_t size() const { return samples_.size(); }

  private:
    std::size_t capacity_;
    std::size_t dropped_ = 0;
    std::vector<std::int64_t> samples_;
};

} // namespace toonval
