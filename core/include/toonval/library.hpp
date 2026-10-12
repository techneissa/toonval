// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "toonval/keyboard.hpp"
#include "toonval/midi_file.hpp"
#include "toonval/musicxml.hpp"
#include "toonval/progress.hpp"
#include "toonval/song.hpp"

namespace toonval {

// The song library index behind the song browser. The app scans
// folders and hands core the bytes of each file and its name; core has no file system. Everything
// here is a pure function over those values and the run history.
//
// One SongEntry per file: what the browser shows (title, length, difficulty, best score) and what
// its filters and sorts need (pitch range, notes per hand, last played).

// ---- Text

// True when `text` is well-formed UTF-8: no overlong forms, no surrogates, nothing above U+10FFFF.
bool is_valid_utf8(std::string_view text);

// Text from a MIDI file or a file name as valid UTF-8. Many files are not UTF-8, and the format
// does not say what they are, so this is a best effort with this fallback order:
// 1. Valid UTF-8 is returned unchanged.
// 2. Shift-JIS, when every high byte fits Shift-JIS and at least one is kana (half-width
//    katakana, hiragana, katakana) or a full-width Latin letter or digit. Those are decoded by
//    arithmetic; kanji need a table of thousands of entries that core does not carry, so each
//    kanji becomes U+FFFD. A title made only of kanji is therefore unusable (choose_title skips
//    it).
// 3. Otherwise Windows-1252, a superset of Latin-1: each byte is one character, and the five
//    bytes 1252 leaves undefined (0x81, 0x8D, 0x8F, 0x90, 0x9D) become U+FFFD.
// The guess can be wrong: a Latin-1 word like "Café" followed by an ASCII letter looks like a
// Shift-JIS pair, but with no kana to confirm it step 3 wins; Shift-JIS text of only kanji decodes
// as Latin-1 mojibake. The result is always valid UTF-8.
std::string utf8_from_midi_text(std::string_view bytes);

// The longest title kept, as for run records.
inline constexpr std::size_t kMaxLibraryTitleBytes = kMaxRunTitleBytes;

// A display title from a file name: the folder is dropped ('/' or '\' separated), a known song
// extension (.mid .midi .kar .smf .rmi .musicxml .xml .mxl .mscz .mscx, any case) is dropped,
// underscores become spaces, runs of white space and control characters collapse to one space, the
// ends are trimmed, and the result is cut to kMaxLibraryTitleBytes at a character boundary. The
// name is decoded with utf8_from_midi_text first.
std::string title_from_file_name(std::string_view file_name);

// The name in the first track's Track Name event (meta 0x03), the "sequence name" of a format 0 or
// format 1 file, as the raw bytes; empty when there is none or the data is not a readable MIDI
// file. Reads only inside `bytes`.
std::string midi_sequence_name(std::span<const std::uint8_t> bytes);

// The title to show. The file name is what the owner chose, so it wins; when it is empty, generic
// ("untitled", "song", "midi", "new song", "track 1", "control track", only digits) or has no
// letter or digit, the sequence name stands in (cleaned the same way, with the same tests). If
// neither is usable the file name stays if it has anything, else "Untitled".
std::string choose_title(std::string_view file_name, std::string_view sequence_name);

// Lower case with accents removed, for search and for sorting by title. Handles ASCII, Latin-1
// Supplement and Latin Extended-A letters (Á->a, ß->ss, Æ->ae, Ł->l), combining marks (dropped),
// full-width Latin letters and digits, and plain Greek and Cyrillic lower casing. Other characters
// are kept as they are. Invalid UTF-8 is read as Latin-1 first (utf8_from_midi_text).
std::string fold_for_search(std::string_view text);

// True when every white-space separated word of `query` (folded the same way) occurs in
// `folded_title`. An empty query matches everything.
bool matches_search(std::string_view folded_title, std::string_view query);

// ---- Difficulty

// What the guess is made from. All are measured on the playable (non-drum) notes.
struct DifficultyFactors {
    // Distinct onsets (notes starting within kChordWindowSeconds of each other count as one) per
    // second, from the first onset to the last: (onsets - 1) / (last - first). 0 for fewer than
    // two onsets.
    double onsets_per_second = 0.0;
    // Notes per chord, where a chord is the notes one hand starts together (all notes together
    // when the hands are not known). 1 when every chord is a single note.
    double notes_per_onset = 1.0;
    // Mean width in semitones of the chords of two or more notes, 0 without chords.
    double mean_chord_span = 0.0;
    // Highest pitch minus lowest.
    double pitch_range = 0.0;
};

// The levels 1 (easy) to 5 (hard):
//   score = 0.40 * clamp((onsets_per_second - 0.5) / 6.5)     busy music: 7 onsets a second is full
//         + 0.25 * clamp((notes_per_onset - 1) / 2.5)        chords: 3.5 notes a chord is full
//         + 0.20 * clamp(mean_chord_span / 14)               a hand stretched over 14 semitones
//         + 0.15 * clamp((pitch_range - 12) / 48)            an octave is free, 5 octaves full
//   level = 1 + min(4, floor(5 * score))
// clamp keeps a term in 0..1. Each term never falls as its factor rises, so the level never falls
// either. Speed weighs most because it is what makes a falling-note song hard to read in time.
int difficulty_level(const DifficultyFactors& factors);

// ---- Keyboard fit

enum class KeyFit : std::uint8_t {
    Fits,          // every note is on a key as written
    FitsWithShift, // a transposition of at most an octave puts every note on a key
    TooWide,       // no shift of up to an octave does: the song is wider than the keyboard, or
                   // sits further away. Playing it folds notes by octaves (see transpose.hpp).
};

struct KeyboardFit {
    KeyFit fit = KeyFit::Fits;
    // The shift in semitones (the smallest in magnitude that fits; -12..12) for FitsWithShift,
    // otherwise 0. Pass it as the transposition of Transposition.
    int semitones = 0;
    bool fits() const { return fit != KeyFit::TooWide; }
};
// Whether notes from `lowest` to `highest` fit `range`; a negative `lowest` (no notes) fits.
KeyboardFit fit_to_keyboard(int lowest, int highest, KeyRange range);

// ---- Entries

struct SongEntry {
    std::uint64_t hash =
        0;                 // content_hash() of the file's bytes: the song's identity in the history
    std::string title;     // for display
    std::string title_key; // fold_for_search(title): for sorting and searching
    double duration_s = 0.0;
    int bar_count = 0;
    int note_count = 0; // playable (non-drum) notes
    int left_notes = 0;
    int right_notes = 0;
    int unknown_notes = 0; // no hand: the song needs the hand split (hand_split.hpp)
    int lowest_pitch = -1; // -1 when there are no playable notes
    int highest_pitch = -1;
    DifficultyFactors factors;
    int difficulty = 1; // difficulty_level(factors)

    // From the run history (attach_history).
    bool has_best = false;
    BestEntry best;          // the best accuracy over every mode, hands and speed
    int run_count = 0;       // runs of this song, repeats left out
    int last_run_index = -1; // position in the history of the latest run, -1 if never played
    std::int32_t last_run_day = 0;

    bool hands_known() const { return unknown_notes == 0; }
    bool played() const { return run_count > 0; }
};

// The fit of a song's notes on a keyboard.
inline KeyboardFit fit_to_keyboard(const SongEntry& entry, KeyRange range) {
    return fit_to_keyboard(entry.lowest_pitch, entry.highest_pitch, range);
}

// The entry for a loaded song: `bytes` are the file's contents (for the hash and the sequence name)
// and `file_name` its name as the folder scan saw it (may be empty).
SongEntry make_song_entry(const Song& song, std::span<const std::uint8_t> bytes,
                          std::string_view file_name);
// The same for a score file: `score_title` is the work or movement title the file names (it stands
// in for the MIDI sequence name in choose_title).
SongEntry make_song_entry(const Song& song, std::span<const std::uint8_t> bytes,
                          std::string_view file_name, std::string_view score_title);

struct LibraryLoad {
    bool ok = false;
    SongFileFormat format = SongFileFormat::Unknown;   // what the bytes are, by content
    MidiFileError error = MidiFileError::None;         // NotMidi when the bytes are a score file
    ScoreFileError score_error = ScoreFileError::None; // set when a score file failed to load
    std::string message;
    SongEntry entry; // empty unless ok
};
// Loads a song file and makes its entry, for the scan. The format comes from the bytes, not the
// file name: "MThd" is a Standard MIDI File (load_midi), XML is MusicXML or MuseScore XML and a zip
// is .mxl or .mscz (load_score). Bytes that are none of these report the MIDI error. Never throws.
LibraryLoad load_library_entry(std::span<const std::uint8_t> bytes, std::string_view file_name);

// Sets the history fields of `entry` from `runs` (oldest first, as progress.hpp expects), replacing
// what an earlier call set. The best is the highest accuracy over song_bests(), a tie going to the
// higher speed; a song with no run of at least kMinGradedForBest graded notes has none.
void attach_history(SongEntry& entry, std::span<const RunRecord> runs);

// ---- Browsing

enum class SortKey : std::uint8_t {
    Title,          // ascending A to Z
    Difficulty,     // ascending: easy first
    RecentlyPlayed, // ascending: the oldest run first, descending: the latest first; songs never
                    // played always come last
    Length,         // ascending: short first
};

struct LibrarySort {
    SortKey key = SortKey::Title;
    bool descending = false; // reverses the key; ties always go by title A to Z, then by hash
};

enum class KeyboardFilter : std::uint8_t {
    Any,
    FitsAsIs,      // every note on a key as written
    FitsWithShift, // fits as written or with a shift of up to an octave
};

enum class PlayedFilter : std::uint8_t { Any, PlayedOnly, UnplayedOnly };

struct LibraryFilter {
    std::string text; // matches_search against the title; empty matches all
    int min_difficulty = 1;
    int max_difficulty = 5; // an empty range (min > max) matches nothing
    KeyboardFilter keyboard = KeyboardFilter::Any;
    KeyRange range = kRange61; // the player's keyboard, used by `keyboard`
    PlayedFilter played = PlayedFilter::Any;
};

bool matches_filter(const SongEntry& entry, const LibraryFilter& filter);

// Indices into `entries` of the songs that pass the filter, in sort order. Does not copy or change
// the entries.
std::vector<std::size_t> browse_library(std::span<const SongEntry> entries,
                                        const LibraryFilter& filter = {},
                                        const LibrarySort& sort = {});

} // namespace toonval
