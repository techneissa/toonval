// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/hand_path.hpp"

#include "toonval/progress.hpp"
#include "toonval/transport.hpp"

#include <algorithm>
#include <cmath>

namespace toonval {

namespace {

struct Reading {
    bool counted = false;
    double accuracy = 0.0;
};

Reading read_run(const RunRecord& run, const HandPathScope& scope, int min_graded) {
    int graded = 0;
    int missed = 0;
    int wrong = 0;
    if (scope.whole_song) {
        graded = run.all.graded;
        missed = run.all.graded - run.all.hit;
        wrong = run.wrong;
    } else {
        for (const RunBar& bar : run.measures) {
            if (bar.measure >= scope.first_measure && bar.measure <= scope.last_measure) {
                graded += bar.graded;
                missed += bar.missed;
                wrong += bar.wrong;
            }
        }
    }
    if (graded < std::max(1, min_graded)) {
        return {};
    }
    double errors = static_cast<double>(missed) + kWrongPressWeight * static_cast<double>(wrong);
    return {true, std::clamp(1.0 - errors / static_cast<double>(graded), 0.0, 1.0)};
}

HandProgress read_hand(std::span<const RunRecord> runs, std::uint64_t song_hash, HandFilter hands,
                       const HandPathScope& scope, const HandPathOptions& options, int goal,
                       bool skipped) {
    HandProgress p;
    p.hands = hands;
    p.skipped = skipped;
    if (song_hash == 0) {
        return p;
    }
    // Newest first, up to recent_runs counted runs of this hand.
    int taken = 0;
    for (std::size_t i = runs.size(); i-- > 0 && taken < options.recent_runs;) {
        const RunRecord& run = runs[i];
        if (run.song_hash != song_hash || run.hands != hands) {
            continue;
        }
        Reading r = read_run(run, scope, options.min_graded);
        if (!r.counted) {
            continue;
        }
        if (taken == 0) {
            p.last_speed_percent = run.speed_percent;
            p.last_accuracy = r.accuracy;
        }
        ++taken;
        p.best_accuracy = std::max(p.best_accuracy, r.accuracy);
        if (r.accuracy >= options.mastery_accuracy) {
            p.best_speed_percent = std::max(p.best_speed_percent, run.speed_percent);
        }
    }
    p.runs = taken;
    p.mastered = !skipped && p.best_speed_percent >= goal;
    return p;
}

// The song's length, 0 for a missing or non-finite one (as SongSections does).
double length_of(const Song& song) {
    return std::isfinite(song.duration_s) && song.duration_s > 0.0 ? song.duration_s : 0.0;
}

int clamp_speed(int percent) { return snap_speed_percent(percent); }

const char* hand_name(HandFilter hands) {
    return hands == HandFilter::Right  ? "Right hand"
           : hands == HandFilter::Left ? "Left hand"
                                       : "Both hands";
}

const char* hand_name_lower(HandFilter hands) {
    return hands == HandFilter::Right  ? "the right hand"
           : hands == HandFilter::Left ? "the left hand"
                                       : "both hands";
}

HandFilter hands_of(PathStage stage) {
    return stage == PathStage::RightHand  ? HandFilter::Right
           : stage == PathStage::LeftHand ? HandFilter::Left
                                          : HandFilter::Both;
}

} // namespace

HandPathScope scope_of_section(const Song& song, const Section* section) {
    HandPathScope scope;
    bool right = false;
    bool left = false;
    const double from = section != nullptr ? section->start_s : 0.0;
    const double to = section != nullptr ? section->end_s : length_of(song);
    for (const Note& n : song.notes) {
        if (n.drum || n.start_s < from || n.start_s >= to) {
            continue;
        }
        right = right || n.hand == Hand::Right;
        left = left || n.hand == Hand::Left;
    }
    scope.has_right = right;
    scope.has_left = left;
    if (section != nullptr) {
        scope.whole_song = false;
        // A song with no bars has sections on bar 0, but its runs record bar 1 (Song::measure_at).
        const bool bars = !song.measures.empty();
        scope.first_measure = bars ? section->first_measure : 1;
        scope.last_measure = bars ? section->last_measure : 1;
    }
    return scope;
}

HandPathStatus hand_path(std::span<const RunRecord> runs, std::uint64_t song_hash,
                         const HandPathScope& scope, const HandPathOptions& options) {
    const int hand_goal = clamp_speed(options.hand_goal_speed_percent);
    const int both_goal = clamp_speed(options.both_goal_speed_percent);
    // Which stages exist: right then left then both; with one hand in the scope, that hand alone;
    // with none known, both hands as one.
    const bool two = scope.has_right && scope.has_left;
    const bool none = !scope.has_right && !scope.has_left;
    const bool skip_right = !scope.has_right && !none;
    const bool skip_left = !scope.has_left && !none;
    const bool skip_both = !two && !none;

    HandPathStatus status;
    status.right = read_hand(runs, song_hash, HandFilter::Right, scope, options, hand_goal,
                             skip_right || none);
    status.left =
        read_hand(runs, song_hash, HandFilter::Left, scope, options, hand_goal, skip_left || none);
    status.both =
        read_hand(runs, song_hash, HandFilter::Both, scope, options, both_goal, skip_both);

    if (status.both.mastered || (skip_both && (skip_right || status.right.mastered) &&
                                 (skip_left || status.left.mastered))) {
        status.stage = PathStage::Done;
    } else if (!status.right.skipped && !status.right.mastered) {
        status.stage = PathStage::RightHand;
    } else if (!status.left.skipped && !status.left.mastered) {
        status.stage = PathStage::LeftHand;
    } else {
        status.stage = PathStage::BothHands;
    }
    return status;
}

PathSuggestion next_practice(const HandPathStatus& status, const HandPathOptions& options) {
    PathSuggestion s;
    s.stage = status.stage;
    if (status.stage == PathStage::Done) {
        s.advice = PathAdvice::AllDone;
        s.hands = HandFilter::Both;
        s.speed_percent = 0;
        return s;
    }
    s.hands = hands_of(status.stage);
    const HandProgress& hp = status.stage == PathStage::RightHand  ? status.right
                             : status.stage == PathStage::LeftHand ? status.left
                                                                   : status.both;
    const int goal = status.stage == PathStage::BothHands
                         ? clamp_speed(options.both_goal_speed_percent)
                         : clamp_speed(options.hand_goal_speed_percent);
    const int step = std::clamp(options.speed_step_percent, kSpeedStepPercent, kMaxSpeedPercent);
    int start = clamp_speed(options.start_speed_percent);
    if (status.stage == PathStage::BothHands) {
        // Both hands begin where the weaker hand got to.
        int hands_best = std::min(status.right.skipped ? 150 : status.right.best_speed_percent,
                                  status.left.skipped ? 150 : status.left.best_speed_percent);
        if (hands_best < 150) {
            start = std::max(start, clamp_speed(hands_best));
        }
    }
    start = std::min(start, goal);

    if (hp.runs == 0) {
        s.advice = PathAdvice::Start;
        s.speed_percent = start;
        s.moved_on =
            status.stage != PathStage::RightHand && (status.right.mastered || status.left.mastered);
        return s;
    }
    const int last = clamp_speed(hp.last_speed_percent);
    if (hp.last_accuracy >= options.mastery_accuracy) {
        s.advice = PathAdvice::RaiseSpeed;
        s.speed_percent = std::min(goal, clamp_speed(last + step));
    } else if (hp.last_accuracy >= options.slow_down_below) {
        s.advice = PathAdvice::KeepPracticing;
        s.speed_percent = std::min(goal, last);
    } else {
        s.advice = PathAdvice::SlowDown;
        s.speed_percent = std::max(kMinSpeedPercent, clamp_speed(last - step));
    }
    return s;
}

std::string path_hint(const HandPathStatus& status, const PathSuggestion& suggestion) {
    auto percent = [](int v) { return std::to_string(v) + "%"; };
    switch (suggestion.advice) {
    case PathAdvice::AllDone:
        return "Everything on this path is done.";
    case PathAdvice::Start: {
        std::string text;
        if (suggestion.moved_on) {
            const HandProgress& done = suggestion.stage == PathStage::LeftHand
                                           ? status.right
                                           : (status.left.mastered ? status.left : status.right);
            text = std::string(hand_name(done.hands)) + " at " + percent(done.best_speed_percent) +
                   ": done. Try ";
        } else {
            text = "Start with ";
        }
        return text + hand_name_lower(suggestion.hands) + " at " +
               percent(suggestion.speed_percent) + ".";
    }
    case PathAdvice::RaiseSpeed:
        return std::string(hand_name(suggestion.hands)) + ": clean run. Try " +
               percent(suggestion.speed_percent) + ".";
    case PathAdvice::KeepPracticing:
        return std::string(hand_name(suggestion.hands)) + ": close. Once more at " +
               percent(suggestion.speed_percent) + ".";
    case PathAdvice::SlowDown:
        return std::string(hand_name(suggestion.hands)) + ": a little slower, " +
               percent(suggestion.speed_percent) + ".";
    }
    return {};
}

} // namespace toonval
