// SPDX-License-Identifier: GPL-3.0-or-later
#include "synth_test.hpp"

#include "audio_out.hpp"

#include <chrono>
#include <cstdio>
#include <initializer_list>

namespace toonval::app {
namespace {

std::int64_t steady_ns() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

// Waits `ms` while keeping the audio output's bookkeeping going.
void wait(AudioOut& audio, int ms) {
    const std::int64_t until = steady_ns() + std::int64_t{ms} * 1'000'000;
    while (steady_ns() < until) {
        SDL_Delay(5);
        audio.update(steady_ns());
    }
}

} // namespace

int run_synth_test(const AudioOptions& options) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    AudioOut audio;
    if (!audio.start(options)) {
        return 1;
    }
    SynthSink& piano = audio.sink();

    std::printf("synth test: C major scale up and down, getting louder\n");
    const int scale[] = {60, 62, 64, 65, 67, 69, 71, 72, 71, 69, 67, 65, 64, 62, 60};
    int velocity = 40;
    for (const int note : scale) {
        piano.note_on(note, velocity);
        wait(audio, 260);
        piano.note_off(note);
        wait(audio, 20);
        velocity += 5;
    }

    std::printf("synth test: pedalled arpeggio, held 2 s after the last key\n");
    piano.sustain(true);
    for (const int note : {36, 43, 48, 52, 55, 60, 64, 67, 72}) {
        piano.note_on(note, 85);
        wait(audio, 120);
        piano.note_off(note);
    }
    wait(audio, 2000);
    piano.sustain(false);
    wait(audio, 800);

    std::printf(
        "synth test: ten-note fortissimo chord (the limiter holds it), then all notes off\n");
    for (const int note : {36, 43, 48, 52, 55, 60, 64, 67, 72, 76}) {
        piano.note_on(note, 127);
    }
    wait(audio, 1200);
    piano.all_notes_off();
    wait(audio, 500);

    audio.report(steady_ns());
    audio.stop();
    return 0;
}

} // namespace toonval::app
