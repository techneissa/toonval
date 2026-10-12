// SPDX-License-Identifier: GPL-3.0-or-later
// The pitch detector runs on the real-time analysis thread, so process() must never allocate.
// This file replaces the global operator new to count allocations made while a flag is set.
#include "toonval/pitch_detect.hpp"

#include <cmath>
#include <cstdlib>
#include <new>
#include <vector>

#include <doctest/doctest.h>

namespace {
thread_local bool g_counting = false;
thread_local int g_allocations = 0;
} // namespace

void* operator new(std::size_t size) {
    if (g_counting) {
        ++g_allocations;
    }
    if (void* p = std::malloc(size == 0 ? 1 : size)) {
        return p;
    }
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

using namespace toonval;

namespace {

constexpr int kRate = 48000;

// Harmonic note: eight partials of amplitude 0.3/n, `cents` from the equal-tempered `note`, from
// `start` to `end`.
void add_note(std::vector<float>& audio, int note, double cents, int start, int end) {
    const double f = 440.0 * std::exp2((note - 69) / 12.0 + cents / 1200.0);
    for (int i = start; i < end; ++i) {
        const double t = static_cast<double>(i - start) / kRate;
        double v = 0.0;
        for (int n = 1; n <= 8; ++n) {
            v += 0.3 / n * std::sin(6.283185307 * f * n * t + 0.3 * (n - 1));
        }
        audio[static_cast<size_t>(i)] = static_cast<float>(v);
    }
}

// Runs `audio` through the detector in 256-sample blocks with the counter on, and returns the
// number of events; `allocations` gets the allocations made inside process().
int run_counting(PitchDetector& d, const std::vector<float>& audio, int& allocations) {
    PitchEvent events[8];
    int got = 0;
    g_allocations = 0;
    g_counting = true;
    for (size_t i = 0; i + 256 <= audio.size(); i += 256) {
        got += d.process(audio.data() + i, 256, events, 8);
    }
    g_counting = false;
    allocations = g_allocations;
    return got;
}

} // namespace

TEST_CASE("pitch detector: process() does not allocate, in open and expected-note mode") {
    // Two notes and a gap, so every path runs: gate, onset, acquisition, tracking, note-off.
    std::vector<float> audio(static_cast<size_t>(kRate), 0.0f);
    add_note(audio, 57, 0.0, 2400, kRate / 2);
    add_note(audio, 64, 0.0, kRate / 2, kRate * 3 / 4);
    // The counter itself works.
    g_allocations = 0;
    g_counting = true;
    // Direct calls: an optimiser may drop a new-expression whose pointer is never used.
    void* probe = ::operator new(sizeof(int));
    g_counting = false;
    ::operator delete(probe);
    REQUIRE(g_allocations == 1);

    for (int pass = 0; pass < 4; ++pass) {
        CAPTURE(pass);
        PitchDetectorConfig cfg;
        cfg.method = pass == 1 ? PitchMethod::Yin : PitchMethod::Mpm;
        PitchDetector d(cfg);
        if (pass == 2) {
            const int chord[] = {57, 64};
            d.set_expected_notes(chord, 2);
        } else if (pass == 3) {
            const int single[] = {57};
            d.set_expected_notes(single, 1);
        }
        int allocations = -1;
        const int got = run_counting(d, audio, allocations);
        CHECK(allocations == 0);
        CHECK(got > 0);
    }
}

TEST_CASE("pitch detector: process() does not allocate while it learns the tuning") {
    // Six notes, the whole instrument 20 cents sharp, so the learned A4 moves.
    std::vector<float> audio(static_cast<size_t>(kRate * 2 + 4800), 0.0f);
    int start = 2400;
    for (int note : {57, 60, 64, 67, 62, 65}) {
        add_note(audio, note, 20.0, start, start + kRate / 3);
        start += kRate / 3;
    }
    PitchDetectorConfig cfg;
    cfg.learn_tuning = true;
    PitchDetector d(cfg);
    int allocations = -1;
    const int got = run_counting(d, audio, allocations);
    CHECK(allocations == 0);
    CHECK(got > 0);
    // The learning path ran: the tuning left 440 Hz for about 20 cents above it.
    CHECK(d.a4_hz() > 443.0f);
}
