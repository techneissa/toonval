// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/latency_stats.hpp"

#include <algorithm>

namespace toonval {

LatencyStats::LatencyStats(std::size_t capacity) : capacity_(std::max<std::size_t>(capacity, 1)) {
    samples_.reserve(capacity_);
}

void LatencyStats::add(std::int64_t ns) {
    if (samples_.size() >= capacity_) {
        ++dropped_;
        return;
    }
    samples_.push_back(ns);
}

LatencySummary LatencyStats::take_summary() {
    LatencySummary s;
    s.dropped = dropped_;
    dropped_ = 0;
    if (samples_.empty()) {
        return s;
    }
    std::sort(samples_.begin(), samples_.end());
    const std::size_t n = samples_.size();
    s.count = n;
    s.min_ns = samples_.front();
    s.max_ns = samples_.back();
    s.median_ns = n % 2 == 1 ? samples_[n / 2] : (samples_[n / 2 - 1] + samples_[n / 2]) / 2;
    samples_.clear();
    return s;
}

} // namespace toonval
