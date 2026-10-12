// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "toonval/key_state.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace toonval {

// One played note as a block on the rising-notes display. While the key is held `open` is true and
// `end_ns` is meaningless; once released `end_ns` holds the release time.
struct TrailBlock {
    std::uint8_t note = 0;
    std::uint8_t velocity = 0; // Note-on velocity.
    std::int64_t start_ns = 0;
    std::int64_t end_ns = 0;
    bool open = false;
};

// A block placed on the playing field. `bottom` and `top` are fractions 0..1 of the visible span:
// 0 is the top edge of the keyboard, 1 is the top edge of the screen. bottom <= top always.
struct TrailBar {
    std::uint8_t note = 0;
    std::uint8_t velocity = 0;
    bool held = false; // Key still down: the bar is growing and its bottom sits on the keyboard.
    double bottom = 0.0;
    double top = 0.0;
    std::int64_t start_ns = 0; // The block's note-on time, e.g. for a press flash.
};

// The rising notes of free play. A block grows up from the key while it is held, then rises off the
// screen after release. Pure data: every call takes the current time from the caller, in the same
// clock as the event times fed to apply().
class NoteTrail {
  public:
    // Default cap on stored blocks, enough for several minutes of fast playing between prunes.
    static constexpr std::size_t kDefaultMaxBlocks = 4096;
    // Smallest cap accepted: one open block per MIDI note, so an open block is never evicted.
    static constexpr std::size_t kMinMaxBlocks = 128;

    explicit NoteTrail(std::size_t max_blocks = kDefaultMaxBlocks);

    // Applies a KeyState transition that happened at `time_ns`.
    // Pressed opens a block; a Pressed for a note that already has an open block closes the old one
    // at `time_ns` first. Released closes the note's open block (ignored when none is open).
    // AllReleased closes every open block. None and notes outside 0..127 change nothing. A release
    // time before the block's start is raised to the start, so a block never ends before it begins.
    // When the cap is exceeded the closed block with the oldest end is dropped; open blocks are
    // never dropped.
    void apply(const KeyTransition& transition, std::int64_t time_ns);

    // Drops closed blocks whose end is more than `horizon_ns` older than `now_ns`. Open blocks
    // stay. Call it once per frame with the span the renderer shows, so memory stays bounded.
    void prune(std::int64_t now_ns, std::int64_t horizon_ns);

    // The blocks to draw, oldest start first. `span_ns` is how much time fits between the keyboard
    // top and the screen top; a span of 0 or less gives nothing. Fractions are clamped to 0..1, so
    // times in the future of `now_ns` (clock skew) never give negative heights. Left out: blocks
    // already off the top of the screen, and closed blocks with no height after clamping.
    std::vector<TrailBar> visible(std::int64_t now_ns, std::int64_t span_ns) const;
    // Same, written into `out` (cleared first) so a frame loop can reuse one buffer.
    void visible(std::int64_t now_ns, std::int64_t span_ns, std::vector<TrailBar>& out) const;

    const std::vector<TrailBlock>& blocks() const { return blocks_; }
    std::size_t size() const { return blocks_.size(); }
    void clear() { blocks_.clear(); }

  private:
    void close_open(std::uint8_t note, std::int64_t time_ns);
    void enforce_cap();

    std::size_t max_blocks_;
    std::vector<TrailBlock> blocks_; // In press order.
};

} // namespace toonval
