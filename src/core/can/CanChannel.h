// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// One application channel: "CAN 1", as the user thinks of it.
//
// It binds together the four things that always travel together - a backend,
// a queue, a filter and a statistics accumulator - and it is the object the
// project file persists (PLAN.md section 13). The user maps CAN 1 onto
// "Kvaser Virtual CAN 0" once; everything afterwards refers to CAN 1.
//
// Threading: the backend's receive thread calls onFramesReceived(), which does
// nothing but push into the queue. The engine thread calls drain(). Nothing
// else touches either side.

#pragma once

#include "core/Result.h"
#include "core/can/CanFilter.h"
#include "core/can/CanFrame.h"
#include "core/can/CanStatistics.h"
#include "core/can/CanTypes.h"
#include "core/can/FrameQueue.h"
#include "drivers/api/ICanBackend.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace torquebus {

class CanChannel final {
public:
    /// Takes ownership of `backend`. `applicationChannel` is the zero-based
    /// index the user sees as "CAN 1", "CAN 2", ...
    CanChannel(std::uint8_t applicationChannel,
               std::unique_ptr<ICanBackend> backend,
               CanChannelConfig config);

    ~CanChannel();

    CanChannel(const CanChannel&) = delete;
    CanChannel& operator=(const CanChannel&) = delete;
    CanChannel(CanChannel&&) = delete;
    CanChannel& operator=(CanChannel&&) = delete;

    [[nodiscard]] std::uint8_t index() const noexcept { return m_applicationChannel; }

    /// "CAN 1"
    [[nodiscard]] std::string displayName() const;

    [[nodiscard]] const CanChannelConfig& config() const noexcept { return m_config; }

    /// Capabilities of the bound device, for capability-driven UI (rule #9).
    [[nodiscard]] CanCapabilities capabilities() const;

    // --- Lifecycle --------------------------------------------------------

    /// Opens the backend and installs the receive callback. Idempotent.
    [[nodiscard]] Result open();

    /// Goes bus-on. Statistics are reset here, so a measurement never inherits
    /// numbers from the previous one.
    [[nodiscard]] Result start();

    void stop();
    void close();

    [[nodiscard]] bool isOpen() const noexcept;
    [[nodiscard]] bool isRunning() const noexcept { return m_running; }

    // --- Data path --------------------------------------------------------

    /// Queues a frame for transmission. The application channel is stamped on
    /// the frame here, so callers never have to remember to do it.
    [[nodiscard]] Result transmit(CanFrame frame);

    /// Consumer side, called by the engine thread. Moves up to `maximum`
    /// frames into `out`, applies the filter, and updates the statistics.
    /// Returns how many frames survived filtering.
    std::size_t drain(std::vector<CanFrame>& out, std::size_t maximum);

    // --- Filtering --------------------------------------------------------

    [[nodiscard]] CanFilterSet& filters() noexcept { return m_filters; }
    [[nodiscard]] const CanFilterSet& filters() const noexcept { return m_filters; }

    // --- Observation ------------------------------------------------------

    [[nodiscard]] CanStatisticsSnapshot statistics() const { return m_statistics.snapshot(); }

    /// Closes the current statistics sampling window. Engine thread only.
    void closeStatisticsWindow(std::uint64_t elapsedNs);

    /// Latest driver-reported health, merged with our own overflow counter.
    [[nodiscard]] CanBusStatus status() const;

    [[nodiscard]] std::size_t queueDepth() const noexcept { return m_queue.size(); }

private:
    void onFramesReceived(std::span<const CanFrame> frames) noexcept;
    void onStatusChanged(const CanBusStatus& status) noexcept;

    std::uint8_t m_applicationChannel;
    std::unique_ptr<ICanBackend> m_backend;
    CanChannelConfig m_config;

    FrameQueue m_queue;
    CanFilterSet m_filters;
    CanStatistics m_statistics;

    /// Written by the backend's status callback, read by the engine.
    std::atomic<CanBusState> m_state{CanBusState::Offline};
    std::atomic<std::uint64_t> m_hardwareOverruns{0};

    bool m_open{false};
    bool m_running{false};
};

} // namespace torquebus
