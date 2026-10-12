// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/sections.hpp"

#include "toonval/content_hash.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace toonval {

namespace {

// Two times closer than this are the same instant: bar starts come from tick arithmetic and
// markers from the touch screen, so exact equality is not the test.
constexpr double kEps = 1e-6;
// The longest song a section may reach into when there is no song to check against (a file read
// before the song is known); the session state uses the same bound.
constexpr double kMaxSectionSeconds = 24.0 * 60.0 * 60.0;
// The largest step of an auto speed-up policy.
constexpr int kMaxSpeedStepPercent = 25;

// ---- Text

struct Decoded {
    std::uint32_t cp = 0;
    std::size_t len = 0; // 0 when the bytes at this position are not a valid character
};

Decoded decode_utf8(std::string_view s, std::size_t i) {
    auto b0 = static_cast<unsigned char>(s[i]);
    if (b0 < 0x80) {
        return {b0, 1};
    }
    std::size_t extra = 0;
    std::uint32_t cp = 0;
    std::uint32_t min = 0;
    if (b0 >= 0xC2 && b0 <= 0xDF) {
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
        return {};
    }
    if (i + extra >= s.size()) {
        return {};
    }
    for (std::size_t k = 1; k <= extra; ++k) {
        auto b = static_cast<unsigned char>(s[i + k]);
        if ((b & 0xC0) != 0x80) {
            return {};
        }
        cp = (cp << 6) | (b & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        return {};
    }
    return {cp, extra + 1};
}

bool is_space_like(std::uint32_t cp) {
    return cp <= 0x20 || (cp >= 0x7F && cp <= 0xA0) || cp == 0x1680 ||
           (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 || cp == 0x202F ||
           cp == 0x205F || cp == 0x3000;
}

bool is_invisible(std::uint32_t cp) {
    return (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x202A && cp <= 0x202E) ||
           (cp >= 0x2060 && cp <= 0x206F) || cp == 0xFEFF || cp == 0xFFFE || cp == 0xFFFF;
}

// ---- Times

double song_length(const Song& song) {
    return std::isfinite(song.duration_s) && song.duration_s > 0.0 ? song.duration_s : 0.0;
}

// The time a marker is asked for, clamped to the song and snapped to a bar. False for NaN.
bool resolve_time(const Song& song, double at_s, bool snap, double& out) {
    if (std::isnan(at_s)) {
        return false;
    }
    out = std::clamp(at_s, 0.0, song_length(song));
    if (snap && !song.measures.empty()) {
        out = snap_to_measure(song, out, SnapMode::Nearest);
    }
    return true;
}

// ---- Derived fields

std::size_t bar_at_start(const Song& song, double t) {
    const auto& bars = song.measures;
    auto it = std::upper_bound(bars.begin(), bars.end(), t + kEps,
                               [](double v, const Measure& m) { return v < m.start_s; });
    return it == bars.begin() ? 0 : static_cast<std::size_t>(it - bars.begin()) - 1;
}

// The last bar that starts before `t`, so a section ending on a bar line does not reach into the
// next bar.
std::size_t bar_before(const Song& song, double t) {
    const auto& bars = song.measures;
    auto it = std::lower_bound(bars.begin(), bars.end(), t - kEps,
                               [](const Measure& m, double v) { return m.start_s < v; });
    return it == bars.begin() ? 0 : static_cast<std::size_t>(it - bars.begin()) - 1;
}

std::string auto_name(const Section& s, std::size_t index) {
    if (s.snapped_to_bars) {
        if (s.first_measure == s.last_measure) {
            return "Bar " + std::to_string(s.first_measure);
        }
        return "Bars " + std::to_string(s.first_measure) + "-" + std::to_string(s.last_measure);
    }
    return "Section " + std::to_string(index + 1);
}

// Recomputes every field that follows from the times and the song's bars.
void refresh(SongSections& set, const Song& song) {
    for (std::size_t i = 0; i < set.sections.size(); ++i) {
        Section& s = set.sections[i];
        if (song.measures.empty()) {
            s.first_measure = s.last_measure = 0;
            s.snapped_to_bars = false;
        } else {
            std::size_t first = bar_at_start(song, s.start_s);
            std::size_t last = std::max(first, bar_before(song, s.end_s));
            s.first_measure = song.measures[first].number;
            s.last_measure = song.measures[last].number;
            double last_end = last + 1 < song.measures.size() ? song.measures[last + 1].start_s
                                                              : song_length(song);
            s.snapped_to_bars = std::abs(song.measures[first].start_s - s.start_s) <= kEps &&
                                std::abs(last_end - s.end_s) <= kEps;
        }
        if (s.custom_name) {
            s.name = sanitize_section_name(s.name);
            s.custom_name = !s.name.empty();
        }
        if (!s.custom_name) {
            s.name = auto_name(s, i);
        }
    }
}

// ---- Lookup helpers

std::ptrdiff_t index_of(const SongSections& set, SectionId id) {
    for (std::size_t i = 0; i < set.sections.size(); ++i) {
        if (set.sections[i].id == id) {
            return static_cast<std::ptrdiff_t>(i);
        }
    }
    return -1;
}

bool is_boundary(const SongSections& set, double t) {
    for (const Section& s : set.sections) {
        if (std::abs(s.end_s - t) <= kEps) {
            return true;
        }
    }
    return false;
}

EditResult fail(EditError error) { return EditResult{false, error, kNoSection}; }
EditResult done(SectionId id) { return EditResult{true, EditError::None, id}; }

bool ids_left(const SongSections& set, std::size_t needed) {
    return set.next_id <= kMaxSectionId && kMaxSectionId - set.next_id + 1 >= needed;
}

// Places one marker at the already resolved time `t`. The sections it works on must be a copy:
// callers keep the original on failure.
EditResult cut(SongSections& set, const Song& song, double t) {
    const double length = song_length(song);
    if (t <= kEps || length - t <= kEps || is_boundary(set, t)) {
        return fail(EditError::AtMarker);
    }
    if (set.sections.empty()) {
        if (t < kMinSectionSeconds || length - t < kMinSectionSeconds) {
            return fail(EditError::TooShort);
        }
        if (!ids_left(set, 2)) {
            return fail(EditError::TooMany);
        }
        Section left;
        left.id = set.next_id++;
        left.start_s = 0.0;
        left.end_s = t;
        Section right;
        right.id = set.next_id++;
        right.start_s = t;
        right.end_s = length;
        set.sections = {left, right};
        return done(right.id);
    }
    std::size_t i = 0;
    while (i < set.sections.size() && set.sections[i].end_s < t) {
        ++i;
    }
    if (i == set.sections.size()) {
        return fail(EditError::OutOfRange);
    }
    Section& s = set.sections[i];
    if (t - s.start_s < kMinSectionSeconds || s.end_s - t < kMinSectionSeconds) {
        return fail(EditError::TooShort);
    }
    if (set.sections.size() >= kMaxSectionsPerSong || !ids_left(set, 1)) {
        return fail(EditError::TooMany);
    }
    Section right;
    right.id = set.next_id++;
    right.start_s = t;
    right.end_s = s.end_s;
    s.end_s = t;
    SectionId id = right.id;
    set.sections.insert(set.sections.begin() + static_cast<std::ptrdiff_t>(i) + 1, right);
    return done(id);
}

// True when `a` stays on a section longer than `b`: more passes, with 0 (until skipped) the most.
bool repeats_more(int a, int b) { return a == 0 ? b != 0 : (b != 0 && a > b); }

// Joins sections lo..hi (indices) into lo. The ids of the absorbed sections are appended to
// `absorbed_out` when it is not null.
void join(SongSections& set, std::size_t lo, std::size_t hi,
          std::vector<SectionId>* absorbed_out = nullptr) {
    const SectionId keep = set.sections[lo].id;
    std::unordered_set<SectionId> absorbed;
    for (std::size_t i = lo + 1; i <= hi; ++i) {
        absorbed.insert(set.sections[i].id);
        if (absorbed_out != nullptr) {
            absorbed_out->push_back(set.sections[i].id);
        }
    }
    set.sections[lo].end_s = set.sections[hi].end_s;
    set.sections.erase(set.sections.begin() + static_cast<std::ptrdiff_t>(lo) + 1,
                       set.sections.begin() + static_cast<std::ptrdiff_t>(hi) + 1);
    // Plan items of the absorbed sections move to the survivor. Consecutive items that land on it
    // collapse into one: the larger repeat count, clean passes only if either was, and the first
    // item's speed-up.
    std::vector<PlanItem> plan;
    bool last_was_moved = false;
    for (PlanItem item : set.plan) {
        bool moved = absorbed.count(item.section) != 0;
        if (moved) {
            item.section = keep;
        }
        if (!plan.empty() && plan.back().section == keep && (moved || last_was_moved) &&
            item.section == keep) {
            PlanItem& first = plan.back();
            if (repeats_more(item.repeats, first.repeats)) {
                first.repeats = item.repeats;
            }
            first.clean_passes_only = first.clean_passes_only || item.clean_passes_only;
            continue;
        }
        plan.push_back(item);
        last_was_moved = moved;
    }
    set.plan = std::move(plan);
}

bool plan_item_ok(const SongSections& set, const PlanItem& item) {
    return item == sanitized(item) && index_of(set, item.section) >= 0;
}

bool names_ok(const SongSections& set) {
    for (const Section& s : set.sections) {
        if (s.name.empty() || s.name != sanitize_section_name(s.name)) {
            return false;
        }
    }
    return true;
}

bool ids_ok(const SongSections& set) {
    SectionId highest = 0;
    std::unordered_set<SectionId> seen;
    for (const Section& s : set.sections) {
        if (s.id == kNoSection || s.id > kMaxSectionId || !seen.insert(s.id).second) {
            return false;
        }
        highest = std::max(highest, s.id);
    }
    return set.next_id > highest && set.next_id <= kMaxSectionId + 1;
}

} // namespace

std::string sanitize_section_name(std::string_view text) {
    std::string out;
    bool pending_space = false;
    for (std::size_t i = 0; i < text.size();) {
        Decoded d = decode_utf8(text, i);
        if (d.len == 0) {
            ++i; // an invalid byte is dropped
            continue;
        }
        std::string_view bytes = text.substr(i, d.len);
        i += d.len;
        if (is_space_like(d.cp)) {
            pending_space = !out.empty();
            continue;
        }
        if (is_invisible(d.cp)) {
            continue;
        }
        std::size_t needed = bytes.size() + (pending_space ? 1 : 0);
        if (out.size() + needed > kMaxSectionNameBytes) {
            break;
        }
        if (pending_space) {
            out.push_back(' ');
            pending_space = false;
        }
        out.append(bytes);
    }
    return out;
}

PlanItem sanitized(const PlanItem& item) {
    PlanItem out = item;
    out.repeats = std::clamp(out.repeats, 0, kMaxPlanRepeats);
    out.speed_up.start_percent = snap_speed_percent(out.speed_up.start_percent);
    // Clamp before rounding: adding to a huge value would overflow.
    const int step = std::clamp(out.speed_up.step_percent, kSpeedStepPercent, kMaxSpeedStepPercent);
    out.speed_up.step_percent =
        (step + kSpeedStepPercent / 2) / kSpeedStepPercent * kSpeedStepPercent;
    out.speed_up.max_percent =
        std::max(snap_speed_percent(out.speed_up.max_percent), out.speed_up.start_percent);
    return out;
}

// ---- SongSections: lookup

const Section* SongSections::find(SectionId id) const {
    std::ptrdiff_t i = index_of(*this, id);
    return i < 0 ? nullptr : &sections[static_cast<std::size_t>(i)];
}

const Section* SongSections::at(double seconds) const {
    if (sections.empty() || std::isnan(seconds) || seconds < sections.front().start_s ||
        seconds > sections.back().end_s) {
        return nullptr;
    }
    for (const Section& s : sections) {
        if (seconds < s.end_s) {
            return &s;
        }
    }
    return &sections.back();
}

LoopSpan SongSections::span_of(SectionId id) const {
    LoopSpan span;
    if (const Section* s = find(id)) {
        span.ok = true;
        span.start_s = s->start_s;
        span.end_s = s->end_s;
    }
    return span;
}

std::vector<double> SongSections::markers() const {
    std::vector<double> out;
    for (std::size_t i = 0; i + 1 < sections.size(); ++i) {
        out.push_back(sections[i].end_s);
    }
    return out;
}

// ---- SongSections: edits. Each works on a copy and keeps it only on success.

EditResult SongSections::add_marker(const Song& song, double at_s, bool snap_to_bars) {
    double t = 0.0;
    if (!resolve_time(song, at_s, snap_to_bars, t)) {
        return fail(EditError::OutOfRange);
    }
    SongSections work = *this;
    EditResult r = cut(work, song, t);
    if (r.ok) {
        refresh(work, song);
        *this = std::move(work);
    }
    return r;
}

EditResult SongSections::split(const Song& song, SectionId id, double at_s, bool snap_to_bars) {
    std::ptrdiff_t i = index_of(*this, id);
    if (i < 0) {
        return fail(EditError::NotFound);
    }
    double t = 0.0;
    if (!resolve_time(song, at_s, snap_to_bars, t)) {
        return fail(EditError::OutOfRange);
    }
    const Section& s = sections[static_cast<std::size_t>(i)];
    if (t < s.start_s - kEps || t > s.end_s + kEps) {
        return fail(EditError::OutOfRange);
    }
    SongSections work = *this;
    EditResult r = cut(work, song, t);
    if (r.ok) {
        refresh(work, song);
        *this = std::move(work);
    }
    return r;
}

EditResult SongSections::add_range(const Song& song, double start_s, double end_s,
                                   bool snap_to_bars, std::string_view name) {
    double a = 0.0;
    double b = 0.0;
    if (!resolve_time(song, start_s, snap_to_bars, a) ||
        !resolve_time(song, end_s, snap_to_bars, b)) {
        return fail(EditError::OutOfRange);
    }
    if (a > b) {
        std::swap(a, b);
    }
    if (b - a < kMinSectionSeconds) {
        return fail(EditError::TooShort);
    }
    for (const Section& s : sections) {
        if (s.end_s > a + kEps && s.end_s < b - kEps && &s != &sections.back()) {
            return fail(EditError::Overlaps);
        }
    }
    const double length = song_length(song);
    SongSections work = *this;
    for (double t : {a, b}) {
        if (t <= kEps || length - t <= kEps || is_boundary(work, t)) {
            continue;
        }
        EditResult r = cut(work, song, t);
        if (!r.ok) {
            return r;
        }
    }
    if (work.sections.empty()) { // the whole song, with no marker to make
        if (!ids_left(work, 1)) {
            return fail(EditError::TooMany);
        }
        Section whole;
        whole.id = work.next_id++;
        whole.start_s = 0.0;
        whole.end_s = length;
        work.sections.push_back(whole);
    }
    Section* found = nullptr;
    for (Section& s : work.sections) {
        if (std::abs(s.start_s - a) <= kEps && std::abs(s.end_s - b) <= kEps) {
            found = &s;
        }
    }
    if (found == nullptr) {
        return fail(EditError::Overlaps);
    }
    std::string cleaned = sanitize_section_name(name);
    if (!cleaned.empty()) {
        found->name = cleaned;
        found->custom_name = true;
    }
    SectionId id = found->id;
    refresh(work, song);
    *this = std::move(work);
    return done(id);
}

EditResult SongSections::remove_marker(const Song& song, double near_s,
                                       std::vector<SectionId>* absorbed) {
    if (std::isnan(near_s)) {
        return fail(EditError::OutOfRange);
    }
    if (sections.size() < 2) {
        return fail(EditError::NotFound);
    }
    std::size_t best = 0;
    for (std::size_t i = 1; i + 1 < sections.size(); ++i) {
        if (std::abs(sections[i].end_s - near_s) < std::abs(sections[best].end_s - near_s)) {
            best = i;
        }
    }
    SongSections work = *this;
    SectionId keep = work.sections[best].id;
    std::vector<SectionId> gone;
    join(work, best, best + 1, &gone);
    refresh(work, song);
    *this = std::move(work);
    if (absorbed != nullptr) {
        *absorbed = std::move(gone);
    }
    return done(keep);
}

EditResult SongSections::merge(const Song& song, std::span<const SectionId> ids,
                               std::vector<SectionId>* absorbed) {
    std::vector<std::size_t> picked;
    for (SectionId id : ids) {
        std::ptrdiff_t i = index_of(*this, id);
        if (i < 0) {
            return fail(EditError::NotFound);
        }
        picked.push_back(static_cast<std::size_t>(i));
    }
    std::sort(picked.begin(), picked.end());
    picked.erase(std::unique(picked.begin(), picked.end()), picked.end());
    if (picked.size() < 2) {
        return fail(EditError::NotEnough);
    }
    if (picked.back() - picked.front() + 1 != picked.size()) {
        return fail(EditError::NotAdjacent);
    }
    SongSections work = *this;
    SectionId keep = work.sections[picked.front()].id;
    std::vector<SectionId> gone;
    join(work, picked.front(), picked.back(), &gone);
    refresh(work, song);
    *this = std::move(work);
    if (absorbed != nullptr) {
        *absorbed = std::move(gone);
    }
    return done(keep);
}

EditResult SongSections::erase(const Song& song, SectionId id) {
    std::ptrdiff_t found = index_of(*this, id);
    if (found < 0) {
        return fail(EditError::NotFound);
    }
    SongSections work = *this;
    auto i = static_cast<std::size_t>(found);
    SectionId took_over = kNoSection;
    if (i > 0) {
        work.sections[i - 1].end_s = work.sections[i].end_s;
        took_over = work.sections[i - 1].id;
    } else if (work.sections.size() > 1) {
        work.sections[1].start_s = work.sections[0].start_s;
        took_over = work.sections[1].id;
    }
    work.sections.erase(work.sections.begin() + static_cast<std::ptrdiff_t>(i));
    std::erase_if(work.plan, [&](const PlanItem& p) { return p.section == id; });
    refresh(work, song);
    *this = std::move(work);
    return done(took_over);
}

EditResult SongSections::rename(SectionId id, std::string_view name) {
    std::ptrdiff_t i = index_of(*this, id);
    if (i < 0) {
        return fail(EditError::NotFound);
    }
    Section& s = sections[static_cast<std::size_t>(i)];
    std::string cleaned = sanitize_section_name(name);
    s.custom_name = !cleaned.empty();
    s.name = s.custom_name ? cleaned : auto_name(s, static_cast<std::size_t>(i));
    return done(id);
}

// ---- SongSections: the playlist

EditResult SongSections::plan_append(const PlanItem& item) {
    if (index_of(*this, item.section) < 0) {
        return fail(EditError::NotFound);
    }
    if (plan.size() >= kMaxPlanItems) {
        return fail(EditError::TooMany);
    }
    plan.push_back(sanitized(item));
    return done(item.section);
}

EditResult SongSections::plan_remove(std::size_t index) {
    if (index >= plan.size()) {
        return fail(EditError::NotFound);
    }
    SectionId id = plan[index].section;
    plan.erase(plan.begin() + static_cast<std::ptrdiff_t>(index));
    return done(id);
}

EditResult SongSections::plan_move(std::size_t from, std::size_t to) {
    if (from >= plan.size() || to >= plan.size()) {
        return fail(EditError::NotFound);
    }
    SectionId id = plan[from].section;
    auto first = plan.begin();
    if (from < to) {
        std::rotate(first + static_cast<std::ptrdiff_t>(from),
                    first + static_cast<std::ptrdiff_t>(from) + 1,
                    first + static_cast<std::ptrdiff_t>(to) + 1);
    } else {
        std::rotate(first + static_cast<std::ptrdiff_t>(to),
                    first + static_cast<std::ptrdiff_t>(from),
                    first + static_cast<std::ptrdiff_t>(from) + 1);
    }
    return done(id);
}

EditResult SongSections::plan_set(std::size_t index, const PlanItem& item) {
    if (index >= plan.size() || index_of(*this, item.section) < 0) {
        return fail(EditError::NotFound);
    }
    plan[index] = sanitized(item);
    return done(item.section);
}

// ---- SongSections: checks and repairs

bool SongSections::valid(const Song& song) const {
    if (sections.size() > kMaxSectionsPerSong || plan.size() > kMaxPlanItems || !ids_ok(*this) ||
        !names_ok(*this)) {
        return false;
    }
    for (const PlanItem& item : plan) {
        if (!plan_item_ok(*this, item)) {
            return false;
        }
    }
    if (sections.empty()) {
        return true;
    }
    if (sections.front().start_s != 0.0 || sections.back().end_s != song_length(song)) {
        return false;
    }
    for (std::size_t i = 0; i < sections.size(); ++i) {
        const Section& s = sections[i];
        if (!(s.end_s - s.start_s >= kMinSectionSeconds)) {
            return false;
        }
        if (i + 1 < sections.size() && s.end_s != sections[i + 1].start_s) {
            return false;
        }
    }
    SongSections derived = *this;
    refresh(derived, song);
    return derived.sections == sections;
}

bool SongSections::well_formed() const {
    if (sections.size() > kMaxSectionsPerSong || plan.size() > kMaxPlanItems || !ids_ok(*this) ||
        !names_ok(*this)) {
        return false;
    }
    for (const PlanItem& item : plan) {
        if (!plan_item_ok(*this, item)) {
            return false;
        }
    }
    double previous_end = 0.0;
    for (const Section& s : sections) {
        if (!std::isfinite(s.start_s) || !std::isfinite(s.end_s) || s.start_s < previous_end ||
            s.end_s > kMaxSectionSeconds || !(s.end_s - s.start_s >= kMinSectionSeconds)) {
            return false;
        }
        previous_end = s.end_s;
    }
    return true;
}

void SongSections::normalize() {
    std::vector<Section> kept;
    std::unordered_set<SectionId> seen;
    SectionId highest = 0;
    // Non-finite times first: they would make the sort's comparison invalid.
    std::erase_if(sections, [](const Section& s) {
        return !std::isfinite(s.start_s) || !std::isfinite(s.end_s);
    });
    std::stable_sort(sections.begin(), sections.end(),
                     [](const Section& a, const Section& b) { return a.start_s < b.start_s; });
    double previous_end = 0.0;
    for (Section s : sections) {
        if (kept.size() >= kMaxSectionsPerSong) {
            break;
        }
        if (s.id == kNoSection || s.id > kMaxSectionId || s.start_s < previous_end ||
            s.end_s > kMaxSectionSeconds || !(s.end_s - s.start_s >= kMinSectionSeconds) ||
            !seen.insert(s.id).second) {
            continue;
        }
        s.name = sanitize_section_name(s.name);
        s.custom_name = s.custom_name && !s.name.empty();
        if (!s.custom_name) {
            s.name = "Section " + std::to_string(kept.size() + 1);
        }
        s.snapped_to_bars = false;
        s.first_measure = s.last_measure = 0;
        previous_end = s.end_s;
        highest = std::max(highest, s.id);
        kept.push_back(std::move(s));
    }
    sections = std::move(kept);
    next_id = std::clamp(next_id, highest + 1, kMaxSectionId + 1);

    std::vector<PlanItem> items;
    for (const PlanItem& item : plan) {
        if (items.size() >= kMaxPlanItems) {
            break;
        }
        if (index_of(*this, item.section) >= 0) {
            items.push_back(sanitized(item));
        }
    }
    plan = std::move(items);
}

void SongSections::fit_to_song(const Song& song) {
    normalize();
    const double length = song_length(song);
    std::vector<Section> kept;
    for (Section s : sections) {
        if (s.start_s >= length) {
            break; // sorted: nothing later fits either
        }
        s.end_s = std::min(s.end_s, length);
        if (s.end_s - s.start_s >= kMinSectionSeconds) {
            kept.push_back(std::move(s));
        }
    }
    // Gaps only ever grow a section, so the minimum length still holds afterwards.
    for (std::size_t i = 0; i + 1 < kept.size(); ++i) {
        kept[i].end_s = kept[i + 1].start_s;
    }
    if (!kept.empty()) {
        kept.front().start_s = 0.0;
        kept.back().end_s = length;
    }
    sections = std::move(kept);
    std::erase_if(plan, [&](const PlanItem& p) { return index_of(*this, p.section) < 0; });
    refresh(*this, song);
}

// ---- SectionLibrary

const SongSections* SectionLibrary::find(std::uint64_t song_hash) const {
    for (const SongSections& s : songs_) {
        if (s.song_hash == song_hash) {
            return &s;
        }
    }
    return nullptr;
}

SongSections* SectionLibrary::edit(std::uint64_t song_hash) {
    if (song_hash == 0) {
        return nullptr;
    }
    if (clock_ == std::numeric_limits<std::uint32_t>::max()) {
        // Renumber in use order so the counter never wraps.
        std::stable_sort(
            songs_.begin(), songs_.end(),
            [](const SongSections& a, const SongSections& b) { return a.last_used < b.last_used; });
        clock_ = 0;
        for (SongSections& s : songs_) {
            s.last_used = ++clock_;
        }
    }
    auto matches = [&](const SongSections& s) { return s.song_hash == song_hash; };
    auto it = std::find_if(songs_.begin(), songs_.end(), matches);
    if (it == songs_.end()) {
        songs_.emplace_back();
        songs_.back().song_hash = song_hash;
    }
    std::find_if(songs_.begin(), songs_.end(), matches)->last_used = ++clock_;
    trim(); // never removes the entry just used: its counter is the highest
    return &*std::find_if(songs_.begin(), songs_.end(), matches);
}

void SectionLibrary::put(SongSections entry) {
    if (entry.song_hash == 0) {
        return;
    }
    if (entry.sections.empty() && entry.plan.empty()) {
        erase(entry.song_hash);
        return;
    }
    clock_ = std::max(clock_, entry.last_used);
    for (SongSections& s : songs_) {
        if (s.song_hash == entry.song_hash) {
            s = std::move(entry);
            return;
        }
    }
    songs_.push_back(std::move(entry));
    trim();
}

void SectionLibrary::erase(std::uint64_t song_hash) {
    auto it = std::find_if(songs_.begin(), songs_.end(),
                           [&](const SongSections& s) { return s.song_hash == song_hash; });
    if (it == songs_.end()) {
        return;
    }
    stale_.push_back(std::string(kSectionsSectionPrefix) + content_hash_hex(song_hash));
    songs_.erase(it);
}

void SectionLibrary::trim() {
    while (songs_.size() > kMaxSongsWithSections) {
        auto oldest = std::min_element(
            songs_.begin(), songs_.end(),
            [](const SongSections& a, const SongSections& b) { return a.last_used < b.last_used; });
        stale_.push_back(std::string(kSectionsSectionPrefix) + content_hash_hex(oldest->song_hash));
        songs_.erase(oldest);
    }
}

} // namespace toonval
