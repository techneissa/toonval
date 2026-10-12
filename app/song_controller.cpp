// SPDX-License-Identifier: GPL-3.0-or-later
#include "song_controller.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

namespace toonval::app {

namespace {

void report_stuck(const Playback& p) {
    std::fprintf(stderr, "toonval: a song Playback was dropped with %zu notes sounding\n",
                 p.sounding_count());
}

} // namespace

const char* song_sound_name(SongSound sound) {
    switch (sound) {
    case SongSound::Mute:
        return "Mute";
    case SongSound::OtherHand:
        return "Other hand";
    case SongSound::BothHands:
        return "Both hands";
    }
    return "Mute";
}

PianoPlays to_piano_plays(SongSound sound) {
    switch (sound) {
    case SongSound::Mute:
        return PianoPlays::Nothing;
    case SongSound::OtherHand:
        return PianoPlays::OtherHand;
    case SongSound::BothHands:
        return PianoPlays::Both;
    }
    return PianoPlays::Nothing;
}

SongSound song_sound_from(PianoPlays plays) {
    switch (plays) {
    case PianoPlays::Nothing:
        return SongSound::Mute;
    case PianoPlays::OtherHand:
    case PianoPlays::Left:
    case PianoPlays::Right:
        return SongSound::OtherHand;
    case PianoPlays::Both:
        return SongSound::BothHands;
    }
    return SongSound::Mute;
}

const char* hands_name(HandFilter hands) {
    switch (hands) {
    case HandFilter::Both:
        return "Both hands";
    case HandFilter::Left:
        return "Left hand";
    case HandFilter::Right:
        return "Right hand";
    }
    return "Both hands";
}

const char* mode_name(PracticeMode mode) {
    switch (mode) {
    case PracticeMode::Wait:
        return "Wait";
    case PracticeMode::Flow:
        return "Flow";
    case PracticeMode::FreePlay:
        return "Free play";
    }
    return "Flow";
}

SongController::SongController() {
    playback_.set_teardown_check(&report_stuck);
    playback_.follow(&source_);
    flow_.follow(&source_);
    wait_.follow(&source_);
}

SongController::~SongController() {
    // The app calls shutdown() first; without it Playback's teardown check reports the bug.
    playback_.follow(nullptr);
    flow_.follow(nullptr);
    wait_.follow(nullptr);
}

// ---- Plumbing

void SongController::enqueue(std::vector<PlaybackMessage>& msgs) {
    for (PlaybackMessage m : msgs) {
        // Never earlier than a message already queued: the outbox is in Playback's order, so a
        // note-off cannot overtake its note-on.
        m.at_ns = std::max(m.at_ns, outbox_last_ns_);
        outbox_last_ns_ = m.at_ns;
        outbox_.push_back(m);
    }
    msgs.clear();
}

void SongController::pump(std::int64_t now_ns) {
    transport_.update(now_ns);
    transport_.take_ops(ops_);
    for (const TransportOp& op : ops_) {
        playback_.apply(op, scratch_);
        flow_.apply(op);
        wait_.apply(op);
        if (op.kind == TransportOp::Kind::Play) {
            start_run_if_needed(op.at_ns);
        }
    }
    ops_.clear();
    enqueue(scratch_);
}

void SongController::apply_mutes(std::int64_t now_ns) {
    bool left = true;
    bool right = true;
    bool unknown = true;
    bool drums = true;
    switch (sound_) {
    case SongSound::Mute:
        break;
    case SongSound::BothHands:
        left = right = unknown = drums = false;
        break;
    case SongSound::OtherHand:
        // The hand not practised sounds. The evaluator grades notes with no hand only when both
        // hands are practised (they are the player's then), so they stay muted exactly then and
        // sound otherwise. Drums are never the player's (a piano has none): they always sound.
        left = hands_ != HandFilter::Right;
        right = hands_ != HandFilter::Left;
        unknown = hands_ == HandFilter::Both;
        drums = false;
        break;
    }
    playback_.set_hand_muted(Hand::Left, left, now_ns, scratch_);
    playback_.set_hand_muted(Hand::Right, right, now_ns, scratch_);
    playback_.set_hand_muted(Hand::Unknown, unknown, now_ns, scratch_);
    playback_.set_drums_muted(drums, now_ns, scratch_);
    enqueue(scratch_);
}

void SongController::rebuild_evaluators(std::int64_t /*now_ns*/) {
    FlowOptions f;
    f.window_ms = options_.hit_window_ms;
    f.latency_ms = options_.latency_ms;
    f.hands = hands_;
    f.lookahead_s = options_.lookahead_s;
    WaitOptions w;
    w.hands = hands_;
    w.lookahead_s = options_.lookahead_s;
    const Song empty;
    const Song& song = loaded_ ? song_.song : empty;
    flow_.set_song(song, f);
    wait_.set_song(song, w);
    results_seen_ = 0;
    grade_by_note_.assign(song.notes.size(), -1);
    passes_.clear();
}

void SongController::restart_here(std::int64_t now_ns) {
    if (!loaded_) {
        return;
    }
    pump(now_ns);
    if (transport_.held()) {
        transport_.release(now_ns);
    }
    transport_.seek(now_ns, transport_.position_s());
    pump(now_ns);
    follow_wait(now_ns);
}

void SongController::follow_wait(std::int64_t now_ns) {
    const WaitDirective d = wait_.update(now_ns);
    if (!loaded_ || mode_ != PracticeMode::Wait) {
        return;
    }
    if (d.hold_now) {
        transport_.hold(now_ns);
    }
    if (d.release) {
        transport_.release(now_ns);
    }
    if (d.arm_hold) {
        transport_.hold_at(now_ns, d.hold_at_s);
    }
    pump(now_ns);
}

void SongController::start_run_if_needed(std::int64_t now_ns) {
    if (run_active_ || !loaded_) {
        return;
    }
    run_active_ = true;
    run_started_ns_ = now_ns;
    run_playing_ns_ = 0;
    passes_.clear();
}

const EvaluationSummary* SongController::summary() const {
    if (!loaded_) {
        return nullptr;
    }
    return mode_ == PracticeMode::Wait ? &wait_.summary() : &flow_.summary();
}

void SongController::collect_grades(std::int64_t now_ns) {
    while (!grades_.empty() && now_ns - grades_.front().at_ns > kGradeShowNs) {
        grades_.erase(grades_.begin());
    }
    const EvaluationSummary* s = summary();
    if (s == nullptr) {
        return;
    }
    for (std::size_t i = results_seen_; i < s->notes.size(); ++i) {
        const NoteResult& r = s->notes[i];
        if (r.note_index < grade_by_note_.size()) {
            grade_by_note_[r.note_index] = static_cast<std::int8_t>(r.grade);
        }
        grades_.push_back({r.note_index, r.grade, r.expected_pitch, r.hand, now_ns});
    }
    results_seen_ = s->notes.size();
}

void SongController::finish_run(std::int64_t now_ns) {
    if (!run_active_) {
        return;
    }
    run_active_ = false;
    if (mode_ == PracticeMode::Flow) {
        flow_.finish();
    }
    collect_grades(now_ns);
    std::vector<PassResult> passes;
    if (mode_ == PracticeMode::Wait) {
        wait_.take_passes(passes);
    } else {
        flow_.take_passes(passes);
    }
    passes_.insert(passes_.end(), passes.begin(), passes.end());
    const EvaluationSummary* s = summary();
    // A run where nothing was played (watching, or stepping away) is not an attempt.
    if (s != nullptr && s->all.graded > 0 && (s->all.hit > 0 || s->wrong > 0)) {
        RunContext ctx;
        ctx.song_hash = song_.hash;
        ctx.song_title = song_.title;
        ctx.mode = mode_ == PracticeMode::Wait ? RunMode::Wait : RunMode::Flow;
        ctx.hands = hands_;
        ctx.speed_percent = transport_.speed_percent();
        ctx.transpose_semitones = semitones_;
        ctx.duration_ms = run_playing_ns_ / 1'000'000;
        finished_.push_back({make_run_record(*s, ctx, passes_), run_started_ns_, now_ns});
    }
    passes_.clear();
    flow_.reset_results();
    wait_.reset_results();
    results_seen_ = 0;
    std::fill(grade_by_note_.begin(), grade_by_note_.end(), static_cast<std::int8_t>(-1));
}

void SongController::start_countdown(std::int64_t now_ns) {
    const int n = std::clamp(options_.resume_countdown_s, 1, 6);
    countdown_end_ns_ = now_ns + static_cast<std::int64_t>(n) * 1'000'000'000;
    countdown_ticks_left_ = n;
}

void SongController::cancel_countdown() {
    countdown_end_ns_.reset();
    countdown_ticks_left_ = 0;
}

double SongController::bar_start_at(double song_s) const {
    return snap_to_measure(song_.song, song_s, SnapMode::Floor);
}

// ---- Song

void SongController::load(LoadedSong song, std::int64_t now_ns) {
    pump(now_ns);
    finish_run(now_ns);
    cancel_countdown();
    loop_a_.reset();
    loop_b_.reset();
    song_ = std::move(song);
    loaded_ = true;
    playback_.set_song(song_.song, now_ns, scratch_);
    enqueue(scratch_);
    transport_.set_song(song_.song);
    window_.build(song_.song);
    grades_.clear();
    rebuild_evaluators(now_ns);
    apply_mutes(now_ns);
    pump(now_ns);
}

void SongController::unload(std::int64_t now_ns) {
    if (!loaded_) {
        return;
    }
    pump(now_ns);
    finish_run(now_ns);
    cancel_countdown();
    loop_a_.reset();
    loop_b_.reset();
    const Song empty;
    playback_.set_song(empty, now_ns, scratch_);
    enqueue(scratch_);
    transport_.set_song(empty);
    loaded_ = false;
    song_ = LoadedSong{};
    window_.build(empty);
    grades_.clear();
    rebuild_evaluators(now_ns);
    pump(now_ns);
}

// ---- Transport actions

void SongController::play(std::int64_t now_ns) {
    if (!loaded_) {
        return;
    }
    if (menu_open_) {
        resume_after_menu_ = true; // plays through the countdown when the menu closes
        return;
    }
    cancel_countdown();
    pump(now_ns);
    if (transport_.ended()) {
        replay(now_ns);
        return;
    }
    transport_.play(now_ns);
    pump(now_ns);
    follow_wait(now_ns);
}

void SongController::pause(std::int64_t now_ns) {
    cancel_countdown();
    resume_after_menu_ = false;
    if (!loaded_) {
        return;
    }
    pump(now_ns);
    transport_.pause(now_ns);
    pump(now_ns);
}

void SongController::toggle_play(std::int64_t now_ns) {
    if (!loaded_) {
        return;
    }
    if (counting_down()) {
        pause(now_ns);
    } else if (menu_open_) {
        resume_after_menu_ = !resume_after_menu_;
    } else if (transport_.playing()) {
        pause(now_ns);
    } else {
        play(now_ns);
    }
}

void SongController::replay(std::int64_t now_ns) {
    if (!loaded_) {
        return;
    }
    pump(now_ns);
    finish_run(now_ns);
    const double start = transport_.loop_enabled() ? transport_.loop_start_s() : 0.0;
    transport_.seek(now_ns, start);
    pump(now_ns);
    if (menu_open_) {
        resume_after_menu_ = true;
        return;
    }
    cancel_countdown();
    transport_.play(now_ns);
    pump(now_ns);
    follow_wait(now_ns);
}

void SongController::seek(double song_s, std::int64_t now_ns) {
    if (!loaded_) {
        return;
    }
    pump(now_ns);
    transport_.seek(now_ns, song_s);
    pump(now_ns);
    follow_wait(now_ns);
}

void SongController::skip_back(std::int64_t now_ns) {
    if (!loaded_) {
        return;
    }
    pump(now_ns);
    const double pos = transport_.position_s();
    const double step = std::clamp(options_.skip_back_seconds, 1, 30);
    const double floor = transport_.loop_enabled() && pos >= transport_.loop_start_s() - 1e-9
                             ? transport_.loop_start_s()
                             : 0.0;
    seek(std::max(floor, pos - step), now_ns);
}

void SongController::skip_forward(std::int64_t now_ns) {
    if (!loaded_) {
        return;
    }
    pump(now_ns);
    const double step = std::clamp(options_.skip_forward_seconds, 1, 30);
    seek(std::min(transport_.duration_s(), transport_.position_s() + step), now_ns);
}

void SongController::change_speed(int delta_percent, std::int64_t now_ns) {
    set_speed(transport_.speed_percent() + delta_percent, now_ns);
}

void SongController::set_speed(int percent, std::int64_t now_ns) {
    pump(now_ns);
    transport_.set_speed_percent(now_ns, percent);
    pump(now_ns);
}

void SongController::set_loop_start_here(std::int64_t now_ns) {
    if (!loaded_) {
        return;
    }
    pump(now_ns);
    loop_a_ = bar_start_at(transport_.position_s());
    if (loop_b_ && std::abs(*loop_b_ - *loop_a_) >= kMinLoopSeconds) {
        set_loop(*loop_a_, *loop_b_, now_ns);
    }
}

void SongController::set_loop_end_here(std::int64_t now_ns) {
    if (!loaded_) {
        return;
    }
    pump(now_ns);
    const double pos = transport_.position_s();
    loop_b_ = snap_to_measure(song_.song, pos + 1e-6, SnapMode::Ceil);
    if (!loop_a_) {
        loop_a_ = bar_start_at(pos); // B alone loops the current bar
    }
    if (std::abs(*loop_b_ - *loop_a_) >= kMinLoopSeconds) {
        set_loop(*loop_a_, *loop_b_, now_ns);
    }
}

bool SongController::set_loop(double start_s, double end_s, std::int64_t now_ns) {
    if (!loaded_) {
        return false;
    }
    pump(now_ns);
    if (!transport_.set_loop(now_ns, start_s, end_s)) {
        return false;
    }
    transport_.set_loop_enabled(now_ns, true);
    loop_a_ = transport_.loop_start_s();
    loop_b_ = transport_.loop_end_s();
    pump(now_ns);
    return true;
}

void SongController::clear_loop(std::int64_t now_ns) {
    loop_a_.reset();
    loop_b_.reset();
    if (!loaded_) {
        return;
    }
    pump(now_ns);
    transport_.clear_loop(now_ns);
    pump(now_ns);
}

// ---- Practice choices

void SongController::set_practice_hands(HandFilter hands, std::int64_t now_ns) {
    if (hands == hands_) {
        return;
    }
    pump(now_ns);
    finish_run(now_ns); // each hand's results are kept apart
    hands_ = hands;
    rebuild_evaluators(now_ns);
    apply_mutes(now_ns);
    restart_here(now_ns);
}

void SongController::set_song_sound(SongSound sound, std::int64_t now_ns) {
    pump(now_ns);
    sound_ = sound;
    apply_mutes(now_ns);
}

void SongController::cycle_song_sound(std::int64_t now_ns) {
    switch (sound_) {
    case SongSound::Mute:
        set_song_sound(SongSound::OtherHand, now_ns);
        break;
    case SongSound::OtherHand:
        set_song_sound(SongSound::BothHands, now_ns);
        break;
    case SongSound::BothHands:
        set_song_sound(SongSound::Mute, now_ns);
        break;
    }
}

void SongController::set_mode(PracticeMode mode, std::int64_t now_ns) {
    if (mode == PracticeMode::FreePlay) {
        mode = PracticeMode::Flow; // with a song, the choice is Flow or Wait
    }
    if (mode == mode_) {
        return;
    }
    pump(now_ns);
    finish_run(now_ns);
    mode_ = mode;
    grades_.clear();
    restart_here(now_ns); // Wait Mode finds the chord to wait for; Flow lets go of a hold
}

void SongController::toggle_mode(std::int64_t now_ns) {
    set_mode(mode_ == PracticeMode::Wait ? PracticeMode::Flow : PracticeMode::Wait, now_ns);
}

void SongController::set_transpose(int semitones, std::int64_t now_ns) {
    semitones_ = std::clamp(semitones, -kMaxTransposeSemitones, kMaxTransposeSemitones);
    pump(now_ns);
    source_.set(Transposition(semitones_, keyboard_), now_ns);
    pump(now_ns);
}

void SongController::set_keyboard(KeyRange keyboard, std::int64_t now_ns) {
    if (keyboard.lowest == keyboard_.lowest && keyboard.highest == keyboard_.highest) {
        return;
    }
    keyboard_ = keyboard;
    set_transpose(semitones_, now_ns);
}

void SongController::set_options(const SongOptions& options, std::int64_t now_ns) {
    if (options == options_) {
        return;
    }
    const bool grading = options.hit_window_ms != options_.hit_window_ms ||
                         options.latency_ms != options_.latency_ms ||
                         options.lookahead_s != options_.lookahead_s;
    options_ = options;
    if (grading && loaded_) {
        pump(now_ns);
        finish_run(now_ns);
        rebuild_evaluators(now_ns);
        restart_here(now_ns);
    }
}

// ---- App events

void SongController::menu_opened(std::int64_t now_ns) {
    if (menu_open_) {
        return;
    }
    menu_open_ = true;
    resume_after_menu_ = loaded_ && (transport_.playing() || counting_down());
    cancel_countdown();
    if (loaded_) {
        pump(now_ns);
        transport_.pause(now_ns);
        pump(now_ns);
    }
}

void SongController::menu_closed(std::int64_t now_ns) {
    if (!menu_open_) {
        return;
    }
    menu_open_ = false;
    if (resume_after_menu_ && loaded_) {
        start_countdown(now_ns);
    }
    resume_after_menu_ = false;
}

void SongController::lost_focus(std::int64_t now_ns) {
    pause(now_ns);
    silence(now_ns);
}

void SongController::silence(std::int64_t now_ns) {
    pump(now_ns);
    playback_.silence(now_ns, scratch_);
    enqueue(scratch_);
}

void SongController::shutdown(std::int64_t now_ns, std::vector<PlaybackMessage>& out) {
    pump(now_ns);
    finish_run(now_ns);
    cancel_countdown();
    if (loaded_ && transport_.playing()) {
        transport_.pause(now_ns);
        pump(now_ns);
    }
    playback_.silence(now_ns, scratch_);
    enqueue(scratch_);
    out.insert(out.end(), outbox_.begin(), outbox_.end());
    outbox_.clear();
    outbox_last_ns_ = INT64_MIN;
}

// ---- Per frame

void SongController::update(std::int64_t now_ns, std::span<const MidiEvent> player_events) {
    if (last_now_ns_ == 0) {
        last_now_ns_ = now_ns;
    }
    const bool was_playing = transport_.playing();
    pump(now_ns);

    if (countdown_end_ns_) {
        const std::int64_t end = *countdown_end_ns_;
        while (countdown_ticks_left_ > 0 &&
               now_ns >= end - static_cast<std::int64_t>(countdown_ticks_left_) * 1'000'000'000) {
            --countdown_ticks_left_;
            scratch_.push_back({PlaybackMessage::Kind::NoteOn, kTickChannel, kTickNote,
                                kTickVelocity, now_ns, transport_.position_s()});
            scratch_.push_back({PlaybackMessage::Kind::NoteOff, kTickChannel, kTickNote, 0,
                                now_ns + kTickLengthNs, transport_.position_s()});
            enqueue(scratch_);
        }
        if (now_ns >= end) {
            cancel_countdown();
            if (loaded_) {
                transport_.play(now_ns);
                pump(now_ns);
                follow_wait(now_ns);
            }
        }
    }

    if (loaded_) {
        for (const MidiEvent& ev : player_events) {
            flow_.apply(ev);
            wait_.apply(ev);
        }
        flow_.update(now_ns);
        follow_wait(now_ns);
        collect_grades(now_ns);

        std::vector<PassResult> passes;
        wait_.take_passes(passes); // the other mode's passes are dropped
        if (mode_ == PracticeMode::Wait) {
            for (const PassResult& p : passes) {
                transport_.report_pass(now_ns, p.clean);
            }
            passes_.insert(passes_.end(), passes.begin(), passes.end());
        }
        passes.clear();
        flow_.take_passes(passes);
        if (mode_ == PracticeMode::Flow) {
            for (const PassResult& p : passes) {
                transport_.report_pass(now_ns, p.clean);
            }
            passes_.insert(passes_.end(), passes.begin(), passes.end());
        }
        pump(now_ns);

        if (run_active_ && was_playing) {
            run_playing_ns_ += std::max<std::int64_t>(0, now_ns - last_now_ns_);
        }
        if (transport_.ended()) {
            finish_run(now_ns);
        }
    } else {
        collect_grades(now_ns);
    }
    last_now_ns_ = now_ns;
}

void SongController::take_due_messages(std::int64_t now_ns, std::vector<PlaybackMessage>& out) {
    while (!outbox_.empty() && outbox_.front().at_ns <= now_ns) {
        out.push_back(outbox_.front());
        outbox_.pop_front();
    }
    if (outbox_.empty()) {
        outbox_last_ns_ = INT64_MIN;
    }
}

std::vector<FinishedRun> SongController::take_finished_runs() {
    std::vector<FinishedRun> out;
    out.swap(finished_);
    return out;
}

// ---- What the screen shows

int SongController::bar() const {
    if (!loaded_ || song_.song.measures.empty()) {
        return 0;
    }
    const auto& m = song_.song.measures;
    const double pos = transport_.position_s();
    auto it = std::upper_bound(m.begin(), m.end(), pos + 1e-9,
                               [](double s, const Measure& x) { return s < x.start_s; });
    return it == m.begin() ? m.front().number : std::prev(it)->number;
}

int SongController::bar_count() const {
    return loaded_ && !song_.song.measures.empty() ? song_.song.measures.back().number : 0;
}

void SongController::visible_notes(double ahead_s, std::vector<VisibleNote>& out) const {
    out.clear();
    if (!loaded_) {
        return;
    }
    NoteWindowOptions o;
    o.ahead_s = ahead_s;
    o.hand = HandFilter::Both;
    o.transposition = source_.value();
    window_.query(transport_.position_s(), o, out);
}

std::optional<KeyRange> SongController::key_range() const {
    if (!loaded_) {
        return std::nullopt;
    }
    int lo = 128;
    int hi = -1;
    // Lowest and highest song pitch, then mapped: the map is monotonic apart from folding, and
    // folded notes land inside the keyboard anyway.
    int song_lo = 128;
    int song_hi = -1;
    for (const Note& n : song_.song.notes) {
        if (!n.drum) {
            song_lo = std::min(song_lo, static_cast<int>(n.pitch));
            song_hi = std::max(song_hi, static_cast<int>(n.pitch));
        }
    }
    if (song_hi < 0) {
        return std::nullopt;
    }
    for (const int p : {song_lo, song_hi}) {
        const TransposedPitch t = source_.value().map(p);
        if (t.playable) {
            lo = std::min(lo, t.pitch);
            hi = std::max(hi, t.pitch);
        }
    }
    if (hi < 0) {
        return std::nullopt;
    }
    return KeyRange{lo, hi};
}

bool SongController::waiting() const {
    return loaded_ && mode_ == PracticeMode::Wait && transport_.held() &&
           wait_.waiting_for_s() >= 0.0;
}

std::vector<SongController::HintKey> SongController::waiting_keys() const {
    std::vector<HintKey> out;
    if (!waiting()) {
        return out;
    }
    const double at = wait_.waiting_for_s();
    const auto& notes = song_.song.notes;
    auto it = std::lower_bound(notes.begin(), notes.end(), at - 2.0 * kChordSeconds,
                               [](const Note& n, double s) { return n.start_s < s; });
    for (const int key : wait_.waiting_keys()) {
        HintKey h{key, Hand::Unknown};
        bool found = false;
        for (auto n = it; n != notes.end() && n->start_s <= at + 2.0 * kChordSeconds; ++n) {
            if (n->drum || source_.value().expected_pitch(*n).pitch != key) {
                continue;
            }
            if (!found) {
                h.hand = n->hand;
                found = true;
            } else if (h.hand != n->hand) {
                h.hand = Hand::Unknown;
            }
        }
        out.push_back(h);
    }
    return out;
}

int SongController::grade_of(std::uint32_t note_index) const {
    return note_index < grade_by_note_.size() ? grade_by_note_[note_index] : -1;
}

int SongController::combo() const {
    const EvaluationSummary* s = summary();
    return s != nullptr ? s->combo : 0;
}

int SongController::longest_combo() const {
    const EvaluationSummary* s = summary();
    return s != nullptr ? s->longest_combo : 0;
}

int SongController::countdown() const {
    if (!countdown_end_ns_) {
        return 0;
    }
    const std::int64_t left = *countdown_end_ns_ - last_now_ns_;
    return std::clamp(static_cast<int>((left + 999'999'999) / 1'000'000'000), 1, 6);
}

std::string SongController::status_line() const {
    if (!loaded_) {
        return {};
    }
    std::string s = song_.title.empty() ? "Untitled" : song_.title;
    const auto add = [&](const std::string& part) {
        s += " \xC2\xB7 "; // a middle dot
        s += part;
    };
    add(mode_name(mode_));
    add(hands_name(hands_));
    add(std::to_string(transport_.speed_percent()) + "%");
    add("Bar " + std::to_string(bar()) + "/" + std::to_string(bar_count()));
    if (transport_.loop_enabled()) {
        add("Loop");
    } else if (loop_a_) {
        add("A set");
    }
    add(std::string("Sound: ") + song_sound_name(sound_));
    if (semitones_ != 0) {
        add((semitones_ > 0 ? "+" : "") + std::to_string(semitones_) + " st");
    }
    if (mode_ == PracticeMode::Flow) {
        add("Combo " + std::to_string(combo()));
    }
    switch (transport_.state()) {
    case TransportState::Paused:
        add(counting_down() ? "Ready" : "Paused");
        break;
    case TransportState::Held:
        add("Waiting");
        break;
    case TransportState::Ended:
        add("End");
        break;
    case TransportState::CountIn:
    case TransportState::Playing:
        break;
    }
    return s;
}

// ---- Saved state

SessionState SongController::session_state(const SessionState& base) const {
    SessionState s = base;
    s.speed_percent = transport_.speed_percent();
    s.practice_hands = hands_;
    s.piano_plays = to_piano_plays(sound_);
    s.mode = mode_;
    s.transpose_semitones = semitones_;
    if (!loaded_ || song_.library_path.empty()) {
        s.song_path.clear();
        s.song_hash = 0;
        s.position_s = 0.0;
        s.loop_start_s = s.loop_end_s = 0.0;
        s.loop_enabled = false;
        return s;
    }
    s.song_path = song_.library_path;
    s.song_hash = song_.hash;
    s.position_s = transport_.ended() ? 0.0 : transport_.position_s();
    s.loop_start_s = transport_.loop_enabled() ? transport_.loop_start_s() : 0.0;
    s.loop_end_s = transport_.loop_enabled() ? transport_.loop_end_s() : 0.0;
    s.loop_enabled = transport_.loop_enabled();
    return s;
}

void SongController::apply_session(const SessionState& state, std::int64_t now_ns) {
    set_mode(state.mode, now_ns);
    set_practice_hands(state.practice_hands, now_ns);
    set_song_sound(song_sound_from(state.piano_plays), now_ns);
    set_speed(state.speed_percent, now_ns);
    set_transpose(state.transpose_semitones, now_ns);
    if (!loaded_) {
        return;
    }
    if (state.has_loop()) {
        set_loop(state.loop_start_s, state.loop_end_s, now_ns);
        if (!state.loop_enabled) {
            pump(now_ns);
            transport_.set_loop_enabled(now_ns, false);
            pump(now_ns);
        }
    }
    seek(state.position_s, now_ns);
}

// ---- Results

std::string results_line(const RunRecord& record, const AttemptChange& change) {
    const auto pct = [](double a) { return std::to_string(static_cast<int>(std::lround(a * 100.0))) + "%"; };
    std::string s = "Results: ";
    std::vector<std::string> parts;
    const bool right = record.hands != HandFilter::Left && record.right.graded > 0;
    const bool left = record.hands != HandFilter::Right && record.left.graded > 0;
    if (right) {
        parts.push_back("right " + pct(record.right.accuracy()));
    }
    if (left) {
        parts.push_back("left " + pct(record.left.accuracy()));
    }
    if (!right && !left) {
        parts.push_back(pct(record.accuracy(record.hands)));
    }
    for (std::size_t i = 0; i < parts.size(); ++i) {
        s += (i > 0 ? ", " : "") + parts[i];
    }
    s += " \xC2\xB7 longest combo " + std::to_string(record.longest_combo);
    if (!change.has_previous) {
        s += " \xC2\xB7 first try like this";
    } else {
        const int delta = static_cast<int>(std::lround(change.accuracy_delta * 100.0));
        switch (change.trend) {
        case Trend::Better:
            s += " \xC2\xB7 up " + std::to_string(std::max(delta, 1)) + "% on last time";
            break;
        case Trend::Worse:
            s += " \xC2\xB7 down " + std::to_string(std::max(-delta, 1)) + "% on last time";
            break;
        case Trend::Same:
            s += " \xC2\xB7 same as last time";
            break;
        }
    }
    if (change.new_best) {
        s += " \xC2\xB7 new best";
    }
    return s;
}

} // namespace toonval::app
