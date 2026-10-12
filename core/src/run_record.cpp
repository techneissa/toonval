// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/run_record.hpp"

#include "toonval/transport.hpp"
#include "toonval/transpose.hpp"

#include <algorithm>

namespace toonval {

namespace {

int clamp_count(std::int64_t v) {
    return static_cast<int>(std::clamp<std::int64_t>(v, 0, kMaxRunCount));
}

RunHand sanitized_hand(RunHand h) {
    h.graded = clamp_count(h.graded);
    h.hit = std::min(clamp_count(h.hit), h.graded);
    return h;
}

// Strict UTF-8: no overlong forms, no surrogates, nothing above U+10FFFF.
bool valid_utf8(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size()) {
        auto b0 = static_cast<unsigned char>(s[i]);
        std::size_t extra = 0;
        std::uint32_t cp = 0;
        std::uint32_t min = 0;
        if (b0 < 0x80) {
            ++i;
            continue;
        } else if (b0 >= 0xC2 && b0 <= 0xDF) {
            extra = 1;
            cp = b0 & 0x1F;
            min = 0x80;
        } else if (b0 >= 0xE0 && b0 <= 0xEF) {
            extra = 2;
            cp = b0 & 0x0F;
            min = 0x800;
        } else if (b0 >= 0xF0 && b0 <= 0xF4) {
            extra = 3;
            cp = b0 & 0x07;
            min = 0x10000;
        } else {
            return false;
        }
        if (i + extra >= s.size()) {
            return false;
        }
        for (std::size_t k = 1; k <= extra; ++k) {
            auto b = static_cast<unsigned char>(s[i + k]);
            if ((b & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (b & 0x3F);
        }
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            return false;
        }
        i += extra + 1;
    }
    return true;
}

std::string sanitized_title(const std::string& title) {
    std::string out;
    out.reserve(std::min(title.size(), kMaxRunTitleBytes + 4));
    for (char c : title) {
        auto u = static_cast<unsigned char>(c);
        if (u >= 0x20 && u != 0x7f) {
            out += c;
        }
    }
    if (!valid_utf8(out)) {
        return {};
    }
    if (out.size() > kMaxRunTitleBytes) {
        std::size_t cut = kMaxRunTitleBytes;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) {
            --cut;
        }
        out.resize(cut);
    }
    return out;
}

// ---- Writing

void put_varint(std::string& out, std::uint64_t v) {
    while (v >= 0x80) {
        out += static_cast<char>((v & 0x7f) | 0x80);
        v >>= 7;
    }
    out += static_cast<char>(v);
}

void put_zigzag(std::string& out, std::int64_t v) {
    auto u = static_cast<std::uint64_t>(v);
    put_varint(out, (u << 1) ^ (v < 0 ? ~std::uint64_t{0} : 0));
}

// ---- Reading

struct Reader {
    std::string_view s;
    std::size_t pos = 0;
    RunParseStatus status = RunParseStatus::Ok;

    bool fail(RunParseStatus st) {
        if (status == RunParseStatus::Ok) {
            status = st;
        }
        return false;
    }
    std::size_t left() const { return s.size() - pos; }

    bool byte(std::uint8_t& b) {
        if (pos >= s.size()) {
            return fail(RunParseStatus::Truncated);
        }
        b = static_cast<std::uint8_t>(s[pos++]);
        return true;
    }
    bool varint(std::uint64_t& v) {
        v = 0;
        for (int i = 0; i < 10; ++i) {
            std::uint8_t b = 0;
            if (!byte(b)) {
                return false;
            }
            if (i == 9 && b > 1) {
                return fail(RunParseStatus::Corrupt); // more than 64 bits
            }
            v |= static_cast<std::uint64_t>(b & 0x7f) << (7 * i);
            if ((b & 0x80) == 0) {
                return true;
            }
        }
        return fail(RunParseStatus::Corrupt); // more than 10 bytes
    }
    bool zigzag(std::int64_t& v) {
        std::uint64_t u = 0;
        if (!varint(u)) {
            return false;
        }
        v = static_cast<std::int64_t>((u >> 1) ^ (~(u & 1) + 1));
        return true;
    }
    // A count, clamped into 0..kMaxRunCount like everything else the parser accepts.
    bool count(int& v) {
        std::uint64_t u = 0;
        if (!varint(u)) {
            return false;
        }
        v = static_cast<int>(std::min<std::uint64_t>(u, kMaxRunCount));
        return true;
    }
    bool hand(RunHand& h) { return count(h.graded) && count(h.hit); }
};

} // namespace

RunRecord sanitized(const RunRecord& in) {
    RunRecord r = in;
    r.song_title = sanitized_title(in.song_title);
    r.mode = in.mode == RunMode::Wait ? RunMode::Wait : RunMode::Flow;
    r.hands = (in.hands == HandFilter::Left || in.hands == HandFilter::Right) ? in.hands
                                                                              : HandFilter::Both;
    r.speed_percent = snap_speed_percent(in.speed_percent);
    r.transpose_semitones =
        std::clamp(in.transpose_semitones, -kMaxTransposeSemitones, kMaxTransposeSemitones);
    r.start_utc_s = std::clamp(in.start_utc_s, -kMaxRunUtcSeconds, kMaxRunUtcSeconds);
    r.end_utc_s = std::clamp(in.end_utc_s, r.start_utc_s, kMaxRunUtcSeconds);
    r.local_day = std::clamp(in.local_day, -kMaxRunDay, kMaxRunDay);
    r.duration_ms = std::clamp<std::int64_t>(in.duration_ms, 0, kMaxRunDurationMs);

    r.counts.perfect = clamp_count(in.counts.perfect);
    r.counts.good = clamp_count(in.counts.good);
    r.counts.early = clamp_count(in.counts.early);
    r.counts.late = clamp_count(in.counts.late);
    r.counts.missed = clamp_count(in.counts.missed);
    r.counts.played = clamp_count(in.counts.played);
    r.wrong = clamp_count(in.wrong);
    r.unplayable = clamp_count(in.unplayable);
    r.longest_combo = clamp_count(in.longest_combo);
    r.clean_passes = clamp_count(in.clean_passes);
    r.left = sanitized_hand(in.left);
    r.right = sanitized_hand(in.right);
    r.unknown = sanitized_hand(in.unknown);
    r.all = sanitized_hand(in.all);

    r.measures.clear();
    r.measures.reserve(std::min(in.measures.size(), kMaxRunMeasures));
    for (RunBar bar : in.measures) {
        bar.measure = std::clamp(bar.measure, 0, kMaxRunMeasureNumber);
        bar.graded = clamp_count(bar.graded);
        bar.missed = clamp_count(bar.missed);
        bar.wrong = clamp_count(bar.wrong);
        r.measures.push_back(bar);
    }
    std::stable_sort(r.measures.begin(), r.measures.end(),
                     [](const RunBar& a, const RunBar& b) { return a.measure < b.measure; });
    std::size_t out = 0;
    for (std::size_t i = 0; i < r.measures.size(); ++i) {
        if (out > 0 && r.measures[out - 1].measure == r.measures[i].measure) {
            RunBar& into = r.measures[out - 1];
            into.graded = clamp_count(std::int64_t{into.graded} + r.measures[i].graded);
            into.missed = clamp_count(std::int64_t{into.missed} + r.measures[i].missed);
            into.wrong = clamp_count(std::int64_t{into.wrong} + r.measures[i].wrong);
        } else {
            r.measures[out++] = r.measures[i];
        }
    }
    r.measures.resize(std::min(out, kMaxRunMeasures));
    for (RunBar& bar : r.measures) {
        bar.missed = std::min(bar.missed, bar.graded);
    }
    return r;
}

RunRecord make_run_record(const EvaluationSummary& summary, const RunContext& context,
                          std::span<const PassResult> passes) {
    RunRecord r;
    r.song_hash = context.song_hash;
    r.song_title = context.song_title;
    r.mode = context.mode;
    r.hands = context.hands;
    r.speed_percent = context.speed_percent;
    r.transpose_semitones = context.transpose_semitones;
    r.start_utc_s = context.start_utc_s;
    r.end_utc_s = context.end_utc_s;
    r.local_day = context.local_day;
    r.duration_ms = context.duration_ms;

    r.counts = {summary.counts.perfect, summary.counts.good,   summary.counts.early,
                summary.counts.late,    summary.counts.missed, summary.counts.played};
    r.wrong = summary.wrong;
    r.unplayable = summary.unplayable;
    r.longest_combo = summary.longest_combo;
    r.clean_passes = static_cast<int>(
        std::count_if(passes.begin(), passes.end(), [](const PassResult& p) { return p.clean; }));
    r.left = {summary.left.graded, summary.left.hit};
    r.right = {summary.right.graded, summary.right.hit};
    r.unknown = {summary.unknown.graded, summary.unknown.hit};
    r.all = {summary.all.graded, summary.all.hit};
    for (const MeasureStats& m : summary.measures) {
        r.measures.push_back({m.measure, m.graded, m.missed, m.wrong});
    }
    return sanitized(r);
}

std::int32_t local_day_from_utc(std::int64_t utc_s, std::int32_t utc_offset_s) {
    std::int64_t local = std::clamp(utc_s, -kMaxRunUtcSeconds, kMaxRunUtcSeconds) + utc_offset_s;
    std::int64_t day = local / 86'400;
    if (local % 86'400 < 0) {
        --day;
    }
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(day, -kMaxRunDay, kMaxRunDay));
}

// Howard Hinnant's days_from_civil / civil_from_days.
std::int32_t day_from_civil(const CivilDate& date) {
    std::int64_t y = date.year;
    const std::int64_t m = std::clamp(date.month, 1, 12);
    const std::int64_t d = std::clamp(date.day, 1, 31);
    y -= m <= 2 ? 1 : 0;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const std::int64_t yoe = y - era * 400;
    const std::int64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const std::int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<std::int32_t>(era * 146'097 + doe - 719'468);
}

CivilDate civil_from_day(std::int32_t day) {
    const std::int64_t z = std::int64_t{day} + 719'468;
    const std::int64_t era = (z >= 0 ? z : z - 146'096) / 146'097;
    const std::int64_t doe = z - era * 146'097;
    const std::int64_t yoe = (doe - doe / 1'460 + doe / 36'524 - doe / 146'096) / 365;
    const std::int64_t y = yoe + era * 400;
    const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const std::int64_t mp = (5 * doy + 2) / 153;
    const std::int64_t d = doy - (153 * mp + 2) / 5 + 1;
    const std::int64_t m = mp < 10 ? mp + 3 : mp - 9;
    CivilDate c;
    c.year = static_cast<int>(y + (m <= 2 ? 1 : 0));
    c.month = static_cast<int>(m);
    c.day = static_cast<int>(d);
    return c;
}

std::string serialize_run_record(const RunRecord& record) {
    const RunRecord r = sanitized(record);
    std::string out;
    out.reserve(64 + r.song_title.size() + r.measures.size() * 5);
    out += "TR";
    out += '\x01';
    for (int i = 0; i < 8; ++i) {
        out += static_cast<char>((r.song_hash >> (8 * i)) & 0xff);
    }
    put_varint(out, r.song_title.size());
    out += r.song_title;
    out += static_cast<char>(r.mode);
    out += static_cast<char>(r.hands);
    put_varint(out, static_cast<std::uint64_t>(r.speed_percent));
    put_zigzag(out, r.transpose_semitones);
    put_zigzag(out, r.start_utc_s);
    put_varint(out, static_cast<std::uint64_t>(r.end_utc_s - r.start_utc_s));
    put_zigzag(out, r.local_day);
    put_varint(out, static_cast<std::uint64_t>(r.duration_ms));
    for (int v : {r.counts.perfect, r.counts.good, r.counts.early, r.counts.late, r.counts.missed,
                  r.counts.played, r.wrong, r.unplayable, r.longest_combo, r.clean_passes}) {
        put_varint(out, static_cast<std::uint64_t>(v));
    }
    for (const RunHand* h : {&r.left, &r.right, &r.unknown, &r.all}) {
        put_varint(out, static_cast<std::uint64_t>(h->graded));
        put_varint(out, static_cast<std::uint64_t>(h->hit));
    }
    put_varint(out, r.measures.size());
    int previous = 0;
    bool first = true;
    for (const RunBar& bar : r.measures) {
        put_varint(out, static_cast<std::uint64_t>(first ? bar.measure : bar.measure - previous));
        put_varint(out, static_cast<std::uint64_t>(bar.graded));
        put_varint(out, static_cast<std::uint64_t>(bar.missed));
        put_varint(out, static_cast<std::uint64_t>(bar.wrong));
        previous = bar.measure;
        first = false;
    }
    return out;
}

RunParseStatus parse_run_record(std::string_view bytes, RunRecord& out) {
    Reader in{bytes};
    if (bytes.size() < 2) {
        return (bytes.empty() || bytes[0] == 'T') ? RunParseStatus::Truncated
                                                  : RunParseStatus::BadMagic;
    }
    if (bytes[0] != 'T' || bytes[1] != 'R') {
        return RunParseStatus::BadMagic;
    }
    in.pos = 2;
    std::uint8_t version = 0;
    if (!in.byte(version)) {
        return in.status;
    }
    if (version == 0) {
        return RunParseStatus::Corrupt;
    }
    if (version > 1) {
        return RunParseStatus::NewerVersion;
    }

    RunRecord r;
    if (in.left() < 8) {
        return RunParseStatus::Truncated;
    }
    for (int i = 0; i < 8; ++i) {
        r.song_hash |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(bytes[in.pos++]))
                       << (8 * i);
    }
    std::uint64_t u = 0;
    if (!in.varint(u)) {
        return in.status;
    }
    if (u > kMaxRunTitleBytes) {
        return RunParseStatus::Corrupt;
    }
    if (u > in.left()) {
        return RunParseStatus::Truncated;
    }
    r.song_title.assign(bytes.substr(in.pos, static_cast<std::size_t>(u)));
    in.pos += static_cast<std::size_t>(u);

    std::uint8_t mode = 0;
    std::uint8_t hands = 0;
    if (!in.byte(mode) || !in.byte(hands)) {
        return in.status;
    }
    if (mode > 1 || hands > 2) {
        return RunParseStatus::Corrupt;
    }
    r.mode = static_cast<RunMode>(mode);
    r.hands = static_cast<HandFilter>(hands);

    std::int64_t i64 = 0;
    if (!in.varint(u)) {
        return in.status;
    }
    r.speed_percent = static_cast<int>(std::min<std::uint64_t>(u, 1000));
    if (!in.zigzag(i64)) {
        return in.status;
    }
    r.transpose_semitones = static_cast<int>(std::clamp<std::int64_t>(i64, -1000, 1000));
    if (!in.zigzag(i64)) {
        return in.status;
    }
    r.start_utc_s = std::clamp(i64, -kMaxRunUtcSeconds, kMaxRunUtcSeconds);
    if (!in.varint(u)) {
        return in.status;
    }
    r.end_utc_s = r.start_utc_s +
                  static_cast<std::int64_t>(std::min<std::uint64_t>(u, 2 * kMaxRunUtcSeconds));
    if (!in.zigzag(i64)) {
        return in.status;
    }
    r.local_day = static_cast<std::int32_t>(std::clamp<std::int64_t>(i64, -kMaxRunDay, kMaxRunDay));
    if (!in.varint(u)) {
        return in.status;
    }
    r.duration_ms = static_cast<std::int64_t>(std::min<std::uint64_t>(u, kMaxRunDurationMs));

    if (!(in.count(r.counts.perfect) && in.count(r.counts.good) && in.count(r.counts.early) &&
          in.count(r.counts.late) && in.count(r.counts.missed) && in.count(r.counts.played) &&
          in.count(r.wrong) && in.count(r.unplayable) && in.count(r.longest_combo) &&
          in.count(r.clean_passes) && in.hand(r.left) && in.hand(r.right) && in.hand(r.unknown) &&
          in.hand(r.all))) {
        return in.status;
    }

    if (!in.varint(u)) {
        return in.status;
    }
    if (u > kMaxRunMeasures) {
        return RunParseStatus::Corrupt;
    }
    // Four bytes at least per bar: a count with no bytes behind it allocates nothing.
    r.measures.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(u, in.left() / 4)));
    std::int64_t measure = 0;
    for (std::uint64_t i = 0; i < u; ++i) {
        std::uint64_t step = 0;
        RunBar bar;
        if (!in.varint(step) || !in.count(bar.graded) || !in.count(bar.missed) ||
            !in.count(bar.wrong)) {
            return in.status;
        }
        measure = std::min<std::int64_t>(
            measure +
                static_cast<std::int64_t>(std::min<std::uint64_t>(step, kMaxRunMeasureNumber)),
            kMaxRunMeasureNumber);
        bar.measure = static_cast<int>(measure);
        r.measures.push_back(bar);
    }
    if (in.left() != 0) {
        return RunParseStatus::Corrupt;
    }
    out = sanitized(r);
    return RunParseStatus::Ok;
}

} // namespace toonval
