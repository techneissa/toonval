// SPDX-License-Identifier: GPL-3.0-or-later
// CI accuracy gate for the pitch detector on the synthetic piano set: every key A0-C8 at two
// velocities, with room noise and an interfering tone. Targets: under 0.5 % wrong notes, under
// 0.2 % octave errors, decisions within 40 ms from C4 up. Fails if accuracy or that latency drops.
// tools/pitch-eval runs the full set. Under AddressSanitizer every fourth key is used: the run is
// about five times slower there, and the sanitizer is after memory errors, not accuracy (the
// other builds gate every key).
#include "toonval/pitch_eval.hpp"

#include <doctest/doctest.h>

using namespace toonval;

namespace {

#if defined(__SANITIZE_ADDRESS__)
constexpr int kKeyStep = 4;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
constexpr int kKeyStep = 4;
#else
constexpr int kKeyStep = 1;
#endif
#else
constexpr int kKeyStep = 1;
#endif

void check_targets(PitchMethod method) {
    SynthEvalOptions options;
    options.velocities = {0.4f, 0.9f};
    options.variants = 1;
    options.note_step = kKeyStep;
    options.seed = 11;
    options.clip.note_s = 0.2f;
    options.detector.method = method;
    const EvalReport r = evaluate_synthetic(options);
    REQUIRE(r.total.notes == 2 * ((kPianoHighNote - kPianoLowNote) / kKeyStep + 1));
    CHECK(rate(r.total.wrong, r.total.notes) < 0.005f);
    CHECK(rate(r.total.octave, r.total.notes) < 0.002f);
    // Not a detector that stays safe by never answering.
    CHECK(rate(r.total.correct, r.total.notes) > 0.95f);
    float c4_up_max_ms = 0.0f;
    for (int reg = register_of(60); reg < kRegisterCount; ++reg) {
        c4_up_max_ms = std::max(c4_up_max_ms,
                                percentile(r.registers[static_cast<size_t>(reg)].latency_ms, 100));
    }
    CHECK(c4_up_max_ms <= 40.0f);
}

} // namespace

TEST_CASE("pitch accuracy: McLeod meets the accuracy targets on the synthetic piano set") {
    check_targets(PitchMethod::Mpm);
}

TEST_CASE("pitch accuracy: YIN meets the accuracy targets on the synthetic piano set") {
    check_targets(PitchMethod::Yin);
}
