// SPDX-License-Identifier: GPL-3.0-or-later
#include "song_transport.hpp"

#include "toonval/song_builder.hpp"

#include <doctest/doctest.h>

using namespace toonval;
using toonval::app::SongController;
using toonval::app::SongTransport;
namespace ui = toonval::app::ui;

namespace {

constexpr std::int64_t kS = 1'000'000'000;

// 16 bars of quarter notes at 120 BPM: 32 s.
app::LoadedSong test_song() {
    app::LoadedSong loaded;
    Song& song = loaded.song;
    constexpr int kTpq = 480;
    song.tempo = make_tempo_map(kTpq, 120.0);
    for (int beat = 0; beat < 64; ++beat) {
        Note n;
        n.start_tick = static_cast<std::int64_t>(beat) * kTpq;
        n.end_tick = n.start_tick + kTpq - 20;
        n.pitch = static_cast<std::uint8_t>(60 + beat % 8);
        n.hand = Hand::Right;
        song.notes.push_back(n);
    }
    finish_song(song);
    loaded.title = "Test";
    loaded.hash = 7;
    return loaded;
}

struct Fixture {
    SongController songs;
    SongTransport port{songs};

    explicit Fixture(int resume_s = 3, int back_s = 5, int forward_s = 5) {
        app::SongOptions o;
        o.resume_countdown_s = resume_s;
        o.skip_back_seconds = back_s;
        o.skip_forward_seconds = forward_s;
        songs.set_options(o, kS);
        songs.load(test_song(), kS);
        songs.update(kS, {});
        port.update(kS);
    }
    void at(std::int64_t now) {
        songs.update(now, {});
        port.update(now);
    }
};

} // namespace

TEST_CASE("the view shows the loaded song paused at the start, with its bars") {
    Fixture f;
    const ui::SongView& v = f.port.view();
    CHECK(v.has_song);
    CHECK(v.title == "Test");
    CHECK_FALSE(v.playing);
    CHECK(v.position_s == 0.0);
    CHECK(v.duration_s == doctest::Approx(32.0).epsilon(0.05));
    CHECK(v.bar_starts_s.size() == 16);
}

TEST_CASE("without a song the transport is hidden") {
    SongController songs;
    SongTransport port(songs);
    port.update(kS);
    CHECK_FALSE(port.view().has_song);
    CHECK(port.view().countdown_left_s == 0.0);
}

TEST_CASE("the skip steps come from the settings and move the song") {
    Fixture f(3, 2, 7);
    CHECK(f.port.view().skip_back_s == 2);
    CHECK(f.port.view().skip_forward_s == 7);
    f.port.skip_forward(2 * kS);
    CHECK(f.port.view().position_s == doctest::Approx(7.0));
    f.port.skip_back(2 * kS);
    CHECK(f.port.view().position_s == doctest::Approx(5.0));
}

TEST_CASE("closing the settings counts down the saved seconds, smoothly, then plays on") {
    Fixture f(4);
    f.port.toggle_play(kS);
    f.at(2 * kS);
    CHECK(f.port.view().playing);
    f.songs.menu_opened(2 * kS);
    f.at(3 * kS);
    CHECK_FALSE(f.port.view().playing);
    f.songs.menu_closed(3 * kS);
    f.at(3 * kS);
    CHECK(f.port.view().countdown_total_s == 4);
    CHECK(f.port.view().countdown_left_s == doctest::Approx(4.0).epsilon(0.01));
    f.at(3 * kS + kS / 2);
    CHECK(f.port.view().countdown_left_s == doctest::Approx(3.5).epsilon(0.01));
    CHECK_FALSE(f.port.view().playing);
    f.at(7 * kS + kS / 10);
    CHECK(f.port.view().countdown_left_s == 0.0);
    CHECK(f.port.view().playing);
}

TEST_CASE("the song sound goes to the controller") {
    Fixture f;
    f.port.set_sound(ui::SongSound::BothHands, kS);
    CHECK(f.songs.song_sound() == app::SongSound::BothHands);
    CHECK(f.port.view().sound == ui::SongSound::BothHands);
    f.port.set_sound(ui::SongSound::Mute, kS);
    CHECK(f.songs.song_sound() == app::SongSound::Mute);
}

TEST_CASE("replay starts from the beginning and plays") {
    Fixture f;
    f.port.seek(10.0, kS);
    CHECK(f.port.view().position_s == doctest::Approx(10.0));
    f.port.replay(2 * kS);
    CHECK(f.port.view().position_s == doctest::Approx(0.0));
    CHECK(f.port.view().playing);
}
