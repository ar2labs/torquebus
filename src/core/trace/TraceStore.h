// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Where a measurement's frames live while you look at them.
//
// The target from PLAN.md section 17 is a million retained frames with a
// responsive UI, and that number drives every decision here:
//
//   * Flat and contiguous. One vector, indexed by row. A table view asks for
//     row 743,912 while the user drags the scrollbar, and that has to be a
//     subscript, not a walk.
//
//   * Bounded, and the oldest goes first. A trace that grows until the machine
//     swaps is worse than one that says "showing the last million frames".
//     Discards are counted and shown, never silent.
//
//   * Derived columns are computed once, on insert. Delta and cycle time need
//     the previous frame - overall and per identifier - which is cheap while
//     the frame is in hand and O(n) if asked for later, per repaint, per row.
//
// Not thread safe. The store is written from the pipeline executor thread and
// read from the GUI thread, and the model in the ui layer is what mediates
// between them: it copies the rows it needs under a snapshot, rather than
// letting the view reach into the store while the executor is appending.

#pragma once

#include "core/can/CanFrame.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace torquebus {

/// One line of the trace, ready to display.
///
/// 104 bytes: an 88-byte CanFrame plus 16 of derived columns. At the million
/// rows this store is sized for, that is **99 MB** - a real cost, chosen
/// deliberately, and the reason the derived fields are as small as they are.
///
/// Cycle and delta are microseconds in 32-bit fields rather than nanoseconds in
/// 64: a gap of more than ~71 minutes between two frames of the same identifier
/// is not a cycle time any more, it is "this identifier stopped". The frame
/// itself keeps the nanosecond timestamp it arrived with, so no precision is
/// lost where precision means something.
///
/// The assertion below is not decoration. Adding one more 8-byte field here
/// costs another 8 MB of resident memory at full capacity, and that should be a
/// decision someone makes on purpose.
struct TraceRow final {
    CanFrame frame{};

    /// Microseconds since the previous frame in the trace, on any channel.
    /// Zero on the first row.
    std::uint32_t deltaUs{};

    /// Microseconds since the previous frame with the same identifier on the
    /// same channel. Zero the first time an identifier is seen - which is why
    /// the trace shows a blank rather than "0 ms" for a first sighting.
    std::uint32_t cycleUs{};

    /// How many times this identifier has been seen on this channel, including
    /// this frame. Starts at 1.
    std::uint64_t occurrence{};
};

// Equality, not <=. MSVC warns (C4296) that a <= against the exact size is
// always true, and it is right - but the intent was never "at most": it is
// "this is the size, and changing it costs 8 MB per 8 bytes at full capacity".
// An equality assert states that, and fires when someone changes the layout so
// the new number is written down deliberately.
static_assert(sizeof(TraceRow) == 104,
              "TraceRow is multiplied by a million: every 8 bytes here is 8 MB "
              "of resident memory at full capacity. If this fired, the layout "
              "changed - update the number and the memory figures in the docs.");

/// Aggregate for one identifier on one channel - the Fixed / Unique ID view.
struct TraceIdentifierStats final {
    std::uint8_t channel{};
    std::uint32_t identifier{};
    CanFrameFormat format{CanFrameFormat::Standard};

    std::uint64_t count{};

    /// Most recent frame seen for this identifier, so the fixed view can show
    /// current payload without going back to the ring.
    CanFrame lastFrame{};

    std::uint32_t lastCycleUs{};
    std::uint32_t minCycleUs{};
    std::uint32_t maxCycleUs{};

    /// Bytes that differed between the last two frames, as a bitmask over the
    /// payload. This is what lets the trace highlight what changed - the single
    /// most useful thing a fixed-ID view does, and it is free here because both
    /// frames are in hand.
    std::uint64_t changedBytes{};
};

class TraceStore final {
public:
    /// A million rows is ~99 MB. That is the documented target, and it is a
    /// deliberate use of memory rather than an accident: a professional trace
    /// that forgets what happened thirty seconds ago is not much use during a
    /// fault that took a minute to reproduce.
    static constexpr std::size_t kDefaultCapacity = 1'000'000;

    explicit TraceStore(std::size_t capacity = kDefaultCapacity);

    /// Appends a batch. Called from the pipeline executor thread.
    void append(std::span<const CanFrame> frames);

    /// Discards everything, including the identifier index.
    void clear();

    // --- Chronological view ----------------------------------------------

    /// Rows currently retained. Never more than capacity().
    [[nodiscard]] std::size_t size() const noexcept { return m_size; }
    [[nodiscard]] std::size_t capacity() const noexcept { return m_capacity; }
    [[nodiscard]] bool empty() const noexcept { return m_size == 0; }

    /// Row by index, 0 being the oldest still retained. Behaviour is undefined
    /// past size(), the same as a vector - the model never asks beyond it.
    [[nodiscard]] const TraceRow& row(std::size_t index) const noexcept
    {
        return m_rows[(m_first + index) % m_capacity];
    }

    /// Total frames appended since the last clear, including those the ring has
    /// since overwritten. This is the number that says "you are looking at the
    /// last million of eight million".
    [[nodiscard]] std::uint64_t totalAppended() const noexcept { return m_totalAppended; }

    /// Frames dropped off the front because the ring wrapped.
    [[nodiscard]] std::uint64_t discarded() const noexcept { return m_discarded; }

    // --- Fixed / Unique ID view -------------------------------------------

    /// One entry per identifier seen, in first-seen order so the view does not
    /// reshuffle itself while the user is reading it.
    [[nodiscard]] const std::vector<TraceIdentifierStats>& identifiers() const noexcept
    {
        return m_identifiers;
    }

private:
    /// Packs channel, format and identifier into one key.
    [[nodiscard]] static std::uint64_t keyFor(const CanFrame& frame) noexcept
    {
        return (static_cast<std::uint64_t>(frame.channel) << 40U)
            | (static_cast<std::uint64_t>(frame.isExtended() ? 1U : 0U) << 32U)
            | frame.identifier;
    }

    void appendOne(const CanFrame& frame);

    std::size_t m_capacity;
    std::vector<TraceRow> m_rows;

    /// Index of the oldest retained row inside m_rows.
    std::size_t m_first{0};
    std::size_t m_size{0};

    std::uint64_t m_totalAppended{0};
    std::uint64_t m_discarded{0};
    std::uint64_t m_previousTimestampNs{0};

    std::vector<TraceIdentifierStats> m_identifiers;

    /// Key -> index into m_identifiers. Kept separate so the vector stays
    /// contiguous and in first-seen order for display.
    std::unordered_map<std::uint64_t, std::size_t> m_identifierIndex;
};

} // namespace torquebus
