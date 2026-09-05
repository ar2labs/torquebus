// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// PEAK-System adapters, through the Qt SerialBus "peakcan" plugin.
//
// The second real backend, and the one that tests the claim the first one
// made. Kvaser arrived as one implementation of ICanBackend and one line in the
// registry, with nothing above the driver layer changing. If that claim holds,
// PEAK costs exactly the same - and it does: this file, its .cpp, and one line
// in CanBackendRegistry::registerBuiltins.
//
// Two things are different from Kvaser, and neither reaches past this header.
//
// **TorqueBus does not link PCAN-Basic.** It goes through Qt SerialBus, which
// loads PCANBasic.dll at runtime. So there is nothing to find at configure
// time, and a machine without the PEAK driver produces a backend that reports
// itself unavailable rather than a build that fails - the same shape as Kvaser
// without CANlib, reached by a different route.
//
// **QCanBusDevice is a QObject and wants an event loop.** It is not thread
// safe, it delivers frames through a signal, and its plugin does its reading on
// the event loop of whichever thread it lives on. So this backend owns a thread
// with an event loop, creates the device on it, and never touches the device
// from anywhere else. Everything crossing that boundary crosses it explicitly.
// See the .cpp - that arrangement is the whole substance of this backend.
//
// No Qt type appears below. `QCanBusDevice` is included by PeakCanBackend.cpp
// and nowhere else, so the rest of TorqueBus cannot tell this apart from the
// virtual backend (rule #4).

#pragma once

#include "core/Result.h"
#include "core/can/CanFrame.h"
#include "core/can/CanTypes.h"
#include "drivers/api/ICanBackend.h"

#include <memory>
#include <span>
#include <string_view>

namespace torquebus {

class PeakCanBackend final : public ICanBackend {
public:
    PeakCanBackend();
    ~PeakCanBackend() override;

    [[nodiscard]] std::string_view name() const noexcept override { return "peak"; }

    /// True when this build has Qt SerialBus *and* the peakcan plugin is
    /// actually loadable on this machine.
    ///
    /// Both halves matter and they fail differently: a build without SerialBus
    /// has no plugin to ask for, and a machine without PCANBasic.dll has the
    /// plugin but no devices. Either way the honest answer is false, and the
    /// Hardware Manager shows a greyed-out entry rather than an empty list the
    /// user has to interpret.
    [[nodiscard]] bool isAvailable() const noexcept override;

    [[nodiscard]] CanDeviceInfoList enumerate() override;

    [[nodiscard]] Result open(const CanChannelConfig& config) override;
    [[nodiscard]] Result start() override;
    void stop() override;
    void close() override;
    [[nodiscard]] bool isOpen() const noexcept override;

    [[nodiscard]] Result transmit(const CanFrame& frame) override;

    [[nodiscard]] CanBusStatus status() const override;
    [[nodiscard]] CanCapabilities capabilities() const override;

    void setFrameHandler(FrameHandler handler) override;
    void setStatusHandler(StatusHandler handler) override;

    /// True when this binary was built with Qt SerialBus available.
    ///
    /// Distinct from isAvailable(), which also asks the running machine. A test
    /// needs to tell "this build cannot do PEAK at all" from "this build can,
    /// but there is no adapter here" - they call for different skips.
    [[nodiscard]] static bool isCompiledIn() noexcept;

    /// The plugin key this backend asks Qt SerialBus for.
    ///
    /// Exposed so a diagnostic can say which plugin was looked for when none
    /// was found, rather than reporting "no PEAK devices" and leaving the user
    /// to guess whether that means no adapter or no driver.
    [[nodiscard]] static std::string_view pluginName() noexcept { return "peakcan"; }

private:
    /// Reads everything the device has and hands it over as one batch.
    ///
    /// Runs on the device's own thread, from its framesReceived signal. Named
    /// here rather than written inline in the connect so that the one function
    /// allowed to touch the device outside a queued call is visible from the
    /// header.
    void drainDevice();

    /// The plugin reported an error. `busError` is a QCanBusDevice::CanBusError,
    /// passed as an int so that no Qt type reaches this header.
    void onDeviceError(int busError);

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace torquebus
