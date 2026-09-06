// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The transport: play, pause, speed, and where in the recording we are.
//
// A replay block already knows how to pour a file through the pipeline in the
// recording's own time. What it could not do is be *driven* - and a twenty
// minute log with no way to pause at the interesting second, or to go back ten
// seconds and watch it again, is a file being played rather than a measurement
// being examined.
//
// The awkward part is not the transport, it is where the transport lives. The
// buttons are in the GUI thread; the replay runs on the executor thread, in the
// middle of the frame path. Rule #5 says the GUI never goes on that path, and a
// mutex shared between the two would put it there - the executor would block on
// a lock held by a thread that is repainting a window.
//
// So this is the whole of the conversation between them, and it is made of
// atomics:
//
//   * The GUI **writes** paused, speed and a seek request.
//   * The replay **reads** those, and writes back position and finished.
//
// Nobody waits for anybody. A control change lands on the next pass through the
// graph - a few milliseconds - which is far below what a hand on a button can
// perceive, and the price is that the transport is never exact to the frame.
// That is the right trade for a player: exactness belongs to the file, not to
// the button.

#pragma once

#include <atomic>
#include <cstdint>

namespace torquebus {

class ReplayControl final {
public:
    /// Slowest and fastest of the ordinary speeds, matching what the panel
    /// offers. Outside this range a replay is either not moving or not being
    /// watched.
    static constexpr double kMinimumSpeed = 0.1;
    static constexpr double kMaximumSpeed = 10.0;

    /// "As fast as the pipeline will take it", the last entry on every
    /// playback speed menu.
    ///
    /// A large multiplier rather than a special case, so nothing on the frame
    /// path has to branch on a mode: at this speed every frame in the buffer is
    /// already due, and the replay is bounded by how fast the graph drains it
    /// rather than by the clock. Which is what the menu entry means.
    static constexpr double kUnlimitedSpeed = 1.0e9;

    ReplayControl() = default;

    ReplayControl(const ReplayControl&) = delete;
    ReplayControl& operator=(const ReplayControl&) = delete;
    ReplayControl(ReplayControl&&) = delete;
    ReplayControl& operator=(ReplayControl&&) = delete;

    // --- Written by the GUI, read by the replay ---------------------------

    void setPaused(bool paused) noexcept { m_paused.store(paused, std::memory_order_relaxed); }

    [[nodiscard]] bool isPaused() const noexcept
    {
        return m_paused.load(std::memory_order_relaxed);
    }

    /// Clamped, because a speed of zero is a pause spelled a confusing way and
    /// a negative one is a replay running backwards, which this does not do.
    void setSpeed(double speed) noexcept
    {
        if (speed < kMinimumSpeed) {
            speed = kMinimumSpeed;
        } else if (speed > kUnlimitedSpeed) {
            speed = kUnlimitedSpeed;
        }

        m_speed.store(speed, std::memory_order_relaxed);
    }

    [[nodiscard]] double speed() const noexcept { return m_speed.load(std::memory_order_relaxed); }

    /// Asks the replay to continue from `positionNs` into the recording.
    ///
    /// Takes effect on a later pass, and a request made while an earlier one is
    /// still being served replaces it: dragging the timeline produces a stream
    /// of these, and serving every intermediate position of a drag would be
    /// slower than serving only where the finger stopped.
    void requestSeek(std::uint64_t positionNs) noexcept
    {
        m_seekTargetNs.store(positionNs, std::memory_order_relaxed);
        // Released after the target, so a replay that sees the new generation
        // sees the target that goes with it.
        m_seekGeneration.fetch_add(1, std::memory_order_release);
    }

    // --- Written by the replay, read by the GUI ---------------------------

    /// Returns true, once, for each seek request, filling in `target`.
    [[nodiscard]] bool takeSeekRequest(std::uint64_t& target) noexcept
    {
        const std::uint32_t generation = m_seekGeneration.load(std::memory_order_acquire);
        if (generation == m_seekServed) {
            return false;
        }

        m_seekServed = generation;
        target = m_seekTargetNs.load(std::memory_order_relaxed);
        return true;
    }

    void publishPosition(std::uint64_t positionNs) noexcept
    {
        m_positionNs.store(positionNs, std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t positionNs() const noexcept
    {
        return m_positionNs.load(std::memory_order_relaxed);
    }

    void publishFinished(bool finished) noexcept
    {
        m_finished.store(finished, std::memory_order_relaxed);
    }

    [[nodiscard]] bool finished() const noexcept
    {
        return m_finished.load(std::memory_order_relaxed);
    }

    // --- What is being played ---------------------------------------------

    /// How long the recording is, from its first frame to its last.
    ///
    /// Set when the file is opened, by whoever scanned it - the format has no
    /// index and no frame count, so this is knowledge somebody paid a pass over
    /// the file for. Zero means nobody has, and the panel shows a transport
    /// without a total rather than a wrong one.
    void setDurationNs(std::uint64_t durationNs) noexcept
    {
        m_durationNs.store(durationNs, std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t durationNs() const noexcept
    {
        return m_durationNs.load(std::memory_order_relaxed);
    }

    /// True while a replay block is actually in the running graph. The panel
    /// has nothing to drive until this is set, and says so rather than
    /// offering buttons that do nothing.
    void setActive(bool active) noexcept { m_active.store(active, std::memory_order_relaxed); }

    [[nodiscard]] bool isActive() const noexcept
    {
        return m_active.load(std::memory_order_relaxed);
    }

    /// Forgets the last measurement's position, keeping the user's choices.
    ///
    /// Speed and paused are deliberately *not* cleared: somebody who set 0.5x
    /// and pressed Start again meant 0.5x. Position, finished and duration
    /// belong to the file and are the run's, not the user's.
    void resetForRun() noexcept
    {
        m_positionNs.store(0, std::memory_order_relaxed);
        m_durationNs.store(0, std::memory_order_relaxed);
        m_finished.store(false, std::memory_order_relaxed);
        m_active.store(false, std::memory_order_relaxed);
    }

private:
    std::atomic<bool> m_paused{false};
    std::atomic<double> m_speed{1.0};

    std::atomic<std::uint64_t> m_seekTargetNs{0};
    std::atomic<std::uint32_t> m_seekGeneration{0};

    /// The last generation the replay acted on. Touched only by the replay
    /// thread, so it is a plain member rather than an atomic.
    std::uint32_t m_seekServed{0};

    std::atomic<std::uint64_t> m_positionNs{0};
    std::atomic<std::uint64_t> m_durationNs{0};
    std::atomic<bool> m_finished{false};
    std::atomic<bool> m_active{false};
};

// A lock inside any of these would be a lock the executor thread takes while
// the GUI thread holds it, which is the whole thing this class exists to avoid.
// Every platform TorqueBus targets is 64-bit, where all four are single
// instructions - the assertion is here so that a port to one where they are not
// fails loudly rather than quietly serialising the frame path.
static_assert(std::atomic<bool>::is_always_lock_free);
static_assert(std::atomic<double>::is_always_lock_free);
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

} // namespace torquebus
