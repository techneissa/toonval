// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "toonval/hand.hpp"
#include "toonval/song.hpp"
#include "toonval/transpose.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace toonval {

enum class HandFilter : std::uint8_t { Both, Left, Right };

struct NoteWindowOptions {
    double ahead_s = 4.0;   // song seconds above the hit line: the top of the note area is y = 1
    double behind_s = 0.25; // song seconds kept below the hit line, so a note leaves smoothly
    HandFilter hand = HandFilter::Both; // Both also shows notes whose hand is Unknown
    bool include_drums = false;         // channel-10 notes are for playback, not for learning
    Transposition transposition;        // the pitch each VisibleNote shows
};

// A note to draw. y is a fraction of the note area: 0 is the hit line, 1 the top of the area, and a
// note that has passed the line has y below 0. Not clipped: `top` can be above 1 and `bottom` below
// 0 for a note that starts above the area or is still sounding, so the renderer can tell. A note
// reaches the hit line (bottom == 0) at its start time.
//
// `pitch`, `folded`, `fold_octaves` and `playable` are the Transposition's answer for the note (see
// TransposedPitch): the key to draw it on, and the marker for a note moved by octaves. A note the
// keyboard can't play stays in the list with `playable` false and `pitch` -1.
struct VisibleNote {
    std::uint32_t index = 0; // into Song::notes
    double bottom = 0.0;     // y of the note's start
    double top = 0.0;        // y of the note's end
    int pitch = -1;
    bool folded = false;
    int fold_octaves = 0;
    bool playable = true;
};

// Finds the notes on screen for a song position without scanning the song: an interval tree over
// the notes sorted by start, so a query costs O(log n + notes on screen) however long the notes
// before it are. Copies what it needs from the Song, so it does not keep a reference to it.
class NoteWindow {
  public:
    NoteWindow() = default;
    explicit NoteWindow(const Song& song) { build(song); }

    void build(const Song& song);
    std::size_t size() const { return entries_.size(); }

    // Replaces the contents of `out` with the notes that overlap
    // (position_s - behind_s, position_s + ahead_s), in order of start. Returns how many index
    // entries the query looked at, for tests and profiling. An `ahead_s` of 0 or less gives none.
    std::size_t query(double position_s, const NoteWindowOptions& options,
                      std::vector<VisibleNote>& out) const;

  private:
    struct Entry {
        double start_s = 0.0;
        double end_s = 0.0;
        std::uint32_t index = 0;
        std::uint8_t pitch = 0;
        Hand hand = Hand::Unknown;
        bool drum = false;
    };
    struct Query {
        double lo; // the window: notes ending after `lo` and starting before `hi`
        double hi;
        double position; // the hit line, for y
        const NoteWindowOptions& options;
    };
    double build_range(std::size_t first, std::size_t last);
    void visit(std::size_t first, std::size_t last, const Query& q, std::vector<VisibleNote>& out,
               std::size_t& examined) const;

    std::vector<Entry> entries_;  // sorted by start
    std::vector<double> max_end_; // per entry: the latest end in the subtree rooted there
};

} // namespace toonval
