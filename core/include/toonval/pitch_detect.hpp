// SPDX-License-Identifier: GPL-3.0-or-later
// Streaming piano pitch detector. Feed it mono float blocks; it reports note-on, note-off and
// "not sure" events. It never reports a guess: when the pitch cannot be confirmed it says "not
// sure". After construction it never allocates, takes no locks and does bounded work per block,
// so it can run on the analysis thread behind the capture ring buffer.
#pragma once

#include <cstdint>
#include <memory>

namespace toonval {

inline constexpr int kPianoLowNote = 21;   // A0
inline constexpr int kPianoHighNote = 108; // C8

// The hard tuning limit: a pitch more than this many cents from the nearest key (under the
// current A4) is never reported as that key. Open detection says "not sure"; expected-note
// mode answers "not sure" too.
inline constexpr float kMaxCentsFromKey = 30.0f;

enum class PitchMethod : std::uint8_t {
    Yin, // de Cheveigne and Kawahara 2002, cumulative mean normalised difference
    Mpm, // McLeod and Wyvill 2005, normalised square difference (NSDF)
};

enum class PitchEventType : std::uint8_t {
    NoteOn,   // a note started and its pitch was confirmed
    NoteOff,  // the current note stopped (silence, a new onset, or its pitch faded)
    NotSure,  // a note started but its pitch could not be confirmed; the app should wait
    Expected, // expected-note mode: the verdict for the expected note or chord after an onset
};

// Expected-note mode verdicts. Neighbor and octave are relative to the expected note: a
// NeighborBelow verdict means the player hit the key one semitone below the expected one.
enum class ExpectedVerdict : std::uint8_t {
    NotSure,
    Expected,
    NeighborBelow,
    NeighborAbove,
    OctaveBelow,
    OctaveAbove,
};

struct PitchEvent {
    PitchEventType type = PitchEventType::NoteOn;
    int note = -1;                 // MIDI note; -1 when unknown (NotSure, an unsure verdict)
    float frequency_hz = 0.0f;     // measured fundamental for NoteOn, 0 otherwise
    float confidence = 0.0f;       // 0..1
    std::int64_t onset_sample = 0; // sample index of the onset this event belongs to
    std::int64_t sample = 0;       // sample index at which the event was decided
    // Expected events only.
    ExpectedVerdict verdict = ExpectedVerdict::NotSure;
    std::uint16_t chord_present = 0; // bit i: expected note i was heard
    std::uint16_t chord_absent = 0;  // bit i: expected note i was clearly missing
};

struct PitchDetectorConfig {
    int sample_rate = 48000;
    PitchMethod method = PitchMethod::Mpm;
    float a4_hz = 440.0f;
    // Learn the instrument's A4 from confident notes (median of the last few, within 50 cents).
    bool learn_tuning = false;
    int low_note = kPianoLowNote;
    int high_note = kPianoHighNote;
    // Noise gate: the gate opens when the RMS of a hop exceeds max(gate_min_rms, floor * ratio).
    float gate_min_rms = 2e-4f;
    float gate_ratio = 4.0f;
    // A note is reported after this many consecutive confident analysis hops.
    int confirm_frames = 3;
    // Minimum per-frame confidence that counts toward confirmation.
    float min_confidence = 0.8f;
    // Periods of the candidate that must be in the window before the octave-below check can rule
    // out the lower octave: two periods of the octave below, four of the candidate. Smaller is
    // faster and riskier.
    float octave_check_periods = 4.0f;
};

// MIDI note plus fractional cents for a frequency under a given A4.
float note_from_hz(float hz, float a4_hz);

// Samples of audio after the onset the detector needs before it may confirm `note`: about two
// periods of the octave below (four periods of the note), or two periods for the lowest octave,
// where nothing lower exists. Bass notes need the longest windows, treble the shortest.
int pitch_window_samples(int note, int sample_rate, float octave_check_periods = 4.0f);

class PitchDetector {
  public:
    // Largest event count one call can produce per 128 samples fed; size `out` from it.
    static constexpr int kMaxEventsPerHop = 3;
    static constexpr int kHop = 128;

    explicit PitchDetector(const PitchDetectorConfig& config = {});
    ~PitchDetector();
    PitchDetector(const PitchDetector&) = delete;
    PitchDetector& operator=(const PitchDetector&) = delete;
    PitchDetector(PitchDetector&&) noexcept;
    PitchDetector& operator=(PitchDetector&&) noexcept;

    // Feed `count` mono samples. Writes up to `capacity` events to `out` and returns how many.
    // Events that do not fit are dropped and counted in dropped_events(). A null `samples` (or a
    // count of 0 or less) does nothing and returns 0.
    int process(const float* samples, int count, PitchEvent* out, int capacity);

    // Forget all audio and note state (keeps the calibration and learned tuning).
    void reset();
    std::int64_t samples_processed() const;
    std::int64_t dropped_events() const;

    // Noise floor: listen to the silent room or instrument between begin and end (about three
    // seconds is enough). The floor becomes the mean hop RMS heard meanwhile.
    void begin_noise_calibration();
    void end_noise_calibration();
    float noise_floor_rms() const;
    void set_noise_floor_rms(float rms);

    void set_a4_hz(float a4_hz);
    float a4_hz() const; // the learned value when learn_tuning is on

    void set_method(PitchMethod method);

    // Expected-note mode: after each onset, answer only "is this the expected note (or chord)?"
    // with an Expected event. Up to 8 notes; count 0 returns to open detection.
    void set_expected_notes(const int* notes, int count);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// One-shot expected-note checks on a frame that starts at the note's onset. Used by the streaming
// detector's expected mode and directly by tests and the evaluator. Allocation-free after
// construction.
struct ExpectedNoteResult {
    ExpectedVerdict verdict = ExpectedVerdict::NotSure;
    int heard_note = -1;
    float confidence = 0.0f;
    float cents = 0.0f; // of the heard note against the tuning
};

enum class ChordNoteState : std::uint8_t { NotSure, Present, Absent };

inline constexpr int kMaxChordNotes = 8;

struct ChordCheckResult {
    int count = 0;
    int notes[kMaxChordNotes] = {};
    ChordNoteState states[kMaxChordNotes] = {};
    float scores[kMaxChordNotes] = {}; // 0..1: share of the note's own partials clearly present
};

class ExpectedNoteChecker {
  public:
    explicit ExpectedNoteChecker(int sample_rate = 48000);
    ~ExpectedNoteChecker();
    ExpectedNoteChecker(const ExpectedNoteChecker&) = delete;
    ExpectedNoteChecker& operator=(const ExpectedNoteChecker&) = delete;

    // Samples after the onset needed to judge `note` (and its octave below) / a chord.
    static int window_samples(int note, int sample_rate);
    static int chord_window_samples(const int* notes, int count, int sample_rate);
    // Longest frame either check uses; longer frames keep their most recent part.
    static constexpr int kMaxFrame = 4096;

    ExpectedNoteResult check(const float* frame, int length, int expected_note,
                             float a4_hz = 440.0f);
    ChordCheckResult check_chord(const float* frame, int length, const int* notes, int count,
                                 float a4_hz = 440.0f);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace toonval
