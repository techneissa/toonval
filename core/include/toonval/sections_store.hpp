// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "toonval/sections.hpp"
#include "toonval/settings_doc.hpp"

#include <string>
#include <vector>

namespace toonval {

// The sections of every song, in the settings file (settings_doc.hpp). One document section per
// song, named after the content hash of the song's bytes, so a damaged entry costs one song and
// nothing else:
//
//   [sections.9f3a0c1b22d4e5f6]
//   version = 1
//   next_id = 3
//   used = 5
//   section.1 = 0 16 Intro
//   section.2 = 16.5 32
//   plan.1 = 2 4 1 1 60 5 100
//
// - The name is `sections.` plus exactly 16 lowercase hex digits (content_hash_hex). Anything else
//   after `sections.` is not an entry: it is reported and removed on the next write.
// - `version` is the layout of the entry (1). A higher number is a newer build's: the entry is
//   not loaded, is reported as NewerVersion, and stays in the file as it is.
// - `next_id`: the id the next new section gets (ids are never reused). `used`: the library's use
//   counter at the last edit, to forget the song used longest ago past kMaxSongsWithSections.
// - `section.<id> = <start_s> <end_s> [name]`: one line per section, in time order. Times are
//   seconds in the shortest text that reads back to the same number. The name follows after one
//   space and is the rest of the line; a section with no name has the default name for its
//   bars. Names are cleaned on reading (sanitize_section_name).
// - `plan.<n> = <section id> <repeats> <clean passes only 0|1> <speed-up on 0|1> <start> <step>
//   <max>`: the saved practice playlist, n = 1, 2, ... in play order.
// - Keys this build does not know, comments and malformed lines in an entry are kept as they are
//   when the entry is written again.
//
// Reading never fails and never throws. A line that does not parse is dropped and reported
// (BadValue); a repeated key reports DuplicateKey and the last one counts. What survives is passed
// through SongSections::normalize(): sorted, overlapping, too short, out-of-range and repeated
// sections dropped, at most kMaxSectionsPerSong sections and kMaxPlanItems playlist items, names
// cleaned. An entry left with no section and no playlist is dropped, unless it holds keys this
// build does not know: it is left in the file as it is (and write_sections keeps those keys when
// the last section is deleted). At most kMaxSongsWithSections entries load, the most recently used
// first. Times are checked against the song only when it loads: call
// SongSections::fit_to_song(song) on the entry then.

inline constexpr int kSectionsFormatVersion = 1;

// Reads every `sections.<hash>` entry of the document. The library's stale_names() lists the
// document sections that were unreadable or over the cap, which write_sections() deletes.
SectionLibrary read_sections(const SettingsDocument& doc,
                             std::vector<SettingsIssue>* issues = nullptr);

// Writes every song of the library into its entry, creating it when missing, and deletes the
// entries named by library.stale_names() (unless the library holds that song again). Entries of
// songs not in the library, and entries this build could not read but did not mark stale (a newer
// version), are not touched, nor is anything else in the document.
void write_sections(SettingsDocument& doc, const SectionLibrary& library);

// The names of the document sections that belong to this layer, for the `other_known_sections` of
// load_settings(): every section whose name starts with `sections.`.
std::vector<std::string> sections_section_names(const SettingsDocument& doc);

} // namespace toonval
