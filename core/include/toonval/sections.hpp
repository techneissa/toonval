// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "toonval/song.hpp"
#include "toonval/transport.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace toonval {

// Named sections of one song (a practice mode), keyed by the song's
// content hash so renaming or moving the file keeps them. Pure values and functions over a Song:
// no file system, no clock, no exceptions.
//
// The model is markers on the song overview. The sections of a song tile it: the first starts at
// 0, each one starts where the one before ends, and the last ends at the song's end (Song::
// duration_s). A song with no markers has no sections at all. Placing the first marker makes two
// sections, every later marker splits the section it lands in, and removing a marker merges the
// two sections around it. Every edit keeps these invariants (valid() checks them):
//
// - sections are sorted by time, touch end to start, and lie inside [0, duration_s];
// - each is at least kMinSectionSeconds long (the shortest A-B loop the transport takes);
// - there are at most kMaxSectionsPerSong, with distinct non-zero ids, names that are valid UTF-8
//   of at most kMaxSectionNameBytes without control characters, and the derived fields
//   (snapped_to_bars, first_measure, last_measure, auto names) match the song's bars;
// - every plan item names a section that exists.
//
// A failed edit changes nothing. Ids are never reused within a song (next_id only grows), so a
// playlist, a saved selection or a running plan survives renames, splits and merges, and an id
// that was deleted never comes back as a different section.

using SectionId = std::uint32_t;
inline constexpr SectionId kNoSection = 0;
// Ids fit an int, as SessionState keeps them.
inline constexpr SectionId kMaxSectionId = 2'000'000'000;

inline constexpr std::size_t kMaxSectionNameBytes = 64;
inline constexpr std::size_t kMaxSectionsPerSong = 64;
inline constexpr std::size_t kMaxSongsWithSections = 256;
inline constexpr std::size_t kMaxPlanItems = 64;
inline constexpr int kMaxPlanRepeats = 99;
// Saved files keep each song's sections in a document section called sections.<hash>.
inline constexpr std::string_view kSectionsSectionPrefix = "sections.";
// A section is at least as long as the shortest A-B loop, so it can always be looped.
inline constexpr double kMinSectionSeconds = kMinLoopSeconds;

// Name text as the app may show and save it: invalid UTF-8 bytes are dropped, control characters
// and every kind of white space (tab, newline, no-break space, line separator, C1 controls)
// become one space, runs of them collapse, the ends are trimmed, invisible format characters
// (zero width, bidi overrides, byte order mark) are removed, and the text is cut to
// kMaxSectionNameBytes at a character boundary. The result is always valid UTF-8, may be empty,
// and sanitizing it again changes nothing.
std::string sanitize_section_name(std::string_view text);

struct Section {
    SectionId id = kNoSection;
    // Always filled. Without custom_name it is the default name, rewritten whenever the section
    // changes: "Bars 9-16" (or "Bar 9") when it sits on bars, else "Section N" with N its
    // 1-based place in the song.
    std::string name;
    bool custom_name = false;
    double start_s = 0.0;
    double end_s = 0.0;
    // Both ends sit on bar starts (the song's end counts as the end of its last bar).
    bool snapped_to_bars = false;
    // Measure::number of the bar holding the start, and of the bar holding the last instant before
    // the end. Exact bounds when snapped_to_bars. Both 0 when the song has no bars.
    int first_measure = 0;
    int last_measure = 0;

    double length_s() const { return end_s - start_s; }
    friend bool operator==(const Section&, const Section&) = default;
};

// One step of a practice plan: a section, played repeats times in a row (see practice_plan.hpp).
struct PlanItem {
    SectionId section = kNoSection;
    // Passes before the plan moves on, 1..kMaxPlanRepeats; 0 stays on the section until skipped.
    int repeats = 1;
    // Count only passes the evaluator called clean.
    bool clean_passes_only = false;
    // Applied when the plan arrives at the section (a disabled policy keeps the speed the plan
    // started with).
    AutoSpeedUp speed_up;

    friend bool operator==(const PlanItem& a, const PlanItem& b) {
        return a.section == b.section && a.repeats == b.repeats &&
               a.clean_passes_only == b.clean_passes_only &&
               a.speed_up.enabled == b.speed_up.enabled &&
               a.speed_up.start_percent == b.speed_up.start_percent &&
               a.speed_up.step_percent == b.speed_up.step_percent &&
               a.speed_up.max_percent == b.speed_up.max_percent;
    }
};
// The same item with repeats in 0..kMaxPlanRepeats and the speed-up policy on the transport's
// grid: start and max 25..150 in steps of 5 with max >= start, step 5..25 in steps of 5.
PlanItem sanitized(const PlanItem& item);

enum class EditError : std::uint8_t {
    None,
    NotFound,    // no such section, or no marker to remove
    OutOfRange,  // a time that is NaN, or not inside the section being split
    AtMarker,    // there already is a marker (or the song's start or end) there
    TooShort,    // a section would be shorter than kMinSectionSeconds
    TooMany,     // kMaxSectionsPerSong (or kMaxPlanItems) reached, or the ids ran out
    NotAdjacent, // merge: the selected sections are not neighbours
    NotEnough,   // merge: fewer than two sections selected
    Overlaps,    // add_range: the range crosses an existing marker
};

struct EditResult {
    bool ok = false;
    EditError error = EditError::None;
    // The section made or kept: the new (right) piece of a split, the merged section, the section
    // that took over a deleted one, the section a rename or range names.
    SectionId id = kNoSection;
};

struct SongSections {
    std::uint64_t song_hash = 0;   // content_hash() of the song file; 0 means no identity
    std::vector<Section> sections; // see the invariants above
    std::vector<PlanItem> plan;    // the saved practice playlist, in play order
    SectionId next_id = 1;         // the id the next new section gets
    std::uint32_t last_used = 0;   // SectionLibrary's use counter at the last edit

    bool empty() const { return sections.empty(); }
    const Section* find(SectionId id) const;
    // The section holding a time: start <= seconds < end (the last one also holds its end).
    const Section* at(double seconds) const;
    // The loop of a section, for Transport::set_loop. ok is false for an unknown id.
    LoopSpan span_of(SectionId id) const;
    // The boundaries between sections, in time order: the markers drawn on the overview.
    std::vector<double> markers() const;

    // ---- Markers and sections. `snap_to_bars` moves a time to the nearest bar start first
    // (Transport's snap_to_measure; a song with no bars is not snapped).

    // Places a marker. With no sections yet this makes two (the song split at the marker);
    // otherwise it splits the section holding the time. The new id is the section after the marker.
    EditResult add_marker(const Song& song, double at_s, bool snap_to_bars = true);
    // Splits one section in two. The left piece keeps its id and name; `at_s` must land inside it.
    EditResult split(const Song& song, SectionId id, double at_s, bool snap_to_bars = true);
    // Makes [start_s, end_s] a section of its own (placing up to two markers) and names it when
    // `name` is not empty: "save this A-B loop". Fails with Overlaps when the range crosses a
    // marker, since that would swallow it; a range equal to an existing section just renames it.
    EditResult add_range(const Song& song, double start_s, double end_s, bool snap_to_bars = true,
                         std::string_view name = {});
    // Removes the marker nearest to `near_s`, merging the sections on both sides.
    // `absorbed`, when not null, gets the id of the section that was merged away (see merge).
    EditResult remove_marker(const Song& song, double near_s,
                             std::vector<SectionId>* absorbed = nullptr);
    // Merges sections that are neighbours in time into the earliest one (its id and, if custom,
    // its name stay). Plan items of the others move to it. Plan items that land next to each other
    // on the merged section (or next to an item that was already on it) collapse into one: it
    // keeps the larger repeat count (0, "until skipped", counts as the largest), counts only clean
    // passes if either item did, and keeps the speed-up of the first item. Items that are not next
    // to each other stay apart.
    // `absorbed`, when not null, gets the ids of the sections merged away, so a running
    // PracticePlan can follow (PracticePlan::sections_merged).
    EditResult merge(const Song& song, std::span<const SectionId> ids,
                     std::vector<SectionId>* absorbed = nullptr);
    // Deletes a section. The section before it takes over its time (the first section hands it
    // to the next one), so the sections still tile the song; deleting the only section leaves none.
    // Its plan items go. The result id is the section that took over, kNoSection if none did.
    EditResult erase(const Song& song, SectionId id);
    // Sets the name (sanitized). An empty result returns the section to its default name.
    EditResult rename(SectionId id, std::string_view name);

    // ---- The saved practice playlist.

    EditResult plan_append(const PlanItem& item);
    EditResult plan_remove(std::size_t index);
    // Moves the item at `from` so it ends up at index `to`.
    EditResult plan_move(std::size_t from, std::size_t to);
    EditResult plan_set(std::size_t index, const PlanItem& item);
    void plan_clear() { plan.clear(); }

    // ---- Binding to a song

    // True when every invariant above holds for `song`.
    bool valid(const Song& song) const;
    // Repairs damaged data (a file the user edited by hand, an old build's) so valid(song) holds:
    // sections are sorted, overlaps and too-short ones dropped, gaps closed, the first start moved
    // to 0 and the last end to the song's end, what lies past the song is cut, derived fields are
    // recomputed. Sections on a song with no length or no room for two are all dropped.
    void fit_to_song(const Song& song);
    // The checks that need no song: ids distinct and non-zero, sorted, non-overlapping, long
    // enough, sane names, plan items valid, counts capped, next_id above every id. Used when
    // reading a file, before the song is known. Gaps are allowed here and closed by fit_to_song.
    void normalize();
    bool well_formed() const;

    friend bool operator==(const SongSections&, const SongSections&) = default;
};

// The sections of every song the player has marked. Past kMaxSongsWithSections the song used
// longest ago is forgotten.
class SectionLibrary {
  public:
    const std::vector<SongSections>& songs() const { return songs_; }
    const SongSections* find(std::uint64_t song_hash) const;
    // The song's entry, created when missing, marked as just used. Past kMaxSongsWithSections the
    // song used longest ago is forgotten (never this one). Null for hash 0. The pointer is valid
    // until the next call that adds or removes a song.
    SongSections* edit(std::uint64_t song_hash);
    // Inserts or replaces an entry and applies the same cap. Hash 0 is ignored; an entry with no
    // sections and no playlist removes the song instead (it has nothing to keep).
    void put(SongSections entry);
    void erase(std::uint64_t song_hash);

    // Names of document sections the next write_sections() should delete: forgotten songs and
    // entries that could not be read (see sections_store.hpp).
    std::vector<std::string>& stale_names() { return stale_; }
    const std::vector<std::string>& stale_names() const { return stale_; }

  private:
    void trim();
    std::vector<SongSections> songs_;
    std::vector<std::string> stale_;
    std::uint32_t clock_ = 0;
};

} // namespace toonval
