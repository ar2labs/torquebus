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

#include <array>
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

/// The bitrate a channel is configured at when nobody says otherwise.
///
/// 250 kbit/s: the J1939 arbitration rate, and so the rate of every heavy
/// vehicle, agricultural machine and marine engine bus - which is the traffic
/// TorqueBus was started to read. Passenger cars are usually 500k and a
/// powertrain bus is sometimes 1M, so this default is wrong somewhere no matter
/// what it is; it is set to the family this tool is aimed at rather than to the
/// one that is most common in the world.
///
/// Getting it wrong is loud rather than subtle. A controller at the wrong rate
/// cannot acknowledge a frame, so it never reaches the bus at all: the error
/// counter climbs, the state goes to warning and then bus-off, and the Trace
/// stays empty. That is a mistake the Statistics panel makes visible in
/// seconds, which is why a default is a convenience here and not a hazard.
inline constexpr std::uint32_t kDefaultBitrate = 250'000;

/// The arbitration rates TorqueBus offers, slowest first.
///
/// Not "every rate that exists". A CAN controller is configured with segment
/// timing, not with a frequency, and deriving segments from an arbitrary number
/// is how you get a channel that opens cleanly and then produces error frames
/// on a real bus. These nine are the ones every backend here already has proper
/// timing for - it is exactly the list KvaserCanBackend accepts, which is the
/// most restrictive of them.
///
/// A bus at some other rate is a real thing and will be reachable when the
/// Hardware Manager can ask for the segment timing properly. Offering the
/// number without the timing would be offering a channel that does not work.
[[nodiscard]] constexpr std::array<std::uint32_t, 9> standardBitrates() noexcept
{
    return {10'000, 50'000, 62'000, 83'000, 100'000, 125'000, 250'000, 500'000, 1'000'000};
}

/// "250 kbit/s", "1 Mbit/s".
///
/// Formatted rather than printed raw because 1000 kbit/s is not how anybody
/// says it, and a list where one row breaks the pattern is a list somebody
/// misreads.
[[nodiscard]] inline std::string describeBitrate(std::uint32_t bitrate)
{
    if (bitrate >= 1'000'000 && bitrate % 1'000'000 == 0) {
        return std::to_string(bitrate / 1'000'000) + " Mbit/s";
    }

    if (bitrate >= 1000 && bitrate % 1000 == 0) {
        return std::to_string(bitrate / 1000) + " kbit/s";
    }

    // An odd rate - 83 kbit/s is stored as 83'000 but 33.333k would not be -
    // is printed as it is rather than rounded into a prettier lie.
    return std::to_string(bitrate) + " bit/s";
}

/// Nominal (arbitration) and data phase bit timing.
struct CanBitTiming final {
    /// Arbitration bitrate in bit/s. See kDefaultBitrate.
    std::uint32_t bitrate{kDefaultBitrate};

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
    Offline, ///< Channel closed / not started.
    ErrorActive, ///< Normal operation.
    ErrorWarning, ///< An error counter passed 96.
    ErrorPassive, ///< An error counter passed 127.
    BusOff ///< Transmit error counter passed 255.
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
    case CanBusState::Offline:
        return "Offline";
    case CanBusState::ErrorActive:
        return "Error active";
    case CanBusState::ErrorWarning:
        return "Error warning";
    case CanBusState::ErrorPassive:
        return "Error passive";
    case CanBusState::BusOff:
        return "Bus off";
    }
    return "Unknown";
}

[[nodiscard]] constexpr const char* toString(CanDirection direction) noexcept
{
    return direction == CanDirection::Tx ? "Tx" : "Rx";
}

using CanDeviceInfoList = std::vector<CanDeviceInfo>;

} // namespace torquebus
