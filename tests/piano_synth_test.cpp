// SPDX-License-Identifier: GPL-3.0-or-later
#include "piano_synth.hpp"
#include "tiny_sf2.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <thread>
#include <vector>

#include <doctest/doctest.h>

using toonval::app::PianoSynth;

namespace {

constexpr int kRate = 48000;
constexpr int kBlock = 128;

// Renders `seconds` of audio in device-sized blocks and returns the interleaved samples.
std::vector<float> render(PianoSynth& synth, double seconds) {
    const int frames = static_cast<int>(seconds * kRate);
    std::vector<float> out(static_cast<std::size_t>(frames) * 2);
    for (int done = 0; done < frames; done += kBlock) {
        synth.render(out.data() + static_cast<std::size_t>(done) * 2,
                     std::min(kBlock, frames - done));
    }
    return out;
}

float peak(const std::vector<float>& s, std::size_t from = 0) {
    float p = 0.0F;
    for (std::size_t i = from; i < s.size(); ++i) {
        p = std::max(p, std::fabs(s[i]));
    }
    return p;
}

bool all_finite(const std::vector<float>& s) {
    return std::all_of(s.begin(), s.end(), [](float x) { return std::isfinite(x); });
}

// Runs `body` once with the tiny sine SoundFont and once with the real piano when the build has it
// (TOONVAL_PIANO_SF2_FILE or the download; see cmake/PianoAsset.cmake).
void for_each_font(const std::function<void(PianoSynth&)>& body) {
    {
        INFO("font: tiny sine");
        PianoSynth synth;
        const std::vector<std::uint8_t> sf2 = toonval::test::tiny_sf2();
        REQUIRE(synth.load_memory(sf2.data(), sf2.size(), kRate));
        body(synth);
    }
#ifdef TOONVAL_TEST_PIANO_SF2
    if (std::filesystem::exists(TOONVAL_TEST_PIANO_SF2)) {
        INFO("font: " << TOONVAL_TEST_PIANO_SF2);
        PianoSynth synth;
        REQUIRE_MESSAGE(synth.load_file(TOONVAL_TEST_PIANO_SF2, kRate), synth.error());
        body(synth);
    }
#endif
}

} // namespace

TEST_CASE("a C major chord sounds, stays finite and peaks below 0 dBFS") {
    for_each_font([](PianoSynth& synth) {
        synth.note_on(60, 100);
        synth.note_on(64, 100);
        synth.note_on(67, 100);
        const auto out = render(synth, 1.0);
        CHECK(all_finite(out));
        CHECK(peak(out) > 0.05F);
        CHECK(peak(out) <= PianoSynth::kCeiling);
    });
}

TEST_CASE("a loud cluster of twenty notes is held under the ceiling by the limiter") {
    for_each_font([](PianoSynth& synth) {
        for (int note = 48; note < 68; ++note) {
            synth.note_on(note, 127);
        }
        const auto out = render(synth, 0.5);
        CHECK(all_finite(out));
        CHECK(peak(out) <= PianoSynth::kCeiling);
        CHECK(peak(out) > 0.5F); // loud, so the limiter is what holds it
    });
}

TEST_CASE("a released note decays to silence") {
    for_each_font([](PianoSynth& synth) {
        synth.note_on(60, 100);
        render(synth, 0.5);
        synth.note_off(60);
        const auto tail = render(synth, 3.0);
        // The last 100 ms are below -80 dBFS and no voice is left.
        CHECK(peak(tail, tail.size() - 2 * 4800) < 1e-4F);
        CHECK(synth.active_voices() == 0);
    });
}

TEST_CASE("the sustain pedal holds released notes until it comes up") {
    for_each_font([](PianoSynth& synth) {
        synth.sustain(true);
        for (const int note : {60, 64, 67, 72}) {
            synth.note_on(note, 90);
            render(synth, 0.15);
            synth.note_off(note);
        }
        const auto held = render(synth, 1.0);
        // A second after the last key went up, the pedal keeps all four notes sounding.
        CHECK(peak(held, held.size() - 2 * 4800) > 1e-3F);
        CHECK(synth.active_voices() >= 4);
        synth.sustain(false);
        const auto tail = render(synth, 3.0);
        CHECK(peak(tail, tail.size() - 2 * 4800) < 1e-4F);
        CHECK(synth.active_voices() == 0);
    });
}

TEST_CASE("all notes off silences everything within 15 ms, pedal included") {
    for_each_font([](PianoSynth& synth) {
        synth.sustain(true);
        for (const int note : {48, 60, 64, 67, 72}) {
            synth.note_on(note, 110);
        }
        synth.note_off(60);
        render(synth, 0.3);
        synth.all_notes_off();
        const auto after = render(synth, 0.05);
        // From 15 ms on, below -60 dBFS.
        CHECK(peak(after, 2 * 720) < 1e-3F);
        // And the pedal is up: a new note released now does not hang on.
        synth.note_on(62, 100);
        render(synth, 0.1);
        synth.note_off(62);
        const auto tail = render(synth, 3.0);
        CHECK(peak(tail, tail.size() - 2 * 4800) < 1e-4F);
    });
}

TEST_CASE("more notes than voices: the cap holds and the oldest released voice goes first") {
    PianoSynth synth;
    const std::vector<std::uint8_t> sf2 = toonval::test::tiny_sf2();
    REQUIRE(synth.load_memory(sf2.data(), sf2.size(), kRate));
    // The sine uses one voice per note, so 64 held notes fill the pool.
    for (int i = 0; i < PianoSynth::kMaxVoices; ++i) {
        synth.note_on(20 + i, 100);
    }
    render(synth, 0.01);
    REQUIRE(synth.active_voices() == PianoSynth::kMaxVoices);
    synth.note_off(50); // released second-to-last...
    synth.note_off(30); // ...and this one later, but it was pressed earlier
    render(synth, 0.01);

    synth.note_on(100, 100);
    render(synth, 0.01);
    CHECK(synth.active_voices() == PianoSynth::kMaxVoices);
    CHECK(synth.stolen_voices() == 1);
    CHECK(synth.voices_for_key(30) == 0); // the oldest of the released notes went
    CHECK(synth.voices_for_key(50) == 1);
    CHECK(synth.voices_for_key(100) == 1);

    // With nothing released left the oldest held note goes, and the new note always sounds.
    synth.note_on(101, 100);
    synth.note_on(102, 100);
    render(synth, 0.01);
    CHECK(synth.voices_for_key(50) == 0); // released, so it goes before any held note
    CHECK(synth.voices_for_key(20) == 0); // then the oldest held
    CHECK(synth.voices_for_key(21) == 1);
    CHECK(synth.voices_for_key(101) == 1);
    CHECK(synth.voices_for_key(102) == 1);
}

TEST_CASE("a pedal-held voice is stolen before a held key's voice") {
    PianoSynth synth;
    const std::vector<std::uint8_t> sf2 = toonval::test::tiny_sf2();
    REQUIRE(synth.load_memory(sf2.data(), sf2.size(), kRate));
    synth.sustain(true);
    for (int i = 0; i < PianoSynth::kMaxVoices; ++i) {
        synth.note_on(20 + i, 100);
    }
    synth.note_off(70); // key up, pedal holds it
    render(synth, 0.01);
    synth.note_on(110, 100);
    render(synth, 0.01);
    CHECK(synth.voices_for_key(70) == 0);
    CHECK(synth.voices_for_key(20) == 1);
    CHECK(synth.voices_for_key(110) == 1);
}

TEST_CASE("the real piano plays from the cap down: 100 pedalled notes stay within the voice pool") {
    for_each_font([](PianoSynth& synth) {
        synth.sustain(true);
        for (int i = 0; i < 100; ++i) {
            synth.note_on(24 + (i * 7) % 80, 100);
            synth.note_off(24 + (i * 7) % 80);
            render(synth, 0.005);
        }
        const auto out = render(synth, 0.2);
        CHECK(synth.active_voices() <= PianoSynth::kMaxVoices);
        CHECK(all_finite(out));
        CHECK(peak(out) <= PianoSynth::kCeiling);
    });
}

TEST_CASE("master volume scales the output") {
    PianoSynth synth;
    const std::vector<std::uint8_t> sf2 = toonval::test::tiny_sf2();
    REQUIRE(synth.load_memory(sf2.data(), sf2.size(), kRate));
    synth.note_on(60, 64);
    const float full = peak(render(synth, 0.5), 2 * 12000);
    synth.set_volume(0.25F);
    const float quarter = peak(render(synth, 0.5), 2 * 12000);
    CHECK(quarter == doctest::Approx(full * 0.25F).epsilon(0.02));
}

TEST_CASE("without a SoundFont the synth accepts every call and renders silence") {
    PianoSynth synth;
    CHECK_FALSE(synth.loaded());
    synth.note_on(60, 100);
    synth.sustain(true);
    synth.set_volume(0.5F);
    synth.all_notes_off();
    std::vector<float> out(2 * kBlock, 1.0F);
    synth.render(out.data(), kBlock);
    CHECK(peak(out) == 0.0F);
    std::int64_t ns = 0;
    CHECK_FALSE(synth.take_latency(ns));
}

TEST_CASE("data that is not a SoundFont is refused and the synth stays silent") {
    PianoSynth synth;
    const std::vector<std::uint8_t> junk(1000, 0x42);
    CHECK_FALSE(synth.load_memory(junk.data(), junk.size(), kRate));
    CHECK_FALSE(synth.error().empty());
    CHECK_FALSE(synth.load_file("/nonexistent/piano.sf2", kRate));
    CHECK_FALSE(synth.loaded());
}

TEST_CASE("a timed note-on reports its press-to-first-sample latency once") {
    PianoSynth synth;
    const std::vector<std::uint8_t> sf2 = toonval::test::tiny_sf2();
    REQUIRE(synth.load_memory(sf2.data(), sf2.size(), kRate));
    const std::int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count();
    synth.note_on_at(60, 100, now - 2'000'000);
    render(synth, 0.01);
    std::int64_t ns = 0;
    REQUIRE(synth.take_latency(ns));
    CHECK(ns >= 2'000'000);
    CHECK(ns < 1'000'000'000);
    CHECK_FALSE(synth.take_latency(ns));
}

// Meant for ThreadSanitizer too: the main thread's calls and the audio thread's render share only
// the command queue, the latency queue and atomics. The two run in lock-step, about 8 commands per
// block with up to 64 in flight, so they overlap for the whole run at any speed (TSan's included)
// and the 512-slot queue never fills.
TEST_CASE("two threads: commands from one, rendering on the other") {
    PianoSynth synth;
    const std::vector<std::uint8_t> sf2 = toonval::test::tiny_sf2();
    REQUIRE(synth.load_memory(sf2.data(), sf2.size(), kRate));
    std::atomic<bool> done{false};
    std::atomic<int> rendered{0};
    std::atomic<int> produced{0};
    std::thread main_thread([&] {
        for (int i = 0; i < 20000; ++i) {
            while (i > 64 + 8 * rendered.load(std::memory_order_relaxed)) {
                std::this_thread::yield();
            }
            const int note = 36 + (i * 5) % 48;
            switch (i % 9) {
            case 0:
                synth.sustain((i / 9) % 2 == 0);
                break;
            case 1:
                synth.set_volume(0.5F + 0.5F * static_cast<float>(i % 3) / 2.0F);
                break;
            case 2:
                if (i % 900 == 2) {
                    synth.all_notes_off();
                }
                break;
            default:
                if (i % 2 == 0) {
                    synth.note_on(note, 40 + i % 80);
                } else {
                    synth.note_off(note);
                }
            }
            produced.store(i + 1, std::memory_order_relaxed);
            std::int64_t ns = 0;
            while (synth.take_latency(ns)) {
            }
        }
        done.store(true, std::memory_order_release);
    });
    std::vector<float> block(2 * kBlock);
    bool finite = true;
    float max_peak = 0.0F;
    int blocks = 0;
    while (!done.load(std::memory_order_acquire) || blocks < 100) {
        while (!done.load(std::memory_order_relaxed) &&
               8 * blocks > produced.load(std::memory_order_relaxed) + 8) {
            std::this_thread::yield();
        }
        synth.render(block.data(), kBlock);
        finite = finite && all_finite(block);
        max_peak = std::max(max_peak, peak(block));
        ++blocks;
        rendered.store(blocks, std::memory_order_relaxed);
    }
    main_thread.join();
    CHECK(finite);
    CHECK(max_peak <= PianoSynth::kCeiling);
    CHECK(synth.active_voices() <= PianoSynth::kMaxVoices);
    CHECK(synth.dropped_commands() == 0);
    MESSAGE("rendered " << blocks << " blocks, " << synth.dropped_commands()
                        << " commands dropped, " << synth.stolen_voices() << " voices stolen");
}
