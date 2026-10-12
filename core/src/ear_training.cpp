// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/ear_training.hpp"

#include "toonval/pitch.hpp"
#include "toonval/song_builder.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace toonval {
namespace {

constexpr int kMaxInterval = 36;

bool question_equal(const EarQuestion& a, const EarQuestion& b) {
    return a.first == b.first && a.second == b.second;
}

} // namespace

std::uint64_t EarRng::next() {
    state_ += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state_;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

std::uint64_t EarRng::below(std::uint64_t n) {
    if (n <= 1) {
        return 0;
    }
    // Reject the top sliver of the 64-bit range that would make the low values likelier.
    const std::uint64_t limit = UINT64_MAX - UINT64_MAX % n;
    std::uint64_t v = next();
    while (v >= limit) {
        v = next();
    }
    return v % n;
}

EarQuestionResult EarTrainer::next() {
    EarQuestionResult result;
    const KeyRange range = settings_.range;
    if (range.lowest > range.highest || range.lowest < kMidiNoteMin ||
        range.highest > kMidiNoteMax) {
        result.message = "the keyboard range is empty or outside MIDI";
        return result;
    }
    auto usable = [&](int note) {
        return range.contains(note) && !(settings_.white_keys_only && is_black_key(note));
    };

    if (settings_.kind == EarQuestionKind::Note) {
        std::vector<int> notes;
        for (int n = range.lowest; n <= range.highest; ++n) {
            if (usable(n)) {
                notes.push_back(n);
            }
        }
        if (notes.empty()) {
            result.message = "no note of the range can be asked";
            return result;
        }
        EarQuestion q;
        q.kind = EarQuestionKind::Note;
        // Never the same note twice in a row, when there is another.
        for (int attempt = 0; attempt < 64; ++attempt) {
            q.first = q.second = notes[rng_.below(notes.size())];
            if (notes.size() == 1 || !have_previous_ || !question_equal(q, previous_)) {
                break;
            }
        }
        previous_ = q;
        have_previous_ = true;
        result.ok = true;
        result.question = q;
        return result;
    }

    if (settings_.min_interval < 1 || settings_.max_interval > kMaxInterval ||
        settings_.min_interval > settings_.max_interval) {
        result.message = "the interval sizes must be within 1.." + std::to_string(kMaxInterval) +
                         " semitones, smallest first";
        return result;
    }
    // Every signed interval size that has at least one first note for which both notes fit.
    struct Choice {
        int semitones;
        std::vector<int> firsts;
    };
    std::vector<Choice> choices;
    for (int size = settings_.min_interval; size <= settings_.max_interval; ++size) {
        for (int sign : {1, -1}) {
            if (sign < 0 && !settings_.allow_descending) {
                continue;
            }
            Choice c{sign * size, {}};
            for (int first = range.lowest; first <= range.highest; ++first) {
                if (usable(first) && usable(first + c.semitones)) {
                    c.firsts.push_back(first);
                }
            }
            if (!c.firsts.empty()) {
                choices.push_back(std::move(c));
            }
        }
    }
    if (choices.empty()) {
        result.message = "no pair of notes of the range makes an interval of that size";
        return result;
    }
    // Draw the size first and then the direction, so small and large intervals are equally
    // likely: pick among the distinct sizes, then among the directions that exist for it.
    EarQuestion q;
    q.kind = EarQuestionKind::Interval;
    q.harmonic = settings_.harmonic;
    for (int attempt = 0; attempt < 64; ++attempt) {
        std::vector<int> sizes;
        for (const Choice& c : choices) {
            if (sizes.empty() || sizes.back() != std::abs(c.semitones)) {
                sizes.push_back(std::abs(c.semitones));
            }
        }
        const int size = sizes[rng_.below(sizes.size())];
        std::vector<const Choice*> directions;
        for (const Choice& c : choices) {
            if (std::abs(c.semitones) == size) {
                directions.push_back(&c);
            }
        }
        const Choice& pick = *directions[rng_.below(directions.size())];
        q.first = pick.firsts[rng_.below(pick.firsts.size())];
        q.second = q.first + pick.semitones;
        if (!have_previous_ || !question_equal(q, previous_)) {
            break;
        }
    }
    previous_ = q;
    have_previous_ = true;
    result.ok = true;
    result.question = q;
    return result;
}

bool check_pitch_answer(const EarQuestion& q, int played_note, bool ignore_octave) {
    if (!is_valid_note(played_note)) {
        return false;
    }
    const int want = expected_pitch(q);
    return ignore_octave ? pitch_class(played_note) == pitch_class(want) : played_note == want;
}

bool check_interval_answer(const EarQuestion& q, int answered_semitones, bool check_direction) {
    if (q.kind != EarQuestionKind::Interval) {
        return false;
    }
    return check_direction ? answered_semitones == q.semitones()
                           : std::abs(answered_semitones) == std::abs(q.semitones());
}

std::string interval_name(int semitones) {
    static constexpr std::array<const char*, 13> kNames = {
        "unison",      "minor 2nd", "major 2nd", "minor 3rd", "major 3rd", "perfect 4th", "tritone",
        "perfect 5th", "minor 6th", "major 6th", "minor 7th", "major 7th", "octave"};
    const int size = std::abs(semitones);
    if (size < static_cast<int>(kNames.size())) {
        return kNames[static_cast<std::size_t>(size)];
    }
    return std::to_string(size) + " semitones";
}

Song ear_question_song(const EarQuestion& q, const ExerciseSettings& play) {
    Song song;
    const int ppq = std::clamp(play.ticks_per_quarter, 1, 32767);
    song.tempo = make_tempo_map(ppq, play.bpm);
    song.time_signatures = {TimeSignature{0, 4, 4}};
    const auto beat = std::max<std::int64_t>(1, std::llround(std::max(play.note_beats, 0.0) * ppq));
    auto add = [&](std::int64_t start, std::int64_t length, int pitch) {
        Note n;
        n.start_tick = start;
        n.end_tick = start + length;
        n.pitch = static_cast<std::uint8_t>(std::clamp(pitch, 0, 127));
        n.velocity = static_cast<std::uint8_t>(std::clamp(play.velocity, 1, 127));
        n.track = 1;
        song.notes.push_back(n);
    };
    if (q.kind == EarQuestionKind::Note) {
        add(0, beat, q.first);
    } else if (q.harmonic) {
        add(0, 2 * beat, q.first);
        add(0, 2 * beat, q.second);
    } else {
        add(0, beat, q.first);
        add(beat, beat, q.second);
    }
    finish_song(song);
    return song;
}

} // namespace toonval
