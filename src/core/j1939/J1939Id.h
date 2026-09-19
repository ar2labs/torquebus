// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The 29-bit J1939 identifier, taken apart.
//
// On an ordinary CAN bus the identifier is a number and a database turns it
// into a name. On J1939 it is structure: three bits of priority, two page bits,
// and then three bytes saying *what* the message is, *who* it is for, and *who
// sent it*. A tool that shows 0x18FEE500 is technically correct and practically
// useless.
//
// Everything else in this directory is built on this file, so this is the file
// that has to be right first.
//
//   bits 28..26  priority         0 is most urgent, 6 is the usual default
//   bit  25      EDP              extended data page
//   bit  24      DP               data page
//   bits 23..16  PF               PDU format - decides everything below
//   bits 15..8   PS               PDU specific - meaning depends on PF
//   bits 7..0    SA               source address
//
// The rule that decides the rest:
//
//   * **PF < 240 (PDU1)** - the message is addressed to somebody. PS is that
//     ECU address, and is **not part of the PGN**.
//   * **PF >= 240 (PDU2)** - the message is broadcast. PS is a group extension
//     and **is part of the PGN**.
//
// Zeroing PS always, or never, are the two ways to get this wrong, and both
// produce the same symptom: a PGN that appears in no database, and a message
// the tool swears it does not recognise.

#pragma once

#include "core/can/CanFrame.h"

#include <cstdint>
#include <optional>

namespace torquebus {

/// The PF value at which PDU1 becomes PDU2.
inline constexpr std::uint8_t kJ1939Pdu2Threshold = 240U;

/// Broadcast destination. A PDU2 message is addressed here by definition.
inline constexpr std::uint8_t kJ1939GlobalAddress = 255U;

/// "No address." Sent by an ECU that lost an address contest and is not
/// arbitrary-address-capable. A legal source address, and one worth showing
/// rather than filtering away: an ECU transmitting from 254 is an ECU that is
/// on the bus and unreachable, which is a thing somebody needs to be told.
inline constexpr std::uint8_t kJ1939NullAddress = 254U;

/// Highest value a PGN can hold: EDP, DP and two bytes.
inline constexpr std::uint32_t kJ1939MaxPgn = 0x3'FFFFU;

// The handful of PGNs the protocol itself is made of. Named here because every
// layer above needs them, and a bare 60416 in three files is three chances to
// mistype it.

/// Request for a PGN (PDU1). 59904.
inline constexpr std::uint32_t kPgnRequest = 0x0'EA00U;

/// Transport protocol, connection management: RTS, CTS, BAM, Abort. 60416.
inline constexpr std::uint32_t kPgnTransportConnection = 0x0'EC00U;

/// Transport protocol, data transfer. 60160.
inline constexpr std::uint32_t kPgnTransportData = 0x0'EB00U;

/// Address claimed, and the NAME backing the claim. 60928.
inline constexpr std::uint32_t kPgnAddressClaimed = 0x0'EE00U;

/// Active diagnostic trouble codes. 65226.
inline constexpr std::uint32_t kPgnDm1 = 0x0'FECAU;

/// Previously active diagnostic trouble codes. 65227.
inline constexpr std::uint32_t kPgnDm2 = 0x0'FECBU;

/// A 29-bit identifier with its fields separated.
///
/// An aggregate of six small values rather than a class wrapping a uint32_t:
/// every consumer wants the fields, almost none of them wants the number back,
/// and the one that does can call identifier().
struct J1939Id final {
    /// 0..7. Lower wins arbitration. 6 is the default for ordinary traffic, 3
    /// is what address claiming and transport use.
    std::uint8_t priority{6U};

    bool extendedDataPage{false};
    bool dataPage{false};

    std::uint8_t pduFormat{0U};
    std::uint8_t pduSpecific{0U};
    std::uint8_t sourceAddress{0U};

    /// Addressed to one ECU, with PS carrying its address.
    [[nodiscard]] constexpr bool isPdu1() const noexcept { return pduFormat < kJ1939Pdu2Threshold; }

    /// Broadcast, with PS carrying a group extension that belongs to the PGN.
    [[nodiscard]] constexpr bool isPdu2() const noexcept { return !isPdu1(); }

    /// The 18-bit Parameter Group Number.
    ///
    /// This is what a database is searched by, and the whole reason the source
    /// address must not be part of it: the same message from two ECUs arrives
    /// under two identifiers and one PGN.
    [[nodiscard]] constexpr std::uint32_t pgn() const noexcept
    {
        const std::uint32_t pages = (static_cast<std::uint32_t>(extendedDataPage) << 17U)
                                    | (static_cast<std::uint32_t>(dataPage) << 16U);
        const std::uint32_t format = static_cast<std::uint32_t>(pduFormat) << 8U;

        // PDU1 PS is an address, not part of the group number. PDU2 PS is.
        return pages | format | (isPdu2() ? static_cast<std::uint32_t>(pduSpecific) : 0U);
    }

    /// Who the message is for: the PS byte for PDU1, and the global address for
    /// PDU2 - because "everyone" is the honest answer there, not "nobody".
    [[nodiscard]] constexpr std::uint8_t destinationAddress() const noexcept
    {
        return isPdu1() ? pduSpecific : kJ1939GlobalAddress;
    }

    /// True when nobody in particular is being addressed. A PDU1 sent to 255 is
    /// broadcast too, and is how a request reaches every ECU at once.
    [[nodiscard]] constexpr bool isBroadcast() const noexcept
    {
        return destinationAddress() == kJ1939GlobalAddress;
    }

    /// Back to the 29-bit number, for putting one on the wire.
    [[nodiscard]] constexpr std::uint32_t identifier() const noexcept
    {
        return (static_cast<std::uint32_t>(priority & 0x07U) << 26U)
               | (static_cast<std::uint32_t>(extendedDataPage) << 25U)
               | (static_cast<std::uint32_t>(dataPage) << 24U)
               | (static_cast<std::uint32_t>(pduFormat) << 16U)
               | (static_cast<std::uint32_t>(pduSpecific) << 8U)
               | static_cast<std::uint32_t>(sourceAddress);
    }
};

/// Takes a 29-bit identifier apart.
///
/// Every 29-bit number is a structurally valid J1939 identifier - there is no
/// bit pattern to reject - so this cannot fail. Whether the bus is J1939 at all
/// is a question about the bus, and the caller owns it.
[[nodiscard]] constexpr J1939Id j1939Decompose(std::uint32_t identifier) noexcept
{
    J1939Id result;
    result.priority = static_cast<std::uint8_t>((identifier >> 26U) & 0x07U);
    result.extendedDataPage = ((identifier >> 25U) & 0x01U) != 0U;
    result.dataPage = ((identifier >> 24U) & 0x01U) != 0U;
    result.pduFormat = static_cast<std::uint8_t>((identifier >> 16U) & 0xFFU);
    result.pduSpecific = static_cast<std::uint8_t>((identifier >> 8U) & 0xFFU);
    result.sourceAddress = static_cast<std::uint8_t>(identifier & 0xFFU);

    return result;
}

/// Takes a frame apart, or says it cannot.
///
/// Empty for an 11-bit frame. A standard-format frame on a J1939 bus is not a
/// J1939 message - it is somebody else traffic, or a proprietary link sharing
/// the wire - and reading its identifier as though the top byte were a PDU
/// format invents a PGN out of nothing.
[[nodiscard]] std::optional<J1939Id> j1939Decompose(const CanFrame& frame) noexcept;

/// Builds an identifier from a PGN and the two addresses.
///
/// `destination` is used only when the PGN is PDU1. A PDU2 group is broadcast
/// by construction and already carries its group extension in the PGN, so there
/// is nowhere to put a destination and nothing it could mean.
[[nodiscard]] constexpr std::uint32_t
j1939Identifier(std::uint32_t pgn,
                std::uint8_t source,
                std::uint8_t destination = kJ1939GlobalAddress,
                std::uint8_t priority = 6U) noexcept
{
    J1939Id id;
    id.priority = priority;
    id.extendedDataPage = ((pgn >> 17U) & 0x01U) != 0U;
    id.dataPage = ((pgn >> 16U) & 0x01U) != 0U;
    id.pduFormat = static_cast<std::uint8_t>((pgn >> 8U) & 0xFFU);
    id.sourceAddress = source;

    // For PDU1 the low byte of a PGN is not part of it, and is where the
    // destination goes. A caller passing a PDU1 PGN with that byte set has a
    // PGN that does not exist; honouring the byte would send the message to
    // whichever ECU the stray value happened to name.
    id.pduSpecific = id.isPdu1() ? destination : static_cast<std::uint8_t>(pgn & 0xFFU);

    return id.identifier();
}

} // namespace torquebus
