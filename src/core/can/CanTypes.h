// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Vendor-neutral description of channels, bit timing, capabilities and bus
// state. Shared by the core, by every driver backend and by the UI; like
// CanFrame, this header pulls in nothing from Qt or from a vendor SDK.

#pragma once

#include "core/can/CanFrame.h"

#include <cstdint>
#include <string>
#include <vector>

namespace torquebus {

/// What a device can actually do. Queried once at enumeration time so that the
/// rest of the code never has to ask "which vendor is this?" (rule #9).
struct CanCapabilities final {
    bool canClassic{true};
    bool canFd{false};
    bool canFdBrs{false};

    bool listenOnly{false};
    bool hardwareTimestamp{false};

    bool errorFrames{false};
    bool hardwareFilters{false};

    bool virtualDevice{false};

    std::uint32_t maxChannels{1};
};

/// One channel exposed by one backend, as returned by ICanBackend::enumerate().
struct CanDeviceInfo final {
    /// Stable identifier used to reopen this exact channel later, e.g.
    /// "kvaser:0" or "peak:can0". Persisted inside .tbsproj files, so it must
    /// not change between runs for the same physical device.
    std::string handle;

    /// Backend that owns the channel ("kvaser", "peak", "virtual").
    std::string backend;

    /// Human readable device name, e.g. "Kvaser Virtual CAN 0".
    std::string name;

    /// Optional vendor serial number, empty when unavailable.
    std::string serialNumber;

    /// Zero-based channel index inside the device.
    std::uint32_t channelIndex{};

    CanCapabilities capabilities{};
};

/// Nominal (arbitration) and data phase bit timing.
struct CanBitTiming final {
    /// Arbitration bitrate in bit/s. 500000 by default.
    std::uint32_t bitrate{500'000};

    /// Data phase bitrate in bit/s, used only when CAN FD with BRS is enabled.
    std::uint32_t dataBitrate{2'000'000};

    /// Sample point as a ratio in [0, 1]. Zero means "let the driver decide".
    double samplePoint{0.0};

    /// Data phase sample point as a ratio in [0, 1]. Zero means automatic.
    double dataSamplePoint{0.0};
};

/// Everything needed to open one channel.
struct CanChannelConfig final {
    /// Device handle from CanDeviceInfo::handle.
    std::string deviceHandle;

    /// Application channel index this hardware channel is mapped to
    /// (CAN 1 == 0). Stamped onto every frame the channel produces.
    std::uint8_t applicationChannel{};

    CanBitTiming timing{};

    bool canFdEnabled{false};
    bool bitRateSwitchEnabled{false};

    /// Receive only; never send an acknowledge bit onto the bus.
    bool listenOnly{false};

    /// Open the channel without going bus-on until start() is called.
    bool silentStartup{false};

    /// Ask the driver to deliver error frames.
    bool receiveErrorFrames{true};
};

/// Controller state as defined by ISO 11898-1 fault confinement.
enum class CanBusState : std::uint8_t {
    Offline,      ///< Channel closed / not started.
    ErrorActive,  ///< Normal operation.
    ErrorWarning, ///< An error counter passed 96.
    ErrorPassive, ///< An error counter passed 127.
    BusOff        ///< Transmit error counter passed 255.
};

/// Snapshot of a channel's health, polled by the UI at a low rate. Never
/// pushed per frame.
struct CanBusStatus final {
    CanBusState state{CanBusState::Offline};

    std::uint32_t transmitErrorCounter{};
    std::uint32_t receiveErrorCounter{};

    /// Frames the driver reported as lost because a hardware or driver-side
    /// queue overflowed.
    std::uint64_t hardwareOverruns{};

    /// Frames TorqueBus itself dropped because its own receive queue was full.
    std::uint64_t softwareOverruns{};
};

[[nodiscard]] constexpr const char* toString(CanBusState state) noexcept
{
    switch (state) {
    case CanBusState::Offline:      return "Offline";
    case CanBusState::ErrorActive:  return "Error active";
    case CanBusState::ErrorWarning: return "Error warning";
    case CanBusState::ErrorPassive: return "Error passive";
    case CanBusState::BusOff:       return "Bus off";
    }
    return "Unknown";
}

[[nodiscard]] constexpr const char* toString(CanDirection direction) noexcept
{
    return direction == CanDirection::Tx ? "Tx" : "Rx";
}

using CanDeviceInfoList = std::vector<CanDeviceInfo>;

} // namespace torquebus
