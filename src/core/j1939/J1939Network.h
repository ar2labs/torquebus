// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// What the J1939 block knows, in a form a panel can read.
//
// The block builds its address table and its fault lists on the executor
// thread, one frame at a time. A panel repaints on its own timer on the UI
// thread. Something has to stand between them, and the shape of that something
// is decided by how often each side moves.
//
// --- Why this is a snapshot and not a lock around the table -----------------
//
// The test report takes a lock on every result appended, and says so: a case
// finishing is a human-scale event, a few per second at most. **This is not
// that.** Every frame touches the address table - a lookup, a counter, a
// timestamp - and a lock there would be a lock on the frame path, which rule #5
// exists to prevent. At 150k frames/s it is not a contention problem, it is a
// design error.
//
// So the block keeps its own state with no lock at all, because only the
// executor touches it, and hands over a **copy once per pass** - one lock and
// one set of allocations per batch rather than per frame, which is the
// granularity the performance contract calls survivable.
//
// It hands one over only when something changed. On a steady bus, where the
// same ECUs send the same messages for an hour, nothing changes after the first
// second and this costs nothing at all.
//
// --- The revision counter ---------------------------------------------------
//
// A panel that copied the whole table every 50 ms to find out whether it had
// changed would be doing the expensive half of the work to answer the cheap
// question. `revision()` is a plain atomic load: when it has not moved, there
// is nothing to repaint and no lock to take.

#pragma once

#include "core/j1939/J1939AddressTable.h"
#include "core/j1939/J1939Diagnostics.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace torquebus {

/// Everything the panel shows, as of one pass.
struct J1939NetworkSnapshot final {
    std::vector<J1939NetworkNode> nodes;
    std::vector<J1939Defeated> defeated;

    /// The latest DM1 and DM2 from each ECU, in address order.
    std::vector<J1939Diagnostic> diagnostics;

    /// Which publication this came from. Zero means nothing has been published
    /// yet, which is a measurement that has not started rather than a bus with
    /// nobody on it - and a panel must say the difference.
    std::uint64_t revision{0U};
};

/// The hand-over point between the block and the panel.
///
/// Owned by the engine and not by the block, for the same reason the test
/// report is: the block dies with the graph at Stop, and what it found out is
/// most worth reading afterwards.
class J1939Network final {
public:
    /// Replaces the published copy. Called by the block, once per pass, and
    /// only when something actually changed.
    void publish(std::vector<J1939NetworkNode> nodes,
                 std::vector<J1939Defeated> defeated,
                 std::vector<J1939Diagnostic> diagnostics);

    /// A copy of the latest publication.
    [[nodiscard]] J1939NetworkSnapshot snapshot() const;

    /// How many publications have happened. A plain atomic load: a panel asks
    /// this on its timer and only calls snapshot() when the answer moved.
    [[nodiscard]] std::uint64_t revision() const noexcept
    {
        return m_revision.load(std::memory_order_acquire);
    }

    /// Forgets everything, and moves the revision so a panel notices.
    ///
    /// Called when a measurement is built, so that the last run's bus
    /// membership is not read as this one's.
    void clear();

private:
    mutable std::mutex m_mutex;

    std::vector<J1939NetworkNode> m_nodes;
    std::vector<J1939Defeated> m_defeated;
    std::vector<J1939Diagnostic> m_diagnostics;

    /// Outside the mutex on purpose: it is the thing read without taking it.
    std::atomic<std::uint64_t> m_revision{0U};
};

} // namespace torquebus
