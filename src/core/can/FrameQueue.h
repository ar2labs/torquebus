// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The hinge of the whole receive path.
//
// One driver thread writes, one engine thread reads, and neither ever takes a
// lock or allocates. This is what makes rule #5 ("a received frame never
// becomes a QObject") mean something in practice: at 100k frames/s a mutex
// per frame, or a heap allocation per frame, is the entire CPU budget.
//
// Capacity is fixed at construction and rounded up to a power of two so that
// the index wrap is a mask rather than a modulo. When the queue is full,
// frames are DROPPED and COUNTED - never buffered without bound, never
// silently lost. An overflow is a number the user can see in the statistics
// panel, because a tool that quietly loses frames is worse than one that says
// it did.

#pragma once

#include "core/can/CanFrame.h"

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace torquebus {

/// Single-producer / single-consumer bounded ring buffer of CAN frames.
///
/// Threading contract:
///   - exactly one thread calls push() / pushBatch()
///   - exactly one (different, or the same) thread calls drain()
///   - any thread may call the observer methods
///
/// Violating the single-producer or single-consumer rule is undefined
/// behaviour, not a slow path. The engine owns one queue per channel and
/// enforces this by construction.
class FrameQueue final {
public:
    /// Default capacity: roughly one second of a saturated 1 Mbit/s classic
    /// CAN bus, which is far more headroom than the 20-60 Hz drain needs.
    static constexpr std::size_t kDefaultCapacity = 16384;

    explicit FrameQueue(std::size_t capacity = kDefaultCapacity)
        : m_capacity{std::bit_ceil(capacity < 2 ? std::size_t{2} : capacity)}
        , m_mask{m_capacity - 1}
        , m_storage(m_capacity)
    { }

    FrameQueue(const FrameQueue&) = delete;
    FrameQueue& operator=(const FrameQueue&) = delete;
    FrameQueue(FrameQueue&&) = delete;
    FrameQueue& operator=(FrameQueue&&) = delete;

    /// Number of frames the queue can hold. Always a power of two, and always
    /// >= the requested capacity.
    [[nodiscard]] std::size_t capacity() const noexcept { return m_capacity; }

    /// Producer side. Returns false when the queue is full.
    ///
    /// Does NOT count an overflow: the queue declined the frame and said so,
    /// and the caller still has it. The counter tracks only frames the queue
    /// itself threw away, which is what pushBatch() does to a tail that does
    /// not fit. A producer that retries on false therefore does not inflate
    /// the drop count, and a producer that discards on false is the one
    /// deciding to lose data - so it is the one that should say so.
    ///
    /// Callable only from the producer thread.
    bool push(const CanFrame& frame) noexcept
    {
        const std::size_t write = m_write.load(std::memory_order_relaxed);
        const std::size_t next = write + 1;

        // acquire: pairs with the consumer's release in drain(), so the slot
        // we are about to overwrite is guaranteed to have been consumed.
        if (next - m_read.load(std::memory_order_acquire) > m_capacity) {
            return false;
        }

        m_storage[write & m_mask] = frame;

        // release: publishes the frame before the index that makes it visible.
        m_write.store(next, std::memory_order_release);
        return true;
    }

    /// Pushes as many frames of `frames` as fit, in order. Returns how many
    /// were accepted; the remainder are discarded by the queue and counted as
    /// overflows - this is the path where frames are genuinely lost, because
    /// the backend's batch is gone the moment the handler returns.
    ///
    /// Backends deliver batches, so this is the hot entry point - it publishes
    /// the write index once for the whole batch instead of once per frame.
    std::size_t pushBatch(std::span<const CanFrame> frames) noexcept
    {
        if (frames.empty()) {
            return 0;
        }

        const std::size_t write = m_write.load(std::memory_order_relaxed);
        const std::size_t read = m_read.load(std::memory_order_acquire);
        const std::size_t free = m_capacity - (write - read);

        const std::size_t accepted = frames.size() < free ? frames.size() : free;

        for (std::size_t index = 0; index < accepted; ++index) {
            m_storage[(write + index) & m_mask] = frames[index];
        }

        if (accepted > 0) {
            m_write.store(write + accepted, std::memory_order_release);
        }

        if (accepted < frames.size()) {
            m_overflows.fetch_add(frames.size() - accepted, std::memory_order_relaxed);
        }

        return accepted;
    }

    /// Consumer side. Moves up to `out.size()` frames into `out` and returns
    /// how many were written.
    ///
    /// Callable only from the consumer thread. The caller owns the destination
    /// buffer and reuses it, so a drain costs no allocation either.
    [[nodiscard]] std::size_t drain(std::span<CanFrame> out) noexcept
    {
        const std::size_t read = m_read.load(std::memory_order_relaxed);
        const std::size_t available = m_write.load(std::memory_order_acquire) - read;

        const std::size_t count = available < out.size() ? available : out.size();

        for (std::size_t index = 0; index < count; ++index) {
            out[index] = m_storage[(read + index) & m_mask];
        }

        if (count > 0) {
            m_read.store(read + count, std::memory_order_release);
        }

        return count;
    }

    /// Drains into a vector, resizing it to the number of frames read. The
    /// vector's capacity is retained across calls, so steady-state draining
    /// still does not allocate.
    std::size_t drainInto(std::vector<CanFrame>& out, std::size_t maximum)
    {
        if (out.size() < maximum) {
            out.resize(maximum);
        }

        const std::size_t count = drain(std::span<CanFrame>{out.data(), maximum});
        out.resize(count);
        return count;
    }

    // --- Observers (safe from any thread, approximate by nature) -----------

    [[nodiscard]] std::size_t size() const noexcept
    {
        return m_write.load(std::memory_order_acquire) - m_read.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    /// Frames the queue itself discarded because it was full, since
    /// construction or since the last resetOverflows(). Surfaced as
    /// CanBusStatus::softwareOverruns. See push() for why a rejected push()
    /// is not counted here.
    [[nodiscard]] std::uint64_t overflows() const noexcept
    {
        return m_overflows.load(std::memory_order_relaxed);
    }

    void resetOverflows() noexcept { m_overflows.store(0, std::memory_order_relaxed); }

    /// Discards everything currently queued. Consumer-side operation; used
    /// when a measurement stops so stale frames do not leak into the next one.
    void clear() noexcept
    {
        m_read.store(m_write.load(std::memory_order_acquire), std::memory_order_release);
    }

private:
    // Producer and consumer indices live on separate cache lines: sharing one
    // would reintroduce, through false sharing, exactly the contention the
    // lock-free design exists to avoid.
    static constexpr std::size_t kCacheLine = 64;

    const std::size_t m_capacity;
    const std::size_t m_mask;

    std::vector<CanFrame> m_storage;

    // MSVC warns (C4324) that the alignas below pads the object. That padding
    // is the entire point - it is what keeps the producer and consumer indices
    // off each other's cache line - so the warning is silenced here rather
    // than project-wide, where it would also hide accidental padding.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

    alignas(kCacheLine) std::atomic<std::size_t> m_write{0};
    alignas(kCacheLine) std::atomic<std::size_t> m_read{0};
    alignas(kCacheLine) std::atomic<std::uint64_t> m_overflows{0};

#ifdef _MSC_VER
#pragma warning(pop)
#endif
};

} // namespace torquebus
