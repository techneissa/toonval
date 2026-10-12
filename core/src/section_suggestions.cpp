// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/section_suggestions.hpp"

#include <algorithm>
#include <cmath>

namespace toonval {

namespace {

// A stretch of bars by index into Song::measures, both ends included.
struct Piece {
    std::size_t first = 0;
    std::size_t last = 0;
    bool trouble = false;
    double heat = 0.0;
    TroubleReason reason = TroubleReason::MissedNotes;

    std::size_t bars() const { return last - first + 1; }
};

// The bars where a phrase may start, and where the count of bars restarts.
struct Grid {
    std::vector<bool> forced;            // a new time signature starts here
    std::vector<std::size_t> count_from; // the bar the phrase count starts from, per bar
};

Grid make_grid(const Song& song) {
    const auto& bars = song.measures;
    Grid grid;
    grid.forced.assign(bars.size(), false);
    grid.count_from.assign(bars.size(), 0);
    std::size_t from = 0;
    for (std::size_t i = 0; i < bars.size(); ++i) {
        if (i == 0) {
            from = bars[0].number == 0 && bars.size() > 1 ? 1 : 0; // the pickup joins bar 1
        } else if (bars[i].numerator != bars[i - 1].numerator ||
                   bars[i].denominator != bars[i - 1].denominator) {
            grid.forced[i] = true;
            from = i;
        }
        grid.count_from[i] = from;
    }
    return grid;
}

// Cuts bars first..last into phrases of `phrase` bars on the grid; a piece under half a phrase
// joins its neighbour in the stretch.
void add_phrases(std::vector<Piece>& out, const Grid& grid, std::size_t first, std::size_t last,
                 std::size_t phrase) {
    const std::size_t min_bars = (phrase + 1) / 2;
    std::vector<Piece> local;
    Piece current{first, first};
    for (std::size_t i = first + 1; i <= last; ++i) {
        bool cut =
            grid.forced[i] || (i > grid.count_from[i] && (i - grid.count_from[i]) % phrase == 0);
        if (cut) {
            local.push_back(current);
            current = Piece{i, i};
        } else {
            current.last = i;
        }
    }
    local.push_back(current);

    std::vector<Piece> merged;
    for (const Piece& p : local) {
        if (!merged.empty() && p.bars() < min_bars) {
            merged.back().last = p.last;
        } else {
            merged.push_back(p);
        }
    }
    if (merged.size() > 1 && merged.front().bars() < min_bars) {
        merged[1].first = merged[0].first;
        merged.erase(merged.begin());
    }
    out.insert(out.end(), merged.begin(), merged.end());
}

double length_of(const Song& song) {
    return std::isfinite(song.duration_s) && song.duration_s > 0.0 ? song.duration_s : 0.0;
}

double piece_start(const Song& song, const Piece& p) {
    return p.first == 0 ? 0.0 : song.measures[p.first].start_s;
}

double piece_end(const Song& song, const Piece& p) {
    return p.last + 1 < song.measures.size() ? song.measures[p.last + 1].start_s : length_of(song);
}

std::vector<Piece> build(const Song& song, const Grid& grid, const std::vector<Piece>& spots,
                         std::size_t phrase) {
    std::vector<Piece> pieces;
    std::size_t next = 0; // first bar not yet covered
    for (const Piece& spot : spots) {
        if (spot.first > next) {
            add_phrases(pieces, grid, next, spot.first - 1, phrase);
        }
        pieces.push_back(spot);
        next = spot.last + 1;
    }
    if (next < song.measures.size()) {
        add_phrases(pieces, grid, next, song.measures.size() - 1, phrase);
    }
    // Bars shorter than a section can be (a tempo in the thousands) would make pieces the
    // sections could not hold: join them to the piece before, or the one after for the first.
    for (std::size_t i = 0; i < pieces.size() && pieces.size() > 1;) {
        if (piece_end(song, pieces[i]) - piece_start(song, pieces[i]) >= kMinSectionSeconds) {
            ++i;
        } else if (i > 0) {
            pieces[i - 1].last = pieces[i].last;
            pieces.erase(pieces.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            pieces[1].first = pieces[0].first;
            pieces.erase(pieces.begin());
        }
    }
    return pieces;
}

} // namespace

SectionSuggestions suggest_sections(const Song& song, std::span<const RunRecord> runs,
                                    std::uint64_t song_hash, const SuggestOptions& options) {
    SectionSuggestions result;
    const auto& bars = song.measures;
    if (bars.size() < 2 || length_of(song) < 2 * kMinSectionSeconds) {
        return result;
    }

    // The trouble spots that fit this song's bars, in time order, none touching another.
    std::vector<Piece> spots;
    if (song_hash != 0 && !runs.empty()) {
        TroublePlan plan = trouble_plan(runs, song_hash, options.trouble);
        auto index_of = [&](int number) -> std::ptrdiff_t {
            std::ptrdiff_t i = static_cast<std::ptrdiff_t>(number) - bars.front().number;
            if (i < 0 || static_cast<std::size_t>(i) >= bars.size() ||
                bars[static_cast<std::size_t>(i)].number != number) {
                return -1;
            }
            return i;
        };
        for (const TroubleSpot& spot : plan.spots) {
            std::ptrdiff_t a = index_of(spot.first_measure);
            std::ptrdiff_t b = index_of(spot.last_measure);
            if (a < 0 || b < a) {
                continue;
            }
            Piece p{static_cast<std::size_t>(a), static_cast<std::size_t>(b), true, spot.heat,
                    spot.reason};
            if (piece_end(song, p) - piece_start(song, p) >= kMinSectionSeconds) {
                spots.push_back(p);
            }
        }
        std::sort(spots.begin(), spots.end(),
                  [](const Piece& x, const Piece& y) { return x.first < y.first; });
        for (std::size_t i = 1; i < spots.size();) {
            if (spots[i].first <= spots[i - 1].last) {
                spots.erase(spots.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                ++i;
            }
        }
        if (spots.size() > kMaxSectionsPerSong / 2 - 1) {
            spots.resize(kMaxSectionsPerSong / 2 - 1);
        }
    }

    std::size_t phrase =
        options.phrase_bars >= 1
            ? static_cast<std::size_t>(std::min(options.phrase_bars, kMaxPhraseBars))
            : (bars.size() > static_cast<std::size_t>(kShortSongBars) ? 8 : 4);
    const Grid grid = make_grid(song);
    std::vector<Piece> pieces = build(song, grid, spots, phrase);
    while (pieces.size() > kMaxSectionsPerSong && phrase < bars.size()) {
        phrase *= 2;
        pieces = build(song, grid, spots, phrase);
    }
    if (pieces.size() < 2 || pieces.size() > kMaxSectionsPerSong) {
        return result; // one section is no suggestion
    }

    result.phrase_bars = static_cast<int>(phrase);
    for (const Piece& p : pieces) {
        SuggestedSection s;
        s.kind = p.trouble ? SuggestionKind::Trouble : SuggestionKind::Phrase;
        s.first_measure = bars[p.first].number;
        s.last_measure = bars[p.last].number;
        s.start_s = piece_start(song, p);
        s.end_s = piece_end(song, p);
        s.heat = p.heat;
        s.reason = p.reason;
        result.from_history = result.from_history || p.trouble;
        result.sections.push_back(s);
    }
    return result;
}

int apply_suggestions(SongSections& sections, const Song& song,
                      const SectionSuggestions& suggestions, bool trouble_only) {
    std::vector<double> times;
    for (const SuggestedSection& s : suggestions.sections) {
        if (trouble_only && s.kind != SuggestionKind::Trouble) {
            continue;
        }
        times.push_back(s.start_s);
        times.push_back(s.end_s);
    }
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end()), times.end());
    int placed = 0;
    for (double t : times) {
        if (sections.add_marker(song, t, false).ok) {
            ++placed;
        }
    }
    return placed;
}

} // namespace toonval
