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
//
// --- More than one bus --------------------------------------------------------
//
// A graph can have a J1939 block on each of two buses - a truck's powertrain
// and its body, a tractor and its implement - and the panels read one
// hand-over. So it keeps what each bus is saying apart: a block publishes the
// view of the bus it reads, under the channel its frames arrive on, and that
// replaces the last view of that bus and of no other. A snapshot is every bus,
// in channel order. Two blocks that read one channel read one bus, and publish
// one view of it in turn.
//
// The lamps are the exception, because a cluster has one stop lamp: they are
// the lamps of the whole vehicle, lit while any bus asks for them. Each block
// files here what its own bus lights (combineLamps) and writes the OR of all of
// them.

#pragma once

#include "core/j1939/J1939AddressTable.h"
#include "core/j1939/J1939Diagnostics.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

namespace torquebus {

/// What one bus is saying, as of the last pass of the J1939 block that reads it.
struct J1939BusView final {
    /// The application channel its frames arrive on: 0 is CAN 1.
    std::uint8_t channel{0U};

    std::vector<J1939NetworkNode> nodes;
    std::vector<J1939Defeated> defeated;

    /// The latest DM1 and DM2 from each ECU, in address order.
    ///
    /// What each ECU last *said*, and so kept after a measurement ends: it is what the J1939 panel
    /// reads afterwards. Nothing here says whether it is still being said - see `activeFaults`.
    std::vector<J1939Diagnostic> diagnostics;

    /// The trouble codes the bus is reporting **now**, from every ECU whose DM1 is still being
    /// repeated (j1939DiagnosticIsCurrent). Empty after a measurement ends and for a bus whose ECUs
    /// have gone quiet - which `diagnostics` is not. What a dashboard lists.
    ///
    /// Published with the rest, so that this list and the ECUs it names (`nodes`) are always from
    /// the same pass.
    std::vector<J1939ActiveFault> activeFaults;

    /// The source address of every ECU whose DM1 is still being repeated
    /// (j1939DiagnosticIsCurrent), in address order - the ones with nothing to report included,
    /// which `activeFaults` cannot say: a healthy ECU has no code to be listed by. So a DM1 in
    /// `diagnostics` whose source is not here is the last thing that ECU said, whether it listed
    /// faults or none. Empty after a measurement ends and for a bus that has gone quiet, like
    /// `activeFaults`, and published with it.
    std::vector<std::uint8_t> reporting;
};

/// Everything the panels show, as of one publication.
struct J1939NetworkSnapshot final {
    /// Every bus a J1939 block has published, in channel order: one for a graph with one J1939
    /// block, which is the usual one, and none before anything has been published. An address is a
    /// seat on one bus, so the same number on two of them is two ECUs.
    std::vector<J1939BusView> buses;

    /// Which publication this came from. Zero means nothing has been published
    /// yet, which is a measurement that has not started rather than a bus with
    /// nobody on it - and a panel must say the difference.
    std::uint64_t revision{0U};

    /// True when this snapshot came from publish() rather than clear().
    bool published{false};

    /// The bus on `channel`, or null when no block has published one.
    [[nodiscard]] const J1939BusView* bus(std::uint8_t channel) const noexcept;
};

/// The hand-over point between the blocks and the panels.
///
/// Owned by the engine and not by a block, for the same reason the test
/// report is: a block dies with the graph at Stop, and what it found out is
/// most worth reading afterwards.
class J1939Network final {
public:
    /// Replaces what the bus on `view.channel` is saying, and leaves every other bus as it was.
    /// Called by the block that reads that bus, once per pass, and only when something actually
    /// changed.
    void publish(J1939BusView view);

    /// The same, from the lists a block keeps: what the bus on `channel` is saying.
    void publish(std::vector<J1939NetworkNode> nodes,
                 std::vector<J1939Defeated> defeated,
                 std::vector<J1939Diagnostic> diagnostics,
                 std::vector<J1939ActiveFault> activeFaults = {},
                 std::vector<std::uint8_t> reporting = {},
                 std::uint8_t channel = 0U);

    /// A copy of the latest publication. Its `revision` is exactly the one its
    /// contents are from: both are read under the lock a publication is made
    /// under.
    [[nodiscard]] J1939NetworkSnapshot snapshot() const;

    /// How many publications have happened. A plain atomic load: a panel asks
    /// this on its timer and only calls snapshot() when the answer moved.
    [[nodiscard]] std::uint64_t revision() const noexcept
    {
        return m_revision.load(std::memory_order_acquire);
    }

    /// Forgets everything - every bus, and what each block said it lights - and moves the revision
    /// so a panel notices.
    ///
    /// Called when a measurement is built, so that the last run's bus
    /// membership is not read as this one's.
    void clear();

    /// The lamps of every bus at once. `lit` is what the block `publisher` says its own bus lights
    /// - stop, warning, malfunction indicator, protect; 1 lit and 0 dark - and the result is what
    /// all of them light together, the OR, which is what the lamp variables have to say. A block
    /// calls it when what its bus lights has changed, so a steady bus takes no lock for it.
    [[nodiscard]] std::array<std::int8_t, 4> combineLamps(const void* publisher,
                                                          const std::array<std::int8_t, 4>& lit);

private:
    mutable std::mutex m_mutex;

    /// One per bus, in channel order.
    std::vector<J1939BusView> m_buses;
    bool m_published{false};

    /// What each block says its bus lights. Keyed by the block, which is only ever compared and
    /// never followed, and forgotten at clear() - before the blocks of a new graph exist.
    std::vector<std::pair<const void*, std::array<std::int8_t, 4>>> m_lamps;

    /// Moved under the mutex, with the contents it counts, and read both ways:
    /// without the lock by revision(), which is the thing a panel asks every
    /// tick, and under it by snapshot().
    std::atomic<std::uint64_t> m_revision{0U};
};

} // namespace torquebus
