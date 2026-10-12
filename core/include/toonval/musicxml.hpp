// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "toonval/song.hpp"

namespace toonval {

// Score files: MusicXML. Uncompressed `.musicxml`/`.xml`,
// compressed `.mxl`, and MuseScore's `.mscz`. Like load_midi(), every loader works on bytes the
// caller read (core has no file system), never throws, never reads outside the bytes, and gives
// ok == false with an error and an English message when the file is no use.

enum class ScoreFileError : std::uint8_t {
    None,
    NotXml,      // not XML at all, or XML with no readable root element
    NotZip,      // an .mxl or .mscz that is not a zip archive, or a damaged one
    NotScore,    // XML or zip that holds no score (another XML dialect, no score file in the zip)
    Unsupported, // a score format or version this loader does not read; the message says what to
                 // do (for .mscz: export to MusicXML from MuseScore)
    Corrupt,     // a score that is damaged or contradicts itself
    NoNotes,     // a readable score without a single playable note
    TooLarge,    // more bytes, entries, notes or bars than any real score needs (zip bomb guard)
};

// What the loader skipped or changed. Counts are over the whole file.
struct ScoreLoadStats {
    int grace_notes_dropped = 0; // grace notes have no length; dropped
    int cue_notes_dropped = 0;   // small cue notes show other instruments; they take time, no note
    int unpitched_dropped = 0;   // percussion on a staff line
    int unreadable_notes = 0;    // pitch missing, out of MIDI range, or no duration
    int ties_merged = 0;         // note heads joined into the note before them
    int unmatched_ties = 0;      // a tie stop with nothing to join (kept as a note of its own)
    int overlaps_split = 0;      // same pitch sounding again on one channel: the first one cut
    int duplicates_merged = 0;   // two notes of one pitch starting together on one channel
    int repeats_ignored = 0;     // repeat signs and endings: the score plays straight through
};

struct ScoreLoadResult {
    bool ok = false;
    ScoreFileError error = ScoreFileError::None;
    std::string message; // English detail for logs
    Song song;           // empty unless ok
    ScoreLoadStats stats;
    std::string title; // work or movement title from the file, may be empty (UTF-8)
};

// Parses MusicXML (partwise, or timewise converted to partwise) from memory.
//
// Mapping, each with a test in tests/musicxml_test.cpp:
// - `divisions` set the tick length; ticks per quarter is a common multiple of 480 and every
//   `divisions` value of the file (capped at 30240; above that positions are rounded to a tick).
// - Tempo: `<sound tempo>` wins, else `<metronome>` (beat unit and dots turned into quarter notes
//   per minute). Read from the first part that has any; default 120. Time signatures likewise from
//   the first part that has any; `3+2` beats are summed, only the first of several signatures in
//   one `<time>` is used.
// - Bars: one per `<measure>`, numbered 1, 2, 3 in file order (the `number` attribute is not
//   used: endings and split bars repeat numbers). A first bar marked implicit, numbered 0 in the
//   file, or shorter than its time signature while a second bar follows is a pickup, numbered 0.
// - Ties (`<tie>`, else `<tied>`) join into one note of the summed length when the pitch, staff
//   and voice match and the next head starts where the last ended. A tie stop with nothing to join
//   is kept as a note of its own.
// - Chords (`<chord/>`), `<backup>` and `<forward>` (several voices) place notes at their
//   position in the bar.
// - Grace notes are dropped, since they have no length and would need time taken from a neighbour.
//   Cue notes and unpitched (percussion) notes take their time but give no note. Rests move time.
// - Hands: in a part with two or more staves, staff 1 is the Right hand and the others the Left.
//   A part with one staff gets the hand its name says (hand_from_name), else Unknown.
// - Fingering: the first digit 1..5 of `<technical><fingering>` goes to Note::finger.
// - Transposing instruments: `<transpose>` is applied (chromatic and octave-change), so notes are
//   at sounding (concert) pitch. This is the pitch MIDI would carry. The song's own
//   transposition stays 0.
// - Dynamics: pp..ff marks, sf/sfz/fp, `<sound dynamics>` and a note's `dynamics` attribute give
//   Note::velocity (ppp 16, pp 33, p 49, mp 64, mf 80, f 96, ff 112, fff 126). A mark holds
//   in its part until the next. The default is mf.
// - Repeats, endings (voltas), D.C./D.S. and codas are ignored: the score plays once, straight
//   through (stats.repeats_ignored counts the signs). Wedges, pedal and articulations are ignored.
// - Parts: one Song::Part per part and staff (track in file order, channel per MusicXML part
//   skipping 10), named after the part, with `midi-program` as the program when given.
//   The same pitch overlapping on one channel is cut or merged as load_midi() does.
ScoreLoadResult load_musicxml(const std::uint8_t* data, std::size_t size);
inline ScoreLoadResult load_musicxml(std::span<const std::uint8_t> data) {
    return load_musicxml(data.data(), data.size());
}

// Parses a compressed MusicXML file: a zip whose META-INF/container.xml names the score
// (the first rootfile); without a usable container, the first `.musicxml` or `.xml` entry outside
// META-INF. Zip limits: kMaxZipEntries entries, kMaxZipEntryBytes for the score and
// kMaxZipTotalBytes declared for all entries; beyond them ScoreFileError::TooLarge.
ScoreLoadResult load_mxl(const std::uint8_t* data, std::size_t size);
inline ScoreLoadResult load_mxl(std::span<const std::uint8_t> data) {
    return load_mxl(data.data(), data.size());
}

// Reads a MuseScore 3 or 4 file: a zip with an embedded `.mscx`. Best effort for the notes: pitch,
// length, dots, tuplets, ties, chords, voices, staves (two staves in a part make Right and Left),
// tempo, time signatures, fingering and dynamics. Grace notes are dropped as above. MuseScore's
// format changes between versions, so a file this cannot read gives ScoreFileError::Unsupported
// and a message that says to export to MusicXML from MuseScore, which is the reliable path.
ScoreLoadResult load_mscz(const std::uint8_t* data, std::size_t size);
inline ScoreLoadResult load_mscz(std::span<const std::uint8_t> data) {
    return load_mscz(data.data(), data.size());
}

// Reads a bare `.mscx` (MuseScore's XML, uncompressed).
ScoreLoadResult load_mscx(const std::uint8_t* data, std::size_t size);

// What a file's first bytes say it is.
enum class SongFileFormat : std::uint8_t { Unknown, Midi, Xml, Zip };
SongFileFormat sniff_song_format(const std::uint8_t* data, std::size_t size);
inline SongFileFormat sniff_song_format(std::span<const std::uint8_t> data) {
    return sniff_song_format(data.data(), data.size());
}

// Loads a score file whatever its extension: XML is MusicXML (or a bare .mscx, told by its root),
// a zip is .mxl or .mscz (told by what is inside). Anything else is NotScore.
ScoreLoadResult load_score(const std::uint8_t* data, std::size_t size);
inline ScoreLoadResult load_score(std::span<const std::uint8_t> data) {
    return load_score(data.data(), data.size());
}

inline constexpr std::size_t kMaxZipEntries = 1000;
inline constexpr std::size_t kMaxZipEntryBytes = 64u * 1024 * 1024;
inline constexpr std::size_t kMaxZipTotalBytes = 256u * 1024 * 1024;
inline constexpr std::size_t kMaxScoreXmlBytes = kMaxZipEntryBytes;
inline constexpr std::size_t kMaxScoreNotes = 1'000'000;

const char* to_string(ScoreFileError error);

} // namespace toonval
