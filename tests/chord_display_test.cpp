// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/chord_display.hpp"

#include <doctest/doctest.h>

#include <vector>

using namespace toonval;

namespace {
constexpr std::int64_t kMs = 1'000'000;

using Notes = std::vector<std::uint8_t>;
const Notes kCMajor = {60, 64, 67};
const Notes kFMajor = {65, 69, 72};
const Notes kBbMajor = {58, 62, 65};
const Notes kNone = {};
} // namespace

TEST_CASE("nothing is shown before any note is held") {
    ChordDisplay display;
    ChordLabel label = display.update(kNone, 0);
    CHECK(label.text.empty());
    CHECK(label.opacity == 0.0);
    label = display.update(kNone, 10'000 * kMs);
    CHECK(label.text.empty());
    CHECK(label.opacity == 0.0);
}

TEST_CASE("a chord label appears once the held set has been stable for the settle time") {
    ChordDisplay display;
    CHECK(display.update(kCMajor, 1000 * kMs).text.empty());
    CHECK(display.update(kCMajor, 1039 * kMs + 999'999).text.empty());
    ChordLabel label = display.update(kCMajor, 1040 * kMs);
    CHECK(label.text == "C");
    CHECK(label.opacity == 1.0);
}

TEST_CASE("the settle time is configurable") {
    ChordDisplayConfig config;
    config.settle_ns = 100 * kMs;
    ChordDisplay display(config);
    display.update(kCMajor, 0);
    CHECK(display.update(kCMajor, 99 * kMs).text.empty());
    CHECK(display.update(kCMajor, 100 * kMs).text == "C");
}

TEST_CASE("a zero settle time shows the label on the first frame") {
    ChordDisplayConfig config;
    config.settle_ns = 0;
    ChordDisplay display(config);
    CHECK(display.update(kCMajor, 0).text == "C");
}

TEST_CASE("rolling a chord never flashes a partial name") {
    ChordDisplay display;
    // C, then E 10 ms later, then G 20 ms later: C-E-G is stable from 20 ms.
    CHECK(display.update(Notes{60}, 0).text.empty());
    CHECK(display.update(Notes{60, 64}, 10 * kMs).text.empty());
    CHECK(display.update(kCMajor, 20 * kMs).text.empty());
    CHECK(display.update(kCMajor, 59 * kMs).text.empty());
    CHECK(display.update(kCMajor, 60 * kMs).text == "C");
}

TEST_CASE("a change to the held set restarts the settle time") {
    ChordDisplay display;
    display.update(kCMajor, 0);
    display.update(Notes{60, 64, 67, 71}, 30 * kMs); // Cmaj7, 30 ms in.
    CHECK(display.update(Notes{60, 64, 67, 71}, 69 * kMs).text.empty());
    CHECK(display.update(Notes{60, 64, 67, 71}, 70 * kMs).text == "Cmaj7");
}

TEST_CASE("a new chord replaces the old label after the settle time, at full opacity") {
    ChordDisplay display;
    display.update(kCMajor, 0);
    display.update(kCMajor, 40 * kMs);
    // Play F major: the C label stays until F has settled.
    ChordLabel label = display.update(kFMajor, 500 * kMs);
    CHECK(label.text == "C");
    CHECK(label.opacity == 1.0);
    label = display.update(kFMajor, 540 * kMs);
    CHECK(label.text == "F");
    CHECK(label.opacity == 1.0);
}

TEST_CASE("the label stays at full opacity while the held set is unsettled") {
    ChordDisplay display;
    display.update(kCMajor, 0);
    display.update(kCMajor, 40 * kMs);
    // Release one note and look every few ms: still the chord, still opaque.
    for (std::int64_t t = 1000; t < 1040; t += 5) {
        const ChordLabel label = display.update(Notes{60, 64}, t * kMs);
        CHECK(label.text == "C");
        CHECK(label.opacity == 1.0);
    }
}

TEST_CASE("the label holds after release, then fades, then disappears at the exact boundaries") {
    ChordDisplay display;
    display.update(kCMajor, 0);
    display.update(kCMajor, 40 * kMs);
    // Everything released at t = 1000 ms; the hold runs from there.
    display.update(kNone, 1000 * kMs);
    ChordLabel label = display.update(kNone, 1000 * kMs + 40 * kMs);
    CHECK(label.text == "C");
    CHECK(label.opacity == 1.0);
    // Full opacity until hold - fade = 1.0 s after release.
    label = display.update(kNone, 2000 * kMs);
    CHECK(label.text == "C");
    CHECK(label.opacity == 1.0);
    // Halfway through the fade.
    label = display.update(kNone, 2250 * kMs);
    CHECK(label.text == "C");
    CHECK(label.opacity == doctest::Approx(0.5));
    // Just before the end.
    label = display.update(kNone, 2500 * kMs - 1);
    CHECK(label.text == "C");
    CHECK(label.opacity > 0.0);
    CHECK(label.opacity < 0.001);
    // Gone at hold = 1.5 s after release.
    label = display.update(kNone, 2500 * kMs);
    CHECK(label.text.empty());
    CHECK(label.opacity == 0.0);
}

TEST_CASE("the hold is timed from the change, not from the frame that noticed it") {
    ChordDisplay display;
    display.update(kCMajor, 0);
    display.update(kCMajor, 40 * kMs);
    display.update(kNone, 1000 * kMs);
    // No frame between the release and a late look: same as if it had been drawn all along.
    ChordLabel label = display.update(kNone, 2250 * kMs);
    CHECK(label.text == "C");
    CHECK(label.opacity == doctest::Approx(0.5));
}

TEST_CASE("hold and fade times are configurable") {
    ChordDisplayConfig config;
    config.hold_ns = 400 * kMs;
    config.fade_ns = 100 * kMs;
    ChordDisplay display(config);
    display.update(kCMajor, 0);
    display.update(kCMajor, 40 * kMs);
    display.update(kNone, 100 * kMs);
    CHECK(display.update(kNone, 400 * kMs).opacity == 1.0);
    CHECK(display.update(kNone, 450 * kMs).opacity == doctest::Approx(0.5));
    CHECK(display.update(kNone, 500 * kMs).text.empty());
}

TEST_CASE("a fade longer than the hold fades over the whole hold") {
    ChordDisplayConfig config;
    config.hold_ns = 400 * kMs;
    config.fade_ns = 900 * kMs;
    ChordDisplay display(config);
    display.update(kCMajor, 0);
    display.update(kCMajor, 40 * kMs);
    display.update(kNone, 100 * kMs);
    CHECK(display.update(kNone, 100 * kMs).opacity == 1.0);
    CHECK(display.update(kNone, 300 * kMs).opacity == doctest::Approx(0.5));
    CHECK(display.update(kNone, 500 * kMs).text.empty());
}

TEST_CASE("a set that stops being a chord starts the hold") {
    ChordDisplay display;
    display.update(kCMajor, 0);
    display.update(kCMajor, 40 * kMs);
    // Left with a lone note, which is not a chord.
    display.update(Notes{60}, 1000 * kMs);
    ChordLabel label = display.update(Notes{60}, 1040 * kMs);
    CHECK(label.text == "C");
    CHECK(label.opacity == 1.0);
    label = display.update(Notes{60}, 2250 * kMs);
    CHECK(label.text == "C");
    CHECK(label.opacity == doctest::Approx(0.5));
    CHECK(display.update(Notes{60}, 2500 * kMs).text.empty());
}

TEST_CASE("a set that is not a chord never gets a label") {
    ChordDisplay display;
    display.update(Notes{60}, 0);
    CHECK(display.update(Notes{60}, 1000 * kMs).text.empty());
    CHECK(display.update(Notes{60, 61, 62, 63}, 2000 * kMs).text.empty());
    CHECK(display.update(Notes{60, 61, 62, 63}, 3000 * kMs).text.empty());
}

TEST_CASE("a new chord during the fade shows at full opacity") {
    ChordDisplay display;
    display.update(kCMajor, 0);
    display.update(kCMajor, 40 * kMs);
    display.update(kNone, 1000 * kMs);
    display.update(kFMajor, 2250 * kMs); // C is half faded.
    ChordLabel label = display.update(kFMajor, 2290 * kMs);
    CHECK(label.text == "F");
    CHECK(label.opacity == 1.0);
}

TEST_CASE("the old label keeps fading while a non-chord set is still unsettled") {
    ChordDisplay display;
    display.update(kCMajor, 0);
    display.update(kCMajor, 40 * kMs);
    display.update(kNone, 1000 * kMs);
    display.update(kNone, 1040 * kMs);
    // A new note arrives mid-fade; it is not settled, so the fade carries on.
    ChordLabel label = display.update(Notes{72}, 2250 * kMs);
    CHECK(label.text == "C");
    CHECK(label.opacity == doctest::Approx(0.5));
}

TEST_CASE("a flicker back to the same chord inside the settle time keeps the label live") {
    ChordDisplay display;
    display.update(kCMajor, 0);
    display.update(kCMajor, 40 * kMs);
    display.update(Notes{60, 64}, 1000 * kMs);
    display.update(kCMajor, 1010 * kMs);
    ChordLabel label = display.update(kCMajor, 1050 * kMs);
    CHECK(label.text == "C");
    CHECK(label.opacity == 1.0);
    // Still opaque long after the old hold would have ended.
    label = display.update(kCMajor, 9000 * kMs);
    CHECK(label.text == "C");
    CHECK(label.opacity == 1.0);
}

TEST_CASE("prefer_flats passes through to the chord name") {
    ChordDisplayConfig config;
    config.prefer_flats = true;
    ChordDisplay flats(config);
    flats.update(kBbMajor, 0);
    CHECK(flats.update(kBbMajor, 40 * kMs).text == "Bb");

    ChordDisplay sharps;
    sharps.update(kBbMajor, 0);
    CHECK(sharps.update(kBbMajor, 40 * kMs).text == "A#");
}

TEST_CASE("inversions show as slash chords") {
    ChordDisplay display;
    const Notes c_over_e = {64, 67, 72};
    display.update(c_over_e, 0);
    CHECK(display.update(c_over_e, 40 * kMs).text == "C/E");
}

TEST_CASE("reset forgets the label") {
    ChordDisplay display;
    display.update(kCMajor, 0);
    display.update(kCMajor, 40 * kMs);
    display.reset();
    const ChordLabel label = display.update(kNone, 50 * kMs);
    CHECK(label.text.empty());
    CHECK(label.opacity == 0.0);
}

TEST_CASE("a clock that steps backwards never produces an opacity outside 0..1") {
    ChordDisplay display;
    display.update(kCMajor, 1000 * kMs);
    display.update(kCMajor, 1040 * kMs);
    display.update(kNone, 2000 * kMs);
    for (std::int64_t t : {1500, 1900, 2000, 2100, 2400, 3500, 3600, 5000}) {
        const ChordLabel label = display.update(kNone, t * kMs);
        CHECK(label.opacity >= 0.0);
        CHECK(label.opacity <= 1.0);
    }
}
