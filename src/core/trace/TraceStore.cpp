// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/trace/TraceStore.h"

#include <algorithm>
#include <limits>

namespace torquebus {
namespace {

/// Nanoseconds to microseconds, saturating rather than wrapping.
///
/// A gap longer than ~71 minutes is not a delta anyone reads as a number; it
/// is "this identifier stopped". Saturating says that; wrapping would show a
/// small, plausible, wrong value - which is worse than an obviously clamped one.
[[nodiscard]] std::uint32_t toMicroseconds(std::uint64_t nanoseconds) noexcept
{
    const std::uint64_t microseconds = nanoseconds / 1000ULL;
    return microseconds > std::numeric_limits<std::uint32_t>::max()
               ? std::numeric_limits<std::uint32_t>::max()
               : static_cast<std::uint32_t>(microseconds);
}

/// Bitmask of payload bytes that differ between two frames.
[[nodiscard]] std::uint64_t changedByteMask(const CanFrame& previous,
                                            const CanFrame& current) noexcept
{
    std::uint64_t mask = 0;
    const std::size_t length =
        std::min<std::size_t>(std::max(previous.length, current.length), kMaxCanPayload);

    for (std::size_t index = 0; index < length; ++index) {
        const std::uint8_t before = index < previous.length ? previous.data[index] : 0U;
        const std::uint8_t after = index < current.length ? current.data[index] : 0U;
        if (before != after) {
            mask |= std::uint64_t{1} << index;
        }
    }

    return mask;
}

} // namespace

TraceStore::TraceStore(std::size_t capacity)
    : m_capacity{capacity == 0 ? 1 : capacity}
{
    // Allocated up front, not grown. A million-row vector reallocating in the
    // middle of a measurement would stall the executor for as long as the copy
    // takes, at exactly the moment the bus is busiest.
    m_rows.resize(m_capacity);
}

void TraceStore::append(std::span<const CanFrame> frames)
{
    for (const CanFrame& frame : frames) {
        appendOne(frame);
    }
}

void TraceStore::appendOne(const CanFrame& frame)
{
    const std::uint64_t key = keyFor(frame);

    // --- Per-identifier aggregate, and the cycle time that comes with it ---

    std::uint32_t cycleUs = 0;
    std::uint64_t occurrence = 1;

    const auto existing = m_identifierIndex.find(key);
    if (existing != m_identifierIndex.end()) {
        TraceIdentifierStats& stats = m_identifiers[existing->second];

        // The frame's own timestamp, not wall time: a replayed log has to
        // produce the same cycle times it did live, or the number is a
        // property of the playback rather than of the bus.
        const std::uint64_t previousNs = stats.lastFrame.timestampNs;
        if (frame.timestampNs >= previousNs) {
            cycleUs = toMicroseconds(frame.timestampNs - previousNs);
        }

        occurrence = ++stats.count;

        stats.changedBytes = changedByteMask(stats.lastFrame, frame);
        stats.lastCycleUs = cycleUs;
        stats.minCycleUs = stats.minCycleUs == 0 ? cycleUs : std::min(stats.minCycleUs, cycleUs);
        stats.maxCycleUs = std::max(stats.maxCycleUs, cycleUs);
        stats.lastFrame = frame;
    } else {
        TraceIdentifierStats stats;
        stats.channel = frame.channel;
        stats.identifier = frame.identifier;
        stats.format = frame.format;
        stats.count = 1;
        stats.lastFrame = frame;
        // Cycle stays zero on a first sighting: there is nothing to measure
        // against yet, and showing "0 ms" would read as "arriving constantly".

        m_identifierIndex.emplace(key, m_identifiers.size());
        m_identifiers.push_back(stats);
    }

    // --- The row itself ---------------------------------------------------

    TraceRow row;
    row.frame = frame;
    row.cycleUs = cycleUs;
    row.occurrence = occurrence;
    row.deltaUs = (m_totalAppended == 0 || frame.timestampNs < m_previousTimestampNs)
                      ? 0U
                      : toMicroseconds(frame.timestampNs - m_previousTimestampNs);

    m_previousTimestampNs = frame.timestampNs;

    const std::size_t slot = (m_first + m_size) % m_capacity;
    m_rows[slot] = row;

    if (m_size < m_capacity) {
        ++m_size;
    } else {
        // Full: this write landed on the oldest row, so the window slides.
        m_first = (m_first + 1) % m_capacity;
        ++m_discarded;
    }

    ++m_totalAppended;
}

void TraceStore::clear()
{
    m_first = 0;
    m_size = 0;
    m_totalAppended = 0;
    m_discarded = 0;
    m_previousTimestampNs = 0;

    m_identifiers.clear();
    m_identifierIndex.clear();

    // m_rows keeps its allocation: clearing a trace to start a new measurement
    // must not hand the memory back and then ask for it again a second later.
}

} // namespace torquebus
