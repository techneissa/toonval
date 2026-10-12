// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/musicxml.hpp"

#include "score_raw.hpp"

#include "toonval/hand.hpp"
#include "toonval/song_builder.hpp"

#include "pugixml.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <numeric>
#include <tuple>

namespace toonval {

using namespace score_detail;

namespace {

constexpr std::int64_t kMaxPpq = 30240; // 480 * 63: fits the 16-bit division of a MIDI file
constexpr long long kMaxDivisions = 1'000'000;
constexpr std::size_t kMaxBarRefs = 1'000'000; // bars of all parts together

bool is(const pugi::xml_node& n, const char* name) { return std::strcmp(n.name(), name) == 0; }

// Velocity of a dynamics mark, 0 when the element is no mark we know.
int velocity_of_mark(const char* name) {
    struct Mark {
        const char* name;
        int velocity;
    };
    static constexpr Mark marks[] = {
        {"pppp", 8}, {"ppp", 16},  {"pp", 33},    {"p", 49},     {"mp", 64},  {"mf", 80},
        {"f", 96},   {"ff", 112},  {"fff", 126},  {"ffff", 127}, {"fp", 96},  {"sf", 112},
        {"sfp", 96}, {"sfz", 112}, {"sffz", 126}, {"fz", 112},   {"rf", 112}, {"rfz", 112},
    };
    for (const Mark& m : marks) {
        if (std::strcmp(m.name, name) == 0) {
            return m.velocity;
        }
    }
    return 0;
}

// MIDI velocity for a percentage of the forte velocity (MusicXML's `dynamics` attribute: 90 is the
// default for a note, 100 a "forte" of the sound).
int velocity_of_percent(double percent) {
    return std::clamp(static_cast<int>(std::lround(percent * 0.9)), 1, 127);
}

// Quarter notes in a beat unit; 0 for a name we don't know.
double quarters_of_unit(const char* unit) {
    struct Unit {
        const char* name;
        double quarters;
    };
    static constexpr Unit units[] = {
        {"maxima", 32.0},     {"long", 16.0},         {"breve", 8.0},     {"whole", 4.0},
        {"half", 2.0},        {"quarter", 1.0},       {"eighth", 0.5},    {"16th", 0.25},
        {"32nd", 0.125},      {"64th", 0.0625},       {"128th", 0.03125}, {"256th", 0.015625},
        {"512th", 0.0078125}, {"1024th", 0.00390625},
    };
    for (const Unit& u : units) {
        if (std::strcmp(u.name, unit) == 0) {
            return u.quarters;
        }
    }
    return 0.0;
}

int step_semitone(char step) {
    switch (step) {
    case 'C':
        return 0;
    case 'D':
        return 2;
    case 'E':
        return 4;
    case 'F':
        return 5;
    case 'G':
        return 7;
    case 'A':
        return 9;
    case 'B':
        return 11;
    default:
        return -1;
    }
}

struct MeasureRef {
    pugi::xml_node content; // the element with notes in it: <measure> (partwise) or <part>
    pugi::xml_node bar;     // the <measure> element (number, implicit)
};

class PartReader {
  public:
    PartReader(RawScore& raw, int part_index, int ppq) : raw_(raw), part_(part_index), ppq_(ppq) {}

    // Returns false when the score is too large.
    bool read(const std::vector<MeasureRef>& measures) {
        first_note_ = raw_.notes.size();
        for (std::size_t m = 0; m < measures.size(); ++m) {
            if (!read_measure(measures[m], static_cast<int>(m))) {
                return false;
            }
        }
        finish_tracks();
        return true;
    }

  private:
    std::int64_t ticks(long long divisions_value) const {
        // divisions_value / divisions_ quarter notes, in ticks, rounded.
        const long long d = std::clamp<long long>(divisions_value, -kMaxEventTicks, kMaxEventTicks);
        const std::int64_t num = d * ppq_;
        const std::int64_t den = divisions_;
        const std::int64_t half = den / 2;
        const std::int64_t t = num >= 0 ? (num + half) / den : -((-num + half) / den);
        return std::clamp<std::int64_t>(t, -kMaxEventTicks, kMaxEventTicks);
    }

    bool duration_ticks(const pugi::xml_node& parent, std::int64_t& out) const {
        long long v = 0;
        if (!parse_int(parent.child_value("duration"), v)) {
            return false;
        }
        out = ticks(v);
        return true;
    }

    bool read_measure(const MeasureRef& ref, int index) {
        RawMeasure& bar = raw_.measures[static_cast<std::size_t>(index)];
        if (ref.bar &&
            (std::strcmp(ref.bar.attribute("implicit").as_string(), "yes") == 0 ||
             (index == 0 && std::strcmp(ref.bar.attribute("number").as_string(), "0") == 0))) {
            bar.implicit = true;
        }
        pos_ = 0;
        last_start_ = 0;
        std::int64_t extent = 0;
        for (const pugi::xml_node& c : ref.content.children()) {
            if (is(c, "attributes")) {
                read_attributes(c, bar);
            } else if (is(c, "note")) {
                if (!read_note(c, index, extent)) {
                    return false;
                }
            } else if (is(c, "backup")) {
                std::int64_t d = 0;
                if (duration_ticks(c, d)) {
                    pos_ = std::max<std::int64_t>(0, pos_ - std::max<std::int64_t>(d, 0));
                }
            } else if (is(c, "forward")) {
                std::int64_t d = 0;
                if (duration_ticks(c, d)) {
                    pos_ = std::min(pos_ + std::max<std::int64_t>(d, 0), kMaxEventTicks);
                    extent = std::max(extent, pos_);
                }
            } else if (is(c, "direction")) {
                read_direction(c, index);
            } else if (is(c, "sound")) {
                read_sound(c, index, pos_);
            } else if (is(c, "barline")) {
                for (const pugi::xml_node& b : c.children()) {
                    if (is(b, "repeat") ||
                        (is(b, "ending") &&
                         std::strcmp(b.attribute("type").as_string(), "start") == 0)) {
                        ++raw_.stats.repeats_ignored;
                    }
                }
            }
        }
        bar.extent = std::max(bar.extent, extent);
        return true;
    }

    void read_attributes(const pugi::xml_node& attrs, RawMeasure& bar) {
        long long v = 0;
        if (parse_int(attrs.child_value("divisions"), v) && v >= 1 && v <= kMaxDivisions) {
            divisions_ = v;
        }
        if (parse_int(attrs.child_value("staves"), v) && v >= 1 && v <= 16) {
            staves_ = std::max(staves_, static_cast<int>(v));
        }
        const pugi::xml_node time = attrs.child("time");
        if (time && !bar.has_signature) {
            read_time(time, bar);
        }
        const pugi::xml_node transpose = attrs.child("transpose");
        if (transpose) {
            long long chromatic = 0;
            long long octaves = 0;
            parse_int(transpose.child_value("chromatic"), chromatic);
            parse_int(transpose.child_value("octave-change"), octaves);
            transpose_ = static_cast<int>(std::clamp<long long>(chromatic, -48, 48) +
                                          12 * std::clamp<long long>(octaves, -4, 4));
        }
    }

    static void read_time(const pugi::xml_node& time, RawMeasure& bar) {
        const pugi::xml_node beats = time.child("beats");
        const pugi::xml_node type = time.child("beat-type");
        if (!beats || !type) {
            return; // senza-misura
        }
        // "3+2" is five beats of the same unit.
        long long sum = 0;
        const char* s = beats.child_value();
        while (*s != '\0') {
            long long part = 0;
            if (parse_int(s, part) && part > 0) {
                sum += part;
            }
            while (*s != '\0' && *s != '+') {
                ++s;
            }
            if (*s == '+') {
                ++s;
            }
        }
        long long den = 0;
        if (!parse_int(type.child_value(), den)) {
            return;
        }
        const bool power_of_two = den >= 1 && den <= 64 && (den & (den - 1)) == 0;
        if (sum < 1 || sum > 255 || !power_of_two) {
            return;
        }
        bar.has_signature = true;
        bar.numerator = static_cast<int>(sum);
        bar.denominator = static_cast<int>(den);
    }

    void read_direction(const pugi::xml_node& dir, int index) {
        std::int64_t position = pos_;
        long long offset = 0;
        if (parse_int(dir.child_value("offset"), offset)) {
            position = std::max<std::int64_t>(0, pos_ + ticks(offset));
        }
        const pugi::xml_node sound = dir.child("sound");
        bool tempo_set = sound && sound.attribute("tempo");
        for (const pugi::xml_node& type : dir.children("direction-type")) {
            for (const pugi::xml_node& item : type.children()) {
                if (is(item, "dynamics")) {
                    for (const pugi::xml_node& mark : item.children()) {
                        const int v = velocity_of_mark(mark.name());
                        if (v > 0) {
                            raw_.dynamics.push_back({part_, index, position, v});
                        }
                    }
                } else if (is(item, "metronome") && !tempo_set) {
                    double bpm = 0.0;
                    if (read_metronome(item, bpm)) {
                        raw_.tempos.push_back({part_, index, position, bpm});
                        tempo_set = true;
                    }
                }
            }
        }
        if (sound) {
            read_sound(sound, index, position);
        }
    }

    static bool read_metronome(const pugi::xml_node& m, double& bpm) {
        const pugi::xml_node unit = m.child("beat-unit");
        double per_minute = 0.0;
        if (!unit || !parse_double(m.child_value("per-minute"), per_minute, true)) {
            return false;
        }
        const double quarters = quarters_of_unit(unit.child_value());
        if (quarters <= 0.0 || !(per_minute > 0.0)) {
            return false;
        }
        int dots = 0;
        for (const pugi::xml_node& c : m.children("beat-unit-dot")) {
            (void)c;
            ++dots;
        }
        bpm = per_minute * quarters * (2.0 - std::pow(0.5, std::min(dots, 8)));
        return true;
    }

    void read_sound(const pugi::xml_node& sound, int index, std::int64_t position) {
        double tempo = 0.0;
        if (parse_double(sound.attribute("tempo").as_string(), tempo) && tempo > 0.0) {
            raw_.tempos.push_back({part_, index, position, tempo});
        }
        double dynamics = 0.0;
        if (parse_double(sound.attribute("dynamics").as_string(), dynamics) && dynamics > 0.0) {
            raw_.dynamics.push_back({part_, index, position, velocity_of_percent(dynamics)});
        }
    }

    bool read_note(const pugi::xml_node& note, int index, std::int64_t& extent) {
        if (note.child("grace")) {
            ++raw_.stats.grace_notes_dropped;
            return true;
        }
        const bool chord = static_cast<bool>(note.child("chord"));
        std::int64_t duration = 0;
        const bool has_duration = duration_ticks(note, duration);
        duration = std::max<std::int64_t>(duration, 0);
        const std::int64_t start = chord ? last_start_ : pos_;
        if (!chord) {
            last_start_ = pos_;
            pos_ = std::min(pos_ + duration, kMaxEventTicks);
        }
        extent = std::min(std::max(extent, start + duration), kMaxEventTicks);

        if (note.child("rest")) {
            return true;
        }
        if (note.child("cue")) {
            ++raw_.stats.cue_notes_dropped;
            return true;
        }
        if (note.child("unpitched")) {
            ++raw_.stats.unpitched_dropped;
            return true;
        }
        const pugi::xml_node pitch = note.child("pitch");
        const char* step_text = pitch.child_value("step");
        long long octave = 0;
        double alter = 0.0;
        parse_double(pitch.child_value("alter"), alter);
        const int semitone = step_semitone(step_text[0]);
        if (!pitch || semitone < 0 || !parse_int(pitch.child_value("octave"), octave) ||
            !has_duration || duration <= 0) {
            ++raw_.stats.unreadable_notes;
            return true;
        }
        const long long midi = (octave + 1) * 12 + semitone + std::lround(alter) + transpose_;
        if (midi < 0 || midi > 127) {
            ++raw_.stats.unreadable_notes;
            return true;
        }

        long long staff = 1;
        long long voice = 1;
        parse_int(note.child_value("staff"), staff);
        parse_int(note.child_value("voice"), voice);
        staff = std::clamp<long long>(staff, 1, 16);
        voice = std::clamp<long long>(voice, 0, 1000);
        max_staff_ = std::max(max_staff_, static_cast<int>(staff));

        bool tie_start = false;
        bool tie_stop = false;
        bool has_tie_element = false;
        for (const pugi::xml_node& t : note.children("tie")) {
            has_tie_element = true;
            const char* type = t.attribute("type").as_string();
            tie_start = tie_start || std::strcmp(type, "start") == 0;
            tie_stop = tie_stop || std::strcmp(type, "stop") == 0;
        }
        int finger = 0;
        for (const pugi::xml_node& n : note.children("notations")) {
            if (!has_tie_element) {
                for (const pugi::xml_node& t : n.children("tied")) {
                    const char* type = t.attribute("type").as_string();
                    tie_start = tie_start || std::strcmp(type, "start") == 0;
                    tie_stop = tie_stop || std::strcmp(type, "stop") == 0;
                }
            }
            if (finger == 0) {
                const char* f = n.child("technical").child_value("fingering");
                for (; f != nullptr && *f != '\0'; ++f) {
                    if (*f >= '1' && *f <= '5') {
                        finger = *f - '0';
                        break;
                    }
                    if (*f >= '0' && *f <= '9') {
                        break; // 0 or 6..9 are no finger of one hand
                    }
                }
            }
        }

        const TieJoiner::Key key{static_cast<int>(staff), static_cast<int>(voice),
                                 static_cast<int>(midi)};
        if (ties_.join(raw_, key, index, start, duration, tie_stop, tie_start)) {
            return true;
        }

        if (raw_.notes.size() >= kMaxScoreNotes) {
            return false;
        }
        RawNote n;
        n.measure = index;
        n.offset = start;
        n.length = duration;
        n.pitch = static_cast<std::uint8_t>(midi);
        n.finger = static_cast<std::int8_t>(finger);
        n.track = static_cast<int>(staff); // a staff number for now; finish_tracks() maps it
        double percent = 0.0;
        if (parse_double(note.attribute("dynamics").as_string(), percent) && percent > 0.0) {
            n.velocity = static_cast<std::uint8_t>(velocity_of_percent(percent));
        }
        raw_.notes.push_back(n);
        if (tie_start) {
            ties_.open(key, raw_.notes.size() - 1, index, start + duration);
        }
        return true;
    }

    // Turns the staff numbers of this part's notes into tracks, one per staff with notes.
    void finish_tracks() {
        std::vector<int> staves;
        for (std::size_t i = first_note_; i < raw_.notes.size(); ++i) {
            staves.push_back(raw_.notes[i].track);
        }
        std::sort(staves.begin(), staves.end());
        staves.erase(std::unique(staves.begin(), staves.end()), staves.end());
        const int staff_count = std::max(staves_, max_staff_);
        const RawPart& part = raw_.parts[static_cast<std::size_t>(part_)];
        const Hand named = hand_from_name(part.name);
        const std::size_t first_track = raw_.tracks.size();
        std::map<int, int> track_of_staff;
        for (int staff : staves) {
            RawTrack t;
            t.part = part_;
            t.name = part.name;
            if (staves.size() > 1) {
                t.name += " (staff " + std::to_string(staff) + ")";
            }
            t.hand = staff_count >= 2 ? (staff == 1 ? Hand::Right : Hand::Left) : named;
            track_of_staff[staff] = static_cast<int>(raw_.tracks.size());
            raw_.tracks.push_back(std::move(t));
        }
        (void)first_track;
        for (std::size_t i = first_note_; i < raw_.notes.size(); ++i) {
            raw_.notes[i].track = track_of_staff[raw_.notes[i].track];
        }
    }

    RawScore& raw_;
    int part_;
    int ppq_;
    long long divisions_ = 1;
    int transpose_ = 0;
    int staves_ = 1;
    int max_staff_ = 1;
    std::int64_t pos_ = 0;
    std::int64_t last_start_ = 0;
    std::size_t first_note_ = 0;
    TieJoiner ties_;
};

// Reads divisions of every <attributes> to find a tick length all of them divide.
int choose_ppq(const std::vector<std::vector<MeasureRef>>& parts) {
    std::int64_t ppq = 480;
    std::vector<long long> seen;
    for (const auto& part : parts) {
        for (const MeasureRef& ref : part) {
            for (const pugi::xml_node& attrs : ref.content.children("attributes")) {
                long long v = 0;
                if (parse_int(attrs.child_value("divisions"), v) && v >= 1 && v <= kMaxDivisions &&
                    std::find(seen.begin(), seen.end(), v) == seen.end()) {
                    seen.push_back(v);
                    if (seen.size() > 64) {
                        return static_cast<int>(ppq);
                    }
                    const std::int64_t next = std::lcm<std::int64_t>(ppq, v);
                    if (next <= kMaxPpq) {
                        ppq = next;
                    }
                }
            }
        }
    }
    return static_cast<int>(ppq);
}

} // namespace

namespace score_detail {

ScoreLoadResult score_from_musicxml(const pugi::xml_document& doc) {
    const pugi::xml_node root = doc.document_element();
    const bool partwise = is(root, "score-partwise");
    const bool timewise = is(root, "score-timewise");
    if (!partwise && !timewise) {
        return make_error(ScoreFileError::NotScore,
                          std::string("the XML root is <") + root.name() +
                              ">, not a MusicXML score (score-partwise or score-timewise)");
    }

    RawScore raw;
    std::map<std::string, int> part_of_id;
    for (const pugi::xml_node& sp : root.child("part-list").children("score-part")) {
        RawPart p;
        p.name = clean_name(sp.child_value("part-name"));
        if (p.name.empty()) {
            p.name = clean_name(sp.child_value("part-abbreviation"));
        }
        p.instrument_name = clean_name(sp.child("score-instrument").child_value("instrument-name"));
        long long program = 0;
        if (parse_int(sp.child("midi-instrument").child_value("midi-program"), program) &&
            program >= 1 && program <= 128) {
            p.program = static_cast<int>(program - 1);
        }
        part_of_id.emplace(sp.attribute("id").as_string(), static_cast<int>(raw.parts.size()));
        raw.parts.push_back(std::move(p));
    }
    auto part_for = [&](const char* id) {
        const auto it = part_of_id.find(id);
        if (it != part_of_id.end()) {
            return it->second;
        }
        RawPart p;
        p.name = *id != '\0' ? id : "Part " + std::to_string(raw.parts.size() + 1);
        part_of_id.emplace(id, static_cast<int>(raw.parts.size()));
        raw.parts.push_back(std::move(p));
        return static_cast<int>(raw.parts.size()) - 1;
    };

    // Per part, its bars in order. Timewise files list each bar with its parts inside it. The bars
    // of all parts together are capped, so a hostile file cannot make this table huge.
    std::vector<std::vector<MeasureRef>> bars_of_part(raw.parts.size());
    std::size_t bar_count = 0;
    std::size_t refs = 0;
    bool too_large = false;
    auto bars_for = [&](int part) -> std::vector<MeasureRef>& {
        if (static_cast<std::size_t>(part) >= bars_of_part.size()) {
            bars_of_part.resize(static_cast<std::size_t>(part) + 1);
        }
        return bars_of_part[static_cast<std::size_t>(part)];
    };
    if (partwise) {
        for (const pugi::xml_node& p : root.children("part")) {
            if (raw.parts.size() > static_cast<std::size_t>(kMaxTracks)) {
                too_large = true;
                break;
            }
            const int part = part_for(p.attribute("id").as_string());
            auto& bars = bars_for(part);
            for (const pugi::xml_node& m : p.children("measure")) {
                if (bars.size() >= kMaxSongMeasures || ++refs > kMaxBarRefs) {
                    too_large = true;
                    break;
                }
                bars.push_back({m, m});
            }
            if (too_large) {
                break;
            }
            bar_count = std::max(bar_count, bars.size());
        }
    } else {
        std::size_t index = 0;
        for (const pugi::xml_node& m : root.children("measure")) {
            if (index >= kMaxSongMeasures) {
                too_large = true;
                break;
            }
            for (const pugi::xml_node& p : m.children("part")) {
                if (raw.parts.size() > static_cast<std::size_t>(kMaxTracks)) {
                    too_large = true;
                    break;
                }
                const int part = part_for(p.attribute("id").as_string());
                auto& bars = bars_for(part);
                refs += index + 1 - std::min(index + 1, bars.size());
                if (refs > kMaxBarRefs) {
                    too_large = true;
                    break;
                }
                bars.resize(std::max(bars.size(), index + 1));
                bars[index] = {p, m};
            }
            if (too_large) {
                break;
            }
            ++index;
        }
        bar_count = index;
    }
    if (too_large || bar_count > kMaxSongMeasures) {
        return make_error(ScoreFileError::TooLarge,
                          "the score has more parts or bars than any song needs");
    }
    if (raw.parts.size() > static_cast<std::size_t>(kMaxTracks)) {
        return make_error(ScoreFileError::TooLarge, "the score has too many parts");
    }
    bars_of_part.resize(raw.parts.size());
    if (bar_count == 0) {
        return make_error(ScoreFileError::NoNotes, "the score has no bars");
    }

    raw.title = clean_name(root.child("work").child_value("work-title"));
    const std::string movement = clean_name(root.child_value("movement-title"));
    if (raw.title.empty()) {
        raw.title = movement;
    } else if (!movement.empty() && movement != raw.title) {
        raw.title += ": " + movement; // "6 Lieder, Op.48: Die Ehre Gottes aus der Natur"
    }
    raw.ppq = choose_ppq(bars_of_part);
    raw.measures.resize(bar_count);

    for (std::size_t p = 0; p < bars_of_part.size(); ++p) {
        PartReader reader(raw, static_cast<int>(p), raw.ppq);
        if (!reader.read(bars_of_part[p])) {
            return make_error(ScoreFileError::TooLarge,
                              "the score has more notes than any song needs");
        }
        if (raw.tracks.size() > static_cast<std::size_t>(kMaxTracks)) {
            return make_error(ScoreFileError::TooLarge, "the score has too many staves");
        }
    }
    return assemble_score(raw);
}

} // namespace score_detail

ScoreLoadResult load_musicxml(const std::uint8_t* data, std::size_t size) {
    pugi::xml_document doc;
    ScoreLoadResult error;
    if (!parse_xml_bytes(data, size, doc, error)) {
        return error;
    }
    return score_from_musicxml(doc);
}

const char* to_string(ScoreFileError error) {
    switch (error) {
    case ScoreFileError::None:
        return "none";
    case ScoreFileError::NotXml:
        return "not XML";
    case ScoreFileError::NotZip:
        return "not a zip file";
    case ScoreFileError::NotScore:
        return "not a score";
    case ScoreFileError::Unsupported:
        return "unsupported";
    case ScoreFileError::Corrupt:
        return "corrupt";
    case ScoreFileError::NoNotes:
        return "no notes";
    case ScoreFileError::TooLarge:
        return "too large";
    }
    return "unknown";
}

} // namespace toonval
