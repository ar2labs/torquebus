// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Universal Pass-Thru hardware backend implementing SAE J2534-1 / J2534-2.

#pragma once

#include "core/Result.h"
#include "core/can/CanFrame.h"
#include "core/can/CanTypes.h"
#include "drivers/api/ICanBackend.h"
#include "plugins/driver-j2534/J2534Registry.h"
#include "plugins/driver-j2534/J2534Types.h"

#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace torquebus {

class J2534CanBackend final : public ICanBackend {
public:
    J2534CanBackend();
    ~J2534CanBackend() override;

    [[nodiscard]] std::string_view name() const noexcept override { return "j2534"; }

    /// True when at least one J2534 device is registered in Windows, or when a test table is
    /// injected.
    [[nodiscard]] bool isAvailable() const noexcept override;

    [[nodiscard]] CanDeviceInfoList enumerate() override;

    [[nodiscard]] Result open(const CanChannelConfig& config) override;
    [[nodiscard]] Result start() override;
    void stop() override;
    void close() override;
    [[nodiscard]] bool isOpen() const noexcept override { return m_open; }

    [[nodiscard]] Result transmit(const CanFrame& frame) override;

    [[nodiscard]] CanBusStatus status() const override;
    [[nodiscard]] CanCapabilities capabilities() const override;

    void setFrameHandler(FrameHandler handler) override { m_frameHandler = std::move(handler); }
    void setStatusHandler(StatusHandler handler) override { m_statusHandler = std::move(handler); }

    /// Static query to match backend contract.
    [[nodiscard]] static bool isCompiledIn() noexcept { return true; }

    /// For unit tests: injects mock function pointers without touching the Windows registry or
    /// filesystem.
    void setFunctionTableForTesting(const j2534::J2534FunctionTable& table,
                                    std::string mockDeviceName = "Mock J2534 Device");

    /// Translates a J2534 PASSTHRU_MSG to a TorqueBus CanFrame.
    [[nodiscard]] static bool msgToCanFrame(const j2534::PASSTHRU_MSG& msg,
                                            CanFrame& outFrame,
                                            std::uint64_t fallbackTimestampNs = 0) noexcept;

    /// Translates a TorqueBus CanFrame to a J2534 PASSTHRU_MSG.
    [[nodiscard]] static bool canFrameToMsg(const CanFrame& frame,
                                            j2534::PASSTHRU_MSG& outMsg) noexcept;

private:
    void rxWorkerLoop();

    j2534::J2534FunctionTable m_api{};
    void* m_module{nullptr};
    unsigned long m_deviceId{0};
    unsigned long m_channelId{0};
    unsigned long m_filterId{0};

    bool m_open{false};
    std::atomic<bool> m_running{false};
    std::thread m_rxThread;

    FrameHandler m_frameHandler;
    StatusHandler m_statusHandler;

    std::atomic<std::uint64_t> m_txFrames{0};
    std::atomic<std::uint64_t> m_rxFrames{0};
    std::atomic<std::uint64_t> m_errorFrames{0};

    std::vector<j2534::J2534DeviceInfo> m_cachedDevices;
    std::string m_mockDeviceName;
    bool m_usingMock{false};
};

} // namespace torquebus
