// SPDX-License-Identifier: GPL-3.0-or-later
#include "midi_input.hpp"

#include "midi_port_identity.hpp"

#include <libremidi/libremidi.hpp>

#include <SDL3/SDL_log.h>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#endif

#include <algorithm>
#include <chrono>
#include <iterator>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>

namespace toonval::app {

namespace {

// Ports the app never opens: its own, loopbacks that are not a keyboard, and nameless ones
// (CoreMIDI briefly lists a source that is going away with an empty name).
bool skip_port(const libremidi::input_port& port) {
    const std::string_view name = port.port_name;
    const std::string_view device = port.device_name;
    return name.empty() || name.starts_with("toonval") || device.starts_with("toonval") ||
           name.find("Midi Through") != std::string_view::npos ||
           device.find("Midi Through") != std::string_view::npos;
}

std::string label(const libremidi::input_port& port) {
    return port.display_name.empty() ? port.port_name : port.display_name;
}

// How often the watcher thread lists ports to catch changes the observer did not report.
constexpr auto kWatchInterval = std::chrono::milliseconds(500);

// A port that failed to open is retried after 0.5 s, then 1, 2, 4 and every 8 s, for as long as it
// stays listed. Opening can fail while a device is still registering (an ALSA connect, for one).
constexpr std::int64_t kRetryFirstNs = 500'000'000;
constexpr std::int64_t kRetryMaxNs = 8'000'000'000;

std::int64_t retry_delay_ns(int attempts) {
    std::int64_t delay = kRetryFirstNs;
    for (int i = 1; i < attempts && delay < kRetryMaxNs; ++i) {
        delay *= 2;
    }
    return std::min(delay, kRetryMaxNs);
}

libremidi::observer_configuration port_list_config() {
    libremidi::observer_configuration conf;
    conf.track_hardware = true;
    // Virtual sources too, so `midi-demo` and software keyboards work; skip_port() drops our own.
    conf.track_virtual = true;
    conf.notify_in_constructor = false;
    return conf;
}

} // namespace

std::int64_t midi_clock_ns() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

MidiInput::MidiInput() {
    // update() does the first scan itself, on the frame thread.
    auto conf = port_list_config();
    const auto bump = [this](const libremidi::input_port&) {
        port_events_.fetch_add(1, std::memory_order_release);
    };
    conf.input_added = bump;
    conf.input_removed = bump;
    watcher_ = std::make_unique<libremidi::observer>(conf);
    enumerator_ = std::make_unique<libremidi::observer>(port_list_config());
    watch_thread_ = std::thread([this] { watch_ports(); });
}

MidiInput::~MidiInput() {
    // Stop port notifications before closing ports, then close every port so no MIDI thread
    // touches a queue after this object is gone.
    {
        const std::lock_guard lock(watch_mutex_);
        stop_watching_ = true;
    }
    watch_wake_.notify_one();
    watch_thread_.join();
    watcher_.reset();
    for (auto& slot : slots_) {
        slot.in.reset();
    }
}

// Watcher thread. Has its own observer, so the frame thread's enumerator_ is never shared.
void MidiInput::watch_ports() {
    libremidi::observer observer(port_list_config());
    using Entry = std::tuple<std::uint64_t, std::string, std::string>;
    std::vector<Entry> last;
    std::vector<Entry> now;
    std::unique_lock lock(watch_mutex_);
    while (!stop_watching_) {
        lock.unlock();
        now.clear();
        for (const auto& port : observer.get_input_ports()) {
            // The instance catches an unplug and replug between two listings, which leaves the
            // handle and name unchanged but the old connection dead.
            now.emplace_back(port.port, label(port), identify_port(port).instance);
        }
        std::sort(now.begin(), now.end());
        if (now != last) {
            last.swap(now);
            port_events_.fetch_add(1, std::memory_order_release);
        }
        lock.lock();
        watch_wake_.wait_for(lock, kWatchInterval, [this] { return stop_watching_; });
    }
}

void MidiInput::pump_system_events() {
#if defined(__APPLE__)
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, true);
#endif
}

bool MidiInput::retry_due(std::int64_t now_ns) const {
    for (const auto& failed : failed_) {
        if (now_ns >= failed.retry_at_ns) {
            return true;
        }
    }
    return false;
}

PortChanges MidiInput::update() {
    PortChanges changes;
    const std::uint32_t events = port_events_.load(std::memory_order_acquire);
    if (!scanned_ || events != seen_port_events_ || retry_due(midi_clock_ns())) {
        seen_port_events_ = events;
        scanned_ = true;
        rescan(changes);
    }
    return changes;
}

void MidiInput::rescan(PortChanges& changes) {
    const auto ports = enumerator_->get_input_ports();
    std::vector<PortIdentity> identities;
    identities.reserve(ports.size());
    for (const auto& port : ports) {
        identities.push_back(identify_port(port));
    }

    // Close inputs whose port is gone. Ports are matched by handle, name and USB instance: the
    // handle alone can be reused by a different device on some backends, and a replug between two
    // listings keeps handle and name but gets a new instance where the platform reports one.
    for (auto& slot : slots_) {
        if (!slot.in) {
            continue;
        }
        bool present = false;
        bool replugged = false;
        for (std::size_t i = 0; i < ports.size(); ++i) {
            if (ports[i].port == slot.port && label(ports[i]) == slot.name) {
                present = identities[i].instance == slot.instance;
                replugged = !present;
                break;
            }
        }
        if (!present) {
            SDL_Log("MIDI input %s: %s", replugged ? "replugged, reopening" : "removed",
                    slot.name.c_str());
            close_slot(slot);
            ++changes.closed;
        }
    }

    // Forget failed opens whose port is no longer listed.
    std::erase_if(failed_, [&ports](const FailedOpen& failed) {
        return std::none_of(ports.begin(), ports.end(), [&failed](const auto& port) {
            return port.port == failed.port && label(port) == failed.name;
        });
    });

    // Open ports that are new, and failed ones that are due for a retry.
    const std::int64_t now = midi_clock_ns();
    for (std::size_t n = 0; n < ports.size(); ++n) {
        const auto& port = ports[n];
        if (skip_port(port)) {
            continue;
        }
        const std::string name = label(port);
        bool open = false;
        for (const auto& slot : slots_) {
            if (slot.in && slot.port == port.port && slot.name == name) {
                open = true;
                break;
            }
        }
        if (open) {
            continue;
        }
        auto failed = std::find_if(failed_.begin(), failed_.end(), [&](const FailedOpen& f) {
            return f.port == port.port && f.name == name;
        });
        if (failed != failed_.end() && now < failed->retry_at_ns) {
            continue;
        }

        Slot* free = nullptr;
        for (std::size_t i = 0; i < kMaxPorts; ++i) {
            Slot& candidate = slots_[(next_slot_ + i) % kMaxPorts];
            if (!candidate.in) {
                free = &candidate;
                next_slot_ = (next_slot_ + i + 1) % kMaxPorts;
                break;
            }
        }
        if (free == nullptr) {
            SDL_Log("MIDI input ignored, %zu already open: %s", kMaxPorts, name.c_str());
            continue;
        }

        auto stream = std::make_shared<PortStream>();
        libremidi::input_configuration conf;
        // MIDI thread. Splits the bytes (state carries over from the last callback), decodes,
        // keeps only what the trainer uses, and pushes; no allocation, lock, or logging.
        conf.on_raw_data = [stream](std::span<const std::uint8_t> bytes,
                                    libremidi::timestamp time_ns) {
            stream->splitter.feed(bytes);
            ChannelMessage msg;
            while (stream->splitter.next(msg)) {
                const MidiEvent ev = decode_midi(msg.bytes.data(), msg.size, time_ns);
                if (is_trainer_event(ev)) {
                    stream->queue.push(ev);
                }
            }
        };
        conf.ignore_sysex = true;
        conf.ignore_timing = true;
        conf.ignore_sensing = true;
        // steady_clock, read by libremidi on the MIDI thread when it processes the message; see
        // midi_clock_ns(). "Absolute" is not one clock across ports: on ALSA its epoch is each
        // port's queue creation. On CoreMIDI it is host time, which agreed with steady_clock
        // within 0.5 us when measured on macOS, but that is not guaranteed on every platform.
        // SystemMonotonic is one clock everywhere.
        conf.timestamps = libremidi::timestamp_mode::SystemMonotonic;

        auto in = std::make_unique<libremidi::midi_in>(
            conf, libremidi::midi_in_configuration_for(*enumerator_));
        if (const auto err = in->open_port(port, "toonval in"); err != stdx::error{}) {
            if (failed == failed_.end()) {
                failed_.push_back({port.port, name, 0, 0});
                failed = std::prev(failed_.end());
                SDL_Log("MIDI input failed to open, will retry: %s", name.c_str());
            }
            ++failed->attempts;
            failed->retry_at_ns = now + retry_delay_ns(failed->attempts);
            continue;
        }
        if (failed != failed_.end()) {
            SDL_Log("MIDI input opened after %d failed attempts: %s", failed->attempts,
                    name.c_str());
            failed_.erase(failed);
        }
        free->in = std::move(in);
        free->stream = std::move(stream);
        free->port = port.port;
        free->name = name;
        free->instance = identities[n].instance;
        free->match = match_keyboard(identities[n].identity);
        ++changes.opened;
        const auto reason = describe(free->match.reason);
        if (free->match.profile != nullptr) {
            SDL_Log("MIDI input opened: %s (%.*s, %.*s)", name.c_str(),
                    static_cast<int>(free->match.profile->name.size()),
                    free->match.profile->name.data(), static_cast<int>(reason.size()),
                    reason.data());
        } else {
            SDL_Log("MIDI input opened: %s (%.*s)", name.c_str(), static_cast<int>(reason.size()),
                    reason.data());
        }
    }
}

void MidiInput::close_slot(Slot& slot) {
    // Closing the port stops its MIDI thread's callbacks. The queued events are discarded with
    // the stream, so nothing from the closed port reaches the caller after it clears its keys.
    slot.in.reset();
    if (slot.stream) {
        closed_dropped_ += slot.stream->queue.dropped();
        closed_resyncs_ += slot.stream->queue.resyncs();
        slot.stream.reset();
    }
    slot.name.clear();
    slot.instance.clear();
    slot.port = 0;
    slot.match = {};
}

std::uint64_t MidiInput::dropped() const {
    std::uint64_t total = closed_dropped_;
    for (const auto& slot : slots_) {
        if (slot.stream) {
            total += slot.stream->queue.dropped();
        }
    }
    return total;
}

std::uint64_t MidiInput::resyncs() const {
    std::uint64_t total = closed_resyncs_;
    for (const auto& slot : slots_) {
        if (slot.stream) {
            total += slot.stream->queue.resyncs();
        }
    }
    return total;
}

const KeyboardProfile* MidiInput::profile() const {
    for (const auto& slot : slots_) {
        if (slot.in && slot.match.profile != nullptr) {
            return slot.match.profile;
        }
    }
    return nullptr;
}

int MidiInput::open_count() const {
    int count = 0;
    for (const auto& slot : slots_) {
        count += slot.in ? 1 : 0;
    }
    return count;
}

std::vector<std::string> MidiInput::open_names() const {
    std::vector<std::string> names;
    for (const auto& slot : slots_) {
        if (slot.in) {
            names.push_back(slot.name);
        }
    }
    return names;
}

} // namespace toonval::app
