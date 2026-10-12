// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace toonval {

struct ChordDisplayConfig {
    // How long the held set must stay unchanged before a new label appears, so rolling a chord
    // does not flash partial names.
    std::int64_t settle_ns = 40'000'000;
    // How long the last label stays after the player lets go or the set stops being a chord.
    std::int64_t hold_ns = 1'500'000'000;
    // The last part of `hold_ns` over which the label fades out. Capped at `hold_ns`.
    std::int64_t fade_ns = 500'000'000;
    // Spell black-key roots as flats ("Bb7") instead of sharps ("A#7").
    bool prefer_flats = false;
};

// What the chord readout shows this frame. `text` is empty when nothing should be drawn, and then
// `opacity` is 0.
struct ChordLabel {
    std::string text;
    double opacity = 0.0; // 0..1
};

// Turns the held notes into a readable chord label. Call update() once per frame. The label only
// follows a held set that has been stable for `settle_ns`, and never names a set that is not a
// chord. After the set stops being a chord, the last label stays for `hold_ns`, fully opaque until
// the last `fade_ns`, then fades to nothing. The hold is timed from the moment the held set
// last changed, not from the frame that noticed it, so it does not depend on the frame rate.
class ChordDisplay {
  public:
    explicit ChordDisplay(ChordDisplayConfig config = {});

    // `held_notes` ascending, as from KeyState::held_notes(). `now_ns` is the caller's clock.
    ChordLabel update(std::span<const std::uint8_t> held_notes, std::int64_t now_ns);

    // Forgets everything, e.g. when the input device is unplugged.
    void reset();

  private:
    ChordDisplayConfig config_;
    std::vector<std::uint8_t> candidate_; // Held set seen on the last update.
    std::int64_t candidate_since_ns_ = 0;
    bool started_ = false;
    bool evaluated_ = false;    // The current candidate has been classified.
    std::string text_;          // Label on screen, empty for none.
    bool live_ = false;         // `text_` names the settled held set.
    std::int64_t ended_ns_ = 0; // When `text_` stopped naming the held set. Valid when !live_.
};

} // namespace toonval
