// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Per-channel counters and bus load.
//
// Updated on the engine thread, one call per frame, so every operation here is
// an increment or an add - no division, no branching on vendor, no allocation.
// The derived values (frames/s, bus load) are computed only when someone asks,
// at the 10 Hz the status bar refreshes, not at the 100 kHz frames arrive.
//
// Bus load is computed from the frame's own bit count rather than from a
// driver-reported percentage: adapters disagree about what they count, and a
// number that changes meaning when you swap adapters is worse than no number.

#pragma once

#include "core/can/CanFrame.h"
#include "core/can/CanTypes.h"

#include <algorithm>
#include <cstdint>

namespace torquebus {

/// A snapshot handed to the UI. Plain values, safe to copy across threads.
struct CanStatisticsSnapshot final {
    std::uint64_t rxFrames{};
    std::uint64_t txFrames{};
    std::uint64_t errorFrames{};

    /// Frames dropped by the filter before entering the pipeline.
    std::uint64_t filteredFrames{};

    /// Frames dropped because a queue was full.
    std::uint64_t droppedFrames{};

    /// Frames per second over the last sampling window.
    double framesPerSecond{};

    /// Bus load over the last sampling window, as a percentage.
    double busLoadPercent{};

    /// Highest bus load seen since the measurement started.
    double peakBusLoadPercent{};

    /// Nominal bitrate the load is computed against, in bit/s.
    std::uint32_t bitrate{};

    CanBusState state{CanBusState::Offline};

    [[nodiscard]] std::uint64_t totalFrames() const noexcept { return rxFrames + txFrames; }
};

/// Accumulator owned by one channel.
///
/// Not thread safe by design: it is touched only by the engine thread, and
/// published to other threads as an immutable snapshot().
class CanStatistics final {
public:
    CanStatistics() = default;

    /// Sets the nominal bitrate used as the denominator of the bus load.
    void setBitrate(std::uint32_t bitrate) noexcept { m_bitrate = bitrate; }

    [[nodiscard]] std::uint32_t bitrate() const noexcept { return m_bitrate; }

    void setState(CanBusState state) noexcept { m_state = state; }

    /// Counts one frame that passed the filter. `bits` is cached by the caller
    /// when it already computed it.
    void recordFrame(const CanFrame& frame) noexcept
    {
        if (frame.error) {
            ++m_errorFrames;
        }

        if (frame.isRx()) {
            ++m_rxFrames;
        } else {
            ++m_txFrames;
        }

        ++m_windowFrames;
        m_windowBits += approximateFrameBitCount(frame);
    }

    void recordFiltered(std::uint64_t count = 1) noexcept { m_filteredFrames += count; }

    /// Overwrites rather than accumulates: the queue's overflow counter is
    /// already cumulative, so adding it every window would multiply it.
    void setDropped(std::uint64_t total) noexcept { m_droppedFrames = total; }

    /// Closes the current sampling window and starts a new one.
    ///
    /// `elapsedNs` is the wall time the window covered. Called by the engine
    /// on a timer; the derived rates are only as accurate as that interval,
    /// which is why the interval is passed in rather than measured here.
    void closeWindow(std::uint64_t elapsedNs) noexcept
    {
        if (elapsedNs == 0) {
            return;
        }

        const double seconds = static_cast<double>(elapsedNs) / 1'000'000'000.0;

        m_framesPerSecond = static_cast<double>(m_windowFrames) / seconds;

        if (m_bitrate > 0) {
            const double capacityBits = static_cast<double>(m_bitrate) * seconds;
            m_busLoadPercent = 100.0 * static_cast<double>(m_windowBits) / capacityBits;
            m_busLoadPercent = std::clamp(m_busLoadPercent, 0.0, 100.0);
            m_peakBusLoadPercent = std::max(m_peakBusLoadPercent, m_busLoadPercent);
        }

        m_windowFrames = 0;
        m_windowBits = 0;
    }

    /// Zeroes everything. Called when a measurement starts, so numbers never
    /// carry over from a previous run.
    void reset() noexcept
    {
        *this = CanStatistics{};
    }

    [[nodiscard]] CanStatisticsSnapshot snapshot() const noexcept
    {
        CanStatisticsSnapshot result;
        result.rxFrames = m_rxFrames;
        result.txFrames = m_txFrames;
        result.errorFrames = m_errorFrames;
        result.filteredFrames = m_filteredFrames;
        result.droppedFrames = m_droppedFrames;
        result.framesPerSecond = m_framesPerSecond;
        result.busLoadPercent = m_busLoadPercent;
        result.peakBusLoadPercent = m_peakBusLoadPercent;
        result.bitrate = m_bitrate;
        result.state = m_state;
        return result;
    }

private:
    std::uint64_t m_rxFrames{};
    std::uint64_t m_txFrames{};
    std::uint64_t m_errorFrames{};
    std::uint64_t m_filteredFrames{};
    std::uint64_t m_droppedFrames{};

    std::uint64_t m_windowFrames{};
    std::uint64_t m_windowBits{};

    double m_framesPerSecond{};
    double m_busLoadPercent{};
    double m_peakBusLoadPercent{};

    // The same default as CanBitTiming, so a channel whose bitrate was never
    // pushed in reports the load it would have at the rate it would open at.
    std::uint32_t m_bitrate{kDefaultBitrate};
    CanBusState m_state{CanBusState::Offline};
};

} // namespace torquebus
