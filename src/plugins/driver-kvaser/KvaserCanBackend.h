// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The first real hardware backend: Kvaser, through CANlib.
//
// Nothing of CANlib appears in this header. `canlib.h` is included by
// KvaserCanBackend.cpp and nowhere else in the entire application, so a CANlib
// type can never leak past the driver layer (rule #4). The rest of TorqueBus
// cannot tell this apart from the virtual backend.
//
// The SDK is optional at build time. When CMake does not find it, this class
// still compiles and still constructs - isAvailable() returns false, enumerate()
// returns nothing, and the Hardware Manager shows a greyed-out entry instead of
// the application failing to start (rule #9, and PLAN.md section 31).

#pragma once

#include "core/Result.h"
#include "core/can/CanFrame.h"
#include "core/can/CanTypes.h"
#include "drivers/api/ICanBackend.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace torquebus {

class KvaserCanBackend final : public ICanBackend {
public:
    KvaserCanBackend();
    ~KvaserCanBackend() override;

    [[nodiscard]] std::string_view name() const noexcept override { return "kvaser"; }

    /// True when this build was compiled against CANlib *and* the driver
    /// reports at least one channel. A machine with the SDK but no Kvaser
    /// software installed answers false, which is the honest answer.
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

    /// True when this binary was built with CANlib available. Distinct from
    /// isAvailable(), which also requires the driver to be present at runtime -
    /// the two failures need different messages in the UI.
    [[nodiscard]] static bool isCompiledIn() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace torquebus
