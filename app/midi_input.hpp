// SPDX-License-Identifier: GPL-3.0-or-later
// Live MIDI input from every connected keyboard, with hot-plug.
#pragma once

#include "toonval/keyboard.hpp"
#include "toonval/keyboard_identity.hpp"
#include "toonval/midi_event.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace libremidi {
class midi_in;
class observer;
} // namespace libremidi

namespace toonval::app {

// The clock every MidiEvent::time_ns is on: std::chrono::steady_clock in nanoseconds. libremidi
// stamps with it on the MIDI thread in timestamp_mode::SystemMonotonic, and the frame loop reads it
// here to compare against those stamps.
std::int64_t midi_clock_ns();

// What changed during one update().
struct PortChanges {
    int opened = 0;
    int closed = 0; // inputs that disappeared; the caller clears held keys
};

// Opens every MIDI input port except the app's own and software loopbacks, and reopens them as
// devices come and go.
//
// Threads: libremidi calls each port's message callback on its own MIDI thread. That callback
// splits and decodes the bytes (the port's splitter keeps running status and a partial message
// between callbacks), keeps only the events the trainer uses, and pushes them into the port's own
// single-producer queue, so several keyboards never share a producer. Each open gets a new splitter
// and queue, so a reused slot never inherits state. Port changes are noticed two ways, both of
// which only bump an atomic counter: libremidi's observer callbacks, and a watcher thread that
// lists the ports every 500 ms and bumps the counter when the list differs (CoreMIDI on macOS 27
// delivered no observer notifications in testing). The list includes each port's USB instance
// where the platform gives one (Linux), so a quick unplug and replug is seen as a change too.
// update() on the frame thread sees the counter move and rescans; it also rescans when a port that
// failed to open is due for a retry. Every other member, including opening and closing ports, is
// used from the frame loop's thread only.
class MidiInput {
  public:
    static constexpr std::size_t kMaxPorts = 8;
    // Per port. Sized so a backlog never builds up in practice: a MIDI 1.0 cable carries at most
    // 31250 / 10 / 2 = 1562 messages a second (two bytes each with running status), which fills
    // 4096 slots in 2.6 s. Dense playing is far below that: ten-note chords sixteen times a
    // second with a continuous pedal is 10 * 2 * 16 + about 100 pedal CCs = 420 events a second,
    // or 7 per 60 FPS frame, so the queue holds about 10 s of it, or a 600-frame stall. A full
    // queue still never leaves a key lit: see MidiEventQueue. 4096 * 16 bytes = 64 KiB per port.
    static constexpr std::size_t kQueueCapacity = 4096;

    MidiInput();
    ~MidiInput();
    MidiInput(const MidiInput&) = delete;
    MidiInput& operator=(const MidiInput&) = delete;

    // Opens new inputs and closes vanished ones when a port change was reported since the last
    // call. The first call always scans. A closed input's queued events are discarded, so nothing
    // from an unplugged keyboard arrives after the caller clears its keys.
    PortChanges update();

    // Runs the main thread's run loop once without waiting. On macOS CoreMIDI updates the port list
    // through it, so a loop without SDL's event pump (the headless --midi-log) calls this each
    // turn. Does nothing elsewhere.
    static void pump_system_events();

    // Pops every queued event from every open input and calls `fn(const MidiEvent&)` for each, in
    // time order across inputs (equal times in slot order). Closed slots are skipped. When an
    // input's queue overflowed, its events include a synthetic All Notes Off at the gap.
    template <typename Fn> void drain(Fn&& fn) {
        merge_by_time<kMaxPorts>(
            [this](std::size_t i, MidiEvent& ev) {
                Slot& slot = slots_[i];
                return slot.in != nullptr && slot.stream != nullptr && slot.stream->queue.pop(ev);
            },
            fn, kQueueCapacity);
    }

    // Events dropped because a queue was full, over all ports since start.
    std::uint64_t dropped() const;
    // Synthetic All Notes Off events inserted after drops, over all ports since start.
    std::uint64_t resyncs() const;

    // The first open input that matches a keyboard profile, or nullptr when none does.
    const KeyboardProfile* profile() const;
    int open_count() const;
    // Names of the open inputs, in slot order.
    std::vector<std::string> open_names() const;

  private:
    // State the MIDI thread owns while the port is open; shared with its callback.
    struct PortStream {
        MidiStreamSplitter splitter;          // MIDI thread only
        MidiEventQueue<kQueueCapacity> queue; // MIDI thread -> frame thread
    };

    struct Slot {
        std::unique_ptr<libremidi::midi_in> in; // null when the slot is free
        std::shared_ptr<PortStream> stream;     // new for every open
        std::uint64_t port = 0;                 // libremidi port handle
        std::string name;
        std::string instance; // USB instance when known; a change means a replug
        KeyboardMatch match;
    };

    // A listed port that failed to open, retried with backoff.
    struct FailedOpen {
        std::uint64_t port = 0;
        std::string name;
        int attempts = 0;
        std::int64_t retry_at_ns = 0;
    };

    void rescan(PortChanges& changes);
    void close_slot(Slot& slot);
    void watch_ports();
    bool retry_due(std::int64_t now_ns) const;

    std::atomic<std::uint32_t> port_events_{0}; // bumped on any port change
    std::uint32_t seen_port_events_ = 0;
    bool scanned_ = false;
    std::size_t next_slot_ = 0;                       // free slots are reused round-robin
    std::unique_ptr<libremidi::observer> watcher_;    // callbacks only
    std::unique_ptr<libremidi::observer> enumerator_; // port lists, frame thread only
    std::array<Slot, kMaxPorts> slots_;
    std::vector<FailedOpen> failed_;
    std::uint64_t closed_dropped_ = 0; // from ports already closed
    std::uint64_t closed_resyncs_ = 0;

    std::mutex watch_mutex_; // guards stop_watching_ only
    std::condition_variable watch_wake_;
    bool stop_watching_ = false;
    std::thread watch_thread_;
};

} // namespace toonval::app
