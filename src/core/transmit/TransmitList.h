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
// One-shot sends were originally a lock-free FrameQueue, on the argument that a
// button press must never block the executor. That argument was inconsistent
// with the one above - a mutex held for the length of a scan is fine in both
// directions - and it broke outright the moment there was more than one
// channel: FrameQueue is single-producer *single-consumer*, and one node per
// channel means several consumers each draining a fragment of the queue.
//
// They are an ordinary guarded vector now. Presses happen at human speed and
// the executor holds the lock for microseconds.

#pragma once

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

    /// A frame waiting to go out once, and the channel it was asked for on.
    struct OneShot final {
        CanFrame frame;
        std::uint8_t channel{0};
    };

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
    [[nodiscard]] bool sendOnce(const CanFrame& frame, std::uint8_t channel);

    /// Queues the entry at `index`, if it exists and is enabled.
    [[nodiscard]] bool sendOnce(std::size_t index);

    /// Everything that should go out on `channel` now, appended to `out`.
    ///
    /// `nowUs` is microseconds since the measurement started - the same clock
    /// the frame timestamps use, so a user comparing a send time against a
    /// trace row is comparing two numbers that mean the same thing.
    ///
    /// Called once per dispatch pass per channel, from the executor thread. The
    /// channel filter is what lets one list serve every bus at once: a row is
    /// collected by the node that serves the channel the row names, and by no
    /// other.
    void collectDue(std::uint64_t nowUs, std::uint8_t channel, std::vector<CanFrame>& out);

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

    /// Guarded by the same mutex. See the note at the top of the file for why
    /// this is not the lock-free queue it started as.
    std::vector<OneShot> m_oneShots;
};

} // namespace torquebus
