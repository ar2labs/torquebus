// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The transmit list, edited from the UI thread and read by the executor.
//
// Two threads touch this, and they touch it in very different ways:
//
//   - the UI thread adds, removes and edits rows, at the speed a person types;
//   - the executor thread asks "what is due?" once per dispatch pass, at up to
//     a thousand times a second.
//
// So the periodic entries live behind a mutex, held for the length of a scan
// over a list that is a few dozen rows long. That is the same trade CanEngine
// already makes for its sink lists, and for the same reason: the contention is
// a person against a loop, and the person always wins by being asleep.
//
// A one-shot send is different. Pressing "send" is a UI-thread event that must
// reach the bus on the next pass, and blocking the executor behind a mutex the
// UI happens to hold at that instant is the one case where the cost lands on
// the measurement. Those go through the lock-free queue instead - the same
// FrameQueue the drivers use.

#pragma once

#include "core/can/FrameQueue.h"
#include "core/transmit/TransmitEntry.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace torquebus {

class TransmitList final {
public:
    /// The fastest period the list will accept, in milliseconds.
    ///
    /// Not a limitation of the engine - it dispatches faster than this - but of
    /// what a period means. Below about a millisecond the interval is shorter
    /// than the jitter of the dispatch loop, so the list would be promising a
    /// rate it cannot hold and the trace would show it not holding it. A user
    /// who genuinely wants a frame per pass is describing a script, not a
    /// transmit list.
    static constexpr std::uint32_t kMinimumCycleMs = 1;

    /// One-shot sends waiting for the next pass.
    ///
    /// Bounded, like every other queue here. A user leaning on the send button
    /// while the engine is stopped would otherwise grow it without limit.
    static constexpr std::size_t kOneShotCapacity = 256;

    TransmitList() = default;

    TransmitList(const TransmitList&) = delete;
    TransmitList& operator=(const TransmitList&) = delete;

    // --- Editing, from the UI thread --------------------------------------

    /// Appends an entry and returns its index.
    std::size_t add(TransmitEntry entry);

    /// Replaces the entry at `index`. Out of range does nothing.
    void update(std::size_t index, TransmitEntry entry);

    void remove(std::size_t index);

    void setEnabled(std::size_t index, bool enabled);

    void clear();

    [[nodiscard]] std::size_t size() const;

    /// A copy of every entry, for the panel to display.
    ///
    /// A copy and not a reference: the executor updates `sentCount` and
    /// `lastSentUs` on its own thread, and handing out a pointer into the live
    /// vector would be handing out a data race with a nice interface on it.
    [[nodiscard]] std::vector<TransmitEntry> entries() const;

    /// The entry at `index`, or nothing if there is no such row.
    [[nodiscard]] bool entryAt(std::size_t index, TransmitEntry& out) const;

    // --- Sending ----------------------------------------------------------

    /// Queues one frame to go out on the next pass, whatever its schedule says.
    ///
    /// Returns false when the queue is full, which is the honest answer: the
    /// frame was not accepted, and a caller that ignores it will wonder why
    /// nothing happened.
    [[nodiscard]] bool sendOnce(const CanFrame& frame);

    /// Queues the entry at `index`, if it exists and is enabled.
    [[nodiscard]] bool sendOnce(std::size_t index);

    /// Everything that should go out now, appended to `out`.
    ///
    /// `nowUs` is microseconds since the measurement started - the same clock
    /// the frame timestamps use, so a user comparing a send time against a
    /// trace row is comparing two numbers that mean the same thing.
    ///
    /// Called once per dispatch pass from the executor thread.
    void collectDue(std::uint64_t nowUs, std::vector<CanFrame>& out);

    /// Forgets when each periodic entry last went out.
    ///
    /// Called at the start of a measurement. Without it a list that ran
    /// yesterday would consider every one of its rows overdue and send them all
    /// in the first pass - a burst nobody asked for, on a bus that may not
    /// expect it.
    void restartSchedule();

private:
    /// Guards m_entries. Held only for the length of a scan.
    mutable std::mutex m_mutex;
    std::vector<TransmitEntry> m_entries;

    /// When each periodic entry is next due, in microseconds. Parallel to
    /// m_entries and maintained with it.
    std::vector<std::uint64_t> m_nextDueUs;

    FrameQueue m_oneShots{kOneShotCapacity};
};

} // namespace torquebus
