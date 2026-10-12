// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/latency_stats.hpp"

#include <doctest/doctest.h>

using namespace toonval;

TEST_CASE("an empty LatencyStats summarises to zero samples") {
    LatencyStats stats;
    const LatencySummary s = stats.take_summary();
    CHECK(s.count == 0);
    CHECK(s.min_ns == 0);
    CHECK(s.median_ns == 0);
    CHECK(s.max_ns == 0);
}

TEST_CASE("the summary gives min, median and max of an odd count") {
    LatencyStats stats;
    for (std::int64_t v : {30, 10, 20, 50, 40}) {
        stats.add(v);
    }
    CHECK(stats.size() == 5);
    const LatencySummary s = stats.take_summary();
    CHECK(s.count == 5);
    CHECK(s.min_ns == 10);
    CHECK(s.median_ns == 30);
    CHECK(s.max_ns == 50);
}

TEST_CASE("the median of an even count is the mean of the two middle samples") {
    LatencyStats stats;
    for (std::int64_t v : {4, 1, 3, 2}) {
        stats.add(v);
    }
    CHECK(stats.take_summary().median_ns == 2); // (2 + 3) / 2, rounded down
    for (std::int64_t v : {10, 20}) {
        stats.add(v);
    }
    CHECK(stats.take_summary().median_ns == 15);
}

TEST_CASE("take_summary starts a new window") {
    LatencyStats stats;
    stats.add(100);
    CHECK(stats.take_summary().count == 1);
    CHECK(stats.size() == 0);
    stats.add(7);
    const LatencySummary s = stats.take_summary();
    CHECK(s.count == 1);
    CHECK(s.min_ns == 7);
    CHECK(s.max_ns == 7);
}

TEST_CASE("a full window drops new samples and counts them") {
    LatencyStats stats(3);
    for (std::int64_t v : {1, 2, 3, 4, 5}) {
        stats.add(v);
    }
    const LatencySummary s = stats.take_summary();
    CHECK(s.count == 3);
    CHECK(s.max_ns == 3);
    CHECK(s.dropped == 2);
    stats.add(9);
    CHECK(stats.take_summary().dropped == 0);
}

TEST_CASE("a capacity of zero is raised to one sample") {
    LatencyStats stats(0);
    stats.add(5);
    CHECK(stats.take_summary().count == 1);
}
