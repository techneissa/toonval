// SPDX-License-Identifier: GPL-3.0-or-later
#include "song_transport.hpp"

#include <algorithm>

namespace toonval::app {

namespace {

ui::SongSound to_ui(SongSound s) {
    switch (s) {
    case SongSound::Mute:
        return ui::SongSound::Mute;
    case SongSound::OtherHand:
        return ui::SongSound::OtherHand;
    case SongSound::BothHands:
        return ui::SongSound::BothHands;
    }
    return ui::SongSound::Mute;
}

SongSound from_ui(ui::SongSound s) {
    switch (s) {
    case ui::SongSound::Mute:
        return SongSound::Mute;
    case ui::SongSound::OtherHand:
        return SongSound::OtherHand;
    case ui::SongSound::BothHands:
        return SongSound::BothHands;
    }
    return SongSound::Mute;
}

} // namespace

SongTransport::SongTransport(SongController& songs) : songs_(songs) {}

void SongTransport::update(std::int64_t now_ns) {
    view_.has_song = songs_.has_song();
    if (!view_.has_song) {
        view_ = ui::SongView{};
        bar_starts_.clear();
        bars_for_ = nullptr;
        countdown_total_s_ = 0;
        return;
    }
    const LoadedSong& loaded = songs_.song();
    if (bars_for_ != &loaded || bars_hash_ != loaded.hash || bar_starts_.empty()) {
        bar_starts_.clear();
        for (const Measure& m : loaded.song.measures) {
            bar_starts_.push_back(m.start_s);
        }
        bars_for_ = &loaded;
        bars_hash_ = loaded.hash;
    }
    view_.title = loaded.title;
    view_.position_s = songs_.position_s();
    view_.duration_s = songs_.duration_s();
    view_.playing = songs_.playing();
    view_.loop_enabled = songs_.loop_enabled();
    view_.loop_start_s = songs_.loop_start_s();
    view_.loop_end_s = songs_.loop_end_s();
    view_.bar_starts_s = bar_starts_;
    view_.sound = to_ui(songs_.song_sound());
    view_.skip_back_s = songs_.skip_back_seconds();
    view_.skip_forward_s = songs_.skip_forward_seconds();

    const int whole = songs_.countdown();
    if (whole <= 0) {
        countdown_total_s_ = 0;
        view_.countdown_left_s = 0.0;
        return;
    }
    if (countdown_total_s_ == 0) {
        countdown_total_s_ = whole;
        countdown_start_ns_ = now_ns;
    }
    // Smooth, but never more than a second away from the controller's own count.
    const double left = static_cast<double>(countdown_total_s_) -
                        static_cast<double>(now_ns - countdown_start_ns_) / 1e9;
    view_.countdown_left_s =
        std::clamp(left, static_cast<double>(whole - 1) + 1e-3, static_cast<double>(whole));
    view_.countdown_total_s = countdown_total_s_;
}

void SongTransport::toggle_play(std::int64_t now_ns) {
    songs_.toggle_play(now_ns);
    update(now_ns);
}

void SongTransport::replay(std::int64_t now_ns) {
    songs_.replay(now_ns);
    update(now_ns);
}

void SongTransport::skip_back(std::int64_t now_ns) {
    songs_.skip_back(now_ns);
    update(now_ns);
}

void SongTransport::skip_forward(std::int64_t now_ns) {
    songs_.skip_forward(now_ns);
    update(now_ns);
}

void SongTransport::seek(double song_s, std::int64_t now_ns) {
    songs_.seek(song_s, now_ns);
    update(now_ns);
}

void SongTransport::set_sound(ui::SongSound sound, std::int64_t now_ns) {
    songs_.set_song_sound(from_ui(sound), now_ns);
    update(now_ns);
}

} // namespace toonval::app
