// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The universal frame representation of the whole application.
//
// Architectural rule #4: CanFrame is independent of every vendor. It never
// includes a Qt header, a CANlib header or a PCAN-Basic header. Backends
// translate their native structures into this type at the boundary, and
// nothing above the driver layer ever sees a vendor type again.
//
// Architectural rule #5: a received frame never becomes a QObject. CanFrame is
// a trivially copyable aggregate so that millions of them can live contiguously
// in a vector and be moved through queues without an allocation.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>

namespace torquebus {

/// Maximum payload of a CAN FD frame, in bytes.
inline constexpr std::size_t kMaxCanPayload = 64U;

/// Maximum payload of a classic CAN frame, in bytes.
inline constexpr std::size_t kMaxClassicCanPayload = 8U;

/// Highest identifier representable in the 11-bit standard format.
inline constexpr std::uint32_t kMaxStandardIdentifier = 0x7FFU;

/// Highest identifier representable in the 29-bit extended format.
inline constexpr std::uint32_t kMaxExtendedIdentifier = 0x1FFF'FFFFU;

enum class CanDirection : std::uint8_t {
    Rx,
    Tx
};

enum class CanFrameFormat : std::uint8_t {
    Standard,
    Extended
};

/// A single CAN / CAN FD frame.
///
/// The layout is deliberately ordered wide-to-narrow so that the struct packs
/// into 80 bytes with no interior padding beyond the flag block.
struct CanFrame final {
    /// Timestamp in nanoseconds. Hardware timestamp when the device provides
    /// one (see CanCapabilities::hardwareTimestamp), otherwise a software
    /// timestamp taken as early as possible in the receive path. The epoch is
    /// the start of the measurement, not the wall clock.
    std::uint64_t timestampNs{};

    /// Raw identifier, without any format bit folded in. Interpret against
    /// `format` to know whether 11 or 29 bits are significant.
    std::uint32_t identifier{};

    /// Application channel index (CAN 1 == 0), not the hardware channel.
    std::uint8_t channel{};

    /// Data length code as transmitted on the bus (0..15).
    std::uint8_t dlc{};

    /// Decoded payload length in bytes (0..64). For classic CAN this equals
    /// `dlc`; for CAN FD it is the expansion of the DLC.
    std::uint8_t length{};

    CanDirection direction{CanDirection::Rx};
    CanFrameFormat format{CanFrameFormat::Standard};

    bool fd{};      ///< CAN FD frame.
    bool brs{};     ///< Bit rate switch (CAN FD only).
    bool esi{};     ///< Error state indicator (CAN FD only).
    bool rtr{};     ///< Remote transmission request (classic CAN only).
    bool error{};   ///< Error frame.

    std::array<std::uint8_t, kMaxCanPayload> data{};

    [[nodiscard]] constexpr bool isExtended() const noexcept
    {
        return format == CanFrameFormat::Extended;
    }

    [[nodiscard]] constexpr bool isRx() const noexcept
    {
        return direction == CanDirection::Rx;
    }
};

static_assert(std::is_trivially_copyable_v<CanFrame>,
              "CanFrame must stay trivially copyable: the whole pipeline "
              "relies on memcpy-able frames.");

/// Converts a data length code into a payload length in bytes.
///
/// Classic CAN caps at 8; CAN FD uses the discrete expansion defined by
/// ISO 11898-1. Values above 15 are clamped to the maximum payload.
[[nodiscard]] constexpr std::uint8_t payloadLengthFromDlc(std::uint8_t dlc, bool fd) noexcept
{
    if (!fd) {
        return dlc > 8U ? std::uint8_t{8U} : dlc;
    }

    switch (dlc) {
    case 9U:  return 12U;
    case 10U: return 16U;
    case 11U: return 20U;
    case 12U: return 24U;
    case 13U: return 32U;
    case 14U: return 48U;
    case 15U: return 64U;
    default:  return dlc > 15U ? std::uint8_t{64U} : dlc;
    }
}

/// Converts a payload length in bytes into the smallest DLC able to carry it.
///
/// CAN FD payloads are quantised, so a length that falls between two steps is
/// rounded up (the sender is expected to pad the remaining bytes).
[[nodiscard]] constexpr std::uint8_t dlcFromPayloadLength(std::uint8_t length, bool fd) noexcept
{
    if (!fd) {
        return length > 8U ? std::uint8_t{8U} : length;
    }

    if (length <= 8U)  { return length; }
    if (length <= 12U) { return 9U; }
    if (length <= 16U) { return 10U; }
    if (length <= 20U) { return 11U; }
    if (length <= 24U) { return 12U; }
    if (length <= 32U) { return 13U; }
    if (length <= 48U) { return 14U; }
    return 15U;
}

/// True when `identifier` fits the given frame format.
[[nodiscard]] constexpr bool isValidIdentifier(std::uint32_t identifier,
                                               CanFrameFormat format) noexcept
{
    return format == CanFrameFormat::Extended ? identifier <= kMaxExtendedIdentifier
                                              : identifier <= kMaxStandardIdentifier;
}

/// Number of bits a frame occupies on the wire, worst-case bit stuffing
/// included. Used by the statistics engine to compute bus load.
[[nodiscard]] constexpr std::uint32_t approximateFrameBitCount(const CanFrame& frame) noexcept
{
    // Classic CAN: 47 bits of overhead for standard, 67 for extended, plus the
    // payload; the +/-20 % worst case of bit stuffing is applied to the
    // stuffable part of the frame.
    const std::uint32_t overhead = frame.isExtended() ? 67U : 47U;
    const std::uint32_t payloadBits = static_cast<std::uint32_t>(frame.length) * 8U;
    const std::uint32_t stuffable = overhead + payloadBits - 13U;
    return overhead + payloadBits + (stuffable / 5U);
}

/// Formats the payload as space-separated uppercase hex, e.g. "01 02 FF".
[[nodiscard]] std::string toHexString(const CanFrame& frame);

/// Formats the identifier as three hex digits (standard) or eight (extended),
/// uppercase and zero padded - the convention every automotive tool uses.
[[nodiscard]] std::string toIdentifierString(const CanFrame& frame);

} // namespace torquebus
