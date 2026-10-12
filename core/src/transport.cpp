// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/transport.hpp"

#include <algorithm>
#include <cmath>

namespace toonval {
namespace {

// A stall of many loop lengths in one update stops wrapping after this many; the position stays
// inside the loop.
constexpr int kMaxStepsPerUpdate = 256;

std::int64_t round_ns(double seconds) { return std::llround(seconds * 1e9); }

const Measure* find_measure(const std::vector<Measure>& bars, int number) {
    auto it = std::find_if(bars.begin(), bars.end(),
                           [number](const Measure& m) { return m.number == number; });
    return it == bars.end() ? nullptr : &*it;
}

} // namespace

int snap_speed_percent(int percent) {
    int p = std::clamp(percent, kMinSpeedPercent, kMaxSpeedPercent);
    return (p + kSpeedStepPercent / 2) / kSpeedStepPercent * kSpeedStepPercent;
}

double snap_to_measure(const Song& song, double seconds, SnapMode mode) {
    const std::vector<Measure>& bars = song.measures;
    if (bars.empty()) {
        return std::clamp(seconds, 0.0, song.duration_s);
    }
    auto by_time = [](double s, const Measure& m) { return s < m.start_s; };
    auto after = std::upper_bound(bars.begin(), bars.end(), seconds, by_time);
    double floor_s = after == bars.begin() ? bars.front().start_s : (after - 1)->start_s;
    auto at_or_after = std::lower_bound(bars.begin(), bars.end(), seconds,
                                        [](const Measure& m, double s) { return m.start_s < s; });
    double ceil_s = at_or_after == bars.end() ? song.duration_s : at_or_after->start_s;
    switch (mode) {
    case SnapMode::Floor:
        return floor_s;
    case SnapMode::Ceil:
        return ceil_s;
    case SnapMode::Nearest:
        break;
    }
    return seconds - floor_s < ceil_s - seconds ? floor_s : ceil_s;
}

LoopSpan measures_span(const Song& song, int first, int last) {
    LoopSpan span;
    const Measure* a = find_measure(song.measures, first);
    const Measure* b = find_measure(song.measures, last);
    if (a == nullptr || b == nullptr || b < a) {
        return span;
    }
    span.ok = true;
    span.start_s = a->start_s;
    span.end_s =
        b + 1 == song.measures.data() + song.measures.size() ? song.duration_s : (b + 1)->start_s;
    return span;
}

int AutoSpeedUp::next_percent(int current_percent, bool clean_pass) const {
    if (!clean_pass || current_percent >= max_percent) {
        return current_percent;
    }
    return std::min(max_percent, current_percent + step_percent);
}

std::size_t Transport::set_song(const Song& song) {
    // Ops still queued describe the old song: no consumer can use them once it has the new one.
    const std::size_t dropped = ops_.size();
    ops_.clear();
    const bool was_playing = playing_;
    const double was_at = position_s_;
    duration_s_ = song.duration_s;
    tempo_ = song.tempo;
    signatures_ = song.time_signatures.empty() ? std::vector<TimeSignature>{TimeSignature{}}
                                               : song.time_signatures;
    measures_ = song.measures;
    position_s_ = 0.0;
    playing_ = false;
    held_ = false;
    ended_ = false;
    hold_at_.reset();
    loop_on_ = false;
    loop_a_ = loop_b_ = 0.0;
    loop_count_ = 0;
    clean_passes_ = 0;
    count_in_left_ = 0.0;
    // Tell the consumers that keep clock state (the evaluators) that the clock stopped and went
    // back to the start, so they agree with the transport whatever order the app resets them in.
    if (was_playing) {
        TransportOp op;
        op.kind = TransportOp::Kind::Pause;
        op.at_ns = last_ns_;
        op.to_s = was_at;
        push(op);
    }
    if (was_at != 0.0) {
        TransportOp op;
        op.kind = TransportOp::Kind::Seek;
        op.at_ns = last_ns_;
        op.from_s = was_at;
        op.to_s = 0.0;
        push(op);
    }
    return dropped;
}

TransportState Transport::state() const {
    if (ended_) {
        return TransportState::Ended;
    }
    if (!playing_) {
        return TransportState::Paused;
    }
    if (held_) {
        return TransportState::Held;
    }
    return count_in_left_ > 0.0 ? TransportState::CountIn : TransportState::Playing;
}

void Transport::push(const TransportOp& op) {
    if (op.kind == TransportOp::Kind::Move && !ops_.empty()) {
        TransportOp& last = ops_.back();
        if (last.kind == TransportOp::Kind::Move && last.end_ns == op.at_ns &&
            last.to_s == op.from_s && last.speed_percent == op.speed_percent) {
            last.end_ns = op.end_ns;
            last.to_s = op.to_s;
            return;
        }
    }
    ops_.push_back(op);
}

void Transport::take_ops(std::vector<TransportOp>& out) {
    out.insert(out.end(), ops_.begin(), ops_.end());
    ops_.clear();
}

double Transport::beat_seconds_at(double song_seconds) const {
    auto tick = static_cast<std::int64_t>(std::floor(tempo_.tick_at(song_seconds)));
    auto tempo = std::upper_bound(tempo_.changes.begin(), tempo_.changes.end(), tick,
                                  [](std::int64_t t, const TempoChange& c) { return t < c.tick; });
    const TempoChange& t = tempo == tempo_.changes.begin() ? tempo_.changes.front() : *(tempo - 1);
    auto sig = std::upper_bound(signatures_.begin(), signatures_.end(), tick,
                                [](std::int64_t k, const TimeSignature& s) { return k < s.tick; });
    const TimeSignature& s = sig == signatures_.begin() ? signatures_.front() : *(sig - 1);
    return t.us_per_quarter / 1e6 * 4.0 / s.denominator;
}

int Transport::numerator_at(double song_seconds) const {
    auto tick = static_cast<std::int64_t>(std::floor(tempo_.tick_at(song_seconds)));
    auto sig = std::upper_bound(signatures_.begin(), signatures_.end(), tick,
                                [](std::int64_t k, const TimeSignature& s) { return k < s.tick; });
    return (sig == signatures_.begin() ? signatures_.front() : *(sig - 1)).numerator;
}

// Click ops for the bar beats in [from_s, to_s). A pickup bar (number 0) is the end of a bar, so
// its beats are numbered back from the next bar's last beat.
void Transport::emit_beats(double from_s, double to_s, std::int64_t from_ns, std::int64_t to_ns) {
    if (measures_.empty() || to_s <= from_s) {
        return;
    }
    auto ns_at = [&](double t) {
        return from_ns +
               std::llround((t - from_s) / (to_s - from_s) * static_cast<double>(to_ns - from_ns));
    };
    auto first = std::upper_bound(measures_.begin(), measures_.end(), from_s,
                                  [](double s, const Measure& m) { return s < m.start_s; });
    std::size_t i =
        first == measures_.begin() ? 0 : static_cast<std::size_t>(first - measures_.begin()) - 1;
    for (; i < measures_.size(); ++i) {
        const Measure& m = measures_[i];
        if (m.start_s >= to_s) {
            return;
        }
        const bool has_next = i + 1 < measures_.size();
        const std::int64_t next_tick = has_next ? measures_[i + 1].start_tick : INT64_MAX;
        const double spacing = 4.0 * tempo_.ticks_per_quarter / m.denominator; // ticks per beat
        int beats = m.numerator;
        int label = 1;
        if (m.number == 0 && has_next) {
            const int bar_beats = measures_[i + 1].numerator;
            beats = std::max(1, static_cast<int>(std::llround(
                                    static_cast<double>(next_tick - m.start_tick) / spacing)));
            label = std::max(1, bar_beats - beats + 1);
        }
        for (int k = 0; k < beats; ++k) {
            double tick = static_cast<double>(m.start_tick) + k * spacing;
            if (tick >= static_cast<double>(next_tick)) {
                break;
            }
            double t = tempo_.seconds_at(std::llround(tick));
            if (t < from_s) {
                continue;
            }
            if (t >= to_s) {
                return;
            }
            TransportOp op;
            op.kind = TransportOp::Kind::Click;
            op.at_ns = ns_at(t);
            op.to_s = t;
            op.beat = label + k;
            op.accent = op.beat == 1;
            push(op);
        }
    }
}

void Transport::settle(std::int64_t now_ns) {
    using Kind = TransportOp::Kind;
    if (!started_) {
        started_ = true;
        last_ns_ = now_ns;
        return;
    }
    if (now_ns <= last_ns_) {
        return;
    }
    const std::int64_t start_ns = last_ns_;
    last_ns_ = now_ns;
    if (!playing_ || held_) {
        return;
    }
    const double wall = static_cast<double>(now_ns - start_ns) / 1e9;
    double spent = 0.0; // wall seconds of this update used so far
    auto ns_at = [&](double w) { return start_ns + round_ns(w); };
    const double rate = speed_percent_ / 100.0; // song seconds per wall second

    if (count_in_left_ > 0.0) {
        const double beats_per_s = rate / count_in_beat_s_;
        const double need = count_in_left_ / beats_per_s;
        const double use = std::min(need, wall);
        const double e0 = count_in_total_ - count_in_left_; // beats counted so far
        const double e1 = e0 + use * beats_per_s;
        while (count_in_next_click_ <= count_in_total_ && count_in_next_click_ - 1 <= e1 + 1e-9) {
            TransportOp op;
            op.kind = Kind::Click;
            op.at_ns = ns_at(std::max(0.0, (count_in_next_click_ - 1 - e0) / beats_per_s));
            op.to_s =
                position_s_ - (count_in_total_ - (count_in_next_click_ - 1)) * count_in_beat_s_;
            op.beat = (count_in_next_click_ - 1) % count_in_numerator_ + 1;
            op.accent = op.beat == 1;
            op.count_in = true;
            push(op);
            ++count_in_next_click_;
        }
        if (use >= need) {
            count_in_left_ = 0.0;
        } else {
            count_in_left_ -= use * beats_per_s;
            return;
        }
        spent = use;
    }

    for (int step = 0; step < kMaxStepsPerUpdate; ++step) {
        if (position_s_ >= duration_s_) {
            TransportOp op;
            op.kind = Kind::End;
            op.at_ns = ns_at(spent);
            op.to_s = duration_s_;
            push(op);
            playing_ = false;
            ended_ = true;
            return;
        }
        const double remain = wall - spent;
        if (remain <= 0.0) {
            return;
        }
        const double p = position_s_;
        double limit = p + remain * rate;
        enum class Stop { None, HoldPoint, End, Wrap } stop = Stop::None;
        if (hold_at_ && p < *hold_at_ && *hold_at_ <= limit) {
            limit = *hold_at_;
            stop = Stop::HoldPoint;
        }
        if (duration_s_ <= limit) {
            limit = duration_s_;
            stop = Stop::End;
        }
        if (loop_on_ && p < loop_b_ && loop_b_ <= limit) {
            limit = loop_b_;
            stop = Stop::Wrap;
        }
        const double t1 = stop == Stop::None ? wall : std::min(wall, spent + (limit - p) / rate);

        TransportOp move;
        move.kind = Kind::Move;
        move.at_ns = ns_at(spent);
        move.end_ns = ns_at(t1);
        move.from_s = p;
        move.to_s = limit;
        move.speed_percent = speed_percent_;
        push(move);
        if (metronome_) {
            emit_beats(p, limit, move.at_ns, move.end_ns);
        }
        position_s_ = limit;
        spent = t1;

        TransportOp op;
        op.at_ns = move.end_ns;
        op.to_s = limit;
        switch (stop) {
        case Stop::None:
            return;
        case Stop::HoldPoint:
            held_ = true;
            hold_at_.reset();
            op.kind = Kind::Hold;
            push(op);
            return;
        case Stop::End:
            op.kind = Kind::End;
            push(op);
            playing_ = false;
            ended_ = true;
            return;
        case Stop::Wrap:
            ++loop_count_;
            op.kind = Kind::Wrap;
            op.from_s = loop_b_;
            op.to_s = loop_a_;
            push(op);
            position_s_ = loop_a_;
            break;
        }
    }
}

void Transport::update(std::int64_t now_ns) { settle(now_ns); }

void Transport::play(std::int64_t now_ns) {
    settle(now_ns);
    if (playing_) {
        return;
    }
    if (ended_ || position_s_ >= duration_s_) {
        position_s_ = 0.0;
    }
    ended_ = false;
    playing_ = true;
    TransportOp op;
    op.kind = TransportOp::Kind::Play;
    op.at_ns = now_ns;
    op.to_s = position_s_;
    push(op);
    count_in_left_ = 0.0;
    if (count_in_beats_ > 0) {
        count_in_total_ = count_in_beats_;
        count_in_left_ = count_in_beats_;
        count_in_beat_s_ = beat_seconds_at(position_s_);
        count_in_numerator_ = std::max(1, numerator_at(position_s_));
        count_in_next_click_ = 1;
        // The first beat sounds now.
        TransportOp click;
        click.kind = TransportOp::Kind::Click;
        click.at_ns = now_ns;
        click.to_s = position_s_ - count_in_total_ * count_in_beat_s_;
        click.beat = 1;
        click.accent = true;
        click.count_in = true;
        push(click);
        count_in_next_click_ = 2;
    }
}

void Transport::pause(std::int64_t now_ns) {
    settle(now_ns);
    if (!playing_) {
        return;
    }
    playing_ = false;
    count_in_left_ = 0.0;
    TransportOp op;
    op.kind = TransportOp::Kind::Pause;
    op.at_ns = now_ns;
    op.to_s = position_s_;
    push(op);
}

void Transport::seek(std::int64_t now_ns, double song_seconds) {
    settle(now_ns);
    if (std::isnan(song_seconds)) {
        return;
    }
    TransportOp op;
    op.kind = TransportOp::Kind::Seek;
    op.at_ns = now_ns;
    op.from_s = position_s_;
    position_s_ = std::clamp(song_seconds, 0.0, duration_s_);
    op.to_s = position_s_;
    push(op);
    ended_ = false;
    count_in_left_ = 0.0;
    hold_at_.reset(); // armed for the old position; Wait Mode arms the next one on its update
}

int Transport::set_speed_percent(std::int64_t now_ns, int percent) {
    settle(now_ns);
    speed_percent_ = snap_speed_percent(percent);
    return speed_percent_;
}

void Transport::hold(std::int64_t now_ns) {
    settle(now_ns);
    if (held_) {
        return;
    }
    held_ = true;
    TransportOp op;
    op.kind = TransportOp::Kind::Hold;
    op.at_ns = now_ns;
    op.to_s = position_s_;
    push(op);
}

void Transport::release(std::int64_t now_ns) {
    settle(now_ns);
    if (!held_) {
        return;
    }
    held_ = false;
    TransportOp op;
    op.kind = TransportOp::Kind::Release;
    op.at_ns = now_ns;
    op.to_s = position_s_;
    push(op);
}

void Transport::hold_at(std::int64_t now_ns, double song_seconds) {
    settle(now_ns);
    if (song_seconds > position_s_) {
        hold_at_ = song_seconds;
    }
}

bool Transport::set_loop(std::int64_t now_ns, double start_s, double end_s) {
    settle(now_ns);
    if (std::isnan(start_s) || std::isnan(end_s)) {
        return false;
    }
    if (start_s > end_s) {
        std::swap(start_s, end_s);
    }
    start_s = std::clamp(start_s, 0.0, duration_s_);
    end_s = std::clamp(end_s, 0.0, duration_s_);
    if (end_s - start_s < kMinLoopSeconds) {
        return false;
    }
    loop_a_ = start_s;
    loop_b_ = end_s;
    loop_on_ = true;
    loop_count_ = 0;
    clean_passes_ = 0;
    return true;
}

void Transport::set_loop_enabled(std::int64_t now_ns, bool enabled) {
    settle(now_ns);
    loop_on_ = enabled && loop_b_ > loop_a_;
}

void Transport::clear_loop(std::int64_t now_ns) {
    settle(now_ns);
    loop_on_ = false;
    loop_a_ = loop_b_ = 0.0;
    loop_count_ = 0;
    clean_passes_ = 0;
}

void Transport::report_pass(std::int64_t now_ns, bool clean) {
    settle(now_ns);
    if (!clean) {
        return;
    }
    ++clean_passes_;
    if (auto_.enabled) {
        speed_percent_ = snap_speed_percent(auto_.next_percent(speed_percent_, true));
    }
}

void Transport::set_auto_speed_up(std::int64_t now_ns, const AutoSpeedUp& policy) {
    settle(now_ns);
    auto_ = policy;
    if (auto_.enabled) {
        speed_percent_ = snap_speed_percent(auto_.start_percent);
    }
}

void Transport::set_count_in_beats(int beats) { count_in_beats_ = std::max(0, beats); }

void Transport::set_metronome(bool enabled) { metronome_ = enabled; }

} // namespace toonval
