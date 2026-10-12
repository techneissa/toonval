// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/progress.hpp"

#include "progress_fixtures.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <vector>

using namespace toonval;
using toonval::testing::make;
using toonval::testing::RunSpec;

namespace {

RunRecord on_day(std::int32_t day, double minutes = 5.0) {
    RunSpec s;
    s.day = day;
    s.minutes = minutes;
    return make(s);
}

std::vector<RunRecord> days(std::initializer_list<std::int32_t> list, double minutes = 5.0) {
    std::vector<RunRecord> v;
    for (std::int32_t d : list) {
        v.push_back(on_day(d, minutes));
    }
    return v;
}

} // namespace

TEST_CASE("daily practice: no runs") {
    std::vector<RunRecord> none;
    CHECK(daily_practice(none).empty());
    std::vector<DayPractice> week = recent_days(none, 50, 7);
    REQUIRE(week.size() == 7);
    CHECK(week.front().day == 44);
    CHECK(week.back().day == 50);
    for (const DayPractice& d : week) {
        CHECK(d.practised_ms == 0);
        CHECK(d.runs == 0);
    }
    DailyGoalProgress g = daily_goal_progress(none, 50, 15);
    CHECK(g.has_goal);
    CHECK(g.minutes_today == 0.0);
    CHECK(g.fraction == 0.0);
    CHECK_FALSE(g.met);
}

TEST_CASE("daily practice adds up the minutes of each local day, days ascending") {
    std::vector<RunRecord> runs = {on_day(11, 4.0), on_day(10, 6.5), on_day(11, 1.5),
                                   on_day(13, 20.0)};
    std::vector<DayPractice> d = daily_practice(runs);
    REQUIRE(d.size() == 3);
    CHECK(d[0].day == 10);
    CHECK(d[0].minutes() == doctest::Approx(6.5));
    CHECK(d[0].runs == 1);
    CHECK(d[1].day == 11);
    CHECK(d[1].minutes() == doctest::Approx(5.5));
    CHECK(d[1].runs == 2);
    CHECK(d[2].day == 13);
    CHECK(d[2].minutes() == doctest::Approx(20.0));
}

TEST_CASE("one run") {
    std::vector<RunRecord> runs = {on_day(77, 12.0)};
    CHECK(daily_practice(runs).size() == 1);
    Streak s = practice_streak(runs, 77);
    CHECK(s.current == 1);
    CHECK(s.longest == 1);
    CHECK(s.today_done);
    CHECK_FALSE(s.at_risk);
    DailyGoalProgress g = daily_goal_progress(runs, 77, 15);
    CHECK(g.minutes_today == doctest::Approx(12.0));
    CHECK(g.fraction == doctest::Approx(0.8));
    CHECK_FALSE(g.met);
}

TEST_CASE("runs across midnight belong to the day the caller gave them") {
    // 23:50 on day 200 and 00:05 on day 201, Berlin in winter (UTC+1).
    const std::int32_t offset = 3600;
    const std::int64_t day200_2350 = 200 * 86'400LL - 3600 + 23 * 3600 + 50 * 60;
    const std::int64_t day201_0005 = day200_2350 + 15 * 60;
    REQUIRE(local_day_from_utc(day200_2350, offset) == 200);
    REQUIRE(local_day_from_utc(day201_0005, offset) == 201);
    RunSpec a;
    a.day = local_day_from_utc(day200_2350, offset);
    a.start_utc_s = day200_2350;
    a.minutes = 10.0;
    RunSpec b;
    b.day = local_day_from_utc(day201_0005, offset);
    b.start_utc_s = day201_0005;
    b.minutes = 10.0;
    std::vector<RunRecord> runs = {make(a), make(b)};
    std::vector<DayPractice> d = daily_practice(runs);
    REQUIRE(d.size() == 2);
    CHECK(d[0].day == 200);
    CHECK(d[1].day == 201);
    CHECK(practice_streak(runs, 201).current == 2);

    // A run that starts at 23:55 and goes on past midnight stays whole on the day it started.
    RunSpec c;
    c.day = 300;
    c.start_utc_s = 300 * 86'400LL + 23 * 3600 + 55 * 60;
    c.minutes = 20.0;
    std::vector<RunRecord> crossing = {make(c)};
    d = daily_practice(crossing);
    REQUIRE(d.size() == 1);
    CHECK(d[0].day == 300);
    CHECK(d[0].minutes() == doctest::Approx(20.0));
    CHECK(recent_days(crossing, 301, 2)[1].practised_ms == 0);
}

TEST_CASE("23 and 25 hour days are one day each because the caller supplies the day key") {
    // Spring forward: local day 500 lasts 23 hours; fall back: day 502 lasts 25. The runs are
    // 23 and 25 hours apart in UTC, one day apart by key; two runs inside the long day are
    // 24h30 apart in UTC and still one day.
    const std::int64_t base = 500 * 86'400LL;
    RunSpec a;
    a.day = 500;
    a.start_utc_s = base;
    RunSpec b;
    b.day = 501;
    b.start_utc_s = base + 23 * 3600;
    RunSpec c;
    c.day = 502;
    c.start_utc_s = base + 23 * 3600 + 25 * 3600;
    RunSpec d1;
    d1.day = 502;
    d1.start_utc_s = c.start_utc_s + 24 * 3600 + 30 * 60;
    RunSpec e;
    e.day = 503;
    e.start_utc_s = d1.start_utc_s + 3600;
    std::vector<RunRecord> runs = {make(a), make(b), make(c), make(d1), make(e)};
    std::vector<DayPractice> days_ = daily_practice(runs);
    REQUIRE(days_.size() == 4);
    CHECK(days_[2].day == 502);
    CHECK(days_[2].runs == 2);
    Streak s = practice_streak(runs, 503);
    CHECK(s.current == 4);
    CHECK(s.longest == 4);
}

TEST_CASE("the same run saved twice is counted once") {
    RunRecord r = on_day(40, 10.0);
    std::vector<RunRecord> runs = {r, on_day(40, 5.0), r, r};
    std::vector<std::size_t> unique = unique_run_indices(runs);
    REQUIRE(unique.size() == 2);
    CHECK(unique[0] == 0);
    CHECK(unique[1] == 1);
    std::vector<DayPractice> d = daily_practice(runs);
    REQUIRE(d.size() == 1);
    CHECK(d[0].minutes() == doctest::Approx(15.0));
    CHECK(d[0].runs == 2);
    CHECK(same_run(runs[0], runs[2]));
    CHECK_FALSE(same_run(runs[0], runs[1]));
    RunRecord faster = r;
    faster.speed_percent = 110;
    CHECK_FALSE(same_run(r, faster));
}

TEST_CASE("a clock that jumped back changes nothing about the days") {
    // The third run was saved last but its clock said day 11 (the Pi booted without a time).
    std::vector<RunRecord> runs = {on_day(12), on_day(13), on_day(11), on_day(14)};
    std::vector<DayPractice> d = daily_practice(runs);
    REQUIRE(d.size() == 4);
    CHECK(d[0].day == 11);
    CHECK(d[3].day == 14);
    CHECK(practice_streak(runs, 14).current == 4);
}

TEST_CASE("runs dated after today are ignored where today matters") {
    // Saved while the clock ran ahead, before it was corrected.
    std::vector<RunRecord> runs = {on_day(10), on_day(11), on_day(30, 90.0)};
    CHECK(practice_streak(runs, 11).current == 2);
    CHECK(daily_goal_progress(runs, 11, 15).minutes_today == doctest::Approx(5.0));
    std::vector<DayPractice> w = recent_days(runs, 11, 3);
    REQUIRE(w.size() == 3);
    CHECK(w.back().day == 11);
    CHECK(w.back().runs == 1);
    CHECK(daily_practice(runs).size() == 3); // the plain list has every day
}

TEST_CASE("recent_days is dense, oldest first, and clamps its count") {
    std::vector<RunRecord> runs = {on_day(8, 3.0), on_day(10, 4.0), on_day(10, 1.0)};
    std::vector<DayPractice> w = recent_days(runs, 10, 4);
    REQUIRE(w.size() == 4);
    CHECK(w[0].day == 7);
    CHECK(w[0].runs == 0);
    CHECK(w[1].day == 8);
    CHECK(w[1].minutes() == doctest::Approx(3.0));
    CHECK(w[2].day == 9);
    CHECK(w[2].practised_ms == 0);
    CHECK(w[3].day == 10);
    CHECK(w[3].minutes() == doctest::Approx(5.0));
    CHECK(recent_days(runs, 10, 0).empty());
    CHECK(recent_days(runs, 10, -3).empty());
    CHECK(recent_days(runs, 10, 1'000'000).size() == static_cast<std::size_t>(kMaxRecentDays));
}

TEST_CASE("the daily goal: minutes today against the goal") {
    std::vector<RunRecord> runs = {on_day(5, 30.0), on_day(6, 6.0), on_day(6, 4.5)};
    DailyGoalProgress g = daily_goal_progress(runs, 6, 15);
    CHECK(g.has_goal);
    CHECK(g.goal_minutes == 15);
    CHECK(g.minutes_today == doctest::Approx(10.5));
    CHECK(g.fraction == doctest::Approx(0.7));
    CHECK_FALSE(g.met);

    runs.push_back(on_day(6, 4.5));
    g = daily_goal_progress(runs, 6, 15);
    CHECK(g.met);
    CHECK(g.fraction == doctest::Approx(1.0));

    runs.push_back(on_day(6, 60.0));
    g = daily_goal_progress(runs, 6, 15);
    CHECK(g.fraction == 1.0); // never above 1
    CHECK(g.minutes_today == doctest::Approx(75.0));

    g = daily_goal_progress(runs, 6, 0); // no goal
    CHECK_FALSE(g.has_goal);
    CHECK_FALSE(g.met);
    CHECK(g.fraction == 0.0);
    CHECK(g.minutes_today == doctest::Approx(75.0));

    g = daily_goal_progress(runs, 6, 5000);
    CHECK(g.goal_minutes == kMaxDailyGoalMinutes);
    g = daily_goal_progress(runs, 6, -4);
    CHECK_FALSE(g.has_goal);

    g = daily_goal_progress(runs, 7, 15); // a day with no practice yet
    CHECK(g.minutes_today == 0.0);
}

TEST_CASE("streak: consecutive days, a gap ends it") {
    auto runs = days({1, 2, 3, 5, 6});
    Streak s = practice_streak(runs, 6);
    CHECK(s.current == 2);
    CHECK(s.longest == 3);
    CHECK(s.today_done);
    runs = days({1, 2, 3, 4, 5, 6});
    s = practice_streak(runs, 6);
    CHECK(s.current == 6);
    CHECK(s.longest == 6);
}

TEST_CASE("streak: today not yet practised does not break a streak that ended yesterday") {
    auto runs = days({3, 4, 5});
    Streak s = practice_streak(runs, 6);
    CHECK(s.current == 3);
    CHECK_FALSE(s.today_done);
    CHECK(s.at_risk);
    // A whole day without practice does.
    s = practice_streak(runs, 7);
    CHECK(s.current == 0);
    CHECK_FALSE(s.at_risk);
    CHECK(s.longest == 3);
}

TEST_CASE("streak: nothing practised") {
    std::vector<RunRecord> none;
    Streak s = practice_streak(none, 10);
    CHECK(s.current == 0);
    CHECK(s.longest == 0);
    CHECK_FALSE(s.today_done);
    CHECK_FALSE(s.at_risk);
    // A run with no practised time is not practice.
    auto zero = days({10}, 0.0);
    s = practice_streak(zero, 10);
    CHECK(s.current == 0);
}

TEST_CASE("streak: several runs on a day are one day; duplicates and order do not matter") {
    auto runs = days({4, 5, 5, 5, 6});
    runs.push_back(runs[0]); // a repeat
    Streak s = practice_streak(runs, 6);
    CHECK(s.current == 3);
    CHECK(s.longest == 3);
    std::reverse(runs.begin(), runs.end());
    CHECK(practice_streak(runs, 6).current == 3);
}

TEST_CASE("streak: the goal-based rule counts only days that reach the goal") {
    // Days 1..4 practised; day 3 only 10 of the 15 minutes.
    std::vector<RunRecord> runs = {on_day(1, 15.0), on_day(2, 8.0),  on_day(2, 7.0),
                                   on_day(3, 10.0), on_day(4, 20.0), on_day(5, 15.0)};
    CHECK(practice_streak(runs, 5).current == 5); // any practice
    Streak s = practice_streak(runs, 5, StreakRule::daily_goal(15));
    CHECK(s.current == 2); // days 4, 5
    CHECK(s.longest == 2); // days 1, 2 as well
    // Today short of the goal so far does not break it: the streak ends yesterday.
    runs.back() = on_day(5, 3.0);
    s = practice_streak(runs, 5, StreakRule::daily_goal(15));
    CHECK(s.current == 1);
    CHECK_FALSE(s.today_done);
    CHECK(s.at_risk);
    // No goal: any practice.
    CHECK(StreakRule::daily_goal(0).min_practised_ms ==
          StreakRule::any_practice().min_practised_ms);
    CHECK(StreakRule::daily_goal(15).min_practised_ms == 15 * 60'000);
    CHECK(StreakRule::daily_goal(99999).min_practised_ms == kMaxDailyGoalMinutes * 60'000LL);
}

TEST_CASE("streak: a clock ahead of today leaves the days up to today alone") {
    auto runs = days({8, 9, 10, 400});
    Streak s = practice_streak(runs, 10);
    CHECK(s.current == 3);
    CHECK(s.longest == 3);
}

TEST_CASE("a long history of 100,000 runs stays fast and right") {
    // 50 runs a day for 2,000 practised days; every 99th practised day is followed by a gap day,
    // so the days present are 0..98, 100..198, ... and the streaks are 99 days long.
    std::vector<RunRecord> runs;
    runs.reserve(100'000);
    for (int i = 0; i < 100'000; ++i) {
        std::int32_t d = i / 50;
        RunSpec s;
        s.day = d + d / 99;
        s.minutes = 0.5;
        s.start_utc_s = i;
        runs.push_back(make(s));
    }
    const std::int32_t today = 2019; // 1999 + 1999 / 99 = 2019
    auto t0 = std::chrono::steady_clock::now();
    Streak s = practice_streak(runs, today);
    std::vector<DayPractice> d = daily_practice(runs);
    DailyGoalProgress g = daily_goal_progress(runs, today, 15);
    std::vector<DayPractice> w = recent_days(runs, today, 30);
    auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count();
    CHECK(ms < 10'000); // O(n^2) would take minutes
    CHECK(s.current == 20);
    CHECK(s.longest == 99);
    CHECK(d.size() == 2000);
    CHECK(g.minutes_today == doctest::Approx(25.0));
    CHECK(w.size() == 30);
}
