// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/recorder.hpp"

#include "toonval/song_builder.hpp"

#include <algorithm>
#include <cmath>

namespace toonval {
namespace {

// Events held at once in the reorder buffer, whatever their times: a flood cannot grow it.
constexpr std::size_t kMaxBuffered = 4096;
constexpr std::uint16_t kRecordTrack = 1;

// Ticks for a time since the start of the song, rounded to the nearest, in integer arithmetic so
// hours at the largest tick rate neither lose precision nor overflow.
std::int64_t ticks_from_ns(std::int64_t dt_ns, std::int64_t ns_per_quarter, int ppq) {
    if (dt_ns <= 0) {
        return 0;
    }
    const std::int64_t whole = dt_ns / ns_per_quarter;
    const std::int64_t rest = dt_ns % ns_per_quarter;
    return whole * ppq + (rest * ppq + ns_per_quarter / 2) / ns_per_quarter;
}

} // namespace

Recorder::Recorder(const RecorderOptions& options) : options_(options) {
    options_.ticks_per_quarter = std::clamp(options_.ticks_per_quarter, 1, 32767);
    options_.reorder_window_ns = std::max<std::int64_t>(options_.reorder_window_ns, 0);
    if (!(options_.lead_s >= 0.0)) {
        options_.lead_s = 0.0;
    }
    reset();
}

void Recorder::reset() {
    pending_ = {};
    seq_ = 0;
    any_event_ = false;
    max_seen_ns_ = 0;
    handled_ns_ = 0;
    for (auto& channel : sounding_) {
        channel.fill(Sounding{});
    }
    notes_.clear();
    controls_.clear();
    pre_note_.assign(16 * 128, -1);
    started_ = false;
    first_note_ns_ = 0;
    truncated_ = false;
}

void Recorder::feed(const MidiEvent& ev) {
    if (ev.type == MidiEventType::Other) {
        return;
    }
    std::int64_t t = ev.time_ns;
    if (any_event_) {
        t = std::max(t, handled_ns_); // too late to sort in: handled at the latest time so far
        max_seen_ns_ = std::max(max_seen_ns_, t);
    } else {
        any_event_ = true;
        handled_ns_ = t;
        max_seen_ns_ = t;
    }
    pending_.push({t, seq_++, ev});
    while (!pending_.empty() &&
           (pending_.size() > kMaxBuffered ||
            pending_.top().time_ns <= max_seen_ns_ - options_.reorder_window_ns)) {
        const Pending p = pending_.top();
        pending_.pop();
        handle(p.ev, p.time_ns);
    }
}

void Recorder::handle(const MidiEvent& ev, std::int64_t time_ns) {
    handled_ns_ = std::max(handled_ns_, time_ns);
    if (ev.channel > 15 || ev.data1 > 127) {
        return;
    }
    switch (ev.type) {
    case MidiEventType::NoteOn:
        if (ev.data2 == 0) {
            end_note(ev.channel, ev.data1, time_ns);
            return;
        }
        if (!started_) {
            started_ = true;
            first_note_ns_ = time_ns;
            // Hand over the controllers set before the first note, at the song's start.
            pre_note_time_ns_ =
                time_ns - static_cast<std::int64_t>(std::llround(options_.lead_s * 1e9));
            for (std::size_t i = 0; i < pre_note_.size(); ++i) {
                if (pre_note_[i] >= 0) {
                    add_control(pre_note_time_ns_, static_cast<int>(i / 128),
                                static_cast<int>(i % 128), pre_note_[i]);
                }
            }
        }
        {
            Sounding& s = sounding_[ev.channel][ev.data1];
            if (s.active && s.start_ns == time_ns) {
                return; // the same key struck twice at once
            }
            if (s.active) {
                end_note(ev.channel, ev.data1, time_ns);
            }
            s = {true, std::min<std::uint8_t>(ev.data2, 127), time_ns};
        }
        return;
    case MidiEventType::NoteOff:
        end_note(ev.channel, ev.data1, time_ns);
        return;
    case MidiEventType::ControlChange:
        if (ev.data1 == kCcAllSoundOff || ev.data1 == kCcAllNotesOff) {
            end_all(time_ns);
            return;
        }
        add_control(time_ns, ev.channel, ev.data1, std::min<int>(ev.data2, 127));
        return;
    case MidiEventType::Other:
        return;
    }
}

void Recorder::end_note(int channel, int pitch, std::int64_t time_ns) {
    Sounding& s = sounding_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(pitch)];
    if (!s.active) {
        return;
    }
    s.active = false;
    if (notes_.size() >= options_.max_notes) {
        truncated_ = true;
        return;
    }
    notes_.push_back({s.start_ns, std::max(time_ns, s.start_ns), static_cast<std::uint8_t>(channel),
                      static_cast<std::uint8_t>(pitch), s.velocity});
}

void Recorder::end_all(std::int64_t time_ns) {
    for (int ch = 0; ch < 16; ++ch) {
        for (int pitch = 0; pitch < 128; ++pitch) {
            end_note(ch, pitch, time_ns);
        }
    }
}

void Recorder::add_control(std::int64_t time_ns, int channel, int number, int value) {
    if (!started_) {
        pre_note_[static_cast<std::size_t>(channel * 128 + number)] =
            static_cast<std::int16_t>(value);
        return;
    }
    if (controls_.size() >= options_.max_events) {
        truncated_ = true;
        return;
    }
    controls_.push_back({time_ns, static_cast<std::uint8_t>(channel),
                         static_cast<std::uint8_t>(number), static_cast<std::uint8_t>(value)});
}

RecordedSong Recorder::finish(std::int64_t stop_time_ns) {
    while (!pending_.empty()) {
        const Pending p = pending_.top();
        pending_.pop();
        handle(p.ev, p.time_ns);
    }
    // Notes still sounding end at the stop time (never before they began).
    const std::int64_t stop = std::max(stop_time_ns, handled_ns_);
    for (int ch = 0; ch < 16; ++ch) {
        for (int pitch = 0; pitch < 128; ++pitch) {
            end_note(ch, pitch, stop);
        }
    }

    RecordedSong result;
    if (notes_.empty()) {
        reset();
        return result;
    }
    const std::int64_t origin = pre_note_time_ns_;
    const std::uint32_t us = us_per_quarter_from_bpm(options_.bpm);
    const std::int64_t ns_per_quarter = std::int64_t{us} * 1000;
    const int ppq = options_.ticks_per_quarter;

    Song& song = result.song;
    song.tempo = make_tempo_map(ppq, options_.bpm);
    song.notes.reserve(notes_.size());
    for (const RawNote& r : notes_) {
        Note n;
        n.start_tick = ticks_from_ns(r.start_ns - origin, ns_per_quarter, ppq);
        n.end_tick =
            std::max(ticks_from_ns(r.end_ns - origin, ns_per_quarter, ppq), n.start_tick + 1);
        n.pitch = r.pitch;
        n.velocity = r.velocity;
        n.track = kRecordTrack;
        n.channel = r.channel;
        song.notes.push_back(n);
    }
    song.events.reserve(controls_.size());
    for (const RawControl& c : controls_) {
        ChannelEvent e;
        e.tick = ticks_from_ns(c.time_ns - origin, ns_per_quarter, ppq);
        e.track = kRecordTrack;
        e.channel = c.channel;
        e.kind = ChannelEventKind::Control;
        e.number = c.number;
        e.value = c.value;
        song.events.push_back(e);
    }
    std::array<bool, 16> used{};
    for (const RawNote& r : notes_) {
        used[r.channel] = true;
    }
    for (std::uint8_t ch = 0; ch < 16; ++ch) {
        if (used[ch]) {
            Part p;
            p.track = kRecordTrack;
            p.channel = ch;
            p.name = options_.part_name;
            song.parts.push_back(std::move(p));
        }
    }
    const bool complete = finish_song(song);

    result.has_notes = true;
    result.notes = static_cast<int>(notes_.size());
    result.events = static_cast<int>(controls_.size());
    result.truncated = truncated_ || !complete;
    reset();
    return result;
}

} // namespace toonval
