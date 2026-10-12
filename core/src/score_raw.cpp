// SPDX-License-Identifier: GPL-3.0-or-later
#include "score_raw.hpp"

#include "toonval/song_builder.hpp"

#include "pugixml.hpp"

#include <algorithm>
#include <cmath>
#include <tuple>

namespace toonval::score_detail {
namespace {

constexpr int kDrumChannelIndex = 9;

std::int64_t nominal_bar_ticks(int ppq, int numerator, int denominator) {
    return std::int64_t{4} * ppq * numerator / denominator;
}

// MIDI channel for the n-th part: 0..8, then 10..15 (channel 10 is percussion).
std::uint8_t channel_of_part(int part) {
    const int c = part % 15;
    return static_cast<std::uint8_t>(c >= kDrumChannelIndex ? c + 1 : c);
}

struct BarSignature {
    int numerator = 4;
    int denominator = 4;
};

} // namespace

ScoreLoadResult make_error(ScoreFileError error, std::string message) {
    ScoreLoadResult r;
    r.ok = false;
    r.error = error;
    r.message = std::move(message);
    return r;
}

bool TieJoiner::join(RawScore& raw, const Key& key, int measure, std::int64_t offset,
                     std::int64_t length, bool stop, bool start) {
    if (!stop) {
        return false;
    }
    const auto it = open_.find(key);
    if (it != open_.end()) {
        Open& o = it->second;
        const bool contiguous = (measure == o.end_measure && offset == o.end_offset) ||
                                (measure == o.end_measure + 1 && offset == 0);
        if (contiguous) {
            raw.notes[o.note].length += length;
            o.end_measure = measure;
            o.end_offset = offset + length;
            ++raw.stats.ties_merged;
            if (!start) {
                open_.erase(it);
            }
            return true;
        }
        open_.erase(it);
    }
    ++raw.stats.unmatched_ties;
    return false;
}

std::string clean_name(const char* text) {
    std::string out;
    bool pending_space = false;
    for (const char* c = text == nullptr ? "" : text; *c != '\0'; ++c) {
        if (*c == ' ' || *c == '\t' || *c == '\n' || *c == '\r') {
            pending_space = !out.empty();
            continue;
        }
        if (pending_space) {
            out += ' ';
            pending_space = false;
        }
        out += *c;
    }
    return out;
}

bool parse_int(const char* text, long long& out) {
    if (text == nullptr) {
        return false;
    }
    while (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r') {
        ++text;
    }
    bool negative = false;
    if (*text == '-' || *text == '+') {
        negative = *text == '-';
        ++text;
    }
    if (*text < '0' || *text > '9') {
        return false;
    }
    long long v = 0;
    for (; *text >= '0' && *text <= '9'; ++text) {
        if (v < 100'000'000'000'000LL) { // saturate far above anything a score holds
            v = v * 10 + (*text - '0');
        }
    }
    out = negative ? -v : v;
    return true;
}

bool parse_double(const char* text, double& out, bool first_number_in_text) {
    if (text == nullptr) {
        return false;
    }
    if (first_number_in_text) {
        while (*text != '\0' && !(*text >= '0' && *text <= '9')) {
            ++text;
        }
    } else {
        while (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r') {
            ++text;
        }
    }
    bool negative = false;
    if (*text == '-' || *text == '+') {
        negative = *text == '-';
        ++text;
    }
    if ((*text < '0' || *text > '9') && *text != '.') {
        return false;
    }
    double v = 0.0;
    bool any = false;
    for (; *text >= '0' && *text <= '9'; ++text) {
        any = true;
        v = std::min(v * 10.0 + (*text - '0'), 1e15);
    }
    if (*text == '.') {
        ++text;
        double scale = 0.1;
        for (; *text >= '0' && *text <= '9'; ++text) {
            any = true;
            v += (*text - '0') * scale;
            scale *= 0.1;
        }
    }
    if (!any) {
        return false;
    }
    out = negative ? -v : v;
    return true;
}

bool parse_xml_bytes(const std::uint8_t* data, std::size_t size, pugi::xml_document& doc,
                     ScoreLoadResult& error) {
    if (data == nullptr || size == 0) {
        error = make_error(ScoreFileError::NotXml, "the file is empty");
        return false;
    }
    if (size > kMaxScoreXmlBytes) {
        error = make_error(ScoreFileError::TooLarge, "the score file is larger than 64 MiB");
        return false;
    }
    const pugi::xml_parse_result parsed = doc.load_buffer(data, size, pugi::parse_default);
    if (!parsed) {
        error = make_error(ScoreFileError::NotXml, std::string("not readable XML: ") +
                                                       parsed.description() + " at byte " +
                                                       std::to_string(parsed.offset));
        return false;
    }
    return true;
}

ScoreLoadResult assemble_score(RawScore& raw) {
    const int ppq = raw.ppq;
    const std::size_t bar_count = raw.measures.size();
    if (raw.notes.empty() || bar_count == 0) {
        return make_error(ScoreFileError::NoNotes, "the score has no playable notes");
    }
    if (bar_count > kMaxSongMeasures) {
        return make_error(ScoreFileError::TooLarge, "the score has more bars than any song needs");
    }

    // The signature in force in each bar.
    std::vector<BarSignature> sig(bar_count);
    BarSignature current;
    for (std::size_t i = 0; i < bar_count; ++i) {
        if (raw.measures[i].has_signature) {
            current = {raw.measures[i].numerator, raw.measures[i].denominator};
        }
        sig[i] = current;
    }

    // Bar lengths: as long as the music in them, but never shorter than the signature, except for
    // a pickup and for bars the file marks implicit.
    const std::int64_t first_nominal = nominal_bar_ticks(ppq, sig[0].numerator, sig[0].denominator);
    const bool pickup = bar_count >= 2 && raw.measures[0].extent > 0 &&
                        (raw.measures[0].implicit || raw.measures[0].extent < first_nominal);
    std::vector<std::int64_t> start(bar_count + 1, 0);
    for (std::size_t i = 0; i < bar_count; ++i) {
        const RawMeasure& m = raw.measures[i];
        const std::int64_t nominal = nominal_bar_ticks(ppq, sig[i].numerator, sig[i].denominator);
        std::int64_t length = nominal;
        if (m.extent > 0) {
            length = ((i == 0 && pickup) || m.implicit) ? m.extent : std::max(m.extent, nominal);
        }
        start[i + 1] = start[i] + std::max<std::int64_t>(length, 1);
    }

    Song song;
    song.tempo.ticks_per_quarter = ppq;
    song.tempo.changes.clear();

    // Tempo comes from the first part that marks any.
    int tempo_part = -1;
    for (const RawTempo& t : raw.tempos) {
        tempo_part = tempo_part < 0 ? t.part : std::min(tempo_part, t.part);
    }
    for (const RawTempo& t : raw.tempos) {
        if (t.part != tempo_part || !(t.bpm > 0.0) || t.measure < 0 ||
            static_cast<std::size_t>(t.measure) >= bar_count) {
            continue;
        }
        song.tempo.changes.push_back({start[static_cast<std::size_t>(t.measure)] + t.offset, 0.0,
                                      us_per_quarter_from_bpm(t.bpm)});
    }
    std::stable_sort(song.tempo.changes.begin(), song.tempo.changes.end(),
                     [](const TempoChange& a, const TempoChange& b) { return a.tick < b.tick; });
    if (song.tempo.changes.empty()) {
        song.tempo.changes.push_back({});
    }

    // Signatures where they change.
    for (std::size_t i = 0; i < bar_count; ++i) {
        if (i == 0 || sig[i].numerator != sig[i - 1].numerator ||
            sig[i].denominator != sig[i - 1].denominator) {
            song.time_signatures.push_back({start[i], static_cast<std::uint8_t>(sig[i].numerator),
                                            static_cast<std::uint8_t>(sig[i].denominator)});
        }
    }

    // Dynamics per part, by tick.
    struct Level {
        std::int64_t tick;
        int velocity;
    };
    std::vector<std::vector<Level>> levels(raw.parts.size());
    for (const RawDynamic& d : raw.dynamics) {
        if (d.part < 0 || static_cast<std::size_t>(d.part) >= levels.size() || d.measure < 0 ||
            static_cast<std::size_t>(d.measure) >= bar_count) {
            continue;
        }
        levels[static_cast<std::size_t>(d.part)].push_back(
            {start[static_cast<std::size_t>(d.measure)] + d.offset, d.velocity});
    }
    for (auto& l : levels) {
        std::stable_sort(l.begin(), l.end(),
                         [](const Level& a, const Level& b) { return a.tick < b.tick; });
    }

    // Notes.
    song.notes.reserve(raw.notes.size());
    for (const RawNote& rn : raw.notes) {
        if (rn.measure < 0 || static_cast<std::size_t>(rn.measure) >= bar_count || rn.length <= 0 ||
            rn.track < 0 || static_cast<std::size_t>(rn.track) >= raw.tracks.size()) {
            continue;
        }
        const RawTrack& track = raw.tracks[static_cast<std::size_t>(rn.track)];
        Note n;
        n.start_tick = start[static_cast<std::size_t>(rn.measure)] + rn.offset;
        n.end_tick = n.start_tick + rn.length;
        n.pitch = rn.pitch;
        n.hand = track.hand;
        n.finger = rn.finger;
        n.track = static_cast<std::uint16_t>(rn.track);
        n.channel = channel_of_part(track.part);
        int velocity = rn.velocity;
        if (velocity == 0) {
            velocity = kDefaultVelocity;
            const auto& l = levels[static_cast<std::size_t>(track.part)];
            const auto it =
                std::upper_bound(l.begin(), l.end(), n.start_tick,
                                 [](std::int64_t t, const Level& v) { return t < v.tick; });
            if (it != l.begin()) {
                velocity = (it - 1)->velocity;
            }
        }
        n.velocity = static_cast<std::uint8_t>(std::clamp(velocity, 1, 127));
        song.notes.push_back(n);
    }

    // One note per pitch and channel sounds at a time, as in a MIDI file.
    {
        std::vector<std::size_t> order(song.notes.size());
        for (std::size_t i = 0; i < order.size(); ++i) {
            order[i] = i;
        }
        std::sort(order.begin(), order.end(), [&](std::size_t x, std::size_t y) {
            const Note& a = song.notes[x];
            const Note& b = song.notes[y];
            return std::tie(a.channel, a.pitch, a.start_tick, b.end_tick, x) <
                   std::tie(b.channel, b.pitch, b.start_tick, a.end_tick, y);
        });
        std::vector<bool> drop(song.notes.size(), false);
        std::size_t cur = order.empty() ? 0 : order[0];
        for (std::size_t k = 1; k < order.size(); ++k) {
            Note& c = song.notes[cur];
            Note& n = song.notes[order[k]];
            if (c.channel != n.channel || c.pitch != n.pitch) {
                cur = order[k];
            } else if (n.start_tick == c.start_tick) {
                c.end_tick = std::max(c.end_tick, n.end_tick);
                if (c.hand == Hand::Unknown) {
                    c.hand = n.hand;
                }
                if (c.finger == 0) {
                    c.finger = n.finger;
                }
                drop[order[k]] = true;
                ++raw.stats.duplicates_merged;
            } else {
                if (n.start_tick < c.end_tick) {
                    c.end_tick = n.start_tick;
                    ++raw.stats.overlaps_split;
                }
                cur = order[k];
            }
        }
        std::size_t out = 0;
        for (std::size_t i = 0; i < song.notes.size(); ++i) {
            if (!drop[i]) {
                song.notes[out++] = song.notes[i];
            }
        }
        song.notes.resize(out);
    }

    // Parts, with the instrument a file names.
    std::vector<int> first_track_of_part(raw.parts.size(), -1);
    for (std::size_t i = 0; i < raw.tracks.size(); ++i) {
        const RawTrack& t = raw.tracks[i];
        const RawPart& p = raw.parts[static_cast<std::size_t>(t.part)];
        Part part;
        part.track = static_cast<std::uint16_t>(i);
        part.channel = channel_of_part(t.part);
        part.name = t.name;
        part.instrument_name = p.instrument_name;
        part.hand = t.hand;
        if (p.program >= 0) {
            part.program = static_cast<std::uint8_t>(p.program);
            part.has_program = true;
        }
        song.parts.push_back(std::move(part));
        int& first = first_track_of_part[static_cast<std::size_t>(t.part)];
        if (first < 0) {
            first = static_cast<int>(i);
        }
    }
    for (std::size_t p = 0; p < raw.parts.size(); ++p) {
        if (raw.parts[p].program >= 0 && first_track_of_part[p] >= 0) {
            ChannelEvent e;
            e.track = static_cast<std::uint16_t>(first_track_of_part[p]);
            e.channel = channel_of_part(static_cast<int>(p));
            e.kind = ChannelEventKind::Program;
            e.value = static_cast<std::uint16_t>(raw.parts[p].program);
            song.events.push_back(e);
        }
    }

    if (!finish_song(song)) {
        return make_error(ScoreFileError::TooLarge, "the score has more bars than any song needs");
    }
    if (song.notes.empty()) {
        return make_error(ScoreFileError::NoNotes, "the score has no playable notes");
    }
    std::erase_if(song.parts, [](const Part& p) { return p.note_count == 0; });

    // finish_song built bars from the signatures alone; the score knows its real bars.
    song.measures.clear();
    const int first_number = pickup ? 0 : 1;
    for (std::size_t i = 0; i < bar_count; ++i) {
        if (i > 0 && start[i] >= song.end_tick) {
            break;
        }
        Measure m;
        m.number = first_number + static_cast<int>(i);
        m.start_tick = start[i];
        m.start_s = song.tempo.seconds_at(start[i]);
        m.numerator = static_cast<std::uint8_t>(sig[i].numerator);
        m.denominator = static_cast<std::uint8_t>(sig[i].denominator);
        song.measures.push_back(m);
    }
    for (Note& n : song.notes) {
        n.measure = song.measure_at(n.start_tick);
    }

    ScoreLoadResult r;
    r.ok = true;
    r.song = std::move(song);
    r.stats = raw.stats;
    r.title = raw.title;
    return r;
}

} // namespace toonval::score_detail
