// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/evaluator.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace toonval {

namespace {

using Kind = TransportOp::Kind;

constexpr std::int64_t kForever = std::numeric_limits<std::int64_t>::max();
constexpr std::int64_t kNever = std::numeric_limits<std::int64_t>::min();
constexpr double kEpsilonS = 1e-9;
// Op history older than this behind the decided edge is dropped.
constexpr std::int64_t kHistoryNs = 2'000'000'000;

std::int64_t to_ns(double ms) { return static_cast<std::int64_t>(std::llround(ms * 1e6)); }

// `x` clamped to [lo, hi]; NaN, which std::clamp passes through, gives `fallback`.
double sane(double x, double lo, double hi, double fallback) {
    return std::isnan(x) ? fallback : std::clamp(x, lo, hi);
}

bool practised(const Note& n, HandFilter hands) {
    if (n.drum) {
        return false;
    }
    switch (hands) {
    case HandFilter::Both:
        return true;
    case HandFilter::Left:
        return n.hand == Hand::Left;
    case HandFilter::Right:
        return n.hand == Hand::Right;
    }
    return false;
}

// A key and the practised notes on it that one press plays.
struct KeyGroup {
    double start_s = 0.0;
    int key = 0;
    int measure = 1;
    std::vector<std::uint32_t> notes;
};

// Groups the practised, playable notes into one entry per key press (notes on one key within
// kUnisonSeconds), sorted by start then key. Starts of unplayable practised notes go to
// `unplayable`.
std::vector<KeyGroup> key_groups(const std::vector<Note>& notes, HandFilter hands,
                                 const Transposition& tr, std::vector<double>& unplayable) {
    struct Item {
        double start_s;
        int key;
        std::uint32_t index;
    };
    std::vector<Item> items;
    unplayable.clear();
    for (std::uint32_t i = 0; i < notes.size(); ++i) {
        const Note& n = notes[i];
        if (!practised(n, hands)) {
            continue;
        }
        const TransposedPitch p = tr.expected_pitch(n);
        if (!p.playable || p.pitch < 0 || p.pitch > 127) {
            unplayable.push_back(n.start_s);
            continue;
        }
        items.push_back({n.start_s, p.pitch, i});
    }
    std::sort(unplayable.begin(), unplayable.end());
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        return a.key < b.key || (a.key == b.key && a.start_s < b.start_s);
    });
    std::vector<KeyGroup> groups;
    for (const Item& it : items) {
        if (!groups.empty() && groups.back().key == it.key &&
            it.start_s - groups.back().start_s <= kUnisonSeconds + kEpsilonS) {
            groups.back().notes.push_back(it.index);
            continue;
        }
        KeyGroup g;
        g.start_s = it.start_s;
        g.key = it.key;
        g.measure = notes[it.index].measure;
        g.notes.push_back(it.index);
        groups.push_back(std::move(g));
    }
    std::stable_sort(groups.begin(), groups.end(), [](const KeyGroup& a, const KeyGroup& b) {
        return a.start_s < b.start_s || (a.start_s == b.start_s && a.key < b.key);
    });
    return groups;
}

// How many unplayable starts lie in [from, to).
int count_in(const std::vector<double>& starts, double from, double to) {
    const auto a = std::lower_bound(starts.begin(), starts.end(), from);
    const auto b = std::lower_bound(starts.begin(), starts.end(), to);
    return static_cast<int>(b - a);
}

HandStats& hand_stats(EvaluationSummary& s, Hand h) {
    switch (h) {
    case Hand::Left:
        return s.left;
    case Hand::Right:
        return s.right;
    case Hand::Unknown:
        break;
    }
    return s.unknown;
}

MeasureStats& stats_for(std::vector<MeasureStats>& v, int measure) {
    auto it = std::lower_bound(v.begin(), v.end(), measure,
                               [](const MeasureStats& m, int n) { return m.measure < n; });
    if (it == v.end() || it->measure != measure) {
        MeasureStats m;
        m.measure = measure;
        it = v.insert(it, m);
    }
    return *it;
}

// Records one final note result in the summary.
void record(EvaluationSummary& s, const NoteResult& r) {
    const bool hit = r.grade != NoteGrade::Missed;
    switch (r.grade) {
    case NoteGrade::Perfect:
        ++s.counts.perfect;
        break;
    case NoteGrade::Good:
        ++s.counts.good;
        break;
    case NoteGrade::Early:
        ++s.counts.early;
        break;
    case NoteGrade::Late:
        ++s.counts.late;
        break;
    case NoteGrade::Missed:
        ++s.counts.missed;
        break;
    case NoteGrade::Played:
        ++s.counts.played;
        break;
    }
    for (HandStats* h : {&hand_stats(s, r.hand), &s.all}) {
        ++h->graded;
        h->hit += hit ? 1 : 0;
    }
    MeasureStats& m = stats_for(s.measures, r.measure);
    ++m.graded;
    m.missed += hit ? 0 : 1;
    if (hit) {
        ++s.combo;
        s.longest_combo = std::max(s.longest_combo, s.combo);
    } else {
        s.combo = 0;
    }
    s.notes.push_back(r);
}

void record_wrong(EvaluationSummary& s, int measure) {
    ++s.wrong;
    s.combo = 0;
    ++stats_for(s.measures, measure).wrong;
}

bool in_dead(const std::vector<std::array<std::int64_t, 2>>& dead, std::int64_t ns) {
    for (const auto& d : dead) {
        if (ns >= d[0] && ns < d[1]) {
            return true;
        }
    }
    return false;
}

void open_dead(std::vector<std::array<std::int64_t, 2>>& dead, std::int64_t from) {
    if (dead.empty() || dead.back()[1] != kForever) {
        dead.push_back({from, kForever});
    }
}

void close_dead(std::vector<std::array<std::int64_t, 2>>& dead, std::int64_t at) {
    if (!dead.empty() && dead.back()[1] == kForever) {
        dead.back()[1] = std::max(at, dead.back()[0]);
    }
}

void prune_dead(std::vector<std::array<std::int64_t, 2>>& dead, std::int64_t before) {
    dead.erase(std::remove_if(dead.begin(), dead.end(),
                              [&](const auto& d) { return d[1] != kForever && d[1] < before; }),
               dead.end());
}

// Keeps only the pass in progress, numbered 0. It lost results to the reset, so it is never clean.
void restart_passes(detail::PassLog& passes) {
    detail::PassState current = passes.size() == 0 ? detail::PassState{} : passes.back();
    current.interrupted = current.interrupted || current.moved;
    current.result = {};
    passes.reset(current);
}

// The pass in progress was cut short (a seek, a song or key change): it ends at `ns` and is never
// reported.
void drop_pass(detail::PassState& pass, double end_s, std::int64_t ns) {
    pass.interrupted = true;
    pass.ended = true;
    pass.dropped = true;
    pass.end_s = end_s;
    pass.end_ns = ns;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// PassLog

namespace detail {

void PassLog::reset(PassState current) {
    current.result.pass = 0;
    passes_.assign(1, current);
    base_ = 0;
    reported_ = 0;
}

void PassLog::push(PassState p) {
    p.result.pass = base_ + static_cast<int>(passes_.size());
    passes_.push_back(p);
}

int PassLog::number_at(std::int64_t ns) const {
    for (std::size_t i = passes_.size(); i-- > 0;) {
        if (passes_[i].start_ns <= ns || i == 0) {
            return base_ + static_cast<int>(i);
        }
    }
    return base_;
}

} // namespace detail

// ---------------------------------------------------------------------------------------------
// TransportTimeline

void TransportTimeline::apply(const TransportOp& op) {
    if (op.kind == Kind::Click) {
        return;
    }
    Span s;
    s.at_ns = op.at_ns;
    s.end_ns = op.kind == Kind::Move ? std::max(op.end_ns, op.at_ns) : op.at_ns;
    s.from_s = op.kind == Kind::Move ? op.from_s : op.to_s;
    s.to_s = op.to_s;
    s.speed_percent = op.kind == Kind::Move ? op.speed_percent
                      : spans_.empty()      ? 100
                                            : spans_.back().speed_percent;
    spans_.push_back(s);
}

double TransportTimeline::song_at(std::int64_t ns) const {
    if (spans_.empty()) {
        return 0.0;
    }
    // The last span starting at or before `ns`.
    auto it = std::upper_bound(spans_.begin(), spans_.end(), ns,
                               [](std::int64_t t, const Span& s) { return t < s.at_ns; });
    if (it == spans_.begin()) {
        return spans_.front().from_s;
    }
    const Span& s = *(it - 1);
    if (ns >= s.end_ns || s.end_ns == s.at_ns) {
        return s.to_s;
    }
    const double f = static_cast<double>(ns - s.at_ns) / static_cast<double>(s.end_ns - s.at_ns);
    return s.from_s + f * (s.to_s - s.from_s);
}

double TransportTimeline::rate_at(std::int64_t ns) const {
    auto it = std::upper_bound(spans_.begin(), spans_.end(), ns,
                               [](std::int64_t t, const Span& s) { return t < s.at_ns; });
    if (it == spans_.begin()) {
        return 1.0;
    }
    return (it - 1)->speed_percent / 100.0;
}

void TransportTimeline::forget_before(std::int64_t ns) {
    auto it = std::upper_bound(spans_.begin(), spans_.end(), ns,
                               [](std::int64_t t, const Span& s) { return t < s.at_ns; });
    if (it - spans_.begin() > 1) {
        spans_.erase(spans_.begin(), it - 1);
    }
}

// ---------------------------------------------------------------------------------------------
// FlowEvaluator

void FlowEvaluator::reset_clock() {
    timeline_.clear();
    dead_.clear();
    dead_.push_back({kNever, kForever}); // nothing counts before the first play
    ignore_before_ns_ = kNever;
    grace_ = {0, 0};
    clock_ns_ = kNever;
    moving_ = false;
}

void FlowEvaluator::set_song(const Song& song, const FlowOptions& options) {
    const FlowOptions d;
    options_ = options;
    options_.window_ms = sane(options_.window_ms, 10.0, 500.0, d.window_ms);
    options_.perfect_fraction = sane(options_.perfect_fraction, 0.0, 1.0, d.perfect_fraction);
    options_.good_fraction = std::max(options_.perfect_fraction,
                                      sane(options_.good_fraction, 0.0, 1.0, d.good_fraction));
    options_.latency_ms = sane(options_.latency_ms, -1000.0, 1000.0, d.latency_ms);
    options_.settle_ms = sane(options_.settle_ms, 0.0, 1000.0, d.settle_ms);
    options_.lookahead_s = sane(options_.lookahead_s, 0.0, 60.0, d.lookahead_s);
    if (source_ != nullptr) {
        options_.transposition = source_->value();
        source_version_ = source_->version();
        source_synced_ = true;
    }
    window_ns_ = to_ns(options_.window_ms);
    perfect_ns_ = static_cast<std::int64_t>(
        std::llround(static_cast<double>(window_ns_) * options_.perfect_fraction));
    good_ns_ = static_cast<std::int64_t>(
        std::llround(static_cast<double>(window_ns_) * options_.good_fraction));
    latency_ns_ = to_ns(options_.latency_ms);
    settle_ns_ = to_ns(options_.settle_ms);

    notes_ = song.notes;
    measures_ = song.measures;
    build_targets();
    // What was undecided belongs to the old song. The clock stays as the Transport last said.
    pending_.clear();
    presses_.clear();
    outcomes_.clear();
    if (clock_ns_ != kNever) {
        ignore_before_ns_ = std::max(ignore_before_ns_, clock_ns_);
        grace_ = {clock_ns_, clock_ns_ + window_ns_};
    }
    detail::PassState fresh;
    fresh.interrupted = moving_; // starts mid-song, so it can't be a clean pass
    fresh.moved = moving_;
    passes_.reset(fresh);
    reset_results();
}

void FlowEvaluator::build_targets() {
    targets_.clear();
    for (KeyGroup& g : key_groups(notes_, options_.hands, options_.transposition, unplayable_s_)) {
        targets_.push_back({g.start_s, g.key, g.measure, std::move(g.notes)});
    }
}

void FlowEvaluator::follow(const TranspositionSource* source) {
    source_ = source;
    source_synced_ = false;
}

void FlowEvaluator::sync_transposition() {
    if (source_ == nullptr || (source_synced_ && source_version_ == source_->version())) {
        return;
    }
    source_synced_ = true;
    source_version_ = source_->version();
    set_transposition(source_->value(), source_->changed_at_ns());
}

void FlowEvaluator::set_transposition(const Transposition& transposition, std::int64_t now_ns) {
    if (transposition == options_.transposition) {
        return;
    }
    drop_undecided(now_ns);
    grace_ = {now_ns, now_ns + window_ns_};
    options_.transposition = transposition;
    build_targets();
    detail::PassState& pass = passes_.back();
    pass.interrupted = pass.interrupted || pass.moved;
}

void FlowEvaluator::drop_undecided(std::int64_t at_ns) {
    resolve(std::min(at_ns - settle_ns_, clock_ns_));
    for (Outcome& o : outcomes_) {
        apply_outcome(o);
    }
    outcomes_.clear();
    pending_.clear();
    presses_.clear();
    ignore_before_ns_ = std::max(ignore_before_ns_, at_ns);
}

void FlowEvaluator::reset_results() {
    summary_ = {};
    restart_passes(passes_);
    for (Pending& p : pending_) {
        p.pass = 0;
    }
    for (Outcome& o : outcomes_) {
        o.pass = 0;
    }
}

NoteGrade FlowEvaluator::grade(std::int64_t error_ns) const {
    const std::int64_t a = error_ns < 0 ? -error_ns : error_ns;
    if (a <= perfect_ns_) {
        return NoteGrade::Perfect;
    }
    if (a <= good_ns_) {
        return NoteGrade::Good;
    }
    return error_ns < 0 ? NoteGrade::Early : NoteGrade::Late;
}

void FlowEvaluator::apply(const TransportOp& op) {
    if (op.kind == Kind::Click) {
        return;
    }
    sync_transposition();
    timeline_.apply(op);
    detail::PassState& pass = passes_.back();
    switch (op.kind) {
    case Kind::Play:
        close_dead(dead_, op.at_ns);
        moving_ = true;
        clock_ns_ = std::max(clock_ns_, op.at_ns);
        if (!pass.moved) {
            pass.start_s = op.to_s;
            pass.start_ns = op.at_ns;
        }
        break;
    case Kind::Release:
        moving_ = true;
        clock_ns_ = std::max(clock_ns_, op.at_ns);
        break;
    case Kind::Pause:
    case Kind::End:
        open_dead(dead_, op.at_ns + window_ns_);
        [[fallthrough]];
    case Kind::Hold:
        moving_ = false;
        clock_ns_ = std::max(clock_ns_, op.at_ns);
        break;
    case Kind::Move: {
        clock_ns_ = std::max(clock_ns_, op.end_ns);
        pass.moved = true;
        summary_.unplayable += count_in(unplayable_s_, op.from_s, op.to_s);
        if (op.to_s <= op.from_s) {
            break;
        }
        auto first = std::lower_bound(targets_.begin(), targets_.end(), op.from_s,
                                      [](const Target& t, double s) { return t.start_s < s; });
        const double span = op.to_s - op.from_s;
        const double wall = static_cast<double>(op.end_ns - op.at_ns);
        for (auto it = first; it != targets_.end() && it->start_s < op.to_s; ++it) {
            Pending p;
            p.target = static_cast<std::uint32_t>(it - targets_.begin());
            p.arrival_ns = op.at_ns + static_cast<std::int64_t>(
                                          std::llround((it->start_s - op.from_s) / span * wall));
            if (p.arrival_ns < ignore_before_ns_) {
                continue; // reached before a key or song change: neither hit nor missed
            }
            p.pass = pass.result.pass;
            pending_.push_back(p);
        }
        break;
    }
    case Kind::Wrap: {
        pass.ended = true;
        pass.end_s = op.from_s;
        pass.end_ns = op.at_ns;
        pass.loop_start_s = op.to_s;
        detail::PassState next;
        next.start_s = op.to_s;
        next.start_ns = op.at_ns;
        next.moved = true;
        passes_.push(next);
        break;
    }
    case Kind::Seek: {
        // Decide what is already decided, then drop the rest.
        drop_undecided(op.at_ns);
        if (pass.moved) {
            drop_pass(pass, op.from_s, op.at_ns);
            detail::PassState next;
            next.start_s = op.to_s;
            next.start_ns = op.at_ns;
            passes_.push(next);
        } else {
            pass.interrupted = false;
            pass.start_s = op.to_s;
            pass.start_ns = op.at_ns;
        }
        break;
    }
    case Kind::Click:
        break;
    }
}

bool FlowEvaluator::ignored(std::int64_t ns) const {
    return ns < ignore_before_ns_ || in_dead(dead_, ns);
}

void FlowEvaluator::apply(const MidiEvent& ev) {
    sync_transposition();
    if (ev.synthetic || ev.type != MidiEventType::NoteOn || ev.data2 == 0 || ev.data1 > 127) {
        return;
    }
    const std::int64_t t = ev.time_ns - latency_ns_;
    if (ignored(ev.time_ns) || t < ignore_before_ns_) {
        return;
    }
    presses_.push_back({ev.data1, t, false});
}

int FlowEvaluator::measure_at(double song_s) const {
    auto it = std::upper_bound(measures_.begin(), measures_.end(), song_s,
                               [](double s, const Measure& m) { return s < m.start_s; });
    if (it == measures_.begin()) {
        return measures_.empty() ? 1 : measures_.front().number;
    }
    return (it - 1)->number;
}

bool FlowEvaluator::excused(const Press& p) const {
    if (p.time_ns >= grace_[0] && p.time_ns < grace_[1]) {
        return true;
    }
    // The key of a note still to come within the lookahead: pressed early, not wrong.
    const double at = timeline_.song_at(p.time_ns);
    auto it = std::upper_bound(targets_.begin(), targets_.end(), at,
                               [](double s, const Target& t) { return s < t.start_s; });
    for (; it != targets_.end() && it->start_s <= at + options_.lookahead_s + kEpsilonS; ++it) {
        if (it->key == p.key) {
            return true;
        }
    }
    return false;
}

// Decides every group of notes and presses that nothing after `frontier` can reach.
//
// Per key, notes and presses sorted by time split into groups wherever two neighbours are more
// than a window apart; no match can cross such a gap. In a closed group the best assignment is
// order-preserving (if note a1 < a2 were matched to presses b2 > b1, swapping keeps both pairs in
// the window and never adds error), so a DP over (notes, presses) finds the most matches with the
// least total |error| (decide_group).
void FlowEvaluator::resolve(std::int64_t frontier) {
    if (pending_.empty() && presses_.empty()) {
        return;
    }
    std::stable_sort(pending_.begin(), pending_.end(), [&](const Pending& a, const Pending& b) {
        const int ka = targets_[a.target].key;
        const int kb = targets_[b.target].key;
        return ka < kb || (ka == kb && a.arrival_ns < b.arrival_ns);
    });
    std::stable_sort(presses_.begin(), presses_.end(), [](const Press& a, const Press& b) {
        return a.key < b.key || (a.key == b.key && a.time_ns < b.time_ns);
    });

    std::size_t ni = 0;
    std::size_t pi = 0;
    while (ni < pending_.size() || pi < presses_.size()) {
        const int key = std::min(ni < pending_.size() ? targets_[pending_[ni].target].key : 1000,
                                 pi < presses_.size() ? presses_[pi].key : 1000);
        std::size_t ne = ni;
        while (ne < pending_.size() && targets_[pending_[ne].target].key == key) {
            ++ne;
        }
        std::size_t pe = pi;
        while (pe < presses_.size() && presses_[pe].key == key) {
            ++pe;
        }
        // Walk this key's items in time order, cutting groups at gaps wider than the window.
        std::size_t a = ni;
        std::size_t b = pi;
        while (a < ne || b < pe) {
            const std::size_t a0 = a;
            const std::size_t b0 = b;
            std::int64_t last = kNever;
            for (;;) {
                const std::int64_t ta = a < ne ? pending_[a].arrival_ns : kForever;
                const std::int64_t tb = b < pe ? presses_[b].time_ns : kForever;
                const std::int64_t t = std::min(ta, tb);
                if (t == kForever || (last != kNever && t - last > window_ns_)) {
                    break;
                }
                last = t;
                if (ta <= tb) {
                    ++a;
                } else {
                    ++b;
                }
            }
            if (last + window_ns_ >= frontier) {
                continue; // still open
            }
            decide_group(a0, a - a0, b0, b - b0);
        }
        ni = ne;
        pi = pe;
    }
    pending_.erase(
        std::remove_if(pending_.begin(), pending_.end(), [](const Pending& p) { return p.done; }),
        pending_.end());
    presses_.erase(
        std::remove_if(presses_.begin(), presses_.end(), [](const Press& p) { return p.done; }),
        presses_.end());
}

// The DP over (i notes, j presses), banded: note i can only match the presses in
// [lower[i], upper[i]), the ones within a window of it, and both bounds grow with i. Row r (the
// first r notes) is stored for j in [lo_r, hi_r] = [lower[r - 1], upper[r - 1]] only:
// - j > hi_r: the presses past hi_r are too late for every note before r, so the cell equals
//   (r, hi_r);
// - j < lo_r is never needed: the fill and the walk back stay at or right of lo_r, where
//   (r, lo_r) equals (r - 1, lo_r) because note r - 1 can't use a press before lo_r.
// Cells hold the same values and the walk makes the same choices as the full table, in
// O(notes * presses per window) time and memory instead of O(notes * presses).
void FlowEvaluator::decide_group(std::size_t a0, std::size_t n, std::size_t b0, std::size_t m) {
    struct Cell {
        int count = 0;
        std::int64_t cost = 0;
    };
    const auto better = [](const Cell& x, const Cell& y) {
        return x.count > y.count || (x.count == y.count && x.cost < y.cost);
    };
    const auto same = [](const Cell& x, const Cell& y) {
        return x.count == y.count && x.cost == y.cost;
    };
    // Row bounds, and where each row starts in `cells`.
    std::vector<std::size_t> lo(n + 1, 0);
    std::vector<std::size_t> hi(n + 1, 0);
    std::vector<std::size_t> row(n + 2, 0);
    std::size_t lower = 0;
    std::size_t upper = 0;
    for (std::size_t r = 1; r <= n; ++r) {
        const std::int64_t arrival = pending_[a0 + r - 1].arrival_ns;
        while (lower < m && presses_[b0 + lower].time_ns < arrival - window_ns_) {
            ++lower;
        }
        upper = std::max(upper, lower);
        while (upper < m && presses_[b0 + upper].time_ns <= arrival + window_ns_) {
            ++upper;
        }
        lo[r] = lower;
        hi[r] = upper;
    }
    for (std::size_t r = 0; r <= n; ++r) {
        row[r + 1] = row[r] + (hi[r] - lo[r] + 1);
    }
    std::vector<Cell> cells(row[n + 1]);
    const auto get = [&](std::size_t r, std::size_t j) -> Cell& {
        return cells[row[r] + std::min(j, hi[r]) - lo[r]];
    };
    const auto feasible = [&](std::size_t r, std::size_t j) { // note r - 1, press j - 1
        return j > lo[r] && j <= hi[r];
    };
    const auto matched = [&](std::size_t r, std::size_t j) {
        Cell c = get(r - 1, j - 1);
        const std::int64_t e = presses_[b0 + j - 1].time_ns - pending_[a0 + r - 1].arrival_ns;
        ++c.count;
        c.cost += e < 0 ? -e : e;
        return c;
    };
    for (std::size_t r = 1; r <= n; ++r) {
        get(r, lo[r]) = get(r - 1, lo[r]);
        for (std::size_t j = lo[r] + 1; j <= hi[r]; ++j) {
            Cell best{-1, 0};
            if (feasible(r, j)) {
                best = matched(r, j);
            }
            if (better(get(r - 1, j), best)) {
                best = get(r - 1, j);
            }
            if (better(get(r, j - 1), best)) {
                best = get(r, j - 1);
            }
            get(r, j) = best;
        }
    }
    // Walk back the same choices, in the same preference order.
    std::vector<long> match(n, -1);
    std::size_t i = n;
    std::size_t j = m;
    while (i > 0 && j > 0) {
        const Cell here = get(i, j);
        if (feasible(i, j) && same(matched(i, j), here)) {
            match[i - 1] = static_cast<long>(j - 1);
            --i;
            --j;
        } else if (same(get(i - 1, j), here)) {
            --i;
        } else {
            --j;
        }
    }

    std::vector<bool> used(m, false);
    for (std::size_t k = 0; k < n; ++k) {
        Pending& p = pending_[a0 + k];
        p.done = true;
        Outcome o;
        o.time_ns = p.arrival_ns;
        o.target = p.target;
        o.pass = p.pass;
        o.arrival_ns = p.arrival_ns;
        if (match[k] >= 0) {
            const std::size_t pj = static_cast<std::size_t>(match[k]);
            used[pj] = true;
            o.hit = true;
            o.press_ns = presses_[b0 + pj].time_ns;
        }
        outcomes_.push_back(o);
    }
    for (std::size_t k = 0; k < m; ++k) {
        Press& p = presses_[b0 + k];
        p.done = true;
        if (!used[k] && !excused(p)) {
            Outcome o;
            o.time_ns = p.time_ns;
            o.wrong = true;
            o.press_ns = p.time_ns;
            o.pass = passes_.number_at(p.time_ns);
            outcomes_.push_back(o);
        }
    }
}

void FlowEvaluator::apply_outcome(const Outcome& o) {
    PassResult& pr = passes_.at(o.pass).result;
    if (o.wrong) {
        record_wrong(summary_, measure_at(timeline_.song_at(o.press_ns)));
        ++pr.wrong;
        return;
    }
    const Target& t = targets_[o.target];
    for (std::uint32_t idx : t.notes) {
        const Note& n = notes_[idx];
        NoteResult r;
        r.note_index = idx;
        r.pass = o.pass;
        r.expected_pitch = t.key;
        r.hand = n.hand;
        r.measure = n.measure;
        r.song_s = n.start_s;
        r.arrival_ns = o.arrival_ns;
        if (o.hit) {
            r.press_ns = o.press_ns;
            r.error_ns = o.press_ns - o.arrival_ns;
            r.grade = grade(r.error_ns);
        } else {
            r.grade = NoteGrade::Missed;
            ++pr.missed;
        }
        ++pr.graded;
        record(summary_, r);
    }
}

void FlowEvaluator::apply_outcomes(std::int64_t safe) {
    std::stable_sort(outcomes_.begin(), outcomes_.end(),
                     [](const Outcome& a, const Outcome& b) { return a.time_ns < b.time_ns; });
    std::size_t used = 0;
    while (used < outcomes_.size() && outcomes_[used].time_ns < safe) {
        apply_outcome(outcomes_[used]);
        ++used;
    }
    outcomes_.erase(outcomes_.begin(), outcomes_.begin() + static_cast<std::ptrdiff_t>(used));
}

void FlowEvaluator::update(std::int64_t now_ns) {
    sync_transposition();
    const std::int64_t clock = moving_ ? clock_ns_ : std::max(clock_ns_, now_ns);
    const std::int64_t frontier = std::min(now_ns - settle_ns_, clock);
    resolve(frontier);
    std::int64_t safe = frontier;
    for (const Pending& p : pending_) {
        safe = std::min(safe, p.arrival_ns);
    }
    for (const Press& p : presses_) {
        safe = std::min(safe, p.time_ns);
    }
    apply_outcomes(safe);
    passes_.report([&](const detail::PassState& p) { return p.end_ns < safe; });
    timeline_.forget_before(safe - window_ns_ - kHistoryNs);
    prune_dead(dead_, safe - window_ns_ - kHistoryNs);
}

void FlowEvaluator::finish() {
    resolve(kForever);
    apply_outcomes(kForever);
    passes_.report([](const detail::PassState&) { return true; });
}

void FlowEvaluator::take_passes(std::vector<PassResult>& out) {
    passes_.take([&](const detail::PassState& p) {
        PassResult r = p.result;
        r.start_s = p.start_s;
        r.end_s = p.end_s;
        r.clean = !p.interrupted && p.start_s <= p.loop_start_s + kEpsilonS && r.missed == 0 &&
                  r.wrong == 0;
        out.push_back(r);
    });
}

// ---------------------------------------------------------------------------------------------
// WaitEvaluator

void WaitEvaluator::reset_clock() {
    timeline_.clear();
    inbox_.clear();
    keys_ = {};
    position_s_ = 0.0;
    last_op_ns_ = kNever;
    playing_ = false;
    held_ = false;
    held_since_ns_ = 0;
    restart_ns_ = kNever;
    dead_.clear();
    dead_.push_back({kNever, kForever});
}

void WaitEvaluator::set_song(const Song& song, const WaitOptions& options) {
    const WaitOptions d;
    options_ = options;
    options_.early_ms = sane(options_.early_ms, 0.0, 2000.0, d.early_ms);
    options_.release_slack_ms = sane(options_.release_slack_ms, 0.0, 10000.0, d.release_slack_ms);
    options_.settle_ms = sane(options_.settle_ms, 0.0, 1000.0, d.settle_ms);
    options_.lookahead_s = sane(options_.lookahead_s, 0.0, 60.0, d.lookahead_s);
    if (source_ != nullptr) {
        options_.transposition = source_->value();
        source_version_ = source_->version();
        source_synced_ = true;
    }
    early_ns_ = to_ns(options_.early_ms);
    slack_ns_ = to_ns(options_.release_slack_ms);
    settle_ns_ = to_ns(options_.settle_ms);

    notes_ = song.notes;
    build_chords();
    // The clock and the keys stay as they are: wait for the first chord at or after the position,
    // with every key down pressed again.
    restart_at(position_s_, last_op_ns_);
    detail::PassState fresh;
    fresh.interrupted = playing_; // starts mid-song, so it can't be a clean pass
    fresh.moved = playing_;
    passes_.reset(fresh);
    reset_results();
}

void WaitEvaluator::build_chords() {
    chords_.clear();
    for (KeyGroup& g : key_groups(notes_, options_.hands, options_.transposition, unplayable_s_)) {
        if (chords_.empty() || g.start_s - chords_.back().start_s > kChordSeconds + kEpsilonS) {
            Chord c;
            c.start_s = g.start_s;
            c.measure = g.measure;
            chords_.push_back(std::move(c));
        }
        std::vector<ChordKey>& keys = chords_.back().keys;
        auto it = std::find_if(keys.begin(), keys.end(),
                               [&](const ChordKey& k) { return k.key == g.key; });
        if (it == keys.end()) {
            keys.push_back({g.key, std::move(g.notes)});
        } else {
            it->notes.insert(it->notes.end(), g.notes.begin(), g.notes.end());
        }
    }
    for (Chord& c : chords_) {
        std::sort(c.keys.begin(), c.keys.end(),
                  [](const ChordKey& a, const ChordKey& b) { return a.key < b.key; });
    }
}

void WaitEvaluator::follow(const TranspositionSource* source) {
    source_ = source;
    source_synced_ = false;
}

void WaitEvaluator::sync_transposition() {
    if (source_ == nullptr || (source_synced_ && source_version_ == source_->version())) {
        return;
    }
    source_synced_ = true;
    source_version_ = source_->version();
    set_transposition(source_->value(), source_->changed_at_ns());
}

void WaitEvaluator::set_transposition(const Transposition& transposition, std::int64_t now_ns) {
    if (transposition == options_.transposition) {
        return;
    }
    options_.transposition = transposition;
    build_chords();
    restart_at(position_s_, now_ns);
    detail::PassState& pass = passes_.back();
    pass.interrupted = pass.interrupted || pass.moved;
}

void WaitEvaluator::reset_results() {
    summary_ = {};
    restart_passes(passes_);
    obligations_.clear(); // they point into summary_.notes
}

double WaitEvaluator::waiting_for_s() const {
    return next_ < chords_.size() ? chords_[next_].start_s : -1.0;
}

std::vector<int> WaitEvaluator::waiting_keys() const {
    std::vector<int> out;
    if (next_ < chords_.size()) {
        for (const ChordKey& k : chords_[next_].keys) {
            out.push_back(k.key);
        }
    }
    return out;
}

void WaitEvaluator::new_pass(double start_s, std::int64_t ns) {
    detail::PassState next;
    next.start_s = start_s;
    next.start_ns = ns;
    next.moved = true;
    passes_.push(next);
}

void WaitEvaluator::restart_at(double song_s, std::int64_t ns) {
    auto it = std::lower_bound(chords_.begin(), chords_.end(), song_s - kEpsilonS,
                               [](const Chord& c, double s) { return c.start_s < s; });
    next_ = static_cast<std::size_t>(it - chords_.begin());
    obligations_.clear();
    sounding_.clear();
    held_since_ns_ = ns;
    restart_ns_ = std::max(restart_ns_, ns);
    for (Key& k : keys_) {
        k.consumed = true; // every key down now must be pressed again
    }
}

void WaitEvaluator::apply(const TransportOp& op) {
    if (op.kind == Kind::Click) {
        return;
    }
    sync_transposition();
    timeline_.apply(op);
    last_op_ns_ = std::max(last_op_ns_, op.kind == Kind::Move ? op.end_ns : op.at_ns);
    detail::PassState& pass = passes_.back();
    switch (op.kind) {
    case Kind::Play:
        close_dead(dead_, op.at_ns);
        playing_ = true;
        position_s_ = op.to_s;
        if (!pass.moved) {
            pass.start_s = op.to_s;
            pass.start_ns = op.at_ns;
        }
        break;
    case Kind::Pause:
    case Kind::End:
        open_dead(dead_, op.at_ns);
        playing_ = false;
        position_s_ = op.to_s;
        break;
    case Kind::Hold:
        held_ = true;
        held_since_ns_ = op.at_ns;
        position_s_ = op.to_s;
        break;
    case Kind::Release:
        held_ = false;
        position_s_ = op.to_s;
        break;
    case Kind::Move: {
        pass.moved = true;
        summary_.unplayable += count_in(unplayable_s_, op.from_s, op.to_s);
        position_s_ = op.to_s;
        const double slack_s = static_cast<double>(slack_ns_) * 1e-9 * op.speed_percent / 100.0;
        obligations_.erase(std::remove_if(obligations_.begin(), obligations_.end(),
                                          [&](const Obligation& o) {
                                              return !o.needs_repress &&
                                                     position_s_ >= o.end_s - slack_s;
                                          }),
                           obligations_.end());
        sounding_.erase(std::remove_if(sounding_.begin(), sounding_.end(),
                                       [&](const Sounding& s) { return position_s_ >= s.end_s; }),
                        sounding_.end());
        break;
    }
    case Kind::Wrap:
        pass.ended = true;
        pass.end_s = op.from_s;
        pass.end_ns = op.at_ns;
        pass.loop_start_s = op.to_s;
        new_pass(op.to_s, op.at_ns);
        position_s_ = op.to_s;
        restart_at(op.to_s, op.at_ns);
        break;
    case Kind::Seek:
        if (pass.moved) {
            drop_pass(pass, op.from_s, op.at_ns);
            new_pass(op.to_s, op.at_ns);
        } else {
            pass.start_s = op.to_s;
            pass.start_ns = op.at_ns;
        }
        position_s_ = op.to_s;
        restart_at(op.to_s, op.at_ns);
        break;
    case Kind::Click:
        break;
    }
}

void WaitEvaluator::release_all(std::int64_t now_ns) {
    MidiEvent ev;
    ev.time_ns = now_ns;
    ev.type = MidiEventType::ControlChange;
    ev.data1 = kCcAllNotesOff;
    ev.synthetic = true; // not the player's doing
    apply(ev);
}

void WaitEvaluator::apply(const MidiEvent& ev) {
    sync_transposition();
    const auto later =
        std::upper_bound(inbox_.begin(), inbox_.end(), ev.time_ns,
                         [](std::int64_t t, const MidiEvent& e) { return t < e.time_ns; });
    inbox_.insert(later, ev);
}

void WaitEvaluator::process(const MidiEvent& ev) {
    switch (ev.type) {
    case MidiEventType::NoteOn:
    case MidiEventType::NoteOff: {
        if (ev.data1 > 127) {
            return;
        }
        Key& k = keys_[ev.data1];
        if (k.seen && ev.time_ns < k.last_ns) {
            return; // older than what this key already showed
        }
        k.seen = true;
        k.last_ns = ev.time_ns;
        if (ev.type == MidiEventType::NoteOn && ev.data2 != 0) {
            press(ev.data1, ev.time_ns);
        } else {
            release_key(ev.data1, ev.time_ns, true);
        }
        return;
    }
    case MidiEventType::ControlChange:
        if (ev.data1 == kCcAllNotesOff || ev.data1 == kCcAllSoundOff) {
            for (int key = 0; key < 128; ++key) {
                release_key(key, ev.time_ns, !ev.synthetic);
            }
        }
        return; // the sustain pedal holds nothing
    case MidiEventType::Other:
        return;
    }
}

bool WaitEvaluator::expected_press(int key, double song_s, double early_s) const {
    const double ahead = std::max(early_s, options_.lookahead_s);
    for (std::size_t i = next_; i < chords_.size(); ++i) {
        const Chord& c = chords_[i];
        if (i != next_ && c.start_s > song_s + ahead + kEpsilonS) {
            break;
        }
        for (const ChordKey& k : c.keys) {
            if (k.key == key) {
                return true;
            }
        }
    }
    for (const Sounding& s : sounding_) {
        if (s.key == key && s.end_s > song_s) {
            return true;
        }
    }
    for (const Obligation& o : obligations_) {
        if (o.key == key) {
            return true;
        }
    }
    return false;
}

void WaitEvaluator::press(int key, std::int64_t ns) {
    Key& k = keys_[static_cast<std::size_t>(key)];
    if (k.down) {
        release_key(key, ns, false); // its key-up was lost: this is a new strike
    }
    k.down = true;
    k.press_ns = ns;
    k.press_s = timeline_.song_at(ns);
    k.press_rate = timeline_.rate_at(ns);
    k.consumed = ns < restart_ns_;
    for (Obligation& o : obligations_) {
        if (o.key == key && o.needs_repress) {
            o.needs_repress = false;
            k.consumed = true;
        }
    }
    if (in_dead(dead_, ns)) {
        k.consumed = true; // pressed while paused or before play: press it again after play
        return;
    }
    if (playing_) {
        const double early_s = static_cast<double>(early_ns_) * 1e-9 * k.press_rate;
        if (!expected_press(key, k.press_s, early_s)) {
            int measure = 1;
            if (next_ < chords_.size()) {
                measure = chords_[next_].measure;
            } else if (!chords_.empty()) {
                measure = chords_.back().measure;
            }
            record_wrong(summary_, measure);
            ++passes_.back().result.wrong;
        }
    }
    try_play(ns);
}

void WaitEvaluator::release_key(int key, std::int64_t ns, bool by_player) {
    Key& k = keys_[static_cast<std::size_t>(key)];
    if (!k.down) {
        return;
    }
    k.down = false;
    if (!by_player || !options_.hold_full_length) {
        return;
    }
    const double song_s = timeline_.song_at(ns);
    const double slack_s = static_cast<double>(slack_ns_) * 1e-9 * timeline_.rate_at(ns);
    for (Obligation& o : obligations_) {
        if (o.key != key || o.needs_repress || song_s >= o.end_s - slack_s) {
            continue;
        }
        o.needs_repress = true;
        for (std::size_t i = 0; i < o.result_count; ++i) {
            summary_.notes[o.result + i].released_early = true;
        }
        passes_.back().result.released_early = true;
    }
}

void WaitEvaluator::try_play(std::int64_t ns) {
    while (next_ < chords_.size()) {
        const Chord& c = chords_[next_];
        for (const ChordKey& ck : c.keys) {
            const Key& k = keys_[static_cast<std::size_t>(ck.key)];
            const double early_s = static_cast<double>(early_ns_) * 1e-9 * k.press_rate;
            if (!k.down || k.consumed || k.press_s < c.start_s - early_s - kEpsilonS) {
                return;
            }
        }
        const bool reached = held_ && position_s_ >= c.start_s - kEpsilonS;
        const std::int64_t arrival = reached ? held_since_ns_ : ns;
        for (const ChordKey& ck : c.keys) {
            keys_[static_cast<std::size_t>(ck.key)].consumed = true;
            double end_s = c.start_s;
            const std::size_t first = summary_.notes.size();
            for (std::uint32_t idx : ck.notes) {
                const Note& n = notes_[idx];
                end_s = std::max(end_s, n.start_s + n.duration_s);
                NoteResult r;
                r.note_index = idx;
                r.pass = passes_.back().result.pass;
                r.grade = NoteGrade::Played;
                r.expected_pitch = ck.key;
                r.hand = n.hand;
                r.measure = n.measure;
                r.song_s = n.start_s;
                r.arrival_ns = arrival;
                r.press_ns = ns;
                r.error_ns = std::max<std::int64_t>(0, ns - arrival);
                record(summary_, r);
                ++passes_.back().result.graded;
            }
            sounding_.push_back({ck.key, end_s});
            if (options_.hold_full_length) {
                obligations_.push_back({ck.key, end_s, first, ck.notes.size(), false});
            }
        }
        ++next_;
    }
}

WaitDirective WaitEvaluator::update(std::int64_t now_ns) {
    sync_transposition();
    const std::int64_t edge = now_ns - settle_ns_;
    std::size_t used = 0;
    while (used < inbox_.size() && inbox_[used].time_ns <= edge) {
        process(inbox_[used]);
        ++used;
    }
    inbox_.erase(inbox_.begin(), inbox_.begin() + static_cast<std::ptrdiff_t>(used));
    passes_.report([&](const detail::PassState& p) { return p.end_ns <= edge; });
    timeline_.forget_before(edge - kHistoryNs);
    prune_dead(dead_, edge - kHistoryNs);

    WaitDirective d;
    if (!playing_) {
        return d;
    }
    const bool repress = std::any_of(obligations_.begin(), obligations_.end(),
                                     [](const Obligation& o) { return o.needs_repress; });
    if (repress) {
        d.hold_now = !held_;
        return d;
    }
    if (next_ < chords_.size()) {
        const double start = chords_[next_].start_s;
        if (position_s_ >= start - kEpsilonS) {
            d.hold_now = !held_;
        } else {
            d.release = held_;
            d.arm_hold = true;
            d.hold_at_s = start;
        }
    } else {
        d.release = held_;
    }
    return d;
}

void WaitEvaluator::take_passes(std::vector<PassResult>& out) {
    passes_.take([&](const detail::PassState& p) {
        PassResult r = p.result;
        r.start_s = p.start_s;
        r.end_s = p.end_s;
        r.clean = !p.interrupted && p.start_s <= p.loop_start_s + kEpsilonS && r.missed == 0 &&
                  r.wrong == 0 && !r.released_early;
        out.push_back(r);
    });
}

} // namespace toonval
