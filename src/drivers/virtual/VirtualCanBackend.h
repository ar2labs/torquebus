// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// A CAN bus made of nothing but memory.
//
// Two instances opened on the same virtual device see each other's traffic,
// exactly like Kvaser Virtual CAN 0 / 1 do - which means the whole application
// can be developed, demonstrated and unit tested on a machine with no CAN
// hardware and no vendor driver installed at all. This is the backend the CI
// runs against (see docs/ARCHITECTURE.md, "Testing without hardware").
//
// Since v0.2 it also behaves like a real backend in the way that matters most
// for the pipeline: delivery happens on the backend's own receive thread, in
// batches, never on the caller's thread. Code that would deadlock or race
// against a real adapter deadlocks and races against this one too - which is
// the entire point of having it.
//
// The optional traffic generator turns it into a load source: a configurable
// number of periodic messages at a configurable rate, used to prove the
// 100k frames/s target without plugging anything in.

#pragma once

#include "core/Result.h"
#include "core/can/CanFrame.h"
#include "core/can/CanTypes.h"
#include "drivers/api/ICanBackend.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace torquebus {

/// Describes synthetic traffic the backend generates on its own.
struct VirtualTrafficPattern final {
    /// Identifiers are allocated sequentially from here.
    std::uint32_t baseIdentifier{0x100};

    /// How many distinct message identifiers to cycle through.
    std::uint32_t messageCount{10};

    /// Payload length of every generated frame, in bytes.
    std::uint8_t payloadLength{8};

    /// Target rate across all messages, in frames per second. Zero disables
    /// the generator.
    std::uint32_t framesPerSecond{0};

    /// Total frames to generate before stopping. Zero means "until stopped",
    /// which is what a demo wants; a finite count is what a test wants.
    std::uint64_t totalFrames{0};

    bool extended{false};

    /// Vary the payload per frame, so a decoder or a "changed bytes"
    /// highlight has something to react to.
    bool varyPayload{true};
};

class VirtualCanBackend final : public ICanBackend {
public:
    /// Number of virtual channels the backend always offers.
    static constexpr std::uint32_t kChannelCount = 2;

    VirtualCanBackend();
    ~VirtualCanBackend() override;

    [[nodiscard]] std::string_view name() const noexcept override { return "virtual"; }

    /// Always true: the virtual backend has no external dependency.
    [[nodiscard]] bool isAvailable() const noexcept override { return true; }

    [[nodiscard]] CanDeviceInfoList enumerate() override;

    [[nodiscard]] Result open(const CanChannelConfig& config) override;
    [[nodiscard]] Result start() override;
    void stop() override;
    void close() override;
    [[nodiscard]] bool isOpen() const noexcept override;

    [[nodiscard]] Result transmit(const CanFrame& frame) override;
    [[nodiscard]] Result transmit(std::span<const CanFrame> frames) override;

    [[nodiscard]] CanBusStatus status() const override;
    [[nodiscard]] CanCapabilities capabilities() const override;

    void setFrameHandler(FrameHandler handler) override;
    void setStatusHandler(StatusHandler handler) override;

    // --- Virtual-backend extras -------------------------------------------

    /// Configures synthetic traffic. Must be called before start().
    void setTrafficPattern(const VirtualTrafficPattern& pattern);

    /// True once a finite traffic pattern has produced every frame it owed.
    [[nodiscard]] bool trafficCompleted() const noexcept;

    /// Blocks until a finite traffic pattern finishes, or the timeout expires.
    /// Returns false on timeout. Tests only.
    bool waitForTrafficCompletion(std::chrono::milliseconds timeout);

    /// Blocks until every frame handed to the backend has been delivered to
    /// the frame handler. Tests only: it removes the need for sleeps.
    bool waitForDelivery(std::chrono::milliseconds timeout);

private:
    /// Called when a worker thread ends because something was thrown out of it.
    ///
    /// The channel really has stopped delivering, so it says Offline - the same
    /// thing it would say for an unplugged adapter, which is the honest answer
    /// and the one the rest of the application already knows how to show.
    void reportThreadStopped(std::string_view reason);

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace torquebus
