// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/exercises.hpp"

#include "toonval/pitch.hpp"
#include "toonval/song_builder.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace toonval {
namespace {

// ----- Fingering tables -----
//
// One octave of a scale from its tonic is seven notes (degrees 1..7). A hand's fingering is the
// finger of the first note, a cycle of seven fingers that repeats every octave (the tonic of the
// second octave takes cycle[0]), and the finger of the last note, the tonic at the top.

struct Pattern {
    int first;
    std::array<int, 7> cycle;
    int top;
};

// Right hand.
constexpr Pattern kRhC{1, {1, 2, 3, 1, 2, 3, 4}, 5};  // C D E G A B, and the minors on A E B C G D
constexpr Pattern kRhF{1, {1, 2, 3, 4, 1, 2, 3}, 4};  // F, F minor
constexpr Pattern kRhBb{4, {4, 1, 2, 3, 1, 2, 3}, 4}; // Bb, Bb minor
constexpr Pattern kRhEb{3, {3, 1, 2, 3, 4, 1, 2}, 3}; // Eb, Eb (D#) minor
constexpr Pattern kRhAb{3, {3, 4, 1, 2, 3, 1, 2}, 3}; // Ab, and the minors on F# C# G#
constexpr Pattern kRhDb{2, {2, 3, 1, 2, 3, 4, 1}, 2}; // Db
constexpr Pattern kRhGb{2, {2, 3, 4, 1, 2, 3, 1}, 2}; // F#/Gb

// Left hand.
constexpr Pattern kLhC{5, {1, 4, 3, 2, 1, 3, 2}, 1}; // C D E F G A, and the minors on A E D G C F
constexpr Pattern kLhB{4, {1, 3, 2, 1, 4, 3, 2}, 1}; // B, B minor
constexpr Pattern kLhFlat{3, {3, 2, 1, 4, 3, 2, 1}, 3}; // Db Eb Ab Bb, C# minor, G# minor
constexpr Pattern kLhGb{4, {4, 3, 2, 1, 3, 2, 1}, 4};   // F#/Gb, F# minor
constexpr Pattern kLhBbMinor{2, {2, 1, 3, 2, 1, 4, 3}, 2};
constexpr Pattern kLhEbMinor{2, {2, 1, 4, 3, 2, 1, 3}, 2};

// Indexed by the tonic's pitch class, C = 0.
constexpr std::array<const Pattern*, 12> kMajorRh{&kRhC,  &kRhDb, &kRhC,  &kRhEb, &kRhC,  &kRhF,
                                                  &kRhGb, &kRhC,  &kRhAb, &kRhC,  &kRhBb, &kRhC};
constexpr std::array<const Pattern*, 12> kMajorLh{&kLhC,    &kLhFlat, &kLhC,    &kLhFlat,
                                                  &kLhC,    &kLhC,    &kLhGb,   &kLhC,
                                                  &kLhFlat, &kLhC,    &kLhFlat, &kLhB};
// Natural and harmonic minor share these.
constexpr std::array<const Pattern*, 12> kMinorRh{&kRhC,  &kRhAb, &kRhC,  &kRhEb, &kRhC,  &kRhF,
                                                  &kRhAb, &kRhC,  &kRhAb, &kRhC,  &kRhBb, &kRhC};
constexpr std::array<const Pattern*, 12> kMinorLh{&kLhC,    &kLhFlat, &kLhC,       &kLhEbMinor,
                                                  &kLhC,    &kLhC,    &kLhGb,      &kLhC,
                                                  &kLhFlat, &kLhC,    &kLhBbMinor, &kLhB};

const Pattern* find_pattern(int root_pc, ScaleType type, Hand hand) {
    const bool major = type == ScaleType::Major;
    const bool minor = type == ScaleType::NaturalMinor || type == ScaleType::HarmonicMinor;
    if ((!major && !minor) || (hand != Hand::Right && hand != Hand::Left)) {
        return nullptr;
    }
    const auto pc = static_cast<std::size_t>(pitch_class(root_pc));
    if (hand == Hand::Right) {
        return major ? kMajorRh[pc] : kMinorRh[pc];
    }
    return major ? kMajorLh[pc] : kMinorLh[pc];
}

// ----- Chord shapes -----

constexpr std::array<int, 3> kMajorTriad{0, 4, 7};
constexpr std::array<int, 3> kMinorTriad{0, 3, 7};
constexpr std::array<int, 3> kDimTriad{0, 3, 6};
constexpr std::array<int, 3> kAugTriad{0, 4, 8};
constexpr std::array<int, 3> kSus2{0, 2, 7};
constexpr std::array<int, 3> kSus4{0, 5, 7};
constexpr std::array<int, 4> kDom7{0, 4, 7, 10};
constexpr std::array<int, 4> kMaj7{0, 4, 7, 11};
constexpr std::array<int, 4> kMin7{0, 3, 7, 10};
constexpr std::array<int, 4> kHalfDim7{0, 3, 6, 10};
constexpr std::array<int, 4> kDim7{0, 3, 6, 9};
constexpr std::array<int, 4> kMinMaj7{0, 3, 7, 11};
constexpr std::array<int, 4> kMaj6{0, 4, 7, 9};
constexpr std::array<int, 4> kMin6{0, 3, 7, 9};
constexpr std::array<int, 4> kAdd9{0, 4, 7, 14};
constexpr std::array<int, 5> kDom9{0, 4, 7, 10, 14};
constexpr std::array<int, 5> kMaj9{0, 4, 7, 11, 14};
constexpr std::array<int, 5> kMin9{0, 3, 7, 10, 14};
constexpr std::array<int, 2> kPower{0, 7};

const char* quality_words(ChordQuality q) {
    switch (q) {
    case ChordQuality::Major:
        return "major";
    case ChordQuality::Minor:
        return "minor";
    case ChordQuality::Diminished:
        return "diminished";
    case ChordQuality::Augmented:
        return "augmented";
    case ChordQuality::Sus2:
        return "sus2";
    case ChordQuality::Sus4:
        return "sus4";
    case ChordQuality::Dominant7:
        return "dominant 7th";
    case ChordQuality::Major7:
        return "major 7th";
    case ChordQuality::Minor7:
        return "minor 7th";
    case ChordQuality::HalfDiminished7:
        return "half-diminished 7th";
    case ChordQuality::Diminished7:
        return "diminished 7th";
    case ChordQuality::MinorMajor7:
        return "minor-major 7th";
    case ChordQuality::Major6:
        return "major 6th";
    case ChordQuality::Minor6:
        return "minor 6th";
    case ChordQuality::Add9:
        return "add9";
    case ChordQuality::Dominant9:
        return "dominant 9th";
    case ChordQuality::Major9:
        return "major 9th";
    case ChordQuality::Minor9:
        return "minor 9th";
    case ChordQuality::Power:
        return "power chord";
    case ChordQuality::None:
        break;
    }
    return "";
}

bool minor_flavoured(ChordQuality q) {
    switch (q) {
    case ChordQuality::Minor:
    case ChordQuality::Diminished:
    case ChordQuality::Minor7:
    case ChordQuality::HalfDiminished7:
    case ChordQuality::Diminished7:
    case ChordQuality::MinorMajor7:
    case ChordQuality::Minor6:
    case ChordQuality::Minor9:
        return true;
    default:
        return false;
    }
}

// ----- Shared plumbing -----

struct Plan {
    std::int64_t step_ticks = 0;
    std::uint32_t velocity = 80;
};

ExerciseResult fail(std::string message) {
    ExerciseResult r;
    r.message = std::move(message);
    return r;
}

bool settings_ok(const ExerciseSettings& s, std::string& why) {
    if (!(s.bpm > 0.0) || !std::isfinite(s.bpm)) {
        why = "the tempo must be above 0";
    } else if (s.ticks_per_quarter < 1 || s.ticks_per_quarter > 32767) {
        why = "ticks per quarter note must be 1..32767";
    } else if (!(s.note_beats > 0.0) || !std::isfinite(s.note_beats) ||
               std::llround(s.note_beats * s.ticks_per_quarter) < 1) {
        why = "the note length must be at least one tick";
    } else if (s.range.lowest > s.range.highest || s.range.lowest < 0 || s.range.highest > 127) {
        why = "the keyboard range is empty or outside MIDI";
    } else {
        return true;
    }
    return false;
}

// Octave shift nearest to 0 that puts [low, high] inside the keyboard. Tries 0, -1, +1, -2, ...
bool fit_octaves(int low, int high, KeyRange range, int& shift) {
    for (int k = 0; k <= 10; ++k) {
        for (int sign : {-1, 1}) {
            if (k == 0 && sign == 1) {
                continue;
            }
            const int s = sign * k;
            if (low + 12 * s >= range.lowest && high + 12 * s <= range.highest) {
                shift = s;
                return true;
            }
        }
    }
    return false;
}

std::string octaves_text(int octaves) {
    return std::to_string(octaves) + (octaves == 1 ? " octave" : " octaves");
}

std::string hands_text(HandsChoice h) {
    switch (h) {
    case HandsChoice::Right:
        return "right hand";
    case HandsChoice::Left:
        return "left hand";
    case HandsChoice::Together:
        return "hands together";
    }
    return {};
}

std::string lower_first(std::string s) {
    if (!s.empty() && s[0] >= 'A' && s[0] <= 'Z') {
        s[0] = static_cast<char>(s[0] - 'A' + 'a');
    }
    return s;
}

std::uint16_t track_of(Hand hand, HandsChoice hands) {
    return hands == HandsChoice::Together && hand == Hand::Left ? 2 : 1;
}

// Adds the Part for a hand.
void add_part(Song& song, Hand hand, std::uint16_t track) {
    Part p;
    p.track = track;
    p.channel = 0;
    p.name = hand == Hand::Right ? "Right Hand" : "Left Hand";
    p.hand = hand;
    song.parts.push_back(std::move(p));
}

Song new_song(const ExerciseSettings& settings) {
    Song song;
    song.tempo = make_tempo_map(settings.ticks_per_quarter, settings.bpm);
    song.time_signatures = {TimeSignature{0, 4, 4}};
    return song;
}

void add_note(Song& song, std::int64_t start, std::int64_t end, int pitch, int velocity, Hand hand,
              std::uint16_t track, int finger) {
    Note n;
    n.start_tick = start;
    n.end_tick = end;
    n.pitch = static_cast<std::uint8_t>(pitch);
    n.velocity = static_cast<std::uint8_t>(std::clamp(velocity, 1, 127));
    n.hand = hand;
    n.track = track;
    n.finger = static_cast<std::int8_t>(finger);
    song.notes.push_back(n);
}

// A line of notes for one hand: pitches in playing order, fingers parallel (0 for none).
struct Line {
    Hand hand = Hand::Right;
    std::vector<int> pitches;
    std::vector<int> fingers;
};

// Orders `up` (ascending positions) and `down` (the same positions seen coming down, from the top)
// by direction. `down_pitches` may differ from the ascending ones (melodic minor).
void arrange(Direction dir, const std::vector<int>& up_pitches, const std::vector<int>& up_fingers,
             const std::vector<int>& down_pitches, Line& line) {
    const std::size_t n = up_pitches.size();
    auto push_up = [&] {
        for (std::size_t i = 0; i < n; ++i) {
            line.pitches.push_back(up_pitches[i]);
            line.fingers.push_back(up_fingers[i]);
        }
    };
    auto push_down = [&](std::size_t stop) { // positions n-1 .. stop
        for (std::size_t i = n; i-- > stop;) {
            line.pitches.push_back(down_pitches[i]);
            line.fingers.push_back(up_fingers[i]);
        }
    };
    switch (dir) {
    case Direction::Up:
        push_up();
        break;
    case Direction::Down:
        push_down(0);
        break;
    case Direction::UpDown:
        push_up();
        // The top is already played; the way down starts below it.
        for (std::size_t i = n - 1; i-- > 0;) {
            line.pitches.push_back(down_pitches[i]);
            line.fingers.push_back(up_fingers[i]);
        }
        break;
    }
}

// Places the lines on the keyboard (one shift for all of them), writes the notes and finishes the
// song. `lines` hold pitches relative to the default register.
ExerciseResult build_lines(std::vector<Line>& lines, HandsChoice hands,
                           const ExerciseSettings& settings, std::string title) {
    int low = 127;
    int high = 0;
    for (const Line& l : lines) {
        for (int p : l.pitches) {
            low = std::min(low, p);
            high = std::max(high, p);
        }
    }
    int shift = 0;
    if (!fit_octaves(low, high, settings.range, shift)) {
        return fail("the exercise does not fit on a keyboard of keys " +
                    std::to_string(settings.range.lowest) + " to " +
                    std::to_string(settings.range.highest));
    }
    ExerciseResult r;
    r.song = new_song(settings);
    const auto step =
        static_cast<std::int64_t>(std::llround(settings.note_beats * settings.ticks_per_quarter));
    for (const Line& l : lines) {
        const std::uint16_t track = track_of(l.hand, hands);
        add_part(r.song, l.hand, track);
        for (std::size_t i = 0; i < l.pitches.size(); ++i) {
            const auto start = static_cast<std::int64_t>(i) * step;
            add_note(r.song, start, start + step, l.pitches[i] + 12 * shift, settings.velocity,
                     l.hand, track, l.fingers[i]);
        }
    }
    finish_song(r.song);
    r.ok = true;
    r.title = std::move(title);
    return r;
}

// Default register: the right hand's lowest note is in C4..B4, the left's in C3..B3.
int default_base(int pc, Hand hand) {
    return (hand == Hand::Right ? kMiddleC : kMiddleC - 12) + pitch_class(pc);
}

} // namespace

std::vector<int> scale_fingers(int root_pc, ScaleType type, Hand hand, int octaves) {
    const Pattern* p = find_pattern(root_pc, type, hand);
    if (p == nullptr || octaves < 1 || octaves > 2) {
        return {};
    }
    std::vector<int> out;
    const int count = 7 * octaves + 1;
    for (int i = 0; i < count; ++i) {
        if (i == 0) {
            out.push_back(p->first);
        } else if (i == count - 1) {
            out.push_back(p->top);
        } else {
            out.push_back(p->cycle[static_cast<std::size_t>(i % 7)]);
        }
    }
    return out;
}

// ----- Scales -----

ExerciseResult make_scale_exercise(const ScaleSpec& spec, const ExerciseSettings& settings) {
    std::string why;
    if (!settings_ok(settings, why)) {
        return fail(why);
    }
    if (spec.octaves < 1 || spec.octaves > 2) {
        return fail("a scale exercise is one or two octaves");
    }
    const std::span<const int> asc = scale_intervals(spec.type);
    const std::span<const int> desc = scale_intervals_descending(spec.type);

    std::vector<Line> lines;
    const Hand hands[2] = {Hand::Right, Hand::Left};
    for (Hand hand : hands) {
        if ((hand == Hand::Right && spec.hands == HandsChoice::Left) ||
            (hand == Hand::Left && spec.hands == HandsChoice::Right)) {
            continue;
        }
        // Together, the left hand sits an octave below the right: both use the right hand's base.
        const int base =
            default_base(spec.root_pc, spec.hands == HandsChoice::Left ? Hand::Left : Hand::Right) -
            (hand == Hand::Left && spec.hands == HandsChoice::Together ? 12 : 0);
        std::vector<int> up;
        std::vector<int> down;
        for (int o = 0; o < spec.octaves; ++o) {
            for (std::size_t i = 0; i < asc.size(); ++i) {
                up.push_back(base + 12 * o + asc[i]);
                down.push_back(base + 12 * o + desc[i]);
            }
        }
        up.push_back(base + 12 * spec.octaves);
        down.push_back(base + 12 * spec.octaves);

        // Fingering is by position, and only a seven-note scale has a table.
        std::vector<int> fingers = asc.size() == 7
                                       ? scale_fingers(spec.root_pc, spec.type, hand, spec.octaves)
                                       : std::vector<int>{};
        fingers.resize(up.size(), 0);
        Line line;
        line.hand = hand;
        arrange(spec.direction, up, fingers, down, line);
        lines.push_back(std::move(line));
    }
    const Tonic tonic = default_tonic(spec.root_pc, spec.type);
    std::string title =
        spelled_name(SpelledNote{tonic.letter, tonic.accidental, tonic.pitch_class()}) + " " +
        lower_first(scale_type_name(spec.type)) + " scale, " + hands_text(spec.hands) + ", " +
        octaves_text(spec.octaves);
    return build_lines(lines, spec.hands, settings, std::move(title));
}

// ----- Chords and arpeggios -----

std::span<const int> chord_intervals(ChordQuality quality) {
    switch (quality) {
    case ChordQuality::Major:
        return kMajorTriad;
    case ChordQuality::Minor:
        return kMinorTriad;
    case ChordQuality::Diminished:
        return kDimTriad;
    case ChordQuality::Augmented:
        return kAugTriad;
    case ChordQuality::Sus2:
        return kSus2;
    case ChordQuality::Sus4:
        return kSus4;
    case ChordQuality::Dominant7:
        return kDom7;
    case ChordQuality::Major7:
        return kMaj7;
    case ChordQuality::Minor7:
        return kMin7;
    case ChordQuality::HalfDiminished7:
        return kHalfDim7;
    case ChordQuality::Diminished7:
        return kDim7;
    case ChordQuality::MinorMajor7:
        return kMinMaj7;
    case ChordQuality::Major6:
        return kMaj6;
    case ChordQuality::Minor6:
        return kMin6;
    case ChordQuality::Add9:
        return kAdd9;
    case ChordQuality::Dominant9:
        return kDom9;
    case ChordQuality::Major9:
        return kMaj9;
    case ChordQuality::Minor9:
        return kMin9;
    case ChordQuality::Power:
        return kPower;
    case ChordQuality::None:
        break;
    }
    return {};
}

namespace {

// The chord's tones for an inversion, as semitones above the root, ascending: the lowest
// `inversion` tones go up an octave. Element 0 is the bass.
std::vector<int> inverted_tones(ChordQuality q, int inversion) {
    std::vector<int> tones(chord_intervals(q).begin(), chord_intervals(q).end());
    for (int i = 0; i < inversion; ++i) {
        tones[static_cast<std::size_t>(i)] += 12;
    }
    std::sort(tones.begin(), tones.end());
    return tones;
}

std::string root_name(int root_pc, ChordQuality q) {
    const Tonic t =
        default_tonic(root_pc, minor_flavoured(q) ? ScaleType::NaturalMinor : ScaleType::Major);
    return spelled_name(SpelledNote{t.letter, t.accidental, t.pitch_class()});
}

} // namespace

ExerciseResult make_arpeggio_exercise(const ArpeggioSpec& spec, const ExerciseSettings& settings) {
    std::string why;
    if (!settings_ok(settings, why)) {
        return fail(why);
    }
    if (spec.octaves < 1 || spec.octaves > 2) {
        return fail("an arpeggio exercise is one or two octaves");
    }
    const std::span<const int> shape = chord_intervals(spec.quality);
    if (shape.empty()) {
        return fail("no chord type given");
    }
    if (shape.back() >= 12) {
        return fail("an arpeggio needs a chord that fits in an octave");
    }
    if (spec.inversion < 0 || spec.inversion >= static_cast<int>(shape.size())) {
        return fail("the chord has no such inversion");
    }
    const std::vector<int> tones = inverted_tones(spec.quality, spec.inversion);

    std::vector<Line> lines;
    for (Hand hand : {Hand::Right, Hand::Left}) {
        if ((hand == Hand::Right && spec.hands == HandsChoice::Left) ||
            (hand == Hand::Left && spec.hands == HandsChoice::Right)) {
            continue;
        }
        // The bass of the inversion is the first note; place it by its own pitch class.
        const int bass_pc = (spec.root_pc + tones.front()) % 12;
        const int base =
            default_base(bass_pc, spec.hands == HandsChoice::Left ? Hand::Left : Hand::Right) -
            (hand == Hand::Left && spec.hands == HandsChoice::Together ? 12 : 0);
        std::vector<int> up;
        for (int o = 0; o < spec.octaves; ++o) {
            for (int t : tones) {
                up.push_back(base + (t - tones.front()) + 12 * o);
            }
        }
        up.push_back(base + 12 * spec.octaves);
        Line line;
        line.hand = hand;
        arrange(spec.direction, up, std::vector<int>(up.size(), 0), up, line);
        lines.push_back(std::move(line));
    }
    std::string title =
        root_name(spec.root_pc, spec.quality) + " " + quality_words(spec.quality) + " arpeggio, ";
    if (spec.inversion == 1) {
        title += "first inversion, ";
    } else if (spec.inversion == 2) {
        title += "second inversion, ";
    } else if (spec.inversion == 3) {
        title += "third inversion, ";
    }
    title += hands_text(spec.hands) + ", " + octaves_text(spec.octaves);
    return build_lines(lines, spec.hands, settings, std::move(title));
}

// ----- Chord drills -----

namespace {

// The notes of one chord in the default register: the bass in C4..B4 (right hand) or C3..B3.
std::vector<int> chord_notes(const ChordDrillItem& item, int base_octave_c) {
    const std::vector<int> tones = inverted_tones(item.quality, item.inversion);
    const int bass_pc = (item.root_pc + tones.front()) % 12;
    const int bass = base_octave_c + bass_pc;
    std::vector<int> notes;
    for (int t : tones) {
        notes.push_back(bass + (t - tones.front()));
    }
    return notes;
}

} // namespace

ExerciseResult make_chord_drill(const ChordDrillSpec& spec, const ExerciseSettings& settings) {
    std::string why;
    if (!settings_ok(settings, why)) {
        return fail(why);
    }
    if (spec.items.empty()) {
        return fail("a chord drill needs at least one chord");
    }
    if (!(spec.step_beats > 0.0) || !(spec.hold_beats > 0.0) || spec.hold_beats > spec.step_beats ||
        !std::isfinite(spec.step_beats)) {
        return fail("the hold time must be above 0 and at most the step time");
    }
    const int ppq = settings.ticks_per_quarter;
    const auto step = static_cast<std::int64_t>(std::llround(spec.step_beats * ppq));
    const auto hold =
        std::max<std::int64_t>(1, static_cast<std::int64_t>(std::llround(spec.hold_beats * ppq)));
    if (step < 1) {
        return fail("the step time must be at least one tick");
    }

    ExerciseResult r;
    r.song = new_song(settings);
    const bool right = spec.hands != HandsChoice::Left;
    const bool left = spec.hands != HandsChoice::Right;
    if (right) {
        add_part(r.song, Hand::Right, 1);
    }
    if (left) {
        add_part(r.song, Hand::Left, spec.hands == HandsChoice::Together ? 2 : 1);
    }
    for (std::size_t i = 0; i < spec.items.size(); ++i) {
        const ChordDrillItem& item = spec.items[i];
        const std::span<const int> shape = chord_intervals(item.quality);
        if (shape.empty()) {
            return fail("no chord type given");
        }
        if (item.inversion < 0 || item.inversion >= static_cast<int>(shape.size())) {
            return fail("the chord has no such inversion");
        }
        // The right hand's chord (or the left hand's alone), and the left hand's bass below it.
        const int base_c = spec.hands == HandsChoice::Left ? kMiddleC - 12 : kMiddleC;
        std::vector<int> hand_notes = chord_notes(item, base_c);
        std::vector<int> all = hand_notes;
        if (spec.hands == HandsChoice::Together) {
            all.insert(all.begin(), hand_notes.front() - 12);
        }
        int shift = 0;
        if (!fit_octaves(all.front(), all.back(), settings.range, shift)) {
            return fail("the chord does not fit on the keyboard");
        }
        for (int& n : all) {
            n += 12 * shift;
        }
        for (int& n : hand_notes) {
            n += 12 * shift;
        }

        const Chord seen = detect_chord(all);
        const std::string asked =
            root_name(item.root_pc, item.quality) + std::string(" ") + quality_words(item.quality) +
            (item.inversion > 0 ? ", inversion " + std::to_string(item.inversion) : std::string());
        if (!seen.found) {
            return fail(asked + " is not recognised as a chord when played");
        }
        if (seen.root_pc != pitch_class(item.root_pc) || seen.quality != item.quality) {
            return fail(asked + " reads as " + chord_name(seen, spec.prefer_flats) +
                        " when played");
        }

        const std::int64_t start = static_cast<std::int64_t>(i) * step;
        const bool left_bass = spec.hands == HandsChoice::Together;
        if (spec.hands == HandsChoice::Left) {
            for (int n : hand_notes) {
                add_note(r.song, start, start + hold, n, settings.velocity, Hand::Left, 1, 0);
            }
        } else {
            for (int n : hand_notes) {
                add_note(r.song, start, start + hold, n, settings.velocity, Hand::Right, 1, 0);
            }
            if (left_bass) {
                add_note(r.song, start, start + hold, all.front(), settings.velocity, Hand::Left, 2,
                         0);
            }
        }
        ExerciseStep s;
        s.label = chord_name(seen, spec.prefer_flats);
        s.notes = all;
        s.start_tick = start;
        r.steps.push_back(std::move(s));
    }
    finish_song(r.song);
    for (ExerciseStep& s : r.steps) {
        s.start_s = r.song.tempo.seconds_at(s.start_tick);
    }
    r.ok = true;
    r.title = std::to_string(spec.items.size()) + (spec.items.size() == 1 ? " chord" : " chords") +
              ", " + hands_text(spec.hands);
    return r;
}

std::vector<ChordDrillItem> inversion_drill(int root_pc, ChordQuality quality) {
    std::vector<ChordDrillItem> items;
    const int count = static_cast<int>(chord_intervals(quality).size());
    for (int inversion = 0; inversion < count; ++inversion) {
        ChordDrillItem item{pitch_class(root_pc), quality, inversion};
        ChordDrillSpec spec;
        spec.items = {item};
        if (make_chord_drill(spec).ok) {
            items.push_back(item);
        }
    }
    return items;
}

} // namespace toonval
