// SPDX-License-Identifier: GPL-3.0-or-later
// Internal to core: what the MusicXML and MuseScore readers hand to one shared assembler, which
// builds the Song. Positions are bar index plus tick offset inside the bar, because a bar's start
// is only known once every part has been read (a bar is as long as its longest part says).
#pragma once

#include "toonval/musicxml.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace pugi {
class xml_document;
}

namespace toonval::score_detail {

inline constexpr std::int64_t kMaxEventTicks = 1'000'000'000; // one duration or offset, at most
inline constexpr int kMaxTracks = 4000;
inline constexpr int kDefaultVelocity = 80; // mf

struct RawPart {
    std::string name;
    std::string instrument_name;
    int program = -1; // 0-based General MIDI program, -1 for none given
};

// One staff that has notes: a Song part (track).
struct RawTrack {
    int part = 0;
    std::string name;
    Hand hand = Hand::Unknown;
};

struct RawNote {
    int measure = 0;
    std::int64_t offset = 0; // ticks from the bar start
    std::int64_t length = 0; // ticks, ties already joined
    std::uint8_t pitch = 60;
    std::uint8_t velocity = 0; // 0: take it from the dynamics of the part
    std::int8_t finger = 0;
    int track = 0; // index into RawScore::tracks
};

struct RawMeasure {
    std::int64_t extent = 0; // longest position any part reached in this bar
    bool implicit = false;
    bool has_signature = false;
    int numerator = 4;
    int denominator = 4;
};

struct RawTempo {
    int part = 0;
    int measure = 0;
    std::int64_t offset = 0;
    double bpm = 120.0; // quarter notes per minute
};

struct RawDynamic {
    int part = 0;
    int measure = 0;
    std::int64_t offset = 0;
    int velocity = kDefaultVelocity;
};

struct RawScore {
    int ppq = 480;
    std::string title;
    std::vector<RawPart> parts;
    std::vector<RawTrack> tracks;
    std::vector<RawNote> notes;
    std::vector<RawMeasure> measures;
    std::vector<RawTempo> tempos;
    std::vector<RawDynamic> dynamics;
    ScoreLoadStats stats;
};

// Joins the pieces of a tied note into one RawNote. Keys are staff, voice and pitch.
class TieJoiner {
  public:
    using Key = std::tuple<int, int, int>;

    // A piece of a note at (measure, offset) of `length` ticks. If it is a tie stop and the note it
    // continues ended right where this piece starts (in the same bar, or at the start of the
    // next), that note grows by `length` and this returns true: the caller adds no note. A stop
    // with nothing to join is counted in stats.unmatched_ties and returns false.
    bool join(RawScore& raw, const Key& key, int measure, std::int64_t offset, std::int64_t length,
              bool stop, bool start);

    // Call after pushing the note for a piece with `start` set (and not joined).
    void open(const Key& key, std::size_t note, int end_measure, std::int64_t end_offset) {
        open_[key] = {note, end_measure, end_offset};
    }

  private:
    struct Open {
        std::size_t note = 0;
        int end_measure = 0;
        std::int64_t end_offset = 0;
    };
    std::map<Key, Open> open_;
};

ScoreLoadResult make_error(ScoreFileError error, std::string message);

// Builds the Song: bar starts, tempo map, velocities from dynamics, overlap repair, parts.
ScoreLoadResult assemble_score(RawScore& raw);

// Readers. Each returns a finished result.
ScoreLoadResult score_from_musicxml(const pugi::xml_document& doc);
ScoreLoadResult score_from_mscx(const pugi::xml_document& doc);

// A name on one line: runs of white space (line breaks too) become one space, the ends are trimmed.
std::string clean_name(const char* text);

// Text to numbers without locale or exceptions. Both skip leading white space; the double reader
// takes a sign, digits and an optional fraction. False when there is no number.
bool parse_int(const char* text, long long& out);
bool parse_double(const char* text, double& out, bool first_number_in_text = false);

// Parses XML bytes. Returns false with a NotXml or TooLarge error in `error` when they are no use.
bool parse_xml_bytes(const std::uint8_t* data, std::size_t size, pugi::xml_document& doc,
                     ScoreLoadResult& error);

} // namespace toonval::score_detail
