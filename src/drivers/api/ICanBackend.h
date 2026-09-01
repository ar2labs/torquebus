// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The single seam between TorqueBus and any CAN hardware on earth.
//
// Everything above this interface - the engine, the trace, the logger, the
// DBC decoder, the UDS stack - is written exactly once and works with every
// backend. Everything below it is vendor-specific and knows nothing about the
// application (rule #2).
//
// Adding Vector, IXXAT, SocketCAN, TOSUN, J2534 or a home-made SLCAN adapter
// later means writing one more implementation of this class and registering
// it. No other file changes.

#pragma once

#include "core/Result.h"
#include "core/can/CanFrame.h"
#include "core/can/CanTypes.h"

#include <cstddef>
#include <functional>
#include <span>
#include <string_view>

namespace torquebus {

/// Called by the backend when frames arrive.
///
/// Threading contract: invoked from the backend's own receive thread, never
/// from the UI thread. The handler must not block, must not allocate on the
/// hot path and must not touch a widget - it hands the batch to a lock-free
/// queue and returns (rule #6).
///
/// Batching contract: backends deliver batches, not single frames. A backend
/// that can only read one frame at a time still accumulates until either its
/// batch fills up or its coalescing window expires.
using FrameHandler = std::function<void(std::span<const CanFrame>)>;

/// Called when the controller changes fault-confinement state, or when the
/// driver reports an overrun. Low frequency; same threading rules apply.
using StatusHandler = std::function<void(const CanBusStatus&)>;

/// One openable channel of one vendor family.
///
/// Lifetime: enumerate() may be called on a closed backend at any time.
/// open() -> start() -> ... -> stop() -> close() is the expected order;
/// implementations must tolerate close() on an already-closed backend and must
/// close themselves in their destructor.
class ICanBackend {
public:
    ICanBackend() = default;
    virtual ~ICanBackend() = default;

    ICanBackend(const ICanBackend&) = delete;
    ICanBackend& operator=(const ICanBackend&) = delete;
    ICanBackend(ICanBackend&&) = delete;
    ICanBackend& operator=(ICanBackend&&) = delete;

    /// Stable backend identifier used in device handles: "kvaser", "peak",
    /// "virtual". Must be constant for the lifetime of the process.
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    /// True when the underlying SDK / driver is actually present on this
    /// machine. A backend whose DLL is missing still constructs successfully
    /// and reports false here, so the Hardware Manager can show
    /// "Kvaser CANlib - not installed" instead of crashing at load time.
    [[nodiscard]] virtual bool isAvailable() const noexcept = 0;

    /// Lists the channels currently visible to this backend. Returns an empty
    /// list rather than an error when the backend is unavailable.
    [[nodiscard]] virtual CanDeviceInfoList enumerate() = 0;

    /// Reserves the channel described by `config` and applies its bit timing.
    /// Does not put the controller on the bus - start() does that.
    [[nodiscard]] virtual Result open(const CanChannelConfig& config) = 0;

    /// Goes bus-on and begins delivering frames to the frame handler.
    [[nodiscard]] virtual Result start() = 0;

    /// Goes bus-off and stops delivering frames. The channel stays open and
    /// can be started again without reapplying the configuration.
    virtual void stop() = 0;

    /// Releases the channel. Safe to call when not open.
    virtual void close() = 0;

    [[nodiscard]] virtual bool isOpen() const noexcept = 0;

    /// Queues one frame for transmission. Returns as soon as the frame is
    /// accepted by the driver; the echo of a successfully sent frame comes
    /// back through the frame handler with direction == Tx, so that the trace
    /// shows what actually reached the bus rather than what was requested.
    [[nodiscard]] virtual Result transmit(const CanFrame& frame) = 0;

    /// Queues a batch. The default implementation forwards frame by frame and
    /// stops at the first failure; backends with a native burst API override it.
    [[nodiscard]] virtual Result transmit(std::span<const CanFrame> frames)
    {
        for (const CanFrame& frame : frames) {
            if (Result result = transmit(frame); result.failed()) {
                return result;
            }
        }
        return Result::ok();
    }

    /// Current controller health. Cheap enough to poll a few times per second.
    [[nodiscard]] virtual CanBusStatus status() const = 0;

    /// Capabilities of the channel that is currently open, or of the backend
    /// in general when nothing is open.
    [[nodiscard]] virtual CanCapabilities capabilities() const = 0;

    /// Installs the receive callback. Must be called before start().
    virtual void setFrameHandler(FrameHandler handler) = 0;

    /// Installs the status callback. Optional.
    virtual void setStatusHandler(StatusHandler handler) = 0;
};

} // namespace torquebus
