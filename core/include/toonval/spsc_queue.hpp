// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace toonval {

// Bounded lock-free queue for exactly one producer thread and one consumer thread, e.g. MIDI events
// from libremidi's callback thread to the frame loop. push() and pop() never block, lock, or
// allocate. When the queue is full push() drops the item and counts it.
//
// Memory ordering: the producer writes the slot, then publishes it with a release store of tail_;
// the consumer's acquire load of tail_ makes the slot's contents visible before it reads them.
// Symmetrically, the consumer's release store of head_ after reading a slot pairs with the
// producer's acquire load of head_, so the producer never overwrites a slot still being read.
// Each index is written by one thread only, so plain load/store suffices (no read-modify-write).
// Indices count up without wrapping in practice (2^64 items) and are masked into the buffer.
template <typename T, std::size_t Capacity> class SpscQueue {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>, "T is copied by value between threads");

  public:
    static constexpr std::size_t capacity() { return Capacity; }

    // Producer thread only. Returns false and counts a drop when the queue is full.
    bool push(const T& item) noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail - head_cache_ == Capacity) {
            head_cache_ = head_.load(std::memory_order_acquire);
            if (tail - head_cache_ == Capacity) {
                dropped_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
        }
        buffer_[tail & kMask] = item;
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Consumer thread only. Returns false when the queue is empty.
    bool pop(T& out) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_cache_) {
            tail_cache_ = tail_.load(std::memory_order_acquire);
            if (head == tail_cache_) {
                return false;
            }
        }
        out = buffer_[head & kMask];
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // Items dropped because the queue was full. Any thread; the value may lag slightly.
    std::uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }

  private:
    static constexpr std::size_t kMask = Capacity - 1;
    // Fixed instead of std::hardware_destructive_interference_size, which varies by compiler
    // flags and warns on GCC. 64 bytes covers x86-64 and the Pi 5's Cortex-A76.
    static constexpr std::size_t kCacheLine = 64;

    // Consumer side: its index and its cached copy of the producer's index.
    alignas(kCacheLine) std::atomic<std::size_t> head_{0};
    std::size_t tail_cache_ = 0;
    // Producer side.
    alignas(kCacheLine) std::atomic<std::size_t> tail_{0};
    std::size_t head_cache_ = 0;
    alignas(kCacheLine) std::atomic<std::uint64_t> dropped_{0};
    alignas(kCacheLine) std::array<T, Capacity> buffer_{};
};

} // namespace toonval
