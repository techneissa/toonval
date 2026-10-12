// SPDX-License-Identifier: GPL-3.0-or-later
// The score loaders get damaged, truncated and hostile input: they must return (ok or an error),
// never crash, and a Song they return must be well formed. Run under ASan and UBSan for the memory
// checks; the assertions here check the Song's shape.
#include "toonval/musicxml.hpp"

#include "zip_builder.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace toonval;

namespace {

using Bytes = std::vector<std::uint8_t>;

// xorshift64*: a fixed sequence, so a failure replays.
class Rng {
  public:
    explicit Rng(std::uint64_t seed) : state_(seed * 2685821657736338717ull + 1) {}
    std::uint64_t next() {
        state_ ^= state_ >> 12;
        state_ ^= state_ << 25;
        state_ ^= state_ >> 27;
        return state_ * 2685821657736338717ull;
    }
    std::size_t below(std::size_t n) { return n == 0 ? 0 : static_cast<std::size_t>(next() % n); }

  private:
    std::uint64_t state_;
};

Bytes mutate(const Bytes& in, Rng& rng) {
    Bytes out = in;
    const std::size_t edits = 1 + rng.below(8);
    for (std::size_t i = 0; i < edits && !out.empty(); ++i) {
        const std::size_t at = rng.below(out.size());
        switch (rng.below(5)) {
        case 0:
            out[at] = static_cast<std::uint8_t>(rng.next());
            break;
        case 1:
            out[at] ^= static_cast<std::uint8_t>(1u << rng.below(8));
            break;
        case 2:
            out.erase(out.begin() + static_cast<std::ptrdiff_t>(at));
            break;
        case 3:
            out.insert(out.begin() + static_cast<std::ptrdiff_t>(at),
                       static_cast<std::uint8_t>(rng.next()));
            break;
        default:
            out.resize(at);
            break; // cut the tail off
        }
    }
    return out;
}

// What every Song returned by a loader must satisfy.
void expect_well_formed(const ScoreLoadResult& r) {
    if (!r.ok) {
        CHECK(r.error != ScoreFileError::None);
        CHECK_FALSE(r.message.empty());
        CHECK(r.song.notes.empty());
        return;
    }
    const Song& s = r.song;
    REQUIRE_FALSE(s.notes.empty());
    REQUIRE_FALSE(s.measures.empty());
    REQUIRE_FALSE(s.time_signatures.empty());
    CHECK(s.tempo.ticks_per_quarter > 0);
    std::int64_t last_start = 0;
    for (const Note& n : s.notes) {
        REQUIRE(n.end_tick > n.start_tick);
        REQUIRE(n.duration_s > 0.0);
        REQUIRE(n.start_tick >= last_start);
        last_start = n.start_tick;
        REQUIRE(n.pitch <= 127);
        REQUIRE(n.velocity >= 1);
        REQUIRE(n.velocity <= 127);
        REQUIRE(n.finger >= 0);
        REQUIRE(n.finger <= 5);
        REQUIRE(n.channel <= 15);
        REQUIRE(n.track < s.parts.size() + 1);
    }
    for (std::size_t i = 1; i < s.measures.size(); ++i) {
        REQUIRE(s.measures[i].start_tick > s.measures[i - 1].start_tick);
        REQUIRE(s.measures[i].number == s.measures[i - 1].number + 1);
    }
    for (const TempoChange& t : s.tempo.changes) {
        REQUIRE(t.us_per_quarter >= 1);
    }
    for (const Part& p : s.parts) {
        CHECK(p.note_count > 0);
    }
}

Bytes read_data(const std::string& name) {
    std::ifstream in(std::string(TOONVAL_TEST_DATA_DIR) + "/" + name, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "missing test data " << name);
    return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string small_score() {
    return "<?xml version=\"1.0\"?><score-partwise><part-list><score-part id=\"P1\">"
           "<part-name>Piano</part-name></score-part></part-list><part id=\"P1\">"
           "<measure number=\"1\"><attributes><divisions>2</divisions><staves>2</staves>"
           "<time><beats>3</beats><beat-type>4</beat-type></time></attributes>"
           "<direction><direction-type><dynamics><f/></dynamics></direction-type>"
           "<sound tempo=\"90\"/></direction>"
           "<note><pitch><step>C</step><octave>4</octave></pitch><duration>2</duration>"
           "<tie type=\"start\"/><voice>1</voice><staff>1</staff>"
           "<notations><technical><fingering>1</fingering></technical></notations></note>"
           "<note><chord/><pitch><step>E</step><alter>-1</alter><octave>4</octave></pitch>"
           "<duration>2</duration><voice>1</voice><staff>1</staff></note>"
           "<backup><duration>2</duration></backup>"
           "<note><pitch><step>G</step><octave>2</octave></pitch><duration>4</duration>"
           "<voice>2</voice><staff>2</staff></note></measure>"
           "<measure number=\"2\"><note><pitch><step>C</step><octave>4</octave></pitch>"
           "<duration>6</duration><tie type=\"stop\"/><voice>1</voice><staff>1</staff></note>"
           "<barline><repeat direction=\"backward\"/></barline></measure>"
           "</part></score-partwise>";
}

std::string small_mscx() {
    return "<museScore version=\"3.02\"><Score><Division>480</Division><Part><Staff id=\"1\">"
           "<StaffType group=\"pitched\"/></Staff><trackName>Piano</trackName></Part>"
           "<Staff id=\"1\"><Measure><voice><TimeSig><sigN>4</sigN><sigD>4</sigD></TimeSig>"
           "<Tempo><tempo>2</tempo></Tempo><Chord><durationType>quarter</durationType>"
           "<Note><pitch>60</pitch><Spanner type=\"Tie\"><next/></Spanner></Note></Chord>"
           "<Chord><durationType>half</durationType><dots>1</dots><Note><pitch>60</pitch>"
           "<Spanner type=\"Tie\"><prev/></Spanner></Note></Chord></voice></Measure></Staff>"
           "</Score></museScore>";
}

// TOONVAL_FUZZ_SCALE=20 runs 20 times the mutations, for a long soak under the sanitizers.
int fuzz_scale() {
    const char* text = std::getenv("TOONVAL_FUZZ_SCALE");
    const int n = text == nullptr ? 1 : std::atoi(text);
    return n >= 1 ? n : 1;
}

template <typename Loader>
void fuzz(const Bytes& seed, Loader load, int rounds, std::uint64_t base) {
    int accepted = 0;
    for (int i = 0; i < rounds * fuzz_scale(); ++i) {
        Rng rng(base + static_cast<std::uint64_t>(i));
        const Bytes input = mutate(seed, rng);
        const ScoreLoadResult r = load(input);
        expect_well_formed(r);
        accepted += r.ok ? 1 : 0;
    }
    // Most single-byte edits leave a loadable file; if none did the mutator broke the seed.
    CHECK(accepted > 0);
}

} // namespace

TEST_CASE("score fuzz: mutated MusicXML never crashes and loads well formed") {
    const std::string xml = small_score();
    REQUIRE(load_musicxml(Bytes(xml.begin(), xml.end())).ok);
    fuzz(Bytes(xml.begin(), xml.end()), [](const Bytes& b) { return load_musicxml(b); }, 3000, 1);
}

TEST_CASE("score fuzz: every truncation of a MusicXML file is handled") {
    const std::string xml = small_score();
    const Bytes all(xml.begin(), xml.end());
    for (std::size_t cut = 0; cut <= all.size(); ++cut) {
        expect_well_formed(load_musicxml(all.data(), cut));
    }
}

TEST_CASE("score fuzz: mutated MuseScore XML never crashes and loads well formed") {
    const std::string xml = small_mscx();
    REQUIRE(load_mscx(reinterpret_cast<const std::uint8_t*>(xml.data()), xml.size()).ok);
    fuzz(
        Bytes(xml.begin(), xml.end()), [](const Bytes& b) { return load_mscx(b.data(), b.size()); },
        3000, 100000);
}

TEST_CASE("score fuzz: a mutated real .mxl and .mscz never crash") {
    fuzz(
        read_data("beethoven-op48-4-ehre-gottes.mxl"), [](const Bytes& b) { return load_mxl(b); },
        1500, 200000);
    fuzz(
        read_data("beethoven-op48-4-ehre-gottes-musescore3.mscz"),
        [](const Bytes& b) { return load_score(b); }, 600, 300000);
}

TEST_CASE("score fuzz: mutated stored zips never crash") {
    const std::string xml = small_score();
    const Bytes zip = test::zip_of(
        {test::stored_entry(
             "META-INF/container.xml",
             "<container><rootfiles><rootfile full-path=\"s.xml\"/></rootfiles></container>"),
         test::stored_entry("s.xml", xml)});
    REQUIRE(load_mxl(zip).ok);
    // Mutations of a stored zip fail the entry CRC or the directory far more often than they load.
    for (int i = 0; i < 3000 * fuzz_scale(); ++i) {
        Rng rng(400000 + static_cast<std::uint64_t>(i));
        expect_well_formed(load_score(mutate(zip, rng)));
    }
}

TEST_CASE("score fuzz: pure noise and tiny inputs") {
    Rng rng(7);
    for (int i = 0; i < 500; ++i) {
        Bytes noise(rng.below(200));
        for (auto& b : noise) {
            b = static_cast<std::uint8_t>(rng.next());
        }
        expect_well_formed(load_score(noise));
        expect_well_formed(load_musicxml(noise));
        expect_well_formed(load_mxl(noise));
        expect_well_formed(load_mscz(noise));
    }
    for (const char* tiny : {"", "<", "<a", "<a/>", "<?xml", "PK", "PK\x03\x04", "MThd"}) {
        expect_well_formed(
            load_score(reinterpret_cast<const std::uint8_t*>(tiny), std::string(tiny).size()));
    }
}

TEST_CASE("hostile: absurd numbers do not overflow") {
    const std::string huge =
        "<score-partwise><part-list><score-part id=\"P1\"><part-name>x</part-name></score-part>"
        "</part-list><part id=\"P1\"><measure number=\"1\"><attributes>"
        "<divisions>999999999999</divisions></attributes>"
        "<note><pitch><step>C</step><octave>4</octave></pitch><duration>99999999999999999999</"
        "duration>"
        "</note><backup><duration>-5</duration></backup><forward><duration>99999999999</duration></"
        "forward>"
        "<note><pitch><step>D</step><octave>99999999999</octave></pitch><duration>1</duration></"
        "note>"
        "<note><pitch><step>E</step><alter>1e999</alter><octave>4</octave></pitch><duration>4</"
        "duration></note>"
        "</measure><measure><attributes><divisions>1</divisions><time><beats>999999</beats>"
        "<beat-type>0</beat-type></time></attributes>"
        "<note><pitch><step>F</step><octave>4</octave></pitch><duration>4</duration></note></"
        "measure>"
        "</part></score-partwise>";
    expect_well_formed(load_musicxml(Bytes(huge.begin(), huge.end())));
}

TEST_CASE("hostile: very deep nesting and a very large bar count") {
    std::string deep = "<score-partwise>";
    for (int i = 0; i < 200000; ++i) {
        deep += "<a>";
    }
    expect_well_formed(load_musicxml(Bytes(deep.begin(), deep.end())));

    // More bars than any song needs.
    std::string many =
        "<score-partwise><part-list><score-part id=\"P1\"><part-name>x</part-name></score-part>"
        "</part-list><part id=\"P1\">";
    for (std::size_t i = 0; i < kMaxZipEntries * 120; ++i) {
        many += "<measure/>";
    }
    many += "</part></score-partwise>";
    const ScoreLoadResult r = load_musicxml(Bytes(many.begin(), many.end()));
    CHECK_FALSE(r.ok);
    CHECK(r.error == ScoreFileError::TooLarge);
}

TEST_CASE("hostile: thousands of parts and bars together are refused, not allocated") {
    // 5000 parts of 300 bars is 1.5 million bars to hold: over the limit for a piano trainer.
    std::string xml = "<score-partwise>";
    for (int p = 0; p < 5000; ++p) {
        xml += "<part id=\"P" + std::to_string(p) + "\">";
        for (int m = 0; m < 300; ++m) {
            xml += "<measure/>";
        }
        xml += "</part>";
    }
    xml += "</score-partwise>";
    const ScoreLoadResult r = load_musicxml(Bytes(xml.begin(), xml.end()));
    CHECK_FALSE(r.ok);
    CHECK(r.error == ScoreFileError::TooLarge);
}

TEST_CASE("hostile: an XML entity bomb is not expanded") {
    const std::string bomb =
        "<?xml version=\"1.0\"?><!DOCTYPE lolz [<!ENTITY a \"aaaaaaaaaa\">"
        "<!ENTITY b \"&a;&a;&a;&a;&a;&a;&a;&a;&a;&a;\"><!ENTITY c "
        "\"&b;&b;&b;&b;&b;&b;&b;&b;&b;&b;\">"
        "<!ENTITY d \"&c;&c;&c;&c;&c;&c;&c;&c;&c;&c;\">]><score-partwise><work><work-title>&d;"
        "</work-title></work></score-partwise>";
    const ScoreLoadResult r = load_musicxml(Bytes(bomb.begin(), bomb.end()));
    CHECK_FALSE(r.ok);
    CHECK(r.message.size() < 1000);
}

TEST_CASE("hostile: an .xml larger than the cap is refused without parsing") {
    // Only the size matters: a cheap 65 MiB buffer of spaces around a tag.
    Bytes big(kMaxScoreXmlBytes + 1, ' ');
    big[0] = '<';
    const ScoreLoadResult r = load_musicxml(big);
    CHECK_FALSE(r.ok);
    CHECK(r.error == ScoreFileError::TooLarge);
}
