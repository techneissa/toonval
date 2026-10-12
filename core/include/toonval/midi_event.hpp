// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "toonval/spsc_queue.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

namespace toonval {

enum class MidiEventType : std::uint8_t {
    Other, // Anything the trainer does not act on: SysEx, realtime, malformed, other channel
           // messages.
    NoteOn,
    NoteOff,
    ControlChange,
};

// Where an event came from. Midi events arrive from a MIDI port. The other two are the player's own
// presses on the computer keyboard and on the on-screen keys ("virtual" input): they go through the
// same key, trail and chord path as MIDI, and also to the sound output, which a MIDI keyboard's own
// notes never do (the piano already sounds them).
enum class InputSource : std::uint8_t { Midi, ComputerKeyboard, Pointer };

// Controller numbers the trainer reacts to.
inline constexpr std::uint8_t kCcSustain = 64;
inline constexpr std::uint8_t kCcAllSoundOff = 120;
inline constexpr std::uint8_t kCcAllNotesOff = 123;

// One decoded live-MIDI message. `time_ns` is stamped by the caller on the MIDI thread when the
// message arrives, never by the frame loop.
//
// NoteOn / NoteOff: data1 = note number, data2 = velocity. A NoteOff from a note-on with velocity 0
// carries velocity 0; a real note-off keeps its release velocity.
// ControlChange: data1 = controller number, data2 = value.
// Other: channel, data1 and data2 are 0.
struct MidiEvent {
    std::int64_t time_ns = 0;
    MidiEventType type = MidiEventType::Other;
    std::uint8_t channel = 0; // 0..15
    std::uint8_t data1 = 0;
    std::uint8_t data2 = 0;
    // True for the All Notes Off that MidiEventQueue inserts where it dropped events. Not played
    // by anyone, so never count it as the player's action.
    bool synthetic = false;
    InputSource source = InputSource::Midi;

    bool is_virtual() const { return source != InputSource::Midi; }
};

// True for the events the trainer acts on: NoteOn, NoteOff, and the controllers sustain (64),
// All Sound Off (120) and All Notes Off (123). The MIDI thread queues only these, so a controller
// streaming data (mod wheel, expression) leaves the queue to the notes. Add a controller here
// when core starts to use it.
bool is_trainer_event(const MidiEvent& ev);

// Decodes one complete raw message. A note-on with velocity 0 becomes a NoteOff. Empty, truncated,
// over-long or data-byte-first input, SysEx, system messages (clock 0xF8, active sensing 0xFE, ...)
// and channel messages the trainer ignores all decode as Other. Never throws.
MidiEvent decode_midi(const std::uint8_t* bytes, std::size_t size, std::int64_t time_ns);

inline MidiEvent decode_midi(std::span<const std::uint8_t> bytes, std::int64_t time_ns) {
    return decode_midi(bytes.data(), bytes.size(), time_ns);
}

// One channel message (status 0x80..0xEF and its data bytes) cut out of a raw byte chunk.
struct ChannelMessage {
    std::array<std::uint8_t, 3> bytes{};
    std::size_t size = 0; // 2 or 3
};

// Cuts a port's raw byte stream into channel messages, with running status expanded to a full
// message. The stream arrives in chunks as the driver delivers it (CoreMIDI packet, ALSA event,
// raw ALSA read); running status and a partial message carry over from one chunk to the next, so
// a message split across reads is reassembled, and the result never depends on where the chunks
// were cut. One splitter per port, used only on that port's MIDI thread.
//
// Skipped: SysEx and its continuation bytes, system common messages, data bytes with no running
// status (SysEx and system common clear it, as the MIDI spec says), and a message cut off by a new
// status byte. A real-time byte (0xF8..0xFF) inside a message is skipped and does not interrupt it
// or clear running status. Never allocates or throws.
class MidiStreamSplitter {
  public:
    MidiStreamSplitter() = default;
    // Starts with one chunk already fed.
    MidiStreamSplitter(const std::uint8_t* bytes, std::size_t size) { feed(bytes, size); }
    explicit MidiStreamSplitter(std::span<const std::uint8_t> bytes) { feed(bytes); }

    // Sets the next chunk to read. Call only after next() returned false for the previous one.
    // The bytes must stay valid until then.
    void feed(const std::uint8_t* bytes, std::size_t size) {
        bytes_ = bytes;
        size_ = bytes == nullptr ? 0 : size;
        pos_ = 0;
    }
    void feed(std::span<const std::uint8_t> bytes) { feed(bytes.data(), bytes.size()); }

    // Fills `out` with the next complete channel message. Returns false when the chunk is used up;
    // a message still incomplete then waits for the next chunk.
    bool next(ChannelMessage& out);

    // Forgets running status and any partial message, e.g. when the port is reopened.
    void reset() {
        running_status_ = 0;
        got_ = 0;
    }

  private:
    const std::uint8_t* bytes_ = nullptr;
    std::size_t size_ = 0;
    std::size_t pos_ = 0;
    std::uint8_t running_status_ = 0;    // 0 = none
    std::array<std::uint8_t, 2> data_{}; // data bytes of the message being collected
    std::size_t got_ = 0;
};

// A port's event queue from its MIDI thread (one producer) to the frame loop (one consumer), on
// SpscQueue. When it overflows it drops events, and then inserts one synthetic All Notes Off
// (CC123, `synthetic` set, stamped with the first dropped event's time) at the point of the gap,
// so the frame loop's KeyState releases every key there. A dropped note-off can then never leave a
// key lit. Keys the player still holds look released until pressed again; that is the price of a
// gap. The last slot is kept free for that resync event, so it always fits.
//
// Memory ordering: the producer knows how full the queue is from its own push count and the
// consumer's pop count, which the consumer publishes with a release store after each pop (after
// SpscQueue's own release of its head). The producer's acquire load of it therefore never sees
// more room than SpscQueue has, so the resync push cannot fail.
template <std::size_t Capacity> class MidiEventQueue {
  public:
    static constexpr std::size_t capacity() { return Capacity; }

    // Producer thread only. Never blocks, locks, or allocates.
    void push(const MidiEvent& ev) noexcept {
        const std::size_t used = pushed_ - popped_.load(std::memory_order_acquire);
        if (used + 1 < Capacity) {
            queue_.push(ev);
            ++pushed_;
            in_gap_ = false;
            return;
        }
        dropped_.fetch_add(1, std::memory_order_relaxed);
        if (!in_gap_ && used < Capacity) {
            MidiEvent resync;
            resync.time_ns = ev.time_ns;
            resync.type = MidiEventType::ControlChange;
            resync.data1 = kCcAllNotesOff;
            resync.synthetic = true;
            queue_.push(resync);
            ++pushed_;
            in_gap_ = true;
            resyncs_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Consumer thread only. Returns false when the queue is empty.
    bool pop(MidiEvent& out) noexcept {
        if (!queue_.pop(out)) {
            return false;
        }
        ++popped_local_;
        popped_.store(popped_local_, std::memory_order_release);
        return true;
    }

    // Events dropped because the queue was full. Any thread; may lag slightly.
    std::uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }
    // Resync events inserted, one per run of drops. Any thread; may lag slightly.
    std::uint64_t resyncs() const noexcept { return resyncs_.load(std::memory_order_relaxed); }

  private:
    SpscQueue<MidiEvent, Capacity> queue_;
    // Producer only.
    alignas(64) std::size_t pushed_ = 0;
    bool in_gap_ = false;
    // Consumer only, and its published copy.
    alignas(64) std::size_t popped_local_ = 0;
    std::atomic<std::size_t> popped_{0};
    alignas(64) std::atomic<std::uint64_t> dropped_{0};
    std::atomic<std::uint64_t> resyncs_{0};
};

// Pops events from up to N sources, each already in time order, and calls `fn(const MidiEvent&)`
// for each in time order across all of them (a k-way merge on time_ns). Equal times keep source
// order, lower index first, and each source keeps its own order. `pop(i, ev)` fills `ev` with
// source i's next event and returns false when it has none. At most `max_per_source` events are
// taken from one source per call, so a source that keeps filling cannot hold the caller.
template <std::size_t N, typename Pop, typename Fn>
void merge_by_time(Pop&& pop, Fn&& fn, std::size_t max_per_source) {
    std::array<MidiEvent, N> head{};
    std::array<bool, N> has{};
    std::array<std::size_t, N> taken{};
    for (std::size_t i = 0; i < N; ++i) {
        if (max_per_source > 0 && pop(i, head[i])) {
            has[i] = true;
            taken[i] = 1;
        }
    }
    for (;;) {
        std::size_t best = N;
        for (std::size_t i = 0; i < N; ++i) {
            if (has[i] && (best == N || head[i].time_ns < head[best].time_ns)) {
                best = i;
            }
        }
        if (best == N) {
            return;
        }
        fn(static_cast<const MidiEvent&>(head[best]));
        has[best] = taken[best] < max_per_source && pop(best, head[best]);
        taken[best] += has[best] ? 1 : 0;
    }
}

} // namespace toonval
