// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/midi_writer.hpp"

#include <algorithm>
#include <limits>
#include <tuple>

namespace toonval {
namespace {

constexpr std::uint32_t kNoMeta = std::numeric_limits<std::uint32_t>::max();
constexpr std::int64_t kMaxDelta = 0x0FFFFFFF; // largest variable-length quantity
constexpr std::int64_t kMaxTracks = 65535;

// Order of events that share a tick, lowest first.
constexpr std::int8_t kOrderName = -3;
constexpr std::int8_t kOrderTempo = -2;
constexpr std::int8_t kOrderSignature = -1;
constexpr std::int8_t kOrderNoteOff = 0;
constexpr std::int8_t kOrderEvent = 1;
constexpr std::int8_t kOrderNoteOn = 2;

struct Meta {
    std::uint8_t type = 0;
    std::vector<std::uint8_t> payload;
};

// One event of a track before it is encoded: a channel message of 2 or 3 bytes, or a meta event
// (index into the meta list).
struct Item {
    std::int64_t tick = 0;
    std::int8_t order = 0;
    std::uint8_t status = 0;
    std::uint8_t d1 = 0;
    std::uint8_t d2 = 0;
    std::uint8_t size = 0; // bytes of the channel message, status included
    std::uint32_t meta = kNoMeta;
};

struct Failure {
    bool failed = false;
    std::string message;
    void set(const char* what) {
        if (!failed) {
            failed = true;
            message = what;
        }
    }
};

void put_vlq(std::vector<std::uint8_t>& out, std::uint32_t value) {
    std::uint8_t buf[5];
    int n = 0;
    buf[n++] = static_cast<std::uint8_t>(value & 0x7F);
    while ((value >>= 7) != 0) {
        buf[n++] = static_cast<std::uint8_t>(0x80 | (value & 0x7F));
    }
    while (n > 0) {
        out.push_back(buf[--n]);
    }
}

void put_be(std::vector<std::uint8_t>& out, std::uint32_t value, int bytes) {
    for (int i = bytes - 1; i >= 0; --i) {
        out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    }
}

void put_chunk(std::vector<std::uint8_t>& out, const char* id,
               const std::vector<std::uint8_t>& body) {
    out.insert(out.end(), id, id + 4);
    put_be(out, static_cast<std::uint32_t>(body.size()), 4);
    out.insert(out.end(), body.begin(), body.end());
}

// What one destination track collects before it is encoded.
struct TrackBuild {
    std::vector<Item> items;
    std::vector<Meta> metas;

    void add_meta(std::int64_t tick, std::int8_t order, std::uint8_t type,
                  std::vector<std::uint8_t> payload) {
        Item it;
        it.tick = tick;
        it.order = order;
        it.meta = static_cast<std::uint32_t>(metas.size());
        metas.push_back({type, std::move(payload)});
        items.push_back(it);
    }
    void add_text(std::uint8_t type, const std::string& text) {
        add_meta(0, kOrderName, type, std::vector<std::uint8_t>(text.begin(), text.end()));
    }
    void add_channel(std::int64_t tick, std::int8_t order, std::uint8_t status, std::uint8_t d1,
                     std::uint8_t d2, std::uint8_t size) {
        Item it;
        it.tick = tick;
        it.order = order;
        it.status = status;
        it.d1 = d1;
        it.d2 = d2;
        it.size = size;
        items.push_back(it);
    }
};

// A note ready to write.
struct WNote {
    std::int64_t start = 0;
    std::int64_t end = 0;
    std::uint16_t track = 0;
    std::uint8_t channel = 0;
    std::uint8_t pitch = 0;
    std::uint8_t velocity = 0;
    bool keep = true;
};

std::uint8_t clamp7(int v) { return static_cast<std::uint8_t>(std::clamp(v, 0, 127)); }

// Names of one group of parts (a source track in format 1, the whole song in format 0): the first
// part names the track, and a channel whose part is named differently gets a channel prefix.
void add_names(TrackBuild& tb, const std::vector<const Part*>& parts) {
    if (parts.empty()) {
        return;
    }
    const Part& first = *parts.front();
    if (!first.name.empty()) {
        tb.add_text(0x03, first.name);
    }
    if (!first.instrument_name.empty()) {
        tb.add_text(0x04, first.instrument_name);
    }
    std::vector<bool> seen(16, false);
    seen[first.channel & 0x0F] = true;
    for (std::size_t i = 1; i < parts.size(); ++i) {
        const Part& p = *parts[i];
        if (p.channel > 15 || seen[p.channel]) {
            continue;
        }
        seen[p.channel] = true;
        const bool name = !p.name.empty() && p.name != first.name;
        const bool inst = !p.instrument_name.empty() && p.instrument_name != first.instrument_name;
        if (!name && !inst) {
            continue;
        }
        tb.add_meta(0, kOrderName, 0x20, {p.channel});
        if (name) {
            tb.add_text(0x03, p.name);
        }
        if (inst) {
            tb.add_text(0x04, p.instrument_name);
        }
    }
}

bool encode_track(TrackBuild& tb, bool running_status, std::vector<std::uint8_t>& out,
                  Failure& fail) {
    std::stable_sort(tb.items.begin(), tb.items.end(), [](const Item& a, const Item& b) {
        return a.tick != b.tick ? a.tick < b.tick : a.order < b.order;
    });
    std::int64_t prev = 0;
    std::uint8_t running = 0;
    for (const Item& it : tb.items) {
        const std::int64_t delta = it.tick - prev;
        if (delta > kMaxDelta) {
            fail.set("a gap between two events is too long for a MIDI delta time");
            return false;
        }
        prev = it.tick;
        put_vlq(out, static_cast<std::uint32_t>(delta));
        if (it.meta != kNoMeta) {
            const Meta& m = tb.metas[it.meta];
            out.push_back(0xFF);
            out.push_back(m.type);
            put_vlq(out, static_cast<std::uint32_t>(m.payload.size()));
            out.insert(out.end(), m.payload.begin(), m.payload.end());
            running = 0;
            continue;
        }
        if (!(running_status && running == it.status)) {
            out.push_back(it.status);
        }
        running = it.status;
        out.push_back(it.d1);
        if (it.size == 3) {
            out.push_back(it.d2);
        }
    }
    out.push_back(0x00); // End of Track, at the tick of the last event
    out.push_back(0xFF);
    out.push_back(0x2F);
    out.push_back(0x00);
    return true;
}

bool power_of_two_up_to_64(int v) { return v >= 1 && v <= 64 && (v & (v - 1)) == 0; }

MidiWriteResult write(const Song& song, const MidiWriteOptions& options) {
    MidiWriteResult result;
    Failure fail;
    auto done = [&]() {
        result.ok = false;
        result.message = fail.message;
        result.bytes.clear();
        return result;
    };

    if (options.format != 0 && options.format != 1) {
        fail.set("only MIDI file formats 0 and 1 can be written");
        return done();
    }
    const int ppq = song.tempo.ticks_per_quarter;
    if (ppq < 1 || ppq > 32767) {
        fail.set("ticks per quarter note must be 1..32767");
        return done();
    }
    for (const TempoChange& c : song.tempo.changes) {
        if (c.us_per_quarter < 1 || c.us_per_quarter > 0xFFFFFF) {
            fail.set("a tempo must be 1..16777215 microseconds per quarter note");
            return done();
        }
    }
    for (const TimeSignature& t : song.time_signatures) {
        if (t.numerator == 0 || !power_of_two_up_to_64(t.denominator)) {
            fail.set("a time signature needs a numerator above 0 and a power-of-two denominator up "
                     "to 64");
            return done();
        }
    }

    // Notes: repair overlaps per track, channel and pitch.
    std::vector<WNote> notes;
    notes.reserve(song.notes.size());
    for (const Note& n : song.notes) {
        if (n.pitch > 127 || n.channel > 15 || n.start_tick < 0 || n.end_tick <= n.start_tick) {
            ++result.notes_skipped;
            continue;
        }
        notes.push_back({n.start_tick, n.end_tick, n.track, n.channel, n.pitch,
                         std::clamp<std::uint8_t>(n.velocity, 1, 127), true});
    }
    {
        std::vector<std::size_t> order(notes.size());
        for (std::size_t i = 0; i < order.size(); ++i) {
            order[i] = i;
        }
        std::sort(order.begin(), order.end(), [&](std::size_t x, std::size_t y) {
            const WNote& a = notes[x];
            const WNote& b = notes[y];
            return std::tie(a.track, a.channel, a.pitch, a.start, b.end, x) <
                   std::tie(b.track, b.channel, b.pitch, b.start, a.end, y);
        });
        std::size_t cur = order.empty() ? 0 : order[0];
        for (std::size_t k = 1; k < order.size(); ++k) {
            WNote& c = notes[cur];
            WNote& n = notes[order[k]];
            if (c.track != n.track || c.channel != n.channel || c.pitch != n.pitch) {
                cur = order[k];
            } else if (n.start == c.start) {
                n.keep = false; // a second note-on on the same tick is merged into the first
                ++result.notes_skipped;
            } else if (n.start < c.end) {
                c.end = n.start;
                ++result.notes_shortened;
                cur = order[k];
            } else {
                cur = order[k];
            }
        }
    }

    // Which source tracks exist.
    std::int64_t max_track = 0;
    for (const WNote& n : notes) {
        max_track = std::max<std::int64_t>(max_track, n.track);
    }
    for (const ChannelEvent& e : song.events) {
        max_track = std::max<std::int64_t>(max_track, e.track);
    }
    for (const Part& p : song.parts) {
        max_track = std::max<std::int64_t>(max_track, p.track);
    }
    if (max_track + 1 > kMaxTracks) {
        fail.set("a track number is too high for the MIDI header");
        return done();
    }
    const std::size_t track_count =
        options.format == 0 ? 1 : static_cast<std::size_t>(max_track) + 1;
    std::vector<TrackBuild> builds(track_count);
    auto dest = [&](std::uint16_t source) -> TrackBuild& {
        return options.format == 0 ? builds[0] : builds[source];
    };

    // Names.
    if (options.format == 0) {
        std::vector<const Part*> all;
        for (const Part& p : song.parts) {
            all.push_back(&p);
        }
        add_names(builds[0], all);
    } else {
        std::vector<std::vector<const Part*>> by_track(track_count);
        for (const Part& p : song.parts) {
            by_track[p.track].push_back(&p);
        }
        for (std::size_t t = 0; t < track_count; ++t) {
            add_names(builds[t], by_track[t]);
        }
    }

    // Tempo map and time signatures go in the first track.
    for (const TempoChange& c : song.tempo.changes) {
        std::vector<std::uint8_t> p;
        put_be(p, c.us_per_quarter, 3);
        builds[0].add_meta(std::max<std::int64_t>(c.tick, 0), kOrderTempo, 0x51, std::move(p));
    }
    for (const TimeSignature& t : song.time_signatures) {
        std::uint8_t power = 0;
        while ((1 << power) < t.denominator) {
            ++power;
        }
        builds[0].add_meta(std::max<std::int64_t>(t.tick, 0), kOrderSignature, 0x58,
                           {t.numerator, power, 24, 8});
    }

    for (const WNote& n : notes) {
        if (!n.keep) {
            continue;
        }
        TrackBuild& tb = dest(n.track);
        tb.add_channel(n.start, kOrderNoteOn, static_cast<std::uint8_t>(0x90 | n.channel), n.pitch,
                       n.velocity, 3);
        tb.add_channel(n.end, kOrderNoteOff, static_cast<std::uint8_t>(0x80 | n.channel), n.pitch,
                       64, 3);
        ++result.notes_written;
    }
    for (const ChannelEvent& e : song.events) {
        if (e.channel > 15 || (e.kind == ChannelEventKind::Control && e.number > 127)) {
            ++result.events_skipped;
            continue;
        }
        TrackBuild& tb = dest(e.track);
        const std::int64_t tick = std::max<std::int64_t>(e.tick, 0);
        switch (e.kind) {
        case ChannelEventKind::Program:
            tb.add_channel(tick, kOrderEvent, static_cast<std::uint8_t>(0xC0 | e.channel),
                           clamp7(e.value), 0, 2);
            break;
        case ChannelEventKind::Control:
            tb.add_channel(tick, kOrderEvent, static_cast<std::uint8_t>(0xB0 | e.channel), e.number,
                           clamp7(e.value), 3);
            break;
        case ChannelEventKind::PitchBend: {
            const int bend = std::min<int>(e.value, 16383);
            tb.add_channel(tick, kOrderEvent, static_cast<std::uint8_t>(0xE0 | e.channel),
                           static_cast<std::uint8_t>(bend & 0x7F),
                           static_cast<std::uint8_t>(bend >> 7), 3);
            break;
        }
        }
        ++result.events_written;
    }

    std::vector<std::uint8_t> body;
    put_be(body, static_cast<std::uint32_t>(options.format), 2);
    put_be(body, static_cast<std::uint32_t>(track_count), 2);
    put_be(body, static_cast<std::uint32_t>(ppq), 2);
    put_chunk(result.bytes, "MThd", body);
    for (TrackBuild& tb : builds) {
        std::vector<std::uint8_t> track;
        if (!encode_track(tb, options.running_status, track, fail)) {
            return done();
        }
        if (track.size() > std::numeric_limits<std::uint32_t>::max()) {
            fail.set("a track is larger than a MIDI chunk can hold");
            return done();
        }
        put_chunk(result.bytes, "MTrk", track);
    }
    result.ok = true;
    return result;
}

} // namespace

MidiWriteResult write_midi(const Song& song, const MidiWriteOptions& options) {
    try {
        return write(song, options);
    } catch (...) {
        MidiWriteResult r;
        r.message = "out of memory"; // may itself throw, but then there is nothing left to say
        return r;
    }
}

} // namespace toonval
