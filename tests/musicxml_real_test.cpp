// SPDX-License-Identifier: GPL-3.0-or-later
// A real piece, "Die Ehre Gottes aus der Natur" (Beethoven, Op. 48 No. 4, voice and piano), read
// from MusicXML (.mxl) and from the same score as MuseScore 3 and MuseScore 4 files, and checked
// against the MIDI file MuseScore 4 exports from the same MusicXML. See tests/data/README.md.
#include "toonval/musicxml.hpp"

#include "toonval/midi_file.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <string>
#include <tuple>
#include <vector>

using namespace toonval;

namespace {

using Bytes = std::vector<std::uint8_t>;

Bytes read_data(const std::string& name) {
    std::ifstream in(std::string(TOONVAL_TEST_DATA_DIR) + "/" + name, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "missing test data " << name);
    return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

constexpr const char* kMxl = "beethoven-op48-4-ehre-gottes.mxl";
constexpr const char* kMsz3 = "beethoven-op48-4-ehre-gottes-musescore3.mscz";
constexpr const char* kMsz4 = "beethoven-op48-4-ehre-gottes-musescore4.mscz";
constexpr const char* kMidi = "beethoven-op48-4-ehre-gottes-musescore-export.mid";

// Notes as (start tick, pitch, end tick), sorted. Both files use 480 ticks per quarter note.
struct Key {
    std::int64_t start;
    int pitch;
    std::int64_t end;
    bool operator<(const Key& o) const {
        return std::tie(start, pitch, end) < std::tie(o.start, o.pitch, o.end);
    }
    bool operator==(const Key& o) const {
        return start == o.start && pitch == o.pitch && end == o.end;
    }
};

// MuseScore plays an ornament in bar 6 (ticks 10080 to 10200) as extra and shifted notes in its
// MIDI export; the score holds the plain notes. Those are left out of the comparison.
std::vector<Key> keys_of(const Song& song) {
    std::vector<Key> out;
    for (const Note& n : song.notes) {
        if (n.start_tick >= 10080 && n.start_tick <= 10200) {
            continue;
        }
        out.push_back({n.start_tick, n.pitch, n.end_tick});
    }
    std::sort(out.begin(), out.end());
    return out;
}

// Notes that differ in start or pitch, or whose end differs by more than `end_slack` of the length
// of `a`. MuseScore ends a note early in a MIDI export: about 5% for a normal note, half for a
// staccato one. `b` may end earlier, never later.
int mismatches(const std::vector<Key>& a, const std::vector<Key>& b, double end_slack = 0.0) {
    REQUIRE_MESSAGE(a.size() == b.size(), "notes: " << a.size() << " vs " << b.size());
    int bad = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto length = static_cast<double>(a[i].end - a[i].start);
        const auto early = static_cast<double>(a[i].end - b[i].end);
        const bool end_ok = early >= 0.0 && early <= end_slack * length;
        if (a[i].start != b[i].start || a[i].pitch != b[i].pitch ||
            (a[i].end != b[i].end && !end_ok)) {
            if (bad < 5) {
                MESSAGE("note " << i << ": " << a[i].start << "/" << a[i].pitch << "/" << a[i].end
                                << " vs " << b[i].start << "/" << b[i].pitch << "/" << b[i].end);
            }
            ++bad;
        }
    }
    return bad;
}

} // namespace

TEST_CASE("real piece: the MusicXML notes match what MuseScore exports to MIDI") {
    const Bytes xml = read_data(kMxl);
    const ScoreLoadResult score = load_mxl(xml);
    REQUIRE_MESSAGE(score.ok, score.message);
    const Bytes midi = read_data(kMidi);
    const MidiLoadResult reference = load_midi(midi);
    REQUIRE(reference.ok);
    CHECK(score.song.tempo.ticks_per_quarter == reference.song.tempo.ticks_per_quarter);
    CHECK(score.song.notes.size() == 716);
    CHECK(reference.song.notes.size() == 716);
    // Pitch, start tick and end tick of every note: ties, chords, backup and the two staves of the
    // piano all have to come out as MuseScore plays them.
    CHECK(mismatches(keys_of(score.song), keys_of(reference.song), 0.52) == 0);
    // Bars: same count and same start ticks.
    REQUIRE(score.song.measures.size() == reference.song.measures.size());
    for (std::size_t i = 0; i < score.song.measures.size(); ++i) {
        CHECK(score.song.measures[i].start_tick == reference.song.measures[i].start_tick);
    }
}

TEST_CASE("real piece: structure of the MusicXML score") {
    const Bytes xml = read_data(kMxl);
    const ScoreLoadResult r = load_mxl(xml);
    REQUIRE_MESSAGE(r.ok, r.message);
    CHECK(r.title == "6 Lieder, Op.48: Die Ehre Gottes aus der Natur");
    // 80 BPM and 2/2.
    REQUIRE(r.song.tempo.changes.size() == 1);
    CHECK(r.song.tempo.changes[0].us_per_quarter == 750'000);
    REQUIRE(r.song.time_signatures.size() == 1);
    CHECK(r.song.time_signatures[0].numerator == 2);
    CHECK(r.song.time_signatures[0].denominator == 2);
    // The voice has no hand; the piano has both, from its two staves.
    REQUIRE(r.song.parts.size() == 3);
    CHECK(r.song.parts[0].name == "Singstimme Voice");
    CHECK(r.song.parts[0].hand == Hand::Unknown);
    CHECK(r.song.parts[1].hand == Hand::Right);
    CHECK(r.song.parts[2].hand == Hand::Left);
    int right = 0;
    int left = 0;
    for (const Note& n : r.song.notes) {
        right += n.hand == Hand::Right ? 1 : 0;
        left += n.hand == Hand::Left ? 1 : 0;
    }
    CHECK(right == 336);
    CHECK(left == 295);
    CHECK(r.stats.grace_notes_dropped == 3);
}

TEST_CASE("real piece: MuseScore 3 and MuseScore 4 files give the same notes as the MusicXML") {
    const Bytes xml = read_data(kMxl);
    const ScoreLoadResult reference = load_mxl(xml);
    REQUIRE(reference.ok);
    for (const char* name : {kMsz3, kMsz4}) {
        CAPTURE(name);
        const Bytes zip = read_data(name);
        const ScoreLoadResult r = load_mscz(zip);
        REQUIRE_MESSAGE(r.ok, r.message);
        CHECK(r.song.notes.size() == reference.song.notes.size());
        CHECK(r.song.measures.size() == reference.song.measures.size());
        if (r.song.notes.size() == reference.song.notes.size()) {
            CHECK(mismatches(keys_of(r.song), keys_of(reference.song)) == 0);
        }
        CHECK(std::abs(static_cast<int>(r.song.tempo.changes[0].us_per_quarter) - 750'000) < 100);
        int right = 0;
        int left = 0;
        for (const Note& n : r.song.notes) {
            right += n.hand == Hand::Right ? 1 : 0;
            left += n.hand == Hand::Left ? 1 : 0;
        }
        CHECK(right == 336);
        CHECK(left == 295);
        // The content sniffer finds the same file.
        CHECK(load_score(zip).song.notes.size() == r.song.notes.size());
    }
}
