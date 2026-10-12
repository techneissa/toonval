// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/midi_file.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <new>

namespace toonval {
namespace {

constexpr std::uint8_t kDrumChannel = 9;
constexpr std::size_t kMaxMeasures = 100'000;

struct Failure {
    MidiFileError error = MidiFileError::None;
    std::size_t offset = 0;
    const char* what = "";
};

// Bounds-checked big-endian reader over [pos, end).
class Reader {
  public:
    Reader(const std::uint8_t* data, std::size_t pos, std::size_t end)
        : data_(data), pos_(pos), end_(end) {}

    std::size_t pos() const { return pos_; }
    std::size_t left() const { return end_ - pos_; }
    bool done() const { return pos_ >= end_; }

    bool u8(std::uint8_t& out) {
        if (pos_ >= end_) {
            return false;
        }
        out = data_[pos_++];
        return true;
    }
    bool be(std::uint32_t& out, int bytes) {
        if (left() < static_cast<std::size_t>(bytes)) {
            return false;
        }
        out = 0;
        for (int i = 0; i < bytes; ++i) {
            out = (out << 8) | data_[pos_++];
        }
        return true;
    }
    // SMF variable-length quantity: at most 4 bytes, 28 bits.
    bool vlq(std::uint32_t& out) {
        out = 0;
        for (int i = 0; i < 4; ++i) {
            std::uint8_t b = 0;
            if (!u8(b)) {
                return false;
            }
            out = (out << 7) | (b & 0x7Fu);
            if ((b & 0x80) == 0) {
                return true;
            }
        }
        return false;
    }
    bool skip(std::size_t n) {
        if (left() < n) {
            return false;
        }
        pos_ += n;
        return true;
    }
    const std::uint8_t* here() const { return data_ + pos_; }

  private:
    const std::uint8_t* data_;
    std::size_t pos_;
    std::size_t end_;
};

struct RawNote {
    std::int64_t start_tick;
    std::int64_t end_tick;
    std::uint16_t track;
    std::uint8_t channel;
    std::uint8_t pitch;
    std::uint8_t velocity;
};

struct RawEvent {
    std::int64_t tick;
    std::uint16_t track;
    std::uint8_t channel;
    ChannelEventKind kind;
    std::uint8_t number;
    std::uint16_t value;
};

struct TempoEvent {
    std::int64_t tick;
    std::uint32_t us_per_quarter;
};

struct ChannelInfo {
    std::string name; // from a channel prefix
    std::string instrument;
    std::uint8_t program = 0;
    bool has_program = false;
};

struct TrackInfo {
    std::string name;
    std::string instrument;
    std::array<ChannelInfo, 16> channels;
};

// The note sounding on one channel and pitch of the track being read.
struct Sounding {
    bool active = false;
    // This note-on ended an earlier note of the same pitch at the same tick, so a note-off at
    // that tick belongs to the earlier note.
    bool ended_earlier = false;
    std::uint8_t velocity = 0;
    std::int64_t start_tick = 0;
};

struct Parsed {
    std::vector<RawNote> notes;
    std::vector<RawEvent> events;
    std::vector<TempoEvent> tempos;
    std::vector<TimeSignature> time_signatures;
    std::vector<TrackInfo> tracks;
    MidiLoadStats stats;
};

class TrackParser {
  public:
    TrackParser(Parsed& out, std::uint16_t track) : out_(out), track_(track) {}

    // `cut_off` is set when the chunk's declared length ran past the end of the file: an event
    // the data ends in the middle of is dropped and the track ends at the last whole event.
    bool parse(Reader& r, Failure& fail, bool cut_off = false) {
        std::int64_t tick = 0;
        std::int64_t whole_tick = 0; // tick of the last whole event
        std::uint8_t running = 0;
        auto ends_here = [&](const char* what) {
            if (!cut_off) {
                return corrupt(fail, r, what);
            }
            end_track(whole_tick);
            return true;
        };
        while (!r.done()) {
            std::uint32_t delta = 0;
            if (!r.vlq(delta)) {
                return r.done() ? ends_here("bad delta time") : corrupt(fail, r, "bad delta time");
            }
            tick += delta;
            std::uint8_t status = 0;
            if (!r.u8(status)) {
                return ends_here("event cut off by the end of the track");
            }
            if (status < 0x80) {
                if (running == 0) {
                    return corrupt(fail, r, "data byte with no running status");
                }
                if (!channel_event(r, running, status, tick)) {
                    return r.done() ? ends_here("bad channel message")
                                    : corrupt(fail, r, "bad channel message");
                }
                whole_tick = tick;
                continue;
            }
            if (status < 0xF0) {
                running = status;
                std::uint8_t d1 = 0;
                if (!r.u8(d1) || !channel_event(r, status, d1, tick)) {
                    return r.done() ? ends_here("bad channel message")
                                    : corrupt(fail, r, "bad channel message");
                }
                whole_tick = tick;
                continue;
            }
            running = 0; // SysEx and meta events cancel running status
            if (status == 0xF0 || status == 0xF7) {
                std::uint32_t len = 0;
                if (!r.vlq(len) || !r.skip(len)) {
                    return ends_here("SysEx runs past the end of the track");
                }
                whole_tick = tick;
                continue;
            }
            if (status != 0xFF) {
                return corrupt(fail, r, "system message in a track");
            }
            std::uint8_t type = 0;
            std::uint32_t len = 0;
            if (!r.u8(type) || !r.vlq(len) || r.left() < len) {
                return ends_here("meta event runs past the end of the track");
            }
            const std::uint8_t* p = r.here();
            r.skip(len);
            if (type == 0x2F) {
                break; // End of Track; anything after it in the chunk is ignored
            }
            meta_event(type, p, len, tick);
            whole_tick = tick;
        }
        end_track(tick);
        return true;
    }

  private:
    bool corrupt(Failure& fail, const Reader& r, const char* what) {
        fail = {MidiFileError::Corrupt, r.pos(), what};
        return false;
    }

    // `d1` is the first data byte, already read.
    bool channel_event(Reader& r, std::uint8_t status, std::uint8_t d1, std::int64_t tick) {
        prefix_ = -1;
        std::uint8_t kind = status & 0xF0;
        auto ch = static_cast<std::uint8_t>(status & 0x0F);
        if (d1 >= 0x80) {
            return false;
        }
        if (kind == 0xC0 || kind == 0xD0) {
            if (kind == 0xC0) {
                ChannelInfo& info = out_.tracks[track_].channels[ch];
                if (!info.has_program) {
                    info.program = d1;
                    info.has_program = true;
                }
                out_.events.push_back({tick, track_, ch, ChannelEventKind::Program, 0, d1});
            }
            return true;
        }
        std::uint8_t d2 = 0;
        if (!r.u8(d2) || d2 >= 0x80) {
            return false;
        }
        if (kind == 0x90 && d2 > 0) {
            note_on(ch, d1, d2, tick);
        } else if (kind == 0x80 || kind == 0x90) {
            note_off(ch, d1, tick);
        } else if (kind == 0xB0) {
            // Control changes, the sustain pedal (CC64) included, never touch note lengths.
            out_.events.push_back({tick, track_, ch, ChannelEventKind::Control, d1, d2});
        } else if (kind == 0xE0) {
            auto bend = static_cast<std::uint16_t>((d2 << 7) | d1);
            out_.events.push_back({tick, track_, ch, ChannelEventKind::PitchBend, 0, bend});
        }
        return true;
    }

    void note_on(std::uint8_t ch, std::uint8_t pitch, std::uint8_t velocity, std::int64_t tick) {
        Sounding& s = sounding_[ch][pitch];
        if (s.active && s.start_tick == tick) {
            ++out_.stats.duplicates_merged;
            return;
        }
        bool ended_earlier = false;
        if (s.active) {
            emit(ch, pitch, s, tick);
            ++out_.stats.overlaps_split;
            ended_earlier = true;
        }
        s = {true, ended_earlier, velocity, tick};
    }

    void note_off(std::uint8_t ch, std::uint8_t pitch, std::int64_t tick) {
        Sounding& s = sounding_[ch][pitch];
        if (!s.active) {
            return; // an extra note-off
        }
        if (s.start_tick == tick) {
            if (s.ended_earlier) {
                s.ended_earlier = false; // this off was for the note that just ended
                return;
            }
            ++out_.stats.zero_length_dropped;
            s.active = false;
            return;
        }
        emit(ch, pitch, s, tick);
        s.active = false;
    }

    void emit(std::uint8_t ch, std::uint8_t pitch, const Sounding& s, std::int64_t end_tick) {
        out_.notes.push_back({s.start_tick, end_tick, track_, ch, pitch, s.velocity});
    }

    void end_track(std::int64_t tick) {
        for (std::uint8_t ch = 0; ch < 16; ++ch) {
            for (std::uint8_t pitch = 0; pitch < 128; ++pitch) {
                Sounding& s = sounding_[ch][pitch];
                if (!s.active) {
                    continue;
                }
                if (tick > s.start_tick) {
                    emit(ch, pitch, s, tick);
                    ++out_.stats.unterminated_notes;
                } else {
                    ++out_.stats.zero_length_dropped;
                }
                s.active = false;
            }
        }
    }

    void meta_event(std::uint8_t type, const std::uint8_t* p, std::uint32_t len,
                    std::int64_t tick) {
        TrackInfo& info = out_.tracks[track_];
        switch (type) {
        case 0x03:
        case 0x04: {
            std::string text(reinterpret_cast<const char*>(p), len);
            std::string* target = type == 0x03 ? &info.name : &info.instrument;
            if (prefix_ >= 0) {
                ChannelInfo& c = info.channels[static_cast<std::size_t>(prefix_)];
                target = type == 0x03 ? &c.name : &c.instrument;
            }
            if (target->empty()) {
                *target = std::move(text);
            }
            break;
        }
        case 0x20:
            if (len >= 1 && p[0] < 16) {
                prefix_ = p[0];
            }
            break;
        case 0x51:
            if (len >= 3) {
                std::uint32_t us = (std::uint32_t{p[0]} << 16) | (std::uint32_t{p[1]} << 8) | p[2];
                if (us > 0) {
                    out_.tempos.push_back({tick, us});
                }
            }
            break;
        case 0x58:
            // Denominators up to 1/64; anything else is not a real time signature.
            if (len >= 2 && p[0] > 0 && p[1] <= 6) {
                out_.time_signatures.push_back({tick, p[0], static_cast<std::uint8_t>(1u << p[1])});
            }
            break;
        default:
            break;
        }
    }

    Parsed& out_;
    std::uint16_t track_;
    int prefix_ = -1; // channel named by the last 0x20 meta, until the next channel event
    std::vector<std::array<Sounding, 128>> sounding_ = std::vector<std::array<Sounding, 128>>(16);
};

TempoMap build_tempo_map(std::vector<TempoEvent> tempos, int ppq) {
    std::stable_sort(tempos.begin(), tempos.end(),
                     [](const TempoEvent& a, const TempoEvent& b) { return a.tick < b.tick; });
    TempoMap map;
    map.ticks_per_quarter = ppq;
    map.changes.assign(1, TempoChange{});
    for (const TempoEvent& t : tempos) {
        TempoChange& last = map.changes.back();
        if (t.tick == last.tick) {
            last.us_per_quarter = t.us_per_quarter; // the later event at a tick wins
            continue;
        }
        double s = last.seconds +
                   static_cast<double>(t.tick - last.tick) * last.us_per_quarter / 1e6 / ppq;
        map.changes.push_back({t.tick, s, t.us_per_quarter});
    }
    return map;
}

std::vector<TimeSignature> build_time_signatures(std::vector<TimeSignature> sigs, int ppq) {
    // A bar under one tick long can't be placed.
    std::erase_if(sigs, [ppq](const TimeSignature& t) {
        return std::int64_t{4} * ppq * t.numerator < t.denominator;
    });
    std::stable_sort(sigs.begin(), sigs.end(), [](const TimeSignature& a, const TimeSignature& b) {
        return a.tick < b.tick;
    });
    std::vector<TimeSignature> out{TimeSignature{}};
    for (const TimeSignature& t : sigs) {
        if (t.tick == out.back().tick) {
            out.back() = t;
        } else {
            out.push_back(t);
        }
    }
    return out;
}

std::int64_t bar_ticks(const TimeSignature& t, int ppq, std::int64_t bars) {
    return bars * 4 * ppq * t.numerator / t.denominator;
}

bool build_measures(Song& song, Failure& fail) {
    const int ppq = song.tempo.ticks_per_quarter;
    const auto& sigs = song.time_signatures;
    std::vector<Measure>& bars = song.measures;
    for (std::size_t i = 0; i < sigs.size(); ++i) {
        const TimeSignature& t = sigs[i];
        bool last = i + 1 == sigs.size();
        if (t.tick >= song.end_tick && !bars.empty()) {
            break;
        }
        std::int64_t segment_end = last ? song.end_tick : sigs[i + 1].tick;
        for (std::int64_t k = 0;; ++k) {
            std::int64_t start = t.tick + bar_ticks(t, ppq, k);
            if (start >= segment_end && !(bars.empty() && k == 0)) {
                break;
            }
            if (bars.size() >= kMaxMeasures) {
                fail = {MidiFileError::TooLarge, 0, "more bars than any real song"};
                return false;
            }
            bars.push_back({0, start, song.tempo.seconds_at(start), t.numerator, t.denominator});
        }
    }
    // A first bar shorter than the next one is a pickup: bar 0.
    int first = 1;
    if (bars.size() >= 2) {
        std::int64_t len0 = bars[1].start_tick - bars[0].start_tick;
        TimeSignature next{0, bars[1].numerator, bars[1].denominator};
        if (len0 < bar_ticks(next, ppq, 1)) {
            first = 0;
        }
    }
    for (std::size_t i = 0; i < bars.size(); ++i) {
        bars[i].number = first + static_cast<int>(i);
    }
    return true;
}

void build_parts(Song& song, const std::vector<TrackInfo>& tracks) {
    for (const Note& n : song.notes) {
        auto it = std::find_if(song.parts.begin(), song.parts.end(), [&](const Part& p) {
            return p.track == n.track && p.channel == n.channel;
        });
        if (it == song.parts.end()) {
            const TrackInfo& t = tracks[n.track];
            const ChannelInfo& c = t.channels[n.channel];
            Part p;
            p.track = n.track;
            p.channel = n.channel;
            p.name = c.name.empty() ? t.name : c.name;
            p.instrument_name = c.instrument.empty() ? t.instrument : c.instrument;
            p.program = c.program;
            p.has_program = c.has_program;
            p.drum = n.drum;
            song.parts.push_back(std::move(p));
            it = song.parts.end() - 1;
        }
        ++it->note_count;
    }
    std::sort(song.parts.begin(), song.parts.end(), [](const Part& a, const Part& b) {
        return a.track != b.track ? a.track < b.track : a.channel < b.channel;
    });
}

void build_events(Song& song, std::vector<RawEvent> raw) {
    std::stable_sort(raw.begin(), raw.end(),
                     [](const RawEvent& a, const RawEvent& b) { return a.tick < b.tick; });
    song.events.reserve(raw.size());
    for (const RawEvent& e : raw) {
        song.events.push_back(
            {song.tempo.seconds_at(e.tick), e.tick, e.track, e.channel, e.kind, e.number, e.value});
    }
}

MidiLoadResult fail_result(const Failure& f, MidiLoadStats stats = {}) {
    MidiLoadResult r;
    r.error = f.error;
    char buf[160];
    std::snprintf(buf, sizeof buf, "%s: %s (byte %zu)", to_string(f.error), f.what, f.offset);
    r.message = buf;
    r.stats = stats;
    return r;
}

MidiLoadResult load(const std::uint8_t* data, std::size_t size, const MidiLoadOptions& options) {
    Reader r(data, 0, data == nullptr ? 0 : size);
    if (r.left() < 4 || std::memcmp(r.here(), "MThd", 4) != 0) {
        return fail_result({MidiFileError::NotMidi, 0, "no MThd header"});
    }
    r.skip(4);
    std::uint32_t header_len = 0, format = 0, ntracks = 0, division = 0;
    if (!r.be(header_len, 4) || r.left() < header_len || !r.be(format, 2) || !r.be(ntracks, 2) ||
        !r.be(division, 2)) {
        return fail_result({MidiFileError::Truncated, r.pos(), "header cut off"});
    }
    if (header_len < 6) {
        return fail_result({MidiFileError::Corrupt, 4, "header shorter than 6 bytes"});
    }
    r.skip(header_len - 6);
    if (format == 2) {
        return fail_result({MidiFileError::UnsupportedFormat, 8, "format 2 is not supported"});
    }
    if (format > 2) {
        return fail_result({MidiFileError::Corrupt, 8, "unknown format"});
    }
    if (ntracks == 0) {
        return fail_result({MidiFileError::Corrupt, 10, "no tracks"});
    }
    if (division & 0x8000) {
        return fail_result({MidiFileError::SmpteDivision, 12, "SMPTE time division"});
    }
    if (division == 0) {
        return fail_result({MidiFileError::Corrupt, 12, "zero ticks per quarter note"});
    }
    const int ppq = static_cast<int>(division);

    Parsed parsed;
    std::uint16_t track = 0;
    while (track < ntracks) {
        std::uint32_t chunk_len = 0;
        if (r.left() < 8) {
            return fail_result({MidiFileError::Truncated, r.pos(), "fewer tracks than the header"},
                               parsed.stats);
        }
        bool is_track = std::memcmp(r.here(), "MTrk", 4) == 0;
        r.skip(4);
        r.be(chunk_len, 4);
        // A track whose declared length runs past the end of the file is read up to the end.
        bool cut_off = r.left() < chunk_len;
        if (cut_off && !is_track) {
            return fail_result({MidiFileError::Truncated, r.pos(), "chunk cut off"}, parsed.stats);
        }
        std::size_t chunk_end = r.pos() + (cut_off ? r.left() : chunk_len);
        if (is_track) {
            parsed.tracks.emplace_back();
            Reader tr(data, r.pos(), chunk_end);
            Failure f;
            if (!TrackParser(parsed, track).parse(tr, f, cut_off)) {
                return fail_result(f, parsed.stats);
            }
            parsed.stats.tracks_truncated += cut_off ? 1 : 0;
            ++track;
        }
        r.skip(chunk_end - r.pos()); // unknown chunks (Yamaha XF "XFIH", ...) are skipped
    }

    MidiLoadResult result;
    result.stats = parsed.stats;
    Song& song = result.song;
    song.tempo = build_tempo_map(std::move(parsed.tempos), ppq);
    song.time_signatures = build_time_signatures(std::move(parsed.time_signatures), ppq);

    song.notes.reserve(parsed.notes.size());
    for (const RawNote& raw : parsed.notes) {
        Note n;
        n.start_tick = raw.start_tick;
        n.end_tick = raw.end_tick;
        n.start_s = song.tempo.seconds_at(raw.start_tick);
        n.duration_s = song.tempo.seconds_at(raw.end_tick) - n.start_s;
        n.pitch = raw.pitch;
        n.velocity = raw.velocity;
        n.track = raw.track;
        n.channel = raw.channel;
        n.drum = raw.channel == kDrumChannel;
        if (!n.drum && !options.keep_ghost_notes && n.duration_s < options.ghost_threshold_s) {
            ++result.stats.ghost_notes_dropped;
            continue;
        }
        song.notes.push_back(n);
        song.end_tick = std::max(song.end_tick, n.end_tick);
    }
    std::sort(song.notes.begin(), song.notes.end(), [](const Note& a, const Note& b) {
        if (a.start_tick != b.start_tick) {
            return a.start_tick < b.start_tick;
        }
        if (a.pitch != b.pitch) {
            return a.pitch < b.pitch;
        }
        return a.track != b.track ? a.track < b.track : a.channel < b.channel;
    });
    song.duration_s = song.tempo.seconds_at(song.end_tick);

    Failure f;
    if (!build_measures(song, f)) {
        return fail_result(f, result.stats);
    }
    for (Note& n : song.notes) {
        n.measure = song.measure_at(n.start_tick);
    }
    build_parts(song, parsed.tracks);
    build_events(song, std::move(parsed.events));
    if (options.assign_hands) {
        assign_hands_from_names(song);
    }
    result.ok = true;
    return result;
}

} // namespace

MidiLoadResult load_midi(const std::uint8_t* data, std::size_t size,
                         const MidiLoadOptions& options) {
    MidiFileError error = MidiFileError::TooLarge;
    try {
        return load(data, size, options);
    } catch (const std::bad_alloc&) {
    } catch (...) {
        error = MidiFileError::Corrupt;
    }
    // No message: building one could throw again.
    MidiLoadResult r;
    r.error = error;
    return r;
}

const char* to_string(MidiFileError error) {
    switch (error) {
    case MidiFileError::None:
        return "no error";
    case MidiFileError::NotMidi:
        return "not a MIDI file";
    case MidiFileError::Truncated:
        return "truncated MIDI file";
    case MidiFileError::Corrupt:
        return "corrupt MIDI file";
    case MidiFileError::UnsupportedFormat:
        return "unsupported MIDI file format";
    case MidiFileError::SmpteDivision:
        return "SMPTE time division is not supported";
    case MidiFileError::TooLarge:
        return "MIDI file too large";
    }
    return "unknown error";
}

} // namespace toonval
