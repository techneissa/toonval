// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "toonval/hand.hpp"
#include "toonval/midi_event.hpp"
#include "toonval/note_window.hpp"
#include "toonval/song.hpp"
#include "toonval/transport.hpp"
#include "toonval/transpose.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace toonval {

// Where the song clock was at each wall time, rebuilt from the Transport's ops. The evaluators use
// it to place a key press in the song; it has no clock of its own.
class TransportTimeline {
  public:
    // Ops in the order the Transport produced them. Click ops are ignored.
    void apply(const TransportOp& op);
    // The song position at wall time `ns`: inside a Move it interpolates, between ops it is where
    // the last op left the clock. Before the first op it is where the first op started.
    double song_at(std::int64_t ns) const;
    // The playback rate (speed / 100) at wall time `ns`: the speed of the last Move at or before
    // it, 1.0 before any Move.
    double rate_at(std::int64_t ns) const;
    // Drops history before `ns`, keeping the op in force at `ns`.
    void forget_before(std::int64_t ns);
    void clear() { spans_.clear(); }

  private:
    struct Span {
        std::int64_t at_ns = 0;
        std::int64_t end_ns = 0; // == at_ns except for a Move
        double from_s = 0.0;
        double to_s = 0.0;
        int speed_percent = 100;
    };
    std::vector<Span> spans_; // in op order, so sorted by at_ns
};

enum class NoteGrade : std::uint8_t {
    Perfect,
    Good,
    Early,
    Late,
    Missed,
    Played, // Wait Mode: the note was played; Wait Mode has no timing grade
};

// What became of one expected note.
struct NoteResult {
    std::uint32_t note_index = 0; // into Song::notes
    int pass = 0;                 // see PassResult
    NoteGrade grade = NoteGrade::Missed;
    int expected_pitch = 0; // the key expected, after transposition
    Hand hand = Hand::Unknown;
    int measure = 1;
    double song_s = 0.0;         // the note's start
    std::int64_t arrival_ns = 0; // when the song clock reached the note
    std::int64_t press_ns = 0;   // Flow: the matched press, latency corrected; Wait: when the
                                 // chord was complete. 0 when Missed.
    std::int64_t error_ns = 0;   // Flow: press - arrival (negative is early). Wait: how long the
                                 // song waited for the chord (0 if it never stopped).
    bool released_early = false; // Wait Mode with hold_full_length: let go before the end
};

struct GradeCounts {
    int perfect = 0;
    int good = 0;
    int early = 0;
    int late = 0;
    int missed = 0;
    int played = 0; // Wait Mode
};

struct HandStats {
    int graded = 0; // notes with a final result
    int hit = 0;    // every grade except Missed
    // hit / graded, or 1 when nothing was graded yet.
    double accuracy() const {
        return graded == 0 ? 1.0 : static_cast<double>(hit) / static_cast<double>(graded);
    }
};

// One bar of the trouble-spot map.
struct MeasureStats {
    int measure = 1;
    int graded = 0;
    int missed = 0;
    int wrong = 0; // wrong presses placed in this bar
};

// The result of a run, as plain values so it can be saved as it is.
struct EvaluationSummary {
    GradeCounts counts;
    // Presses that matched no expected note. Counted on their own; they never change the grade
    // of a note.
    int wrong = 0;
    // Notes of the practised hands that no key of the keyboard can play (Transposition gave
    // playable = false). Neither hit nor missed.
    int unplayable = 0;
    int combo = 0;
    int longest_combo = 0;
    HandStats left;
    HandStats right;
    HandStats unknown; // notes with Hand::Unknown, graded when practising both hands
    HandStats all;
    std::vector<MeasureStats> measures; // sorted by measure; only bars that have something
    std::vector<NoteResult> notes;      // in the order results became final
};

// One pass through the A-B loop, reported once every note of it has a final result.
struct PassResult {
    int pass = 0;
    double start_s = 0.0; // where the pass began: the loop start, or a seek or play position
    double end_s = 0.0;   // the loop end it wrapped from
    int graded = 0;
    int missed = 0;
    int wrong = 0;
    bool released_early = false; // Wait Mode
    // Began at or before the loop start, had no seek inside, no miss, no wrong press and (Wait
    // Mode) no note let go early. This is what Transport::report_pass wants.
    bool clean = false;
};

namespace detail {
// Bookkeeping for one pass, shared by both evaluators.
struct PassState {
    double start_s = 0.0;
    double end_s = 0.0;
    std::int64_t start_ns = 0;
    std::int64_t end_ns = 0;
    double loop_start_s = 0.0; // where the wrap that ended it jumped to
    bool ended = false;
    bool interrupted = false; // a seek inside it
    bool reported = false;    // every result of it is final
    bool dropped = false;     // ended by a seek: never reported to the caller
    bool moved = false;
    PassResult result;
};

// The passes not yet handed to the caller, oldest first, numbered from `base`. Passes end and are
// reported in order, so the reported ones are a prefix: each frame looks only at the passes after
// it, and take() forgets the prefix. However many loops, it holds the passes the caller has not
// taken plus the one in progress.
class PassLog {
  public:
    // Keeps only `current`, renumbered 0.
    void reset(PassState current);
    PassState& back() { return passes_.back(); }
    const PassState& back() const { return passes_.back(); }
    // The pass with this number; it must not have been taken.
    PassState& at(int number) { return passes_[static_cast<std::size_t>(number - base_)]; }
    // Appends `p` as the newest pass and numbers it.
    void push(PassState p);
    // The newest pass that started at or before `ns`, or the oldest kept.
    int number_at(std::int64_t ns) const;
    // Marks ended passes reported, oldest first, while `ready(pass)` holds.
    template <typename Ready> void report(Ready ready) {
        while (reported_ < passes_.size()) {
            PassState& p = passes_[reported_];
            if (!p.dropped && (!p.ended || !ready(p))) {
                return;
            }
            p.reported = true;
            ++reported_;
        }
    }
    // Hands each reported pass that was not dropped to `take`, then forgets them all.
    template <typename Take> void take(Take take) {
        for (std::size_t i = 0; i < reported_; ++i) {
            if (!passes_[i].dropped) {
                take(passes_[i]);
            }
        }
        passes_.erase(passes_.begin(), passes_.begin() + static_cast<std::ptrdiff_t>(reported_));
        base_ += static_cast<int>(reported_);
        reported_ = 0;
    }
    std::size_t size() const { return passes_.size(); }

  private:
    std::vector<PassState> passes_;
    int base_ = 0;
    std::size_t reported_ = 0;
};
} // namespace detail

// Flow Mode: the song keeps going and each expected note gets one grade.
//
// Inputs, in the order a frame produces them: the Transport's ops (apply), then the player's MIDI
// events with their arrival stamps (apply), then update(now_ns). Times are on the Transport's
// clock. Nothing reads a clock.
//
// Rules:
// - Timing is in wall time, so the window means the same at every speed. A press's time is its
//   stamp minus latency_ms. error = press - arrival, where arrival is when the song clock reached
//   the note's start (from the Moves, so speed changes are exact).
// - A press matches a note when the key is the note's expected pitch (Transposition) and
//   |error| <= window. Both edges are inside the window. |error| <= perfect_fraction * window is
//   Perfect, <= good_fraction * window is Good, otherwise Early (error < 0) or Late.
// - Each press satisfies at most one expected note and each note at most one press. Among all
//   such assignments the evaluator takes the one with the most matches, then the least total
//   |error|. A press that matches nothing is a wrong note: counted, never able to turn a match
//   into a miss. A note with no press is Missed.
// - Notes of the same expected pitch whose starts are within kUnisonSeconds (both hands on one
//   key, or two notes folded onto one key) are one key press: a single press grades them all. Wait
//   Mode merges them the same way, so both modes agree on how many strikes a passage takes.
// - A press that matches nothing is not wrong when it is the key of a practised note starting
//   within lookahead_s after it (song seconds; set it to the display's NoteWindowOptions::ahead_s,
//   so a key the player can see coming is never wrong). It is pressed early and simply not
//   counted. A press after its note's window is still wrong.
// - Only notes of the practised hand(s) are graded: HandFilter::Both grades Left, Right and
//   Unknown; Left or Right grades only that hand. Drum notes are never graded.
// - Combo: +1 per hit note; Missed and wrong presses reset it to 0. Results become final in time
//   order, so the combo follows the order the notes came in.
// - Results are final once nothing that comes later could change them: a group of notes and
//   presses is decided when the latest of them is more than a window behind both the song clock
//   and now - settle_ms. Events stamped earlier than that which arrive later are too late (a press
//   is then matched only against notes still undecided).
// - Presses before the first play, and from one window after a pause or the song's end until
//   the next play, are ignored, so noodling never counts as wrong. A late press just after a
//   pause still hits.
// - A loop wrap starts a new pass; a late press just after the wrap can still hit the last note
//   of the old pass. A seek drops every undecided note and press (neither hit nor missed) and
//   starts a new pass.
// - A change of transposition (set_transposition or a followed TranspositionSource) acts like a
//   seek in place: undecided notes and presses are dropped, notes are expected in the new key from
//   then on, presses that match nothing in the next window are not wrong, and the pass is not
//   clean.
//
// Options are clamped to the ranges below; a NaN takes the default.
struct FlowOptions {
    double window_ms = 80.0;        // clamped to 10..500
    double perfect_fraction = 0.35; // of the window, clamped to 0..1
    double good_fraction = 0.70;    // of the window, clamped to perfect_fraction..1
    double latency_ms = 0.0;        // the player's measured lag, subtracted from each press; ±1000
    double settle_ms = 15.0;        // how late an event may arrive behind its stamp; 0..1000
    HandFilter hands = HandFilter::Both;
    Transposition transposition{}; // ignored while following a TranspositionSource
    double lookahead_s = 4.0;      // see "not wrong" above; 0..60
};

// Song seconds: notes this close are one chord in Wait Mode, and notes on one key this close are
// one key press in both modes. 30 ms is below the shortest gap a player can strike one key twice
// in (about 60 ms) and above the spread of a chord written as one but quantised apart.
inline constexpr double kChordSeconds = 0.030;
inline constexpr double kUnisonSeconds = kChordSeconds;

class FlowEvaluator {
  public:
    FlowEvaluator() { reset_clock(); }
    explicit FlowEvaluator(const Song& song, const FlowOptions& options = {}) {
        reset_clock();
        set_song(song, options);
    }

    // Copies what it needs from the song and resets the results, undecided notes and presses. It
    // keeps what it has seen of the Transport's clock: if the transport keeps playing, the new
    // song's notes are graded from its next Move (presses within one window after are never
    // wrong); after Transport::set_song, its Pause and Seek ops say the clock stopped, in either
    // order, and grading starts again at the next Play.
    void set_song(const Song& song, const FlowOptions& options);
    const FlowOptions& options() const { return options_; }

    // Expects notes in the new key from `now_ns` (see the rules above). The same value does
    // nothing.
    void set_transposition(const Transposition& transposition, std::int64_t now_ns);
    // Reads the transposition from the session's one TranspositionSource instead: a change is
    // picked up at the next apply() or update(), as set_transposition() at the source's
    // changed_at_ns(). The source must outlive the evaluator, or be unset with follow(nullptr).
    void follow(const TranspositionSource* source);

    void apply(const TransportOp& op);
    // NoteOn with velocity > 0 is a press. Releases, the pedal and synthetic events change nothing.
    void apply(const MidiEvent& ev);
    // Decides what can be decided by `now_ns`.
    void update(std::int64_t now_ns);
    // Decides everything now, as if no more input could come (for the results screen).
    void finish();
    // Clears the results but keeps the song, options, clock and undecided notes. The pass in
    // progress becomes pass 0 and is never clean, since it lost results.
    void reset_results();

    const EvaluationSummary& summary() const { return summary_; }
    // Appends finished passes to `out` and forgets them.
    void take_passes(std::vector<PassResult>& out);

    // Undecided notes and presses, for tests.
    std::size_t pending_notes() const { return pending_.size(); }
    std::size_t pending_presses() const { return presses_.size(); }
    // Passes held: the one in progress and those not yet taken. For tests.
    std::size_t kept_passes() const { return passes_.size(); }

  private:
    struct Target {
        double start_s = 0.0;
        int key = 0;
        int measure = 1;
        std::vector<std::uint32_t> notes;
    };
    struct Pending {
        std::uint32_t target = 0;
        std::int64_t arrival_ns = 0;
        int pass = 0;
        bool done = false;
    };
    struct Press {
        int key = 0;
        std::int64_t time_ns = 0;
        bool done = false;
    };
    struct Outcome {
        std::int64_t time_ns = 0;
        bool wrong = false; // a press that matched nothing; else a target's result
        std::uint32_t target = 0;
        int pass = 0;
        std::int64_t arrival_ns = 0;
        std::int64_t press_ns = 0;
        bool hit = false;
    };

    void reset_clock();
    void build_targets();
    void sync_transposition();
    // Decides what is decided by `at_ns`, drops the rest, and ignores presses stamped before it.
    void drop_undecided(std::int64_t at_ns);
    void resolve(std::int64_t frontier);
    // Decides one closed group: notes [a0, a0 + n) of pending_ and presses [b0, b0 + m).
    void decide_group(std::size_t a0, std::size_t n, std::size_t b0, std::size_t m);
    void apply_outcomes(std::int64_t safe);
    void apply_outcome(const Outcome& o);
    bool ignored(std::int64_t ns) const;
    // A press that matched nothing but is the key of a note sounding or coming soon.
    bool excused(const Press& p) const;
    int measure_at(double song_s) const;
    NoteGrade grade(std::int64_t error_ns) const;

    FlowOptions options_;
    std::int64_t window_ns_ = 80'000'000;
    std::int64_t perfect_ns_ = 0;
    std::int64_t good_ns_ = 0;
    std::int64_t latency_ns_ = 0;
    std::int64_t settle_ns_ = 0;

    std::vector<Note> notes_;
    std::vector<Target> targets_;      // sorted by start
    std::vector<double> unplayable_s_; // starts of practised notes no key can play, sorted
    std::vector<Measure> measures_;

    TransportTimeline timeline_;
    std::vector<Pending> pending_;
    std::vector<Press> presses_;
    std::vector<Outcome> outcomes_;
    detail::PassLog passes_;
    // Wall-time spans where presses are ignored: [first, second). The last may be open.
    std::vector<std::array<std::int64_t, 2>> dead_;
    std::int64_t ignore_before_ns_ = 0;
    // Presses stamped in [first, second) that match nothing are not wrong (after a key change).
    std::array<std::int64_t, 2> grace_{0, 0};
    std::int64_t clock_ns_ = 0;
    bool moving_ = false;

    const TranspositionSource* source_ = nullptr;
    std::uint64_t source_version_ = 0;
    bool source_synced_ = false;

    EvaluationSummary summary_;
};

// Wait Mode: the song stops at each chord until it is played.
//
// Same inputs as FlowEvaluator. update() returns what the Transport should do now.
//
// Rules:
// - A chord is the practised, playable notes whose starts are within kChordSeconds of the
//   chord's first note, with notes on one key merged (kUnisonSeconds). The evaluator asks the
//   Transport to hold at the start of the next unplayed chord.
// - A chord is played when all its keys are down at the same moment. A key counts only if it was
//   pressed at most early_ms (wall time, at the speed of that moment) before the song clock
//   reaches the chord, and its press has not already played an earlier chord. So a key held from
//   before does not count until it is pressed again, and one press never plays two chords.
// - A press stamped while paused, or before the first play, never plays a chord; the key must be
//   pressed again after play.
// - A note-on for a key the evaluator thinks is down is a new strike: its key-up was lost (a
//   dropped event, a hot-plug). release_all() lets go of every key at once for a lost connection.
// - A chord may be played before the clock reaches it; the clock then does not stop for it.
// - With hold_full_length, each note of a played chord must stay down until release_slack_ms
//   before its end. Letting go earlier holds the song where it is until that key is pressed again,
//   and marks the note released_early.
// - A press is wrong when its key is in neither the waiting chord, nor a later chord starting
//   within lookahead_s (song seconds) of the press, nor a practised note still sounding. A key
//   pressed early for a later chord is not wrong, and does not play that chord either. Wrong
//   presses are counted in the waiting chord's bar and never block the chord. Ignored while paused.
// - Key state comes from note-on and note-off only (velocity-0 note-on is a release); the sustain
//   pedal never holds a key. All Notes Off and All Sound Off release every key. Events are applied
//   in stamp order once now - settle_ms has passed them; a late event older than the last one
//   applied for its key is dropped.
// - A seek or loop wrap makes the first chord at or after the new position the waiting one, and
//   every key down at that moment must be pressed again. So does a change of transposition or of
//   song (set_song keeps the clock, as FlowEvaluator::set_song does), and the pass is not clean.
//
// Options are clamped to the ranges below; a NaN takes the default.
struct WaitOptions {
    double early_ms = 150.0; // 0..2000
    bool hold_full_length = false;
    double release_slack_ms = 100.0; // hold_full_length: letting go this close to the end is fine;
                                     // 0..10000
    double settle_ms = 15.0;         // 0..1000
    HandFilter hands = HandFilter::Both;
    Transposition transposition{}; // ignored while following a TranspositionSource
    double lookahead_s = 4.0;      // see "wrong" above; 0..60
};

// What the caller does to the Transport after update(), in this order: hold_now -> hold(now),
// release -> release(now), arm_hold -> hold_at(now, hold_at_s). Repeating a directive is harmless.
struct WaitDirective {
    bool hold_now = false;
    bool release = false;
    bool arm_hold = false;
    double hold_at_s = 0.0;
};

class WaitEvaluator {
  public:
    WaitEvaluator() { reset_clock(); }
    explicit WaitEvaluator(const Song& song, const WaitOptions& options = {}) {
        reset_clock();
        set_song(song, options);
    }

    // As FlowEvaluator::set_song: keeps the clock, so update() holds at the new song's next chord.
    void set_song(const Song& song, const WaitOptions& options);
    const WaitOptions& options() const { return options_; }
    // Waits for chords in the new key from `now_ns`: the waiting chord is the first at or after
    // the position, and keys down must be pressed again. The same value does nothing.
    void set_transposition(const Transposition& transposition, std::int64_t now_ns);
    // As FlowEvaluator::follow.
    void follow(const TranspositionSource* source);

    void apply(const TransportOp& op);
    void apply(const MidiEvent& ev);
    // Every key up as of `now_ns`, as All Notes Off would, but not the player's doing (no
    // released_early). For a lost MIDI connection; CC120, CC123 and the queue's synthetic resync
    // event do the same.
    void release_all(std::int64_t now_ns);
    WaitDirective update(std::int64_t now_ns);
    // As FlowEvaluator::reset_results; a pending "press it again" for hold_full_length is dropped.
    void reset_results();

    const EvaluationSummary& summary() const { return summary_; }
    void take_passes(std::vector<PassResult>& out);

    // The start of the chord being waited for, or a negative number when none is left.
    double waiting_for_s() const;
    // Keys of the chord being waited for, after transposition.
    std::vector<int> waiting_keys() const;
    // As FlowEvaluator::kept_passes.
    std::size_t kept_passes() const { return passes_.size(); }

  private:
    struct ChordKey {
        int key = 0;
        std::vector<std::uint32_t> notes;
    };
    struct Chord {
        double start_s = 0.0;
        int measure = 1;
        std::vector<ChordKey> keys;
    };
    struct Key {
        bool down = false;
        bool consumed = false; // this press already played a chord
        std::int64_t press_ns = 0;
        double press_s = 0.0;
        double press_rate = 1.0;
        std::int64_t last_ns = 0;
        bool seen = false;
    };
    struct Obligation { // hold_full_length
        int key = 0;
        double end_s = 0.0;
        std::size_t result = 0; // index into summary_.notes of the first note on this key
        std::size_t result_count = 0;
        bool needs_repress = false;
    };
    struct Sounding {
        int key = 0;
        double end_s = 0.0;
    };

    void reset_clock();
    void build_chords();
    void sync_transposition();
    void process(const MidiEvent& ev);
    void press(int key, std::int64_t ns);
    void release_key(int key, std::int64_t ns, bool by_player);
    void try_play(std::int64_t ns);
    void restart_at(double song_s, std::int64_t ns);
    bool expected_press(int key, double song_s, double early_s) const;
    void new_pass(double start_s, std::int64_t ns);

    WaitOptions options_;
    std::int64_t early_ns_ = 0;
    std::int64_t slack_ns_ = 0;
    std::int64_t settle_ns_ = 0;

    std::vector<Note> notes_;
    std::vector<Chord> chords_; // sorted by start
    std::vector<double> unplayable_s_;
    std::size_t next_ = 0; // the chord being waited for

    TransportTimeline timeline_;
    std::vector<MidiEvent> inbox_; // sorted by stamp
    std::array<Key, 128> keys_{};
    std::vector<Obligation> obligations_;
    std::vector<Sounding> sounding_;
    detail::PassLog passes_;
    double position_s_ = 0.0;
    std::int64_t last_op_ns_ = 0; // the latest time an op has reported
    bool playing_ = false;
    bool held_ = false;
    std::int64_t held_since_ns_ = 0;
    std::int64_t restart_ns_ = 0; // presses stamped before this belong to before a seek or wrap
    std::vector<std::array<std::int64_t, 2>> dead_;

    const TranspositionSource* source_ = nullptr;
    std::uint64_t source_version_ = 0;
    bool source_synced_ = false;

    EvaluationSummary summary_;
};

} // namespace toonval
